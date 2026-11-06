// 投递速率采样（rate_sample.h）的用例。
//
// 两层：
//   1) 单元层：直接驱动 RateSampler，期望值按 draft 的公式手算，逐字段断言；
//   2) 集成层：SimLink + Transport 端到端，检查样本能否收敛到瓶颈带宽、
//      app-limited 标记是否在该出现时出现。字段漏赋值、时间戳取反这类错误，
//      单元层可能恰好对得上，集成层会偏出一个量级。
#include <functional>
#include <vector>

#include "test_framework.h"
#include "xport/rate_sample.h"
#include "xport/sim.h"
#include "xport/transport.h"

using namespace xport;

// ---- 单元层 ----

// 期望值推导（时间单位 µs，字节）：
//   s0 发于 1000，在途为空 → 区间起点、确认时钟都重置到 1000，快照 {delivered 0, dt 1000, fst 1000}
//   s1 发于 2000              → 快照 {0, 1000, 1000}
//   5000 确认 s0：delivered=1000；send_elapsed=1000-1000=0，ack_elapsed=5000-1000=4000
//   s2 发于 5000              → 快照 {1000, 5000, fst=1000（被 s0 的确认推到 s0 的发送时刻）}
//   6000 确认 s1：delivered=2000；send_elapsed=2000-1000=1000，ack_elapsed=6000-1000=5000
//   9000 确认 s2：delivered=3000；send_elapsed=5000-1000=4000，ack_elapsed=9000-5000=4000
TEST(RateSamplerExactIntervals) {
  RateSampler s;
  const RateSnapshot s0 = s.OnPacketSent(1000, 0);
  const RateSnapshot s1 = s.OnPacketSent(2000, 1000);
  CHECK_EQ(s0.delivered, 0);
  CHECK_EQ(s1.first_sent_t, 1000);

  s.OnPacketAcked(s0, 1000, 1000, 5000);
  RateSample rs = s.Finish(0);
  CHECK(rs.valid);
  CHECK_EQ(rs.delivered, 1000);
  CHECK_EQ(rs.send_elapsed_us, 0);
  CHECK_EQ(rs.ack_elapsed_us, 4000);
  CHECK_EQ(rs.interval_us, 4000);

  const RateSnapshot s2 = s.OnPacketSent(5000, 1000);
  CHECK_EQ(s2.delivered, 1000);
  CHECK_EQ(s2.delivered_t, 5000);
  CHECK_EQ(s2.first_sent_t, 1000);

  s.OnPacketAcked(s1, 2000, 1000, 6000);
  rs = s.Finish(0);
  CHECK(rs.valid);
  CHECK_EQ(rs.delivered, 2000);
  CHECK_EQ(rs.interval_us, 5000);

  s.OnPacketAcked(s2, 5000, 1000, 9000);
  rs = s.Finish(0);
  CHECK(rs.valid);
  CHECK_EQ(rs.delivered, 2000);
  CHECK_EQ(rs.prior_delivered, 1000);
  CHECK_EQ(rs.send_elapsed_us, 4000);
  CHECK_EQ(rs.ack_elapsed_us, 4000);
  CHECK_EQ(rs.RateBps(), 500000);  // 2000 B / 4 ms
}

// 一个 ACK 覆盖多个包：用最晚发出的那个定界，delivered 是整个 ACK 的增量。
TEST(RateSamplerBatchAckUsesLatestSent) {
  RateSampler s;
  const RateSnapshot s0 = s.OnPacketSent(1000, 0);
  const RateSnapshot s1 = s.OnPacketSent(2000, 1000);
  const RateSnapshot s2 = s.OnPacketSent(3000, 2000);

  // 乱序喂入，结果不应依赖顺序
  s.OnPacketAcked(s2, 3000, 1000, 10000);
  s.OnPacketAcked(s0, 1000, 1000, 10000);
  s.OnPacketAcked(s1, 2000, 1000, 10000);
  const RateSample rs = s.Finish(0);
  CHECK(rs.valid);
  CHECK_EQ(rs.delivered, 3000);
  CHECK_EQ(rs.send_elapsed_us, 2000);  // s2 发送时刻 - 区间起点
  CHECK_EQ(rs.ack_elapsed_us, 9000);
  CHECK_EQ(rs.interval_us, 9000);
}

