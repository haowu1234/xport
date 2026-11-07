// BBR v2 / v3（bbr3.h）的用例。
//
// 和 test_bbr.cpp 一样分两层：模型层直接喂 BbrAck / OnPacketLost 断言状态转移，
// 仿真层用 SimLink 检查 v2/v3 相对 v1 宣称的改进——浅缓冲丢包率、inflight_hi、
// 5 s 一次的 ProbeRTT、与 CUBIC 共存。
#include <cstdio>
#include <functional>
#include <vector>

#include "test_framework.h"
#include "xport/bbr3.h"
#include "xport/sim.h"
#include "xport/transport.h"

using namespace xport;

namespace {

constexpr u64 kMss = 1368;

// 理想化驱动：RTT 恒定，每轮 acks 个 ACK、每个确认 kMss。与 test_bbr.cpp 的 ModelFeeder 同构。
struct Feeder3 {
  Bbr3Cc bbr;
  Time now = 0;
  u64 delivered = 0;
  u64 inflight = 0;
  u64 prev_round_delivered = 0;
  Time rtt_us = 10'000;
  // 每个 ACK 之后回调：有的子状态（如 DOWN）不到一轮就结束，按轮采样会漏掉
  std::function<void()> on_ack;

  explicit Feeder3(const Bbr3Params& p) : bbr(CcConfig{}, p) {}

  void Round(u64 sample_bps, int acks, u64 inflight_after, u64 rs_lost = 0,
             bool cwnd_limited = false) {
    const Time step = rtt_us / Time(acks);
    for (int i = 0; i < acks; ++i) {
      now += step;
      BbrAck a;
      a.now = now;
      a.acked = kMss;
      a.prior_inflight = inflight;
      delivered += kMss;
      inflight = inflight_after;
      a.inflight = inflight;
      a.delivered = delivered;
      a.rtt_us = rtt_us;
      a.has_newest_acked = true;
      a.newest_acked_sent_t = now - rtt_us;
      a.cwnd_limited = cwnd_limited;
      a.rs.valid = true;
      a.rs.delivered = u64(double(sample_bps) * double(rtt_us) / 1e6);
      a.rs.interval_us = rtt_us;
      a.rs.prior_delivered = prev_round_delivered;
      a.rs.tx_in_flight = inflight_after;
      a.rs.lost = rs_lost;
      bbr.OnAck(a);
      if (on_ack) on_ack();
    }
    prev_round_delivered = delivered;
  }

  // 带宽每轮翻倍直到平台，走完 Startup + Drain，落进 ProbeBW
  void ToProbeBw(u64 bw) {
    u64 b = bw / 32;
    for (int r = 0; r < 5; ++r, b *= 2) Round(b, 20, 30 * kMss);
    for (int r = 0; r < 5; ++r) Round(bw, 20, 300 * kMss);
    for (int r = 0; r < 5 && !IsProbeBw(); ++r) Round(bw, 20, 10 * kMss);
  }
  bool IsProbeBw() const {
    const Bbr3State s = bbr.state();
    return s == Bbr3State::kProbeBwDown || s == Bbr3State::kProbeBwCruise ||
           s == Bbr3State::kProbeBwRefill || s == Bbr3State::kProbeBwUp;
  }
};

bool Near(double a, double b, double tol) { return a >= b - tol && a <= b + tol; }

}  // namespace

// ---- 参数预设 ----

TEST(Bbr3PresetsMatchSpecAndLinux) {
  const Bbr3Params v3 = Bbr3Params::V3();
  CHECK_EQ(v3.version, 3);
  CHECK(Near(v3.startup_pacing_gain, 2.77, 1e-9));  // draft-06 §5.6.1
  CHECK(Near(v3.startup_cwnd_gain, 2.0, 1e-9));
  CHECK(Near(v3.drain_pacing_gain, 0.5, 1e-9));
  CHECK(Near(v3.down_pacing_gain, 0.9, 1e-9));
  CHECK(Near(v3.up_cwnd_gain, 2.25, 1e-9));
  CHECK_EQ(v3.full_loss_cnt, 6);

  const Bbr3Params v2 = Bbr3Params::V2();  // google/bbr v2alpha tcp_bbr2.c
  CHECK_EQ(v2.version, 2);
  CHECK(Near(v2.startup_pacing_gain, 2.885, 1e-9));
  CHECK(Near(v2.down_pacing_gain, 0.75, 1e-9));
  CHECK_EQ(v2.full_loss_cnt, 8);
  CHECK(!v2.up_exit_on_plateau);
}

