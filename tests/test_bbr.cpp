// BBR v1（bbr.h）的用例。
//
// 两层，和 test_cc.cpp / test_rate_sample.cpp 的分工一致：
//   1) 纯模型层：直接喂 BbrAck，断言状态机的每一次转移和每个增益值。
//      状态机错了在这里就能定位到具体判据，而不是在仿真里看吞吐猜原因；
//   2) 仿真层：SimLink 端到端，检查 BBR 宣称的性质——估计收敛到瓶颈、
//      Drain 真的排空队列、随机丢包下不像 CUBIC 那样塌窗、ProbeRTT 按时触发。
#include <cstdio>
#include <functional>
#include <vector>

#include "test_framework.h"
#include "xport/bbr.h"
#include "xport/sim.h"
#include "xport/transport.h"

using namespace xport;

namespace {

constexpr u64 kMss = 1368;

// 理想化的模型驱动：瓶颈带宽 bw_Bps、RTT 恒定，每个 ACK 确认 kMss，
// 每轮（一个 RTT）确认 per_round 个包。速率样本按真实定义构造：一轮的投递量 / 一轮时长。
struct ModelFeeder {
  BbrCc bbr;
  Time now = 0;
  u64 delivered = 0;
  u64 inflight = 0;
  u64 prev_round_delivered = 0;
  Time rtt_us = 10'000;

  // 跑一轮。sample_bps 为这一轮测到的投递速率；inflight 由调用方设定。
  void Round(u64 sample_bps, int acks, u64 inflight_after, u64 lost = 0, bool app_limited = false) {
    const Time step = rtt_us / Time(acks);
    for (int i = 0; i < acks; ++i) {
      now += step;
      BbrAck a;
      a.now = now;
      a.acked = kMss;
      a.lost = (i == acks - 1) ? lost : 0;
      a.prior_inflight = inflight;
      delivered += kMss;
      inflight = inflight_after;
      a.inflight = inflight;
      a.delivered = delivered;
      a.rtt_us = rtt_us;
      a.has_newest_acked = true;
      a.newest_acked_sent_t = now - rtt_us;
      a.rs.valid = true;
      a.rs.delivered = u64(double(sample_bps) * double(rtt_us) / 1e6);
      a.rs.interval_us = rtt_us;
      // 本轮所有 ACK 的定界包都发出于上一轮结束时：第一个 ACK 推进轮次，
      // 其余 ACK 的 prior_delivered 低于新一轮的终点，不会重复推进
      a.rs.prior_delivered = prev_round_delivered;
      a.rs.is_app_limited = app_limited;
      bbr.OnAck(a);
    }
    prev_round_delivered = delivered;
  }
};

bool Near(double a, double b, double tol) { return a >= b - tol && a <= b + tol; }

}  // namespace

// ---- WindowedMax ----

TEST(WindowedMaxTracksMaximum) {
  WindowedMax m;
  m.Reset(0, 10);
  CHECK_EQ(m.Update(10, 1, 5), 10);
  CHECK_EQ(m.Update(10, 2, 20), 20);
  CHECK_EQ(m.Update(10, 3, 15), 20);
}

// 最大值过期后要回落到窗口内的次大值，而不是一直卡在历史峰值——
// 否则带宽下降后 BBR 会长期按旧带宽发送，持续打满队列。
TEST(WindowedMaxExpiresOldPeak) {
  WindowedMax m;
  m.Reset(0, 100);
  for (u64 t = 1; t <= 10; ++t) m.Update(10, t, 50);
  CHECK_EQ(m.Get(), 100);
  for (u64 t = 11; t <= 25; ++t) m.Update(10, t, 50);
  CHECK_EQ(m.Get(), 50);
}

// ---- 模型层：状态机 ----