// ACK 压缩：一批 ACK 挤在一起到达时，确认间隔很短，但发送间隔仍然反映真实速率。
// 只看确认间隔的实现会在这里算出 10 倍的虚高速率。
TEST(RateSamplerAckCompressionUsesSendInterval) {
  RateSampler s;
  std::vector<RateSnapshot> snaps;
  std::vector<Time> sent;
  // 100 µs 发一包，每包 1000 B → 真实发送速率 10 MB/s
  for (int i = 0; i < 20; ++i) {
    const Time t = 1000 + Time(i) * 100;
    sent.push_back(t);
    snaps.push_back(s.OnPacketSent(t, u64(i) * 1000));
  }
  // 第一个包正常确认，建立确认时钟
  s.OnPacketAcked(snaps[0], sent[0], 1000, 5000);
  (void)s.Finish(0);
  // 剩下 19 个包的 ACK 在 10 µs 内全部到达
  for (int i = 1; i < 20; ++i) s.OnPacketAcked(snaps[i], sent[i], 1000, 5010);
  const RateSample rs = s.Finish(0);
  CHECK(rs.valid);
  CHECK(rs.interval_us >= rs.send_elapsed_us);
  // 速率不能超过真实发送速率太多（容许 1 个包的边界误差）
  CHECK(rs.RateBps() <= 11'000'000);
}

// app-limited：标记时刻之后发出的包带标签；要等标记时的在途数据确认完、
// 再有一个之后发出的包被确认，标记才解除（与 Linux tcp_rate 的语义一致）。
TEST(RateSamplerAppLimitedLifecycle) {
  RateSampler s;
  const RateSnapshot s0 = s.OnPacketSent(1000, 0);
  CHECK(!s0.is_app_limited);

  s.OnAppLimited(/*inflight=*/1000);
  CHECK(s.app_limited());
  const RateSnapshot s1 = s.OnPacketSent(2000, 1000);
  CHECK(s1.is_app_limited);

  s.OnPacketAcked(s0, 1000, 1000, 5000);
  RateSample rs = s.Finish(0);
  CHECK(!rs.is_app_limited);  // s0 是标记前发的
  CHECK(s.app_limited());     // 标记时的在途刚好确认完，还没越过

  s.OnPacketAcked(s1, 2000, 1000, 6000);
  rs = s.Finish(0);
  CHECK(rs.is_app_limited);   // 样本继承 s1 的标签
  CHECK(!s.app_limited());    // 越过阈值，解除

  const RateSnapshot s2 = s.OnPacketSent(6000, 0);
  CHECK(!s2.is_app_limited);
}

// 区间短于 min_rtt 的样本不可信，直接作废。
TEST(RateSamplerRejectsIntervalBelowMinRtt) {
  RateSampler s;
  const RateSnapshot s0 = s.OnPacketSent(1000, 0);
  const RateSnapshot s1 = s.OnPacketSent(1100, 1000);
  s.OnPacketAcked(s0, 1000, 1000, 2000);
  (void)s.Finish(0);
  s.OnPacketAcked(s1, 1100, 1000, 2050);
  const RateSample rs = s.Finish(/*min_rtt_us=*/5000);  // interval = 1050
  CHECK(!rs.valid);
  CHECK_EQ(rs.RateBps(), 0);
}

// 没有任何包被确认的 ACK（纯重复 ACK）不产生样本。
TEST(RateSamplerNoAckedPacketsNoSample) {
  RateSampler s;
  (void)s.OnPacketSent(1000, 0);
  const RateSample rs = s.Finish(0);
  CHECK(!rs.valid);
}

// ---- 集成层 ----

namespace {

struct RateRig {
  SimLink link;
  Transport a;
  Transport b;

  explicit RateRig(const LinkParams& p) : link(p), a(link, 1, 2), b(link, 2, 1) {}