TEST(Bbr3StartsInStartupWithPresetGains) {
  Bbr3Cc b(CcConfig{}, Bbr3Params::V3());
  CHECK(b.state() == Bbr3State::kStartup);
  CHECK(Near(b.PacingGain(), 2.77, 1e-9));
  CHECK(Near(b.CwndGain(), 2.0, 1e-9));
  CHECK(b.InflightHi() == Bbr3Cc::kInf);
  CHECK(b.InflightLo() == Bbr3Cc::kInf);
  Bbr3Cc b2(CcConfig{}, Bbr3Params::V2());
  CHECK(Near(b2.PacingGain(), 2.885, 1e-9));
}

// ---- 模型层：状态机 ----

// 带宽平台 → Drain → 在途降到 BDP → ProbeBW_DOWN（v3 的 DOWN 增益是 0.9）
TEST(Bbr3StartupPlateauDrainsIntoProbeBwDown) {
  Feeder3 f(Bbr3Params::V3());
  const u64 bw = 10'000'000;
  u64 b = bw / 32;
  for (int r = 0; r < 5; ++r, b *= 2) {
    f.Round(b, 20, 30 * kMss);
    CHECK(f.bbr.state() == Bbr3State::kStartup);
  }
  for (int r = 0; r < 4; ++r) f.Round(bw, 20, 300 * kMss);
  CHECK(f.bbr.FullBwReached());
  CHECK(f.bbr.state() == Bbr3State::kDrain);
  CHECK(Near(f.bbr.PacingGain(), 0.5, 1e-9));
  CHECK(!f.bbr.StartupExitedOnLoss());
  f.Round(bw, 20, 10 * kMss);
  CHECK(f.bbr.state() == Bbr3State::kProbeBwDown ||
        f.bbr.state() == Bbr3State::kProbeBwCruise);
}

// v3 的 Drain 最多 3 轮：在途一直降不下来（ACK 聚合）也要进 ProbeBW，v2 不限
TEST(Bbr3DrainTimesOutAfterThreeRoundsV3Only) {
  const u64 bw = 10'000'000;
  for (int v : {2, 3}) {
    Feeder3 f(v == 3 ? Bbr3Params::V3() : Bbr3Params::V2());
    u64 b = bw / 32;
    for (int r = 0; r < 5; ++r, b *= 2) f.Round(b, 20, 30 * kMss);
    for (int r = 0; r < 4; ++r) f.Round(bw, 20, 300 * kMss);
    CHECK(f.bbr.state() == Bbr3State::kDrain);
    for (int r = 0; r < 5; ++r) f.Round(bw, 20, 300 * kMss);
    if (v == 3) {
      CHECK(f.IsProbeBw());
    } else {
      CHECK(f.bbr.state() == Bbr3State::kDrain);
    }
  }
}

// ProbeBW 子状态顺序：DOWN → CRUISE → (2~3 s 或 Reno 轮数) → REFILL → UP → DOWN
TEST(Bbr3ProbeBwCycleOrder) {
  Feeder3 f(Bbr3Params::V3());
  const u64 bw = 10'000'000;
  f.ToProbeBw(bw);
  CHECK(f.IsProbeBw());
  std::vector<Bbr3State> seen;
  f.on_ack = [&] {
    if (seen.empty() || seen.back() != f.bbr.state()) seen.push_back(f.bbr.state());
  };
  for (int r = 0; r < 600 && seen.size() < 8; ++r) f.Round(bw, 20, f.bbr.BdpBytes() / 2);
  f.on_ack = nullptr;
  bool saw_refill_up_down = false;
  for (std::size_t i = 0; i + 2 < seen.size(); ++i) {
    if (seen[i] == Bbr3State::kProbeBwRefill) {
      CHECK(seen[i + 1] == Bbr3State::kProbeBwUp);
      CHECK(seen[i + 2] == Bbr3State::kProbeBwDown);
      saw_refill_up_down = true;
    }
  }
  CHECK(saw_refill_up_down);
  CHECK(f.bbr.BwProbes() >= 1);
}

