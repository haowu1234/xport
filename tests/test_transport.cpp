#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

#include "test_framework.h"
#include "xport/packet.h"
#include "xport/sim.h"
#include "xport/transport.h"

using namespace xport;

namespace {

struct Rig {
  SimLink link;
  Transport a;
  Transport b;

  // 协议不再保存全量数据（消费式生命周期），所以由测试侧负责取走并累积，
  // 最后做逐字节比对——验证强度和以前完全一样，只是"保存"的责任转移到了调用方。
  std::vector<std::vector<u8>> got;

  explicit Rig(const LinkParams& p, const TransportConfig& cfg = TransportConfig{})
      : link(p), a(link, 1, 2, cfg), b(link, 2, 1, cfg) {}

  void Drain() {
    for (u32 sid = 0; sid < 32; ++sid) {
      for (;;) {
        Bytes v = b.Peek(sid);
        if (v.empty()) break;
        if (got.size() <= sid) got.resize(sid + 1);
        got[sid].insert(got[sid].end(), v.begin(), v.end());
        b.Consume(sid, v.size());
      }
    }
  }

  const std::vector<u8>& Got(u32 sid) const {
    static const std::vector<u8> kEmpty;
    return sid < got.size() ? got[sid] : kEmpty;
  }

  bool Run(Time limit_us, const std::function<bool()>& done, Time step_us = 100) {
    std::vector<u8> pkt;
    for (Time now = 0; now < limit_us; now += step_us) {
      a.Tick(now);
      b.Tick(now);
      while (link.Poll(1, &pkt, now)) a.OnPacket(Bytes(pkt), now);
      while (link.Poll(2, &pkt, now)) b.OnPacket(Bytes(pkt), now);
      Drain();  // 每个 tick 都消费：模拟"应用持续取走数据"
      if (done()) return true;
    }
    Drain();
    return done();
  }
};

std::vector<u8> MakeData(std::size_t n, u32 seed) {
  std::vector<u8> v(n);
  u32 x = seed * 2654435761u + 1u;
  for (auto& b : v) {
    x = x * 1103515245u + 12345u;
    b = u8(x >> 16);
  }
  return v;
}

}  // namespace

TEST(TransferExactNoLoss) {
  LinkParams p;
  p.delay_us = 200;
  Rig r(p);

  auto data = MakeData(256 * 1024, 1);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  CHECK(r.Run(5'000'000, [&] { return r.a.SendComplete(); }));
  CHECK_EQ(r.Got(sid).size(), data.size());
  CHECK(std::equal(data.begin(), data.end(), r.Got(sid).begin()));
  CHECK_EQ(r.a.stats().retransmits, 0);
  CHECK_EQ(r.b.stats().bytes_recv, data.size());
}

TEST(TransferSurvivesOnePercentLoss) {
  LinkParams p;
  p.loss_pct = 1.0;
  p.delay_us = 1000;
  p.bandwidth_mbps = 100;
  p.seed = 7;
  Rig r(p);

  auto data = MakeData(512 * 1024, 2);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  CHECK(r.Run(30'000'000, [&] { return r.a.SendComplete(); }));
  CHECK_EQ(r.Got(sid).size(), data.size());
  CHECK(std::equal(data.begin(), data.end(), r.Got(sid).begin()));
  CHECK(r.a.stats().retransmits > 0);      // 确实发生了重传
  CHECK(r.a.stats().retransmits < 200);    // 但不是重传风暴
  CHECK_EQ(r.b.stats().bytes_recv, data.size());  // 重复包不得重复计数
}

TEST(MultiStreamInterleaved) {
  LinkParams p;
  p.loss_pct = 0.5;
  p.delay_us = 500;
  p.bandwidth_mbps = 200;
  p.seed = 11;
  Rig r(p);

  std::vector<std::vector<u8>> datas;
  std::vector<u32> sids;
  for (int i = 0; i < 4; ++i) {
    datas.push_back(MakeData(128 * 1024, u32(i + 10)));
    sids.push_back(r.a.OpenStream());
    r.a.Write(sids.back(), Bytes(datas.back()));
  }

  CHECK(r.Run(30'000'000, [&] { return r.a.SendComplete(); }));
  for (std::size_t i = 0; i < sids.size(); ++i) {
    const auto& got = r.Got(sids[i]);
    CHECK_EQ(got.size(), datas[i].size());
    CHECK(std::equal(datas[i].begin(), datas[i].end(), got.begin()));
  }
}