  // 每个 tick 后调用 on_tick；返回是否在时限内发送完成
  bool Run(Time limit_us, const std::function<void(Time)>& on_tick) {
    std::vector<u8> pkt;
    for (Time now = 0; now < limit_us; now += 100) {
      a.Tick(now);
      b.Tick(now);
      while (link.Poll(1, &pkt, now)) a.OnPacket(Bytes(pkt), now);
      while (link.Poll(2, &pkt, now)) b.OnPacket(Bytes(pkt), now);
      for (u32 sid = 0; sid < 4; ++sid) {
        for (Bytes v = b.Peek(sid); !v.empty(); v = b.Peek(sid)) b.Consume(sid, v.size());
      }
      on_tick(now);
      if (a.SendComplete()) return true;
    }
    return false;
  }
};

std::vector<u8> Payload(std::size_t n) {
  std::vector<u8> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = u8(i * 131u + 7u);
  return v;
}

}  // namespace

// 无丢包的大块传输：非 app-limited 样本的最大值应当贴近瓶颈带宽。
// 这就是 BBR max_bw 过滤器将要看到的值——偏高会打满队列，偏低会自我限速。
TEST(RateSampleMaxTracksBottleneck) {
  LinkParams p;
  p.bandwidth_mbps = 200.0;
  p.delay_us = 5000;  // RTT 10 ms
  RateRig r(p);
  const auto data = Payload(8u << 20);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  u64 max_bps = 0;
  u64 valid = 0;
  u64 app_limited = 0;
  CHECK(r.Run(20'000'000, [&](Time) {
    const RateSample& rs = r.a.LastRateSample();
    if (!rs.valid) return;
    ++valid;
    if (rs.is_app_limited) {
      ++app_limited;
      return;
    }
    if (rs.RateBps() > max_bps) max_bps = rs.RateBps();
  }));

  const double max_mbps = double(max_bps) * 8.0 / 1e6;
  std::printf("    max non-app-limited sample %.1f Mbps, %llu valid / %llu app-limited\n",
              max_mbps, (unsigned long long)valid, (unsigned long long)app_limited);
  CHECK(valid > 100);
  // 样本只计载荷字节，链路按线上字节限速，载荷占比约 97%，所以上限不应超过链路速率
  // 太多；下限 85% 留给头部开销和 ACK 合并造成的边界误差。
  CHECK(max_mbps > 200.0 * 0.85);
  CHECK(max_mbps < 200.0 * 1.15);
}

// 大块传输主体不应被误标 app-limited；否则 BBR 会把有效样本全部丢掉。
TEST(RateSampleBulkTransferMostlyNotAppLimited) {
  LinkParams p;
  p.bandwidth_mbps = 100.0;
  p.delay_us = 10000;
  RateRig r(p);
  const auto data = Payload(4u << 20);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  u64 valid = 0;
  u64 app_limited = 0;
  CHECK(r.Run(30'000'000, [&](Time) {
    const RateSample& rs = r.a.LastRateSample();
    if (!rs.valid) return;
    ++valid;
    if (rs.is_app_limited) ++app_limited;
  }));
  CHECK(valid > 0);
  CHECK(app_limited * 10 < valid);  // 只有尾部（数据发完之后）应当带标签
}

// 小数据量远小于 BDP：数据发完时窗口还空着，采样器必须进入 app-limited。
TEST(RateSampleSmallTransferMarksAppLimited) {
  LinkParams p;
  p.bandwidth_mbps = 1000.0;
  p.delay_us = 1000;
  RateRig r(p);
  const auto data = Payload(10 * 1024);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  bool seen = false;
  CHECK(r.Run(5'000'000, [&](Time) { seen = seen || r.a.rate_sampler().app_limited(); }));
  CHECK(seen);
}

// 有丢包时 delivered 单调、最终覆盖全部数据（判丢的包不计入，重传的副本计入）。
TEST(RateSampleDeliveredMonotonicUnderLoss) {
  LinkParams p;
  p.bandwidth_mbps = 200.0;
  p.delay_us = 5000;
  p.loss_pct = 1.0;
  p.seed = 42;
  RateRig r(p);
  const auto data = Payload(1u << 20);
  const u32 sid = r.a.OpenStream();
  r.a.Write(sid, Bytes(data));

  u64 prev = 0;
  bool monotonic = true;
  CHECK(r.Run(20'000'000, [&](Time) {
    const u64 d = r.a.rate_sampler().delivered();
    if (d < prev) monotonic = false;
    prev = d;
  }));
  CHECK(monotonic);
  CHECK(prev >= data.size());
}