TEST(BbrStartsInStartupWithHighGain) {
  BbrCc b;
  CHECK(b.state() == BbrState::kStartup);
  CHECK(Near(b.PacingGain(), BbrCc::kHighGain, 1e-9));
  CHECK(Near(b.CwndGain(), BbrCc::kHighGain, 1e-9));
  CHECK_EQ(b.cwnd(), 10 * kMss);
}

// Startup 期间带宽每轮翻倍，管道不会被判满；带宽连续 3 轮涨不到 25% 才退出。
TEST(BbrStartupExitsAfterThreeFlatRounds) {
  ModelFeeder f;
  u64 bw = 1'000'000;
  for (int r = 0; r < 5; ++r) {
    f.Round(bw, 20, 30 * kMss);
    bw *= 2;
    CHECK(!f.bbr.FilledPipe());
  }
  const u64 plateau = bw;
  f.Round(plateau, 20, 30 * kMss);  // 刷新 full_bw
  f.Round(plateau, 20, 30 * kMss);
  f.Round(plateau, 20, 30 * kMss);
  CHECK(!f.bbr.FilledPipe());
  f.Round(plateau, 20, 30 * kMss);  // 第三个平台轮
  CHECK(f.bbr.FilledPipe());
  CHECK(f.bbr.state() != BbrState::kStartup);
}

// app-limited 的平台不算"管道满"：应用自己没数据，不说明网络到顶了。
TEST(BbrAppLimitedPlateauDoesNotFillPipe) {
  ModelFeeder f;
  for (int r = 0; r < 10; ++r) f.Round(1'000'000, 20, 5 * kMss, 0, /*app_limited=*/true);
  CHECK(!f.bbr.FilledPipe());
  CHECK(f.bbr.state() == BbrState::kStartup);
}