TEST(DuplicatePacketsDoNotDuplicateData) {
  LinkParams p;
  p.dup_pct = 20.0;
  p.delay_us = 300;
  p.seed = 3;
  Rig r(p);

  auto data = MakeData(64 * 1024, 5);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  CHECK(r.Run(5'000'000, [&] { return r.a.SendComplete(); }));
  CHECK_EQ(r.b.stats().bytes_recv, data.size());  // 幂等
  CHECK(std::equal(data.begin(), data.end(), r.Got(sid).begin()));
}

TEST(RecoversFromTenPercentLoss) {
  LinkParams p;
  p.loss_pct = 10.0;
  p.delay_us = 2000;
  p.bandwidth_mbps = 50;
  p.seed = 21;
  Rig r(p);

  auto data = MakeData(128 * 1024, 9);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  CHECK(r.Run(60'000'000, [&] { return r.a.SendComplete(); }));
  CHECK_EQ(r.Got(sid).size(), data.size());
  CHECK(std::equal(data.begin(), data.end(), r.Got(sid).begin()));
}

TEST(SameSeedIsDeterministic) {
  const auto run_once = [] {
    LinkParams p;
    p.loss_pct = 2.0;
    p.delay_us = 800;
    p.bandwidth_mbps = 80;
    p.seed = 99;
    Rig r(p);
    auto data = MakeData(64 * 1024, 4);
    const u32 sid = r.a.OpenStream();
    r.a.Write(sid, Bytes(data));
    r.Run(20'000'000, [&] { return r.a.SendComplete(); });
    return r.a.stats().retransmits;
  };

  CHECK_EQ(run_once(), run_once());  // 固定种子必须完全复现
}

// ---- M3 新增：流控、DRR、CUBIC 闭环行为 ----

TEST(FlowControlGatesThenResumes) {
  LinkParams p;
  p.delay_us = 1000;
  p.bandwidth_mbps = 100;
  p.queue_us = 20'000;

  TransportConfig cfg;
  // 窗口必须小于 BDP 才会真的挡住发送端：本链路 BDP ≈ 25 KB，
  // 取 64 KB 时发送端永远追不上流控额度（MAX_DATA 随每个 ACK 增长），
  // 测出来的是"没挡住"，什么也证明不了。取 16 KB 才是有效用例。
  cfg.initial_max_data = 16 * 1024;
  Rig r(p, cfg);

  auto data = MakeData(1024 * 1024, 21);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  CHECK(r.Run(20'000'000, [&] { return r.a.SendComplete(); }));
  CHECK(std::equal(data.begin(), data.end(), r.Got(sid).begin()));
  CHECK(r.a.stats().flow_blocked > 0);  // 确实被窗口挡住过
  // 挡住之后必须靠 MAX_DATA 恢复，不能靠流控探测兜底（探测是防死锁的最后手段）
  CHECK(r.a.stats().flow_probes < 3);
}

TEST(FlowControlSurvivesHeavyLoss) {
  LinkParams p;
  p.loss_pct = 3.0;  // MAX_DATA 随 ACK 捎带，这么高的丢包下必然有更新丢失
  p.delay_us = 2000;
  p.bandwidth_mbps = 20;
  p.queue_us = 50'000;
  p.seed = 17;

  TransportConfig cfg;
  cfg.initial_max_data = 32 * 1024;
  Rig r(p, cfg);

  auto data = MakeData(512 * 1024, 22);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  // 关键在于"能跑完"：如果窗口更新丢失后没人重发，两端会永久互等
  CHECK(r.Run(60'000'000, [&] { return r.a.SendComplete(); }));
  CHECK(std::equal(data.begin(), data.end(), r.Got(sid).begin()));
}