// 探测期丢包率 > 2%：inflight_hi 设到越过阈值的那个在途量，并立刻从 UP 退回 DOWN
TEST(Bbr3ProbeLossSetsInflightHi) {
  Feeder3 f(Bbr3Params::V3());
  const u64 bw = 10'000'000;
  f.ToProbeBw(bw);
  for (int r = 0; r < 600 && f.bbr.state() != Bbr3State::kProbeBwUp; ++r) {
    f.Round(bw, 20, f.bbr.BdpBytes() / 2);
  }
  CHECK(f.bbr.state() == Bbr3State::kProbeBwUp);
  CHECK(f.bbr.InflightHi() == Bbr3Cc::kInf);

  // 一个探测期发出的包被判丢：发出时在途 100 包，此后已丢 5 包（5% > 2%）
  RateSnapshot p;
  p.tx_in_flight = 100 * kMss;
  p.lost = 0;
  f.bbr.OnPacketLost(p, kMss, 5 * kMss, f.delivered, f.now);
  CHECK(f.bbr.InflightHi() != Bbr3Cc::kInf);
  // 阈值处的在途量：(0.02 × 99 − 4) / 0.98 < 0 → 取 inflight_prev = 99 包；Beta 下限不超过它
  CHECK(f.bbr.InflightHi() >= 70 * kMss);
  CHECK(f.bbr.InflightHi() <= 100 * kMss);
  CHECK(f.bbr.state() == Bbr3State::kProbeBwDown);
  CHECK_EQ(f.bbr.InflightTooHighEvents(), 1);
}

// 低于 2% 的丢包不碰 inflight_hi
TEST(Bbr3ProbeLossBelowThresholdIgnored) {
  Feeder3 f(Bbr3Params::V3());
  const u64 bw = 10'000'000;
  f.ToProbeBw(bw);
  for (int r = 0; r < 600 && f.bbr.state() != Bbr3State::kProbeBwUp; ++r) {
    f.Round(bw, 20, f.bbr.BdpBytes() / 2);
  }
  RateSnapshot p;
  p.tx_in_flight = 100 * kMss;
  f.bbr.OnPacketLost(p, kMss, 1 * kMss, f.delivered, f.now);  // 1%
  CHECK(f.bbr.InflightHi() == Bbr3Cc::kInf);
  CHECK(f.bbr.state() == Bbr3State::kProbeBwUp);
}

// 非探测阶段丢包：本轮结束时 bw_lo / inflight_lo 按 0.7 收缩，cwnd 被 inflight_lo 截住
TEST(Bbr3CruiseLossShrinksShortTermModel) {
  Feeder3 f(Bbr3Params::V3());
  const u64 bw = 10'000'000;
  f.ToProbeBw(bw);
  for (int r = 0; r < 50 && f.bbr.state() != Bbr3State::kProbeBwCruise; ++r) {
    f.Round(bw, 20, f.bbr.BdpBytes() / 2);
  }
  CHECK(f.bbr.state() == Bbr3State::kProbeBwCruise);
  const u64 cwnd_before = f.bbr.cwnd();
  // 先跑一轮半速：丢包轮结束时用的 bw_latest 是"丢包发生那一轮"的最大样本（draft §5.5.10.3），
  // 所以必须让那一轮本身就是半速，否则 bw_latest 还是满速、bw_lo 不会变
  f.Round(bw / 2, 20, f.bbr.BdpBytes() / 2);
  RateSnapshot p;
  p.tx_in_flight = 50 * kMss;
  f.bbr.OnPacketLost(p, kMss, kMss, f.delivered, f.now);
  // 丢包轮在下一轮的第一个 ACK 结束 → bw_lo = max(bw_latest = 0.5, 0.7 × max_bw) = 0.7 × max_bw
  f.Round(bw / 2, 20, f.bbr.BdpBytes() / 2);
  CHECK(f.bbr.BwLoBps() != Bbr3Cc::kInf);
  CHECK(Near(double(f.bbr.BwLoBps()), 0.7 * double(f.bbr.MaxBwBps()), 0.02 * double(bw)));
  CHECK(f.bbr.InflightLo() != Bbr3Cc::kInf);
  CHECK(f.bbr.cwnd() <= f.bbr.InflightLo());
  CHECK(f.bbr.cwnd() < cwnd_before);
}