// Drain 一直保持到在途降到 1 BDP（加量化余量），然后进 ProbeBW。
TEST(BbrDrainUntilInflightBelowBdp) {
  ModelFeeder f;
  const u64 bw = 10'000'000;  // 10 MB/s × 10 ms = 100 KB BDP
  for (int r = 0; r < 8; ++r) f.Round(bw, 20, 300 * kMss);
  CHECK(f.bbr.state() == BbrState::kDrain);
  CHECK(Near(f.bbr.PacingGain(), BbrCc::kDrainGain, 1e-9));
  const u64 bdp = f.bbr.BdpBytes();
  CHECK(Near(double(bdp), 100'000.0, 2000.0));

  f.Round(bw, 20, 2 * bdp);
  CHECK(f.bbr.state() == BbrState::kDrain);
  f.Round(bw, 20, bdp / 2);
  CHECK(f.bbr.state() == BbrState::kProbeBw);
  // 随机起始相位不能是 0.75 的排队相
  CHECK(f.bbr.CycleIndex() != 1);
}

// ProbeBW 的 8 相增益循环：每相至少一个 RTprop，探测相之后紧跟排队相。
TEST(BbrProbeBwCyclesThroughGains) {
  ModelFeeder f;
  const u64 bw = 10'000'000;
  for (int r = 0; r < 8; ++r) f.Round(bw, 20, 300 * kMss);
  f.Round(bw, 20, 10 * kMss);
  CHECK(f.bbr.state() == BbrState::kProbeBw);

  std::vector<double> gains;
  double last = -1;
  for (int r = 0; r < 40 && gains.size() < 9; ++r) {
    // 在途维持在 1.25 BDP 以上，让探测相能按"在途达标"退出
    f.Round(bw, 20, u64(1.3 * double(f.bbr.BdpBytes())));
    if (f.bbr.PacingGain() != last) {
      last = f.bbr.PacingGain();
      gains.push_back(last);
    }
  }
  bool saw_probe_then_drain = false;
  for (std::size_t i = 0; i + 1 < gains.size(); ++i) {
    if (Near(gains[i], 1.25, 1e-9)) {
      CHECK(Near(gains[i + 1], 0.75, 1e-9));
      saw_probe_then_drain = true;
    }
  }
  CHECK(saw_probe_then_drain);
}

// pacing = gain × BtlBw × 0.99，cwnd 收敛到 cwnd_gain × BDP 附近。
TEST(BbrSteadyStatePacingAndCwndFollowModel) {
  ModelFeeder f;
  const u64 bw = 10'000'000;
  for (int r = 0; r < 8; ++r) f.Round(bw, 20, 300 * kMss);
  for (int r = 0; r < 30; ++r) f.Round(bw, 20, f.bbr.BdpBytes());
  CHECK(f.bbr.state() == BbrState::kProbeBw);
  const double expect_rate = f.bbr.PacingGain() * double(bw) * BbrCc::kPacingMargin;
  CHECK(Near(double(f.bbr.PacingRateBps()), expect_rate, expect_rate * 0.01));
  const double target = 2.0 * double(f.bbr.BdpBytes()) + 3.0 * double(kMss);
  CHECK(Near(double(f.bbr.cwnd()), target, double(kMss)));
}

// 拥塞事件：下一次 OnAck 进入包守恒，cwnd ≈ 在途 + 本次确认；新的一轮开始后解除。
TEST(BbrLossEntersPacketConservation) {
  ModelFeeder f;
  const u64 bw = 10'000'000;
  for (int r = 0; r < 8; ++r) f.Round(bw, 20, 300 * kMss);
  for (int r = 0; r < 20; ++r) f.Round(bw, 20, f.bbr.BdpBytes());
  const u64 before = f.bbr.cwnd();

  f.bbr.OnCongestion(f.now);
  CHECK(f.bbr.InRecovery(f.now));
  CHECK(!f.bbr.InRecovery(f.now + 1));
  const u64 inflight = 40 * kMss;
  BbrAck a;
  a.now = ++f.now;
  a.acked = kMss;
  a.lost = 2 * kMss;
  a.prior_inflight = inflight + 3 * kMss;
  a.inflight = inflight;
  f.delivered += kMss;
  a.delivered = f.delivered;
  a.rtt_us = f.rtt_us;
  a.has_newest_acked = true;
  a.newest_acked_sent_t = f.now - f.rtt_us;  // 降窗前发出的包：仍在恢复期内
  f.bbr.OnAck(a);
  CHECK(f.bbr.InPacketConservation());
  CHECK_EQ(f.bbr.cwnd(), inflight + kMss);
  CHECK(f.bbr.cwnd() < before);
  CHECK_EQ(f.bbr.CongestionEvents(), 1);
}

// RTprop 10 s 没刷新 → ProbeRTT，cwnd 压到 4 包；停留 200 ms 且满一轮后恢复原窗口。
TEST(BbrProbeRttAfterTenSecondsThenRestores) {
  ModelFeeder f;
  const u64 bw = 10'000'000;
  for (int r = 0; r < 8; ++r) f.Round(bw, 20, 300 * kMss);
  // RTT 抬高一点，min_rtt 就不会被刷新
  f.rtt_us = 10'500;
  while (f.now < 10'500'000) f.Round(bw, 20, f.bbr.BdpBytes());
  CHECK(f.bbr.state() == BbrState::kProbeRtt);
  CHECK_EQ(f.bbr.ProbeRttCount(), 1);
  const u64 prior = 2 * f.bbr.BdpBytes();

  f.Round(bw, 20, 4 * kMss);
  CHECK_EQ(f.bbr.cwnd(), f.bbr.MinPipeCwnd());
  // 不满 200 ms 不退出
  const Time armed = f.now;
  while (f.now < armed + 150'000) f.Round(bw, 20, 4 * kMss);
  CHECK(f.bbr.state() == BbrState::kProbeRtt);
  while (f.now < armed + 260'000) f.Round(bw, 20, 4 * kMss);
  CHECK(f.bbr.state() == BbrState::kProbeBw);
  CHECK(f.bbr.cwnd() + kMss >= prior);
}

// ---- 仿真层 ----

namespace {

struct BbrRig {
  SimLink link;
  Transport a;
  Transport b;

  BbrRig(const LinkParams& p, CcAlgo algo) : link(p), a(link, 1, 2, Cfg(algo)), b(link, 2, 1, Cfg(algo)) {}

  static TransportConfig Cfg(CcAlgo algo) {
    TransportConfig c;
    c.cc.algo = algo;
    c.initial_max_data = 16ull << 20;  // 流控不能成为瓶颈，否则测的是窗口不是 CC
    return c;
  }

  bool Run(Time limit_us, Time step_us, const std::function<void(Time)>& on_tick = nullptr) {
    std::vector<u8> pkt;
    for (Time now = 0; now < limit_us; now += step_us) {
      a.Tick(now);
      b.Tick(now);
      while (link.Poll(1, &pkt, now)) a.OnPacket(Bytes(pkt), now);
      while (link.Poll(2, &pkt, now)) b.OnPacket(Bytes(pkt), now);
      for (Bytes v = b.Peek(0); !v.empty(); v = b.Peek(0)) b.Consume(0, v.size());
      if (on_tick) on_tick(now);
      if (a.SendComplete()) return true;
    }
    return false;
  }
};

std::vector<u8> Payload(std::size_t n) {
  std::vector<u8> v(n);
  for (std::size_t i = 0; i < n; ++i) v[i] = u8(i * 167u + 3u);
  return v;
}

}  // namespace

// 估计量收敛：BtlBw 在链路带宽 ±10%（只计载荷，链路按线上字节限速），RTprop 等于传播时延。
TEST(BbrSimEstimatesConvergeToPath) {
  LinkParams p;
  p.bandwidth_mbps = 20.0;
  p.delay_us = 20'000;
  p.queue_us = 100'000;
  BbrRig r(p, CcAlgo::kBbr);
  const auto data = Payload(6u << 20);
  r.a.OpenStream();
  r.a.Write(0, Bytes(data));
  CHECK(r.Run(10'000'000, 10));

  const double bw_mbps = double(r.a.bbr().BtlBwBps()) * 8.0 / 1e6;
  std::printf("    BtlBw %.2f Mbps, RTprop %.2f ms, state %s\n", bw_mbps,
              double(r.a.bbr().MinRttUs()) / 1000.0, BbrStateName(r.a.bbr().state()));
  CHECK(bw_mbps > 18.0 && bw_mbps < 21.0);
  CHECK(r.a.bbr().MinRttUs() >= 40'000 && r.a.bbr().MinRttUs() < 41'000);
  CHECK(r.a.bbr().FilledPipe());
}

// Drain 排空 Startup 冲进去的队列：进入 ProbeBW 后队列深度应回到很小的值，
// 并在其后的稳态里保持——这是 BBR 相对 CUBIC "填满缓冲区" 的核心区别。
TEST(BbrSimDrainEmptiesStartupQueue) {
  LinkParams p;
  p.bandwidth_mbps = 20.0;
  p.delay_us = 20'000;
  p.queue_us = 500'000;  // 深缓冲：CUBIC 会把它填满
  BbrRig r(p, CcAlgo::kBbr);
  const auto data = Payload(6u << 20);
  r.a.OpenStream();
  r.a.Write(0, Bytes(data));

  std::size_t peak_startup = 0;
  std::size_t peak_steady = 0;
  Time probe_bw_since = 0;
  CHECK(r.Run(10'000'000, 10, [&](Time now) {
    const std::size_t q = r.link.queue_depth(2, now);
    const BbrState s = r.a.bbr().state();
    if (s == BbrState::kStartup || s == BbrState::kDrain) peak_startup = std::max(peak_startup, q);
    if (s == BbrState::kProbeBw && probe_bw_since == 0) probe_bw_since = now;
    // 进入 ProbeBW 1 s 之后才算稳态，跳过排空尾巴
    if (probe_bw_since != 0 && now > probe_bw_since + 1'000'000) {
      peak_steady = std::max(peak_steady, q);
    }
  }));
  std::printf("    startup queue peak %zu pkts, steady peak %zu pkts\n", peak_startup, peak_steady);
  CHECK(probe_bw_since != 0);
  CHECK(peak_startup > 50);
  // 稳态队列只来自 1.25 探测相：约 0.25 BDP ≈ 18 包，留余量
  CHECK(peak_steady < 30);
  CHECK(r.link.queue_drops() == 0);
}

// 随机丢包：BBR 不把丢包当拥塞信号，吞吐不应像 CUBIC 那样被压下去。
TEST(BbrSimToleratesRandomLossBetterThanCubic) {
  const auto run = [](CcAlgo algo) {
    LinkParams p;
    p.bandwidth_mbps = 50.0;
    p.delay_us = 10'000;
    p.queue_us = 50'000;
    p.loss_pct = 1.0;
    p.seed = 2026;
    BbrRig r(p, algo);
    const auto data = Payload(8u << 20);
    r.a.OpenStream();
    r.a.Write(0, Bytes(data));
    Time end = 0;
    const bool ok = r.Run(30'000'000, 10, [&](Time now) { end = now; });
    CHECK(ok);
    return double(data.size()) * 8.0 / (double(end) / 1e6) / 1e6;
  };
  const double cubic = run(CcAlgo::kCubic);
  const double bbr = run(CcAlgo::kBbr);
  std::printf("    1%% loss, 50 Mbps: cubic %.1f Mbps, bbr %.1f Mbps\n", cubic, bbr);
  CHECK(bbr > 40.0);
  CHECK(bbr > 1.5 * cubic);
}

// 长时间传输里 ProbeRTT 按 10 s 周期触发，且触发后传输照常完成。
// 这个场景同时守着 transport 的一个死锁：Startup 冲出的队列让 RTT 超过判丢时间阈值后，
// 若只对"在途"包取 RTT 样本，所有包都会在 ACK 前被判丢、SRTT 永远冻结，传输停在 0.2 MB。
TEST(BbrSimProbeRttFiresPeriodically) {
  LinkParams p;
  p.bandwidth_mbps = 10.0;
  p.delay_us = 10'000;
  p.queue_us = 100'000;
  BbrRig r(p, CcAlgo::kBbr);
  const auto data = Payload(30u << 20);  // 约 25 s
  r.a.OpenStream();
  r.a.Write(0, Bytes(data));
  CHECK(r.Run(40'000'000, 20));
  std::printf("    ProbeRTT %llu times\n", (unsigned long long)r.a.bbr().ProbeRttCount());
  CHECK(r.a.bbr().ProbeRttCount() >= 2);
  CHECK(r.a.bbr().ProbeRttCount() <= 3);
}

// 正确性兜底：BBR 下多流、重度丢包仍然逐字节交付（和 CUBIC 同一批不变量）。
TEST(BbrSimTransfersExactlyUnderHeavyLoss) {
  LinkParams p;
  p.bandwidth_mbps = 100.0;
  p.delay_us = 2'000;
  p.loss_pct = 5.0;
  p.seed = 9;
  SimLink link(p);
  TransportConfig c;
  c.cc.algo = CcAlgo::kBbr;
  Transport a(link, 1, 2, c);
  Transport b(link, 2, 1, c);
  const auto d0 = Payload(512 * 1024);
  auto d1 = Payload(300 * 1024);
  for (auto& x : d1) x ^= 0x5a;
  a.Write(a.OpenStream(), Bytes(d0));
  a.Write(a.OpenStream(), Bytes(d1));

  std::vector<u8> got0, got1, pkt;
  bool done = false;
  for (Time now = 0; now < 20'000'000 && !done; now += 50) {
    a.Tick(now);
    b.Tick(now);
    while (link.Poll(1, &pkt, now)) a.OnPacket(Bytes(pkt), now);
    while (link.Poll(2, &pkt, now)) b.OnPacket(Bytes(pkt), now);
    for (Bytes v = b.Peek(0); !v.empty(); v = b.Peek(0)) {
      got0.insert(got0.end(), v.begin(), v.end());
      b.Consume(0, v.size());
    }
    for (Bytes v = b.Peek(1); !v.empty(); v = b.Peek(1)) {
      got1.insert(got1.end(), v.begin(), v.end());
      b.Consume(1, v.size());
    }
    done = a.SendComplete() && got0.size() == d0.size() && got1.size() == d1.size();
  }
  CHECK(done);
  CHECK(got0 == d0);
  CHECK(got1 == d1);
}

// 接收端区间表（RangeSet，512 段）被打满时，没能登记的数据不能被 ACK。
// 旧实现先 RecordRecv 再处理帧：表满时数据被丢、包却照样被确认，发送端永不重传，
// 接收端的连续前缀从此卡死。复现场景是实测卡死的那个：1 条 BBRv1 与 1 条 CUBIC
// 共享 50 Mbps / RTT 30 ms / 8 BDP 瓶颈，CUBIC 先跑 2 s 把缓冲填满，BBR 的 Startup
// 冲进来时一次丢掉上千个包、接收端留下上千个空洞。旧实现 BBR 流的交付停在 6.38 MB。
TEST(RecvRangeOverflowDoesNotAckUnrecordedData) {
  LinkParams p;
  p.bandwidth_mbps = 50.0;
  p.delay_us = 15'000;
  p.queue_us = 240'000;  // 8 × 30 ms
  SimLink link(p);
  link.SetBottleneckGroup(101, 9);
  link.SetBottleneckGroup(102, 9);
  TransportConfig cb;
  cb.cc.algo = CcAlgo::kBbr;
  cb.cc.rng_seed = 7;
  cb.initial_max_data = 64ull << 20;
  cb.recv_window_bytes = 32ull << 20;
  TransportConfig cc = cb;
  cc.cc.algo = CcAlgo::kCubic;
  Transport tb(link, 1, 101, cb), rb(link, 101, 1, cb);
  Transport tc(link, 2, 102, cc), rc(link, 102, 2, cc);
  tb.OpenStream();
  tc.OpenStream();
  static const std::vector<u8> chunk(1u << 20, 0x42);
  u64 wb = 0, wc = 0;
  std::vector<u8> pkt;
  for (Time now = 0; now <= 12'000'000; now += 10) {
    if (wc - tc.StreamSentBytes(0) < chunk.size()) { tc.Write(0, Bytes(chunk)); wc += chunk.size(); }
    if (now >= 2'000'000 && wb - tb.StreamSentBytes(0) < chunk.size()) {
      tb.Write(0, Bytes(chunk));
      wb += chunk.size();
    }
    tb.Tick(now); rb.Tick(now); tc.Tick(now); rc.Tick(now);
    while (link.Poll(1, &pkt, now)) tb.OnPacket(Bytes(pkt), now);
    while (link.Poll(101, &pkt, now)) rb.OnPacket(Bytes(pkt), now);
    while (link.Poll(2, &pkt, now)) tc.OnPacket(Bytes(pkt), now);
    while (link.Poll(102, &pkt, now)) rc.OnPacket(Bytes(pkt), now);
    for (Bytes v = rb.Peek(0); !v.empty(); v = rb.Peek(0)) rb.Consume(0, v.size());
    for (Bytes v = rc.Peek(0); !v.empty(); v = rc.Peek(0)) rc.Consume(0, v.size());
  }
  const double mb = double(rb.DeliveredBytes()) / 1048576.0;
  std::printf("    BBR flow delivered %.2f MB, rx_range_full %llu\n", mb,
              (unsigned long long)rb.stats().rx_range_full);
  // 10 s 里 BBR 流哪怕只拿到 1/4 带宽也有 ~15 MB；卡死时停在 6.38 MB
  CHECK(mb > 12.0);
}