TEST(DrrDoesNotStarveSmallStreams) {
  LinkParams p;
  p.delay_us = 1000;
  p.bandwidth_mbps = 50;
  p.queue_us = 20'000;
  Rig r(p);

  std::vector<std::vector<u8>> datas;
  std::vector<u32> sids;
  // 一大两小：FIFO 调度会让小流排在大流后面，DRR 应该让它们先完成
  const std::size_t sizes[] = {4u << 20, 256u << 10, 256u << 10};
  for (std::size_t i = 0; i < 3; ++i) {
    datas.push_back(MakeData(sizes[i], u32(30 + i)));
    sids.push_back(r.a.OpenStream());
    r.a.Write(sids.back(), Bytes(datas.back()));
  }

  // 只等到总量 25%，此时两个小流应当已经发完（占总量的 12.5%）
  const std::size_t mark = std::size_t(4.5 * 1024 * 1024 * 0.25);
  CHECK(r.Run(30'000'000, [&] { return r.b.DeliveredBytes() >= mark; }));

  CHECK_EQ(r.Got(sids[1]).size(), datas[1].size());
  CHECK_EQ(r.Got(sids[2]).size(), datas[2].size());

  CHECK(r.Run(30'000'000, [&] { return r.a.SendComplete(); }));
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(std::equal(datas[i].begin(), datas[i].end(), r.Got(sids[i]).begin()));
  }
}

TEST(CubicDropsToBetaOnLossEvent) {
  LinkParams p;
  p.delay_us = 10'000;  // RTT 20ms
  p.bandwidth_mbps = 20;
  p.queue_us = 30'000;
  p.loss_from_us = 2'000'000;  // 2s 处整段丢弃 20ms
  p.loss_to_us = 2'020'000;
  Rig r(p);

  auto data = MakeData(12 * 1024 * 1024, 41);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  u64 cwnd_before = 0;
  u64 cwnd_after = 0;
  u64 events_before = 0;

  std::vector<u8> pkt;
  for (Time now = 0; now < 12'000'000; now += 20) {
    r.a.Tick(now);
    r.b.Tick(now);
    while (r.link.Poll(1, &pkt, now)) r.a.OnPacket(Bytes(pkt), now);
    while (r.link.Poll(2, &pkt, now)) r.b.OnPacket(Bytes(pkt), now);
    r.Drain();  // 消费式生命周期：不消费会被流控挡住，链路提前停住

    if (now >= 1'980'000 && cwnd_before == 0) {
      cwnd_before = r.a.Cwnd();
      events_before = r.a.cc().CongestionEvents();
    }
    if (events_before > 0 && r.a.cc().CongestionEvents() > events_before) {
      cwnd_after = r.a.Cwnd();
      break;
    }
  }

  CHECK(cwnd_before > 0);
  CHECK(cwnd_after > 0);
  // 乘性降窗比例必须落在 beta 附近（允许一个 RTT 内的窗口增长误差）
  const double ratio = double(cwnd_after) / double(cwnd_before);
  CHECK(ratio > 0.66 && ratio < 0.74);
}

// ---- 对端可控输入的边界 ----

TEST(StreamIdBeyondLimitIsDropped) {
  // sid 来自线上：一个 sid 很大的包不能让接收端 resize 出巨大的流表
  LinkParams p;
  SimLink link(p);
  TransportConfig cfg;
  cfg.max_streams = 8;
  Transport t(link, 2, 1, cfg);

  const auto deliver = [&](u32 sid, u64 pn) {
    const u8 body[16] = {7};
    u8 payload[kMaxPayload];
    StreamFrame f;
    f.sid = sid;
    f.data = Bytes(body, sizeof body);
    const std::size_t n = EncodeStream(MutBytes(payload, sizeof payload), f);
    Header h;
    h.pkt_num = pn;
    h.payload_len = u16(n);
    u8 pkt[kMtu];
    const std::size_t hn = EncodeHeader(h, MutBytes(pkt, sizeof pkt));
    std::memcpy(pkt + hn, payload, n);
    t.OnPacket(Bytes(pkt, hn + n), Time(pn));
  };

  deliver(1'000'000, 1);
  CHECK_EQ(t.stats().stream_limit, 1);
  CHECK_EQ(t.RecvBufferSize(1'000'000), 0);
  CHECK_EQ(t.ReadableBytes(1'000'000), 0);

  deliver(7, 2);  // 上限内的最后一个 sid 正常收
  CHECK_EQ(t.stats().stream_limit, 1);
  CHECK_EQ(t.ReadableBytes(7), 16);
}