// ProbeRTT：5 s 没刷新 RTT 就进入，cwnd 压到 0.5 BDP（不是 v1 的 4 包），200 ms 后退出
TEST(Bbr3ProbeRttEveryFiveSecondsAtHalfBdp) {
  Feeder3 f(Bbr3Params::V3());
  const u64 bw = 10'000'000;
  f.ToProbeBw(bw);
  f.rtt_us = 10'500;  // 不刷新 min_rtt
  const Time t0 = f.now;
  while (f.now < t0 + 6'000'000 && f.bbr.state() != Bbr3State::kProbeRtt) {
    f.Round(bw, 20, f.bbr.BdpBytes() / 2);
  }
  CHECK(f.bbr.state() == Bbr3State::kProbeRtt);
  CHECK(f.now - t0 < 5'600'000);
  CHECK(Near(f.bbr.CwndGain(), 0.5, 1e-9));
  f.Round(bw, 20, f.bbr.BdpBytes() / 4);
  const double half_bdp = 0.5 * double(f.bbr.BdpBytes());
  CHECK(Near(double(f.bbr.cwnd()), half_bdp, double(kMss) + 1));
  CHECK(f.bbr.cwnd() > 4 * kMss);
  const Time armed = f.now;
  while (f.now < armed + 260'000) f.Round(bw, 20, f.bbr.BdpBytes() / 4);
  CHECK(f.IsProbeBw());
  CHECK_EQ(f.bbr.ProbeRttCount(), 1);
}

// ---- 仿真层 ----

namespace {

struct Rig3 {
  SimLink link;
  Transport a;
  Transport b;
  Rig3(const LinkParams& p, CcAlgo algo) : link(p), a(link, 1, 2, Cfg(algo)), b(link, 2, 1, Cfg(algo)) {}
  static TransportConfig Cfg(CcAlgo algo) {
    TransportConfig c;
    c.cc.algo = algo;
    c.initial_max_data = 16ull << 20;
    return c;
  }
  bool Run(Time limit_us, const std::function<void(Time)>& on_tick = nullptr) {
    std::vector<u8> pkt;
    for (Time now = 0; now < limit_us; now += 10) {
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
  for (std::size_t i = 0; i < n; ++i) v[i] = u8(i * 89u + 5u);
  return v;
}

}  // namespace

// 浅缓冲（0.5 BDP）、无随机丢包：v1 不认丢包，稳态靠 1.25 探测反复撞缓冲；
// v3 有 inflight_hi 之后应当把重传率压到 v1 的一半以下，吞吐基本不掉
TEST(Bbr3SimShallowBufferLowerLossThanV1) {
  const auto run = [](CcAlgo algo, double* retx_pct, double* mbps) {
    LinkParams p;
    p.bandwidth_mbps = 50.0;
    p.delay_us = 20'000;
    p.queue_us = 20'000;
    Rig3 r(p, algo);
    const auto data = Payload(24u << 20);
    r.a.OpenStream();
    r.a.Write(0, Bytes(data));
    Time end = 0;
    CHECK(r.Run(60'000'000, [&](Time now) { end = now; }));
    *retx_pct = 100.0 * double(r.a.stats().retransmits) / double(r.a.stats().pkts_sent);
    *mbps = double(data.size()) * 8.0 / double(end);
  };
  double v1_retx, v1_mbps, v3_retx, v3_mbps;
  run(CcAlgo::kBbr, &v1_retx, &v1_mbps);
  run(CcAlgo::kBbr3, &v3_retx, &v3_mbps);
  std::printf("    shallow 0.5 BDP: v1 retx %.2f%% %.1f Mbps, v3 retx %.2f%% %.1f Mbps\n", v1_retx,
              v1_mbps, v3_retx, v3_mbps);
  CHECK(v3_retx < 0.5 * v1_retx);
  CHECK(v3_mbps > 0.85 * v1_mbps);
}

// 深缓冲下 inflight_hi 从无到有：Startup 因丢包退出或探测撞墙之后必须是有限值，
// 且不应小于 1 BDP（否则会把自己限死在链路容量以下）
TEST(Bbr3SimInflightHiLearnedAndAboveBdp) {
  LinkParams p;
  p.bandwidth_mbps = 20.0;
  p.delay_us = 20'000;
  p.queue_us = 30'000;
  Rig3 r(p, CcAlgo::kBbr3);
  const auto data = Payload(12u << 20);
  r.a.OpenStream();
  r.a.Write(0, Bytes(data));
  CHECK(r.Run(30'000'000));
  const Bbr3Cc& b = r.a.bbr3();
  std::printf("    inflight_hi %llu KB, BDP %llu KB, too-high %llu, probes %llu\n",
              (unsigned long long)(b.InflightHi() / 1024), (unsigned long long)(b.BdpBytes() / 1024),
              (unsigned long long)b.InflightTooHighEvents(), (unsigned long long)b.BwProbes());
  CHECK(b.InflightHi() != Bbr3Cc::kInf);
  CHECK(b.InflightHi() >= b.BdpBytes());
  // 12 MB / 20 Mbps ≈ 5 s，探测间隔 2~3 s：至少一次
  CHECK(b.BwProbes() >= 1);
}

// ProbeRTT 间隔 5 s（v1 是 10 s）：约 25 s 的传输应触发 4~6 次
TEST(Bbr3SimProbeRttEveryFiveSeconds) {
  LinkParams p;
  p.bandwidth_mbps = 10.0;
  p.delay_us = 10'000;
  p.queue_us = 100'000;
  Rig3 r(p, CcAlgo::kBbr3);
  const auto data = Payload(30u << 20);
  r.a.OpenStream();
  r.a.Write(0, Bytes(data));
  CHECK(r.Run(40'000'000));
  std::printf("    v3 ProbeRTT %llu times\n", (unsigned long long)r.a.bbr3().ProbeRttCount());
  CHECK(r.a.bbr3().ProbeRttCount() >= 4);
  CHECK(r.a.bbr3().ProbeRttCount() <= 6);
}

// 随机丢包 1%（低于 2% 阈值）：v2/v3 不应因此塌窗，吞吐仍远高于 CUBIC
TEST(Bbr3SimRandomLossBelowThresholdKeepsThroughput) {
  for (CcAlgo algo : {CcAlgo::kBbr2, CcAlgo::kBbr3}) {
    LinkParams p;
    p.bandwidth_mbps = 50.0;
    p.delay_us = 10'000;
    p.queue_us = 50'000;
    p.loss_pct = 1.0;
    p.seed = 2026;
    Rig3 r(p, algo);
    const auto data = Payload(8u << 20);
    r.a.OpenStream();
    r.a.Write(0, Bytes(data));
    Time end = 0;
    CHECK(r.Run(30'000'000, [&](Time now) { end = now; }));
    const double mbps = double(data.size()) * 8.0 / double(end);
    std::printf("    %s 1%% loss: %.1f Mbps\n", CcAlgoName(algo), mbps);
    CHECK(mbps > 35.0);
  }
}

// 两条 v3 共享瓶颈、错峰 2 s 启动：稳态份额接近（Jain ≥ 0.9）
TEST(Bbr3SimTwoFlowsConvergeToFairShare) {
  LinkParams p;
  p.bandwidth_mbps = 40.0;
  p.delay_us = 20'000;
  p.queue_us = 80'000;
  SimLink link(p);
  link.SetBottleneckGroup(101, 7);
  link.SetBottleneckGroup(102, 7);
  TransportConfig c;
  c.cc.algo = CcAlgo::kBbr3;
  c.initial_max_data = 32ull << 20;
  c.recv_window_bytes = 16ull << 20;
  TransportConfig c0 = c, c1 = c;
  c0.cc.rng_seed = 11;
  c1.cc.rng_seed = 29;
  Transport t0(link, 1, 101, c0), r0(link, 101, 1, c0);
  Transport t1(link, 2, 102, c1), r1(link, 102, 2, c1);
  t0.OpenStream();
  t1.OpenStream();
  static const std::vector<u8> chunk(1u << 20, 0x33);
  u64 w0 = 0, w1 = 0, d0 = 0, d1 = 0;
  std::vector<u8> pkt;
  for (Time now = 0; now < 30'000'000; now += 10) {
    if (w0 - t0.StreamSentBytes(0) < chunk.size()) { t0.Write(0, Bytes(chunk)); w0 += chunk.size(); }
    if (now >= 2'000'000 && w1 - t1.StreamSentBytes(0) < chunk.size()) {
      t1.Write(0, Bytes(chunk));
      w1 += chunk.size();
    }
    t0.Tick(now); r0.Tick(now); t1.Tick(now); r1.Tick(now);
    while (link.Poll(1, &pkt, now)) t0.OnPacket(Bytes(pkt), now);
    while (link.Poll(101, &pkt, now)) r0.OnPacket(Bytes(pkt), now);
    while (link.Poll(2, &pkt, now)) t1.OnPacket(Bytes(pkt), now);
    while (link.Poll(102, &pkt, now)) r1.OnPacket(Bytes(pkt), now);
    for (Bytes v = r0.Peek(0); !v.empty(); v = r0.Peek(0)) r0.Consume(0, v.size());
    for (Bytes v = r1.Peek(0); !v.empty(); v = r1.Peek(0)) r1.Consume(0, v.size());
    if (now == 10'000'000) { d0 = r0.DeliveredBytes(); d1 = r1.DeliveredBytes(); }
  }
  const double a = double(r0.DeliveredBytes() - d0), b = double(r1.DeliveredBytes() - d1);
  const double jain = (a + b) * (a + b) / (2.0 * (a * a + b * b));
  std::printf("    two v3 flows: %.1f / %.1f Mbps, Jain %.3f\n", a * 8 / 20e6, b * 8 / 20e6, jain);
  CHECK(jain >= 0.9);
  CHECK((a + b) * 8 / 20e6 > 0.85 * 40.0);
}

// 正确性兜底：v2/v3 下多流、5% 丢包仍逐字节交付
TEST(Bbr3SimTransfersExactlyUnderHeavyLoss) {
  for (CcAlgo algo : {CcAlgo::kBbr2, CcAlgo::kBbr3}) {
    LinkParams p;
    p.bandwidth_mbps = 100.0;
    p.delay_us = 2'000;
    p.loss_pct = 5.0;
    p.seed = 9;
    SimLink link(p);
    TransportConfig c;
    c.cc.algo = algo;
    Transport a(link, 1, 2, c);
    Transport b(link, 2, 1, c);
    const auto d0 = Payload(512 * 1024);
    auto d1 = Payload(300 * 1024);
    for (auto& x : d1) x ^= 0xa5;
    a.Write(a.OpenStream(), Bytes(d0));
    a.Write(a.OpenStream(), Bytes(d1));
    std::vector<u8> g0, g1, pkt;
    bool done = false;
    for (Time now = 0; now < 20'000'000 && !done; now += 50) {
      a.Tick(now);
      b.Tick(now);
      while (link.Poll(1, &pkt, now)) a.OnPacket(Bytes(pkt), now);
      while (link.Poll(2, &pkt, now)) b.OnPacket(Bytes(pkt), now);
      for (Bytes v = b.Peek(0); !v.empty(); v = b.Peek(0)) { g0.insert(g0.end(), v.begin(), v.end()); b.Consume(0, v.size()); }
      for (Bytes v = b.Peek(1); !v.empty(); v = b.Peek(1)) { g1.insert(g1.end(), v.begin(), v.end()); b.Consume(1, v.size()); }
      done = a.SendComplete() && g0.size() == d0.size() && g1.size() == d1.size();
    }
    CHECK(done);
    CHECK(g0 == d0);
    CHECK(g1 == d1);
  }
}
