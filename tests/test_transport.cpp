#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>

#include "test_framework.h"
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