TEST(AppLimitedSenderKeepsCwndBounded) {
  // 应用写入速率远低于链路：cwnd 不能随 ACK 无限上涨（RFC 7661）。
  // 修复前同一场景 cwnd 涨到约 6.8 MB，实际在途只有 4 KB。
  LinkParams p;
  p.bandwidth_mbps = 1000;
  p.delay_us = 1000;
  p.queue_us = 5000;
  Rig r(p);
  const u32 sid = r.a.OpenStream();
  const auto chunk = MakeData(1400, 3);
  const u64 initial = r.a.Cwnd();

  u64 max_inflight = 0;
  std::vector<u8> pkt;
  for (Time now = 0; now < 2'000'000; now += 100) {
    if (now % 1000 == 0 && now < 1'900'000) r.a.Write(sid, Bytes(chunk));  // 留 100 ms 收尾
    r.a.Tick(now);
    r.b.Tick(now);
    while (r.link.Poll(1, &pkt, now)) r.a.OnPacket(Bytes(pkt), now);
    while (r.link.Poll(2, &pkt, now)) r.b.OnPacket(Bytes(pkt), now);
    r.Drain();
    max_inflight = std::max<u64>(max_inflight, r.a.InFlight());
  }
  CHECK(max_inflight < initial);       // 场景确实是应用受限
  CHECK(r.a.Cwnd() <= 2 * initial);    // 窗口没有被未经验证的 ACK 撑大
  CHECK_EQ(r.Got(sid).size(), 1900u * 1400u);  // 数据照常送达
}

// ---- PTO（RFC 9002 §6.2）----

namespace {
// 连续驱动两端，每个 tick 回调一次 probe(now)；probe 返回 true 时停止
Time Drive(Rig& r, Time limit, const std::function<bool(Time)>& probe) {
  std::vector<u8> pkt;
  for (Time now = 0; now < limit; now += 100) {
    r.a.Tick(now);
    r.b.Tick(now);
    while (r.link.Poll(1, &pkt, now)) r.a.OnPacket(Bytes(pkt), now);
    while (r.link.Poll(2, &pkt, now)) r.b.OnPacket(Bytes(pkt), now);
    r.Drain();
    if (probe(now)) return now;
  }
  return 0;
}
}  // namespace

TEST(PtoRecoversTailLossFasterThanOldRto) {
  // 尾部丢包：最后一批数据包丢了，后面没有新包能触发包阈值，只能靠定时器。
  // 旧实现要等 200 ms 下限的 RTO；PTO ≈ SRTT + 4·RTTVAR + ack_delay，RTT 2 ms 时约 5 ms。
  LinkParams p;
  p.bandwidth_mbps = 100;
  p.delay_us = 1000;
  const auto data = MakeData(256 * 1024, 5);

  // 第一遍（无丢包）：记下最后一个新字节发出的时刻
  Time last_new = 0;
  {
    Rig r(p);
    const u32 sid = r.a.OpenStream();
    r.a.Write(sid, Bytes(data));
    Drive(r, 2'000'000, [&](Time now) {
      if (r.a.stats().bytes_sent >= data.size()) {
        last_new = now;
        return true;
      }
      return false;
    });
  }
  CHECK(last_new > 2000);

  // 第二遍：同一种子、同一时序，从最后一个新字节发出前 1 ms 起丢 6 ms。
  // 窗口要盖住时间阈值判丢后的那轮重传，否则时间阈值就能自己补回来，测不到 PTO。
  // 丢包窗口结束后路径恢复，PTO 探测包是唯一能触发新 ACK 的东西。
  LinkParams q = p;
  q.loss_from_us = last_new - 1000;
  q.loss_to_us = last_new + 5000;
  Rig r(q);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));
  const Time done = Drive(r, 2'000'000, [&](Time) {
    return r.a.SendComplete() && r.Got(sid).size() == data.size();
  });
  CHECK(done > 0);
  CHECK(r.Got(sid) == data);
  CHECK(r.link.dropped() > 0);       // 确实制造了尾部丢包
  CHECK(r.a.stats().pto_fires >= 1);  // 并且是 PTO 补回来的
  CHECK(done - last_new < 50'000);   // 远小于旧的 200 ms RTO 下限
}

TEST(PtoBacksOffExponentiallyOnBlackhole) {
  // 路径完全不通：PTO 必须指数退避，不能按固定周期持续重发
  LinkParams p;
  p.bandwidth_mbps = 100;   // 4 MB 需要约 340 ms，黑洞开始时一定还在传
  p.delay_us = 1000;
  p.loss_from_us = 50'000;  // 50 ms 后两个方向都进入黑洞，持续 2 s
  p.loss_to_us = 2'050'000;
  Rig r(p);
  const auto data = MakeData(4 << 20, 6);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  u64 ptos_in_hole = 0;
  double pto_ms_at_end = 0;
  bool checked = false;
  const Time done = Drive(r, 30'000'000, [&](Time now) {
    if (!checked && now >= 2'049'900) {
      ptos_in_hole = r.a.stats().pto_fires;
      pto_ms_at_end = r.a.stats().pto_ms;
      checked = true;
    }
    return r.a.SendComplete();
  });
  // 2 s 黑洞、基础 PTO 约 5 ms：不退避会触发约 400 次，退避后约 log2(2000/5) ≈ 9 次
  CHECK(ptos_in_hole >= 5);
  CHECK(ptos_in_hole <= 12);
  CHECK(pto_ms_at_end > 200.0);
  // 路径恢复后能继续传完，并且退避已清零
  CHECK(done > 0);
  CHECK(r.Got(sid) == data);
  CHECK(r.a.stats().pto_ms < 50.0);
}

TEST(PersistentCongestionCollapsesCwnd) {
  // 黑洞横跨多个 PTO：判丢时应识别为持续拥塞，窗口回到最小值，而不是只降一次 β
  LinkParams p;
  p.bandwidth_mbps = 100;
  p.delay_us = 1000;
  p.loss_from_us = 100'000;
  p.loss_to_us = 400'000;
  Rig r(p);
  const auto data = MakeData(8 << 20, 7);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  u64 cwnd_before = 0;
  u64 cwnd_min_after = ~0ull;
  const Time done = Drive(r, 30'000'000, [&](Time now) {
    if (now < 100'000) cwnd_before = r.a.Cwnd();
    if (now > 400'000 && now < 500'000) cwnd_min_after = std::min<u64>(cwnd_min_after, r.a.Cwnd());
    return r.a.SendComplete();
  });
  CHECK(cwnd_before > 20 * 1368);  // 先涨起来，后面的回落才有意义
  CHECK(r.a.stats().persistent_congestion >= 1);
  CHECK(cwnd_min_after <= 4 * 1368);  // 回到最小窗口附近，而不是 0.7 × cwnd_before
  CHECK(done > 0);
  CHECK(r.Got(sid) == data);
}

// 发送缓冲按确认释放。旧实现 Write 只往一个 vector 追加、从不截断，长连接写了多少就占多少内存。
// 这里边发边写 64 MB（发送端只领先 1 MB），结束时占用的应当只有在途 + 未发的那一点，
// 而且逐字节交付不受影响——释放错了块，重传会读到别的数据，比对必然失败。
TEST(SendBufferReleasedAsDataIsAcked) {
  LinkParams p;
  p.bandwidth_mbps = 400.0;
  p.delay_us = 2'000;
  p.loss_pct = 0.5;  // 有重传：确认前缀会出现空洞，释放必须等空洞补上
  p.seed = 5;
  TransportConfig cfg;
  cfg.initial_max_data = 8ull << 20;
  cfg.recv_window_bytes = 8ull << 20;
  Rig r(p, cfg);
  const u32 sid = r.a.OpenStream();
  const std::size_t kTotal = 64u << 20, kStep = 1u << 20;
  std::vector<u8> chunk(kStep);
  u64 written = 0, x = 12345;
  std::size_t peak = 0;
  std::vector<u8> expect_tail;  // 只保留最后一块的期望内容，其余用滚动哈希比
  u64 want_hash = 1469598103934665603ull, got_hash = 1469598103934665603ull;
  std::size_t got_len = 0;
  std::vector<u8> pkt;
  bool done = false;
  for (Time now = 0; now < 30'000'000 && !done; now += 50) {
    if (written < kTotal && written - r.a.StreamSentBytes(sid) < kStep) {
      for (auto& b : chunk) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        b = u8(x >> 56);
        want_hash = (want_hash ^ b) * 1099511628211ull;
      }
      r.a.Write(sid, Bytes(chunk));
      written += kStep;
    }
    r.a.Tick(now);
    r.b.Tick(now);
    while (r.link.Poll(1, &pkt, now)) r.a.OnPacket(Bytes(pkt), now);
    while (r.link.Poll(2, &pkt, now)) r.b.OnPacket(Bytes(pkt), now);
    for (Bytes v = r.b.Peek(sid); !v.empty(); v = r.b.Peek(sid)) {
      for (u8 b : v) got_hash = (got_hash ^ b) * 1099511628211ull;
      got_len += v.size();
      r.b.Consume(sid, v.size());
    }
    peak = std::max(peak, r.a.SendBufferBytes());
    done = written == kTotal && r.a.SendComplete() && got_len == kTotal;
  }
  std::printf("    wrote %zu MB, send buffer peak %.2f MB, final %.2f MB\n", kTotal >> 20,
              double(peak) / 1048576.0, double(r.a.SendBufferBytes()) / 1048576.0);
  CHECK(done);
  CHECK(got_hash == want_hash);
  CHECK(peak < (8u << 20));                     // 远小于 64 MB：受流控窗口 + 1 MB 领先量约束
  CHECK(r.a.SendBufferBytes() <= (128u << 10));  // 全部确认后最多剩两块（当前写入块 + 对齐余量）
}

// 1 Gbps × 100 ms 的 BDP 是 12.5 MB（约 9100 包）。旧实现的在途包表固定 8192 槽、
// 默认 max_cwnd 8 MB，在途被卡在 8 MB 左右，吞吐上不去（约 8 MB / 100 ms ≈ 670 Mbps）。
TEST(LargeBdpPathNotCappedByInflightTable) {
  LinkParams p;
  p.bandwidth_mbps = 1000.0;
  p.delay_us = 50'000;
  p.queue_us = 100'000;  // 1 BDP
  TransportConfig cfg;
  cfg.cc.algo = CcAlgo::kBbr;  // Startup 几个 RTT 就能把在途推到 2 × BDP，测得快
  cfg.initial_max_data = 256ull << 20;
  cfg.recv_window_bytes = 64ull << 20;
  Rig r(p, cfg);
  const u32 sid = r.a.OpenStream();
  std::vector<u8> chunk(4u << 20, 0x6b);
  u64 written = 0, max_inflight = 0, at_2s = 0;
  std::vector<u8> pkt;
  for (Time now = 0; now < 4'000'000; now += 10) {
    if (written - r.a.StreamSentBytes(sid) < chunk.size()) {
      r.a.Write(sid, Bytes(chunk));
      written += chunk.size();
    }
    r.a.Tick(now);
    r.b.Tick(now);
    while (r.link.Poll(1, &pkt, now)) r.a.OnPacket(Bytes(pkt), now);
    while (r.link.Poll(2, &pkt, now)) r.b.OnPacket(Bytes(pkt), now);
    for (Bytes v = r.b.Peek(sid); !v.empty(); v = r.b.Peek(sid)) r.b.Consume(sid, v.size());
    max_inflight = std::max<u64>(max_inflight, r.a.InFlight());
    if (now == 2'000'000) at_2s = r.b.DeliveredBytes();
  }
  const double mbps = double(r.b.DeliveredBytes() - at_2s) * 8.0 / 2e6;
  std::printf("    max inflight %.1f MB, goodput 2-4 s %.0f Mbps\n",
              double(max_inflight) / 1048576.0, mbps);
  CHECK(max_inflight > (12u << 20));
  CHECK(mbps > 900.0);
}
