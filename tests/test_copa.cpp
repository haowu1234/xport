// Copa 的纯数学用例。和 test_cc.cpp 同一条思路：不跑仿真器，直接对公式断言，
// 曲线算错时定位到 copa.cpp 而不是整条链路。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "test_framework.h"
#include "xport/copa.h"
#include "xport/sim.h"
#include "xport/transport.h"

using namespace xport;

namespace {

constexpr u64 kMss = 1368;

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

CopaAck Ack(Time now, Time rtt, u64 srtt, u64 bytes, bool limited = true) {
  CopaAck a;
  a.now = now;
  a.rtt_us = rtt;
  a.srtt_us = srtt;
  a.acked = bytes;
  a.cwnd_limited = limited;
  return a;
}

}  // namespace

TEST(CopaStartsInSlowStartAtInitialCwnd) {
  CopaCc cc;
  CHECK_EQ(cc.cwnd(), u64(10 * kMss));
  CHECK(cc.InSlowStart());
  CHECK(Near(cc.Delta(), 0.5, 1e-12));
  CHECK(Near(cc.Velocity(), 1.0, 1e-12));
  CHECK_EQ(cc.CongestionEvents(), u64(0));
}

TEST(CopaSlowStartDoublesOncePerRtt) {
  CopaCc cc;
  // 空队列：所有样本都是传播时延，排队时延为 0，目标速率无穷大，应一直翻倍。
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));        // 第一个样本，只建立估计
  CHECK_EQ(cc.cwnd(), u64(10 * kMss));
  cc.OnAck(Ack(10'000, 20'000, 20'000, kMss));   // 还没过一个 srtt
  CHECK_EQ(cc.cwnd(), u64(10 * kMss));
  cc.OnAck(Ack(20'000, 20'000, 20'000, kMss));   // 满一个 srtt，翻倍
  CHECK_EQ(cc.cwnd(), u64(20 * kMss));
  cc.OnAck(Ack(40'000, 20'000, 20'000, kMss));
  CHECK_EQ(cc.cwnd(), u64(40 * kMss));
  CHECK(cc.InSlowStart());
}

TEST(CopaSlowStartExitsWhenRateExceedsTarget) {
  CopaCc cc;
  // 先在空队列上翻倍几次，窗口到 40 段。
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  cc.OnAck(Ack(20'000, 20'000, 20'000, kMss));
  cc.OnAck(Ack(40'000, 20'000, 20'000, kMss));
  CHECK_EQ(cc.cwnd(), u64(40 * kMss));

  // 排队时延跳到 2 ms：RTT_standing = 22 ms，d_q = 2 ms。
  // 目标窗口 = (22/2) / δ = 22 段 < 40 段，当前速率已超过目标，慢启动必须结束。
  cc.OnAck(Ack(60'000, 22'000, 20'000, kMss));
  CHECK(!cc.InSlowStart());
  CHECK(cc.cwnd() < u64(40 * kMss));  // 退出的同时按超出量减了一截
}

TEST(CopaTargetCwndMatchesFormula) {
  CopaCc cc;
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  cc.OnAck(Ack(20'000, 20'000, 20'000, kMss));
  cc.OnAck(Ack(40'000, 20'000, 20'000, kMss));  // cwnd = 40 段，仍在慢启动

  // d_q = 4 ms，RTT_standing = 24 ms，δ = 0.5 → 目标 = (24/4)/0.5 = 12 段。
  cc.OnAck(Ack(60'000, 24'000, 20'000, kMss));
  CHECK(Near(cc.QueuingDelayUs(), 4000.0, 1e-6));
  CHECK(Near(cc.TargetCwndBytes(), 12.0 * double(kMss), 1.0));
}

TEST(CopaGrowsByVelocityOverDeltaCwndPerAck) {
  CopaCc cc;
  // 用一次大排队时延把慢启动打出去，窗口会被减到接近目标。
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  cc.OnAck(Ack(20'000, 20'000, 20'000, kMss));
  cc.OnAck(Ack(40'000, 20'000, 20'000, kMss));     // cwnd = 40 段
  cc.OnAck(Ack(60'000, 24'000, 20'000, kMss));    // 退出慢启动
  CHECK(!cc.InSlowStart());

  // 之后排队时延回到 0.5 ms：RTT_standing = 20.5 ms，目标 = (20.5/0.5)/0.5 = 82 段，
  // 远大于当前窗口，所以每个 ACK 都是增。增量为 v/(δ·cwnd_pkts) × acked。
  const u64 before = cc.cwnd();
  cc.OnAck(Ack(80'000, 20'500, 20'000, kMss));
  const double cwnd_pkts = double(before) / double(kMss);
  const double expect = double(before) + 1.0 / (0.5 * cwnd_pkts) * double(kMss);
  CHECK(Near(double(cc.cwnd()), expect, 1.0));
  CHECK(cc.cwnd() > before);
}

TEST(CopaStandingRttIgnoresSpike) {
  CopaCc cc;
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  cc.OnAck(Ack(5'000, 20'000, 20'000, kMss));
  // 一个尖刺样本落在 srtt/2 = 10 ms 的窗口内，但窗口里还有 20 ms 的样本，
  // 常驻 RTT 应取最小的那个，排队时延仍为 0。
  cc.OnAck(Ack(8'000, 60'000, 20'000, kMss));
  CHECK_EQ(cc.StandingRttUs(), Time(20'000));
  CHECK(Near(cc.QueuingDelayUs(), 0.0, 1e-6));
}

TEST(CopaStandingRttWindowExpires) {
  CopaCc cc;
  cc.OnAck(Ack(0, 20'000, 40'000, kMss));
  // srtt/2 = 20 ms。30 ms 后旧样本滑出窗口，只剩新的 25 ms 样本。
  cc.OnAck(Ack(30'000, 25'000, 40'000, kMss));
  CHECK_EQ(cc.StandingRttUs(), Time(25'000));
  CHECK_EQ(cc.MinRttUs(), Time(20'000));  // 10 s 窗口内最小值不变
  CHECK(Near(cc.QueuingDelayUs(), 5000.0, 1e-6));
}

TEST(CopaMinRttTracksRouteChange) {
  CopaCc cc;
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  // 路由切换：10 s 窗口滑过去之后，旧的 20 ms 不再计入，最小值变成 50 ms。
  cc.OnAck(Ack(10'000'001, 50'000, 20'000, kMss));
  CHECK_EQ(cc.MinRttUs(), Time(50'000));
}

TEST(CopaDoesNotGrowWhenAppLimited) {
  CopaCc cc;
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  cc.OnAck(Ack(20'000, 20'000, 20'000, kMss));
  const u64 before = cc.cwnd();
  // 队列为空本该翻倍，但窗口没被用满：不增。
  cc.OnAck(Ack(40'000, 20'000, 20'000, kMss, false));
  CHECK_EQ(cc.cwnd(), before);
  CHECK(cc.InSlowStart());
}

TEST(CopaStillShrinksWhenAppLimitedAboveTarget) {
  CopaCc cc;
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  cc.OnAck(Ack(20'000, 20'000, 20'000, kMss));
  cc.OnAck(Ack(40'000, 20'000, 20'000, kMss));  // cwnd = 40 段
  const u64 before = cc.cwnd();
  // 窗口远超目标（目标 12 段），即使应用没把窗口用满也要减——
  // 减窗是在释放多占的队列，不受"是否验证过更大窗口"的约束。
  cc.OnAck(Ack(60'000, 24'000, 20'000, kMss, false));
  CHECK(cc.cwnd() < before);
}

TEST(CopaVelocityDoublesAfterThreeRounds) {
  CopaCc cc;
  // 制造一个远低于目标的稳定局面：先退出慢启动，再让排队时延变小，
  // 窗口会连续多个 RTT 朝同一方向增长。
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  cc.OnAck(Ack(20'000, 20'000, 20'000, kMss));
  cc.OnAck(Ack(40'000, 20'000, 20'000, kMss));      // cwnd = 40 段
  cc.OnAck(Ack(60'000, 24'000, 20'000, kMss));     // 退出慢启动
  CHECK(!cc.InSlowStart());

  // 每 20 ms（一个 srtt）喂一个低排队时延的 ACK，窗口单调上涨。
  // 方向判定从退出慢启动后的第一个 ACK 开始计轮，连续三轮同向后 v 翻倍。
  Time t = 60'000;
  for (int i = 0; i < 6; ++i) {
    t += 20'000;
    cc.OnAck(Ack(t, 20'200, 20'000, kMss));
  }
  CHECK(cc.Velocity() >= 2.0);
  CHECK_EQ(cc.Direction(), 1);
}

TEST(CopaVelocityResetsWhenDirectionReverses) {
  CopaCc cc;
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  cc.OnAck(Ack(20'000, 20'000, 20'000, kMss));
  cc.OnAck(Ack(40'000, 20'000, 20'000, kMss));
  cc.OnAck(Ack(60'000, 24'000, 20'000, kMss));  // 退出慢启动，方向转降

  Time t = 60'000;
  for (int i = 0; i < 6; ++i) {  // 先朝上积累，把 v 翻起来
    t += 20'000;
    cc.OnAck(Ack(t, 20'200, 20'000, kMss));
  }
  CHECK(cc.Velocity() >= 2.0);

  // 排队时延突然很大，目标窗口远小于当前窗口：方向反转，v 立即回到 1，
  // 而不是拿着为"向上"积累的大 v 往下砍。
  const double v_before = cc.Velocity();
  cc.OnAck(Ack(t + 15'000, 200'000, 20'000, kMss));
  CHECK(cc.Velocity() < v_before);
  CHECK(Near(cc.Velocity(), 1.0, 1e-9));
}

TEST(CopaLossDoesNotChangeCwnd) {
  CopaCc cc;
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  cc.OnAck(Ack(20'000, 20'000, 20'000, kMss));
  const u64 before = cc.cwnd();
  cc.OnCongestion(30'000);
  CHECK_EQ(cc.cwnd(), before);  // 默认模式：丢包不是拥塞信号
  CHECK_EQ(cc.CongestionEvents(), u64(1));
  CHECK(cc.InRecovery(30'000));
  CHECK(!cc.InRecovery(30'001));  // 降窗之后发出的包不算在恢复期内
}

TEST(CopaPersistentCongestionResetsToMinCwnd) {
  CopaCc cc;
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  cc.OnAck(Ack(20'000, 20'000, 20'000, kMss));
  cc.OnAck(Ack(40'000, 20'000, 20'000, kMss));
  CHECK(cc.cwnd() > u64(2 * kMss));

  cc.OnPersistentCongestion(50'000);
  CHECK_EQ(cc.cwnd(), u64(2 * kMss));
  CHECK(cc.InSlowStart());
  CHECK(Near(cc.Velocity(), 1.0, 1e-12));
}

TEST(CopaPacingRateIsTwiceCwndOverStandingRtt) {
  CopaCc cc;
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  // 2 × cwnd / RTT_standing。cwnd = 10 × 1368 字节，RTT = 0.02 s。
  const double expect = 2.0 * double(10 * kMss) / 0.02;
  CHECK(Near(double(cc.PacingRateBps(20'000)), expect, expect * 1e-6));
}

TEST(CopaCwndStaysWithinBounds) {
  CopaCc cc;
  // 持续空队列：慢启动每个 RTT 翻倍，不能越过 max_cwnd。
  cc.OnAck(Ack(0, 20'000, 20'000, kMss));
  for (int i = 1; i <= 40; ++i) cc.OnAck(Ack(Time(i) * 20'000, 20'000, 20'000, kMss));
  CHECK(cc.cwnd() <= (256ull << 20));
  CHECK(cc.cwnd() >= u64(2 * kMss));
}

// ---- 模式切换（§2.2）----

namespace {

// 先用空队列建立 min_rtt = 20 ms，然后每 1 ms 喂一个 RTT 恒为 rtt_us 的 ACK，持续 dur_us。
// srtt 固定 20 ms，所以 5 RTT = 100 ms、4 RTT = 80 ms。
void FeedConstant(CopaCc& cc, Time& t, Time rtt_us, Time dur_us) {
  for (Time end = t + dur_us; t < end; t += 1000) cc.OnAck(Ack(t, rtt_us, 20'000, kMss));
}

// 队列在 rtt_lo 与 rtt_hi 之间振荡（周期 period_us），模拟有抖动的 RTT。
void FeedOscillating(CopaCc& cc, Time& t, Time lo, Time hi, Time period_us, Time dur_us) {
  for (Time end = t + dur_us; t < end; t += 1000) {
    const Time phase = t % period_us;
    const Time rtt = phase < period_us / 2 ? hi : lo;
    cc.OnAck(Ack(t, rtt, 20'000, kMss));
  }
}

}  // namespace

TEST(CopaStaysDefaultWhileQueueEmptiesPeriodically) {
  CopaCc cc;
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  // 队列在 0 与 4 ms 之间振荡，周期 60 ms < 5 RTT：每 5 RTT 内都见过空队列
  FeedOscillating(cc, t, 20'000, 24'000, 60'000, 2'000'000);
  CHECK(!cc.InCompetitiveMode());
  CHECK(Near(cc.Delta(), 0.5, 1e-12));
}

TEST(CopaEntersCompetitiveModeWhenQueueNeverEmpties) {
  CopaCc cc;
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  // 有填满缓冲的流：RTT 在 30–34 ms 之间抖，从不回到 20 ms 附近。
  // d_q 最小值 10 ms，远大于 0.1 × (34 − 20) = 1.4 ms。5 RTT（100 ms）后旧的空队列样本滑出窗口。
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, 150'000);
  CHECK(cc.InCompetitiveMode());
  CHECK_EQ(cc.ModeSwitches(), u64(1));
}

TEST(CopaDetectorIgnoresEmptyQueueOlderThanFiveRtts) {
  CopaCc cc;
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  // 不到 5 RTT：窗口里还留着空队列样本，不切
  FeedConstant(cc, t, 32'000, 80'000);
  CHECK(!cc.InCompetitiveMode());
  // 超过 5 RTT：空队列样本滑出，切
  FeedConstant(cc, t, 32'000, 40'000);
  CHECK(cc.InCompetitiveMode());
}

CcConfig AimdCfg() {
  CcConfig cfg;
  cfg.copa_cubic_competitive = false;  // 论文 / genericCC 的 AIMD
  return cfg;
}

TEST(CopaCompetitiveDeltaIsAdditiveIncreaseOnInverse) {
  CopaCc cc(AimdCfg());
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, 150'000);
  CHECK(cc.InCompetitiveMode());
  const double inv0 = 1.0 / cc.Delta();
  // 再过 10 个 RTT：1/δ 每 RTT +1
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, 200'000);
  const double inv1 = 1.0 / cc.Delta();
  CHECK(Near(inv1 - inv0, 10.0, 1.0));
  CHECK(cc.Delta() < 0.5);
}

TEST(CopaCompetitiveLossHalvesInverseOncePerRtt) {
  CopaCc cc(AimdCfg());
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, 400'000);
  CHECK(cc.InCompetitiveMode());
  const double inv0 = 1.0 / cc.Delta();
  CHECK(inv0 > 8.0);

  cc.OnCongestion(t);
  CHECK(Near(1.0 / cc.Delta(), inv0 / 2.0, 1e-9));
  // 同一个 RTT 内的第二次丢包不再减
  cc.OnCongestion(t + 5'000);
  CHECK(Near(1.0 / cc.Delta(), inv0 / 2.0, 1e-9));
  // 下一个 RTT 的丢包再减一半
  cc.OnCongestion(t + 25'000);
  CHECK(Near(1.0 / cc.Delta(), inv0 / 4.0, 1e-9));
}

// CUBIC 曲线（默认）：丢包时 1/δ 乘 β = 0.7，每 RTT 至多一次
TEST(CopaCubicCompetitiveLossMultipliesByBeta) {
  CopaCc cc;
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, 400'000);
  CHECK(cc.InCompetitiveMode());
  const double inv0 = 1.0 / cc.Delta();
  CHECK(inv0 > 8.0);
  cc.OnCongestion(t);
  CHECK(Near(1.0 / cc.Delta(), inv0 * 0.7, 1e-9));
  cc.OnCongestion(t + 5'000);
  CHECK(Near(1.0 / cc.Delta(), inv0 * 0.7, 1e-9));
}

// 丢包后 1/δ 沿曲线回到丢包前的值：用时 K = ∛(W_max·(1−β)/C)，之后凸增长超过它
TEST(CopaCubicCompetitiveRegrowsToWmaxAtK) {
  CopaCc cc;
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, 2'000'000);
  CHECK(cc.InCompetitiveMode());
  const double wmax = 1.0 / cc.Delta();
  cc.OnCongestion(t);
  const double k_s = std::cbrt(wmax * 0.3 / 0.4);
  const Time k_us = Time(k_s * 1e6);
  // 走到 K 之前一点：曲线在 W_max 下方。每 RTT +1 的下限在这段时间里也会抬高 1/δ，
  // 所以只断言"不超过 W_max 太多"而不断言精确值
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, k_us + 2 * 1'000'000);
  CHECK(1.0 / cc.Delta() > wmax);  // K 之后 2 s：已越过 W_max
}

// 长时间处于竞争模式时，CUBIC 曲线的 1/δ 增长比每 RTT +1 快（凸段）
TEST(CopaCubicCompetitiveOutgrowsAimd) {
  CopaCc cubic;
  CopaCc aimd(AimdCfg());
  for (CopaCc* cc : {&cubic, &aimd}) {
    Time t = 0;
    FeedConstant(*cc, t, 20'000, 200'000);
    FeedOscillating(*cc, t, 30'000, 34'000, 40'000, 10'000'000);
    CHECK(cc->InCompetitiveMode());
  }
  // 10 s：AIMD 约 2 + 500 = 502；CUBIC 0.4·10³ = 400 叠加 +1 下限之后更大
  CHECK(1.0 / cubic.Delta() > 1.0 / aimd.Delta());
}

TEST(CopaCompetitiveDeltaNeverExceedsDefault) {
  CopaCc cc;
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, 150'000);
  CHECK(cc.InCompetitiveMode());
  for (int i = 0; i < 10; ++i) cc.OnCongestion(t + Time(i) * 30'000);
  CHECK(cc.Delta() <= 0.5 + 1e-12);
}

TEST(CopaReturnsToDefaultAndResetsDelta) {
  CopaCc cc;
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, 400'000);
  CHECK(cc.InCompetitiveMode());
  CHECK(cc.Delta() < 0.5);
  // 竞争流离开，队列排空
  FeedConstant(cc, t, 20'000, 20'000);
  CHECK(!cc.InCompetitiveMode());
  CHECK(Near(cc.Delta(), 0.5, 1e-12));
  CHECK_EQ(cc.ModeSwitches(), u64(2));
}

TEST(CopaDefaultModeLossKeepsDelta) {
  CopaCc cc;
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  CHECK(!cc.InCompetitiveMode());
  cc.OnCongestion(t);
  CHECK(Near(cc.Delta(), 0.5, 1e-12));
}

TEST(CopaModeSwitchDisabledStaysDefault) {
  CcConfig cfg;
  cfg.copa_mode_switch = false;
  CopaCc cc(cfg);
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, 1'000'000);
  CHECK(!cc.InCompetitiveMode());
  CHECK(Near(cc.Delta(), 0.5, 1e-12));
  CHECK_EQ(cc.ModeSwitches(), u64(0));
}

TEST(CopaPersistentCongestionLeavesCompetitiveMode) {
  CopaCc cc;
  Time t = 0;
  FeedConstant(cc, t, 20'000, 200'000);
  FeedOscillating(cc, t, 30'000, 34'000, 40'000, 400'000);
  CHECK(cc.InCompetitiveMode());
  cc.OnPersistentCongestion(t);
  CHECK(!cc.InCompetitiveMode());
  CHECK(Near(cc.Delta(), 0.5, 1e-12));
}

// ---- 仿真层：SimLink 端到端，检查论文宣称的性质 ----

namespace {

struct CopaRig {
  SimLink link;
  Transport a;
  Transport b;

  CopaRig(const LinkParams& p, CcAlgo algo) : link(p), a(link, 1, 2, Cfg(algo)), b(link, 2, 1, Cfg(algo)) {}

  static TransportConfig Cfg(CcAlgo algo) {
    TransportConfig c;
    c.cc.algo = algo;
    c.initial_max_data = 16ull << 20;  // 流控不能成为瓶颈
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

struct SimStats {
  double goodput_mbps = 0;
  double q_avg = 0;   // 稳态平均队列（包）
  std::size_t q_max = 0;
  bool ok = false;
};

SimStats RunBulk(const LinkParams& p, CcAlgo algo, std::size_t bytes, Time warmup_us) {
  CopaRig r(p, algo);
  const auto data = Payload(bytes);
  r.a.OpenStream();
  r.a.Write(0, Bytes(data));
  SimStats st;
  double q_sum = 0;
  u64 q_n = 0;
  Time end = 0;
  st.ok = r.Run(60'000'000, 10, [&](Time now) {
    end = now;
    if (now < warmup_us || now % 1000 != 0) return;
    const std::size_t q = r.link.queue_depth(2, now);
    q_sum += double(q);
    ++q_n;
    st.q_max = std::max(st.q_max, q);
  });
  st.goodput_mbps = double(bytes) * 8.0 / (double(end) / 1e6) / 1e6;
  st.q_avg = q_n ? q_sum / double(q_n) : 0;
  return st;
}

}  // namespace

// 论文 §3：单条 Copa 流在稳态下队列在 0 到 2.5/δ = 5 包之间振荡，均值约 1.25/δ = 2.5 包，
// 与缓冲大小无关。深缓冲（500 ms）上 CUBIC 会把队列填到数百包，Copa 应保持在个位数到十几包，
// 同时链路基本跑满。
TEST(CopaSimKeepsQueueShortOnDeepBuffer) {
  LinkParams p;
  p.bandwidth_mbps = 20.0;
  p.delay_us = 20'000;
  p.queue_us = 500'000;
  const SimStats copa = RunBulk(p, CcAlgo::kCopa, 16u << 20, 2'000'000);
  const SimStats cubic = RunBulk(p, CcAlgo::kCubic, 16u << 20, 2'000'000);
  std::printf("    20 Mbps / 40 ms / 500 ms buf: copa %.1f Mbps q_avg %.1f q_max %zu pkts; "
              "cubic %.1f Mbps q_avg %.1f q_max %zu\n",
              copa.goodput_mbps, copa.q_avg, copa.q_max, cubic.goodput_mbps, cubic.q_avg,
              cubic.q_max);
  CHECK(copa.ok);
  CHECK(cubic.ok);
  CHECK(copa.goodput_mbps > 16.0);       // 至少 80% 利用率（含启动阶段）
  CHECK(copa.q_avg < 10.0);              // 论文预测 2.5 包，留余量给 ACK 合并和 pacing 突发
  CHECK(copa.q_avg * 10.0 < cubic.q_avg);
}

// 默认模式下丢包不是拥塞信号：1% 随机丢包时 Copa 吞吐应远高于 CUBIC。
TEST(CopaSimToleratesRandomLossBetterThanCubic) {
  LinkParams p;
  p.bandwidth_mbps = 50.0;
  p.delay_us = 10'000;
  p.queue_us = 50'000;
  p.loss_pct = 1.0;
  p.seed = 2026;
  const SimStats copa = RunBulk(p, CcAlgo::kCopa, 8u << 20, 0);
  const SimStats cubic = RunBulk(p, CcAlgo::kCubic, 8u << 20, 0);
  std::printf("    1%% loss, 50 Mbps: cubic %.1f Mbps, copa %.1f Mbps\n", cubic.goodput_mbps,
              copa.goodput_mbps);
  CHECK(copa.ok);
  CHECK(copa.goodput_mbps > 1.5 * cubic.goodput_mbps);
}

// 重丢包下仍然逐字节完整交付（守 transport 的重传路径在 Copa 分支下没有被绕开）。
TEST(CopaSimTransfersExactlyUnderHeavyLoss) {
  LinkParams p;
  p.bandwidth_mbps = 20.0;
  p.delay_us = 10'000;
  p.queue_us = 50'000;
  p.loss_pct = 5.0;
  p.seed = 7;
  CopaRig r(p, CcAlgo::kCopa);
  const auto data = Payload(2u << 20);
  r.a.OpenStream();
  r.a.Write(0, Bytes(data));
  std::vector<u8> got;
  std::vector<u8> pkt;
  bool done = false;
  for (Time now = 0; now < 60'000'000 && !done; now += 10) {
    r.a.Tick(now);
    r.b.Tick(now);
    while (r.link.Poll(1, &pkt, now)) r.a.OnPacket(Bytes(pkt), now);
    while (r.link.Poll(2, &pkt, now)) r.b.OnPacket(Bytes(pkt), now);
    for (Bytes v = r.b.Peek(0); !v.empty(); v = r.b.Peek(0)) {
      got.insert(got.end(), v.begin(), v.end());
      r.b.Consume(0, v.size());
    }
    done = r.a.SendComplete() && got.size() == data.size();
  }
  CHECK(done);
  CHECK(got == data);
}

namespace {

struct TwoFlowResult {
  double a_mbps = 0, b_mbps = 0;
  u64 a_switches = 0;
  double a_competitive_frac = 0;  // 测量窗口内 flow a 处于竞争模式的时间占比
};

// 两条流共享瓶颈，从 10 s 起测到 30 s。flow a 先启动，b 晚 2 s。
TwoFlowResult RunTwo(CcAlgo a_algo, CcAlgo b_algo, bool mode_switch, double buf_bdp) {
  LinkParams p;
  p.bandwidth_mbps = 40.0;
  p.delay_us = 20'000;
  const double bdp_us = 2.0 * double(p.delay_us);
  p.queue_us = u32(buf_bdp * bdp_us);
  SimLink link(p);
  link.SetBottleneckGroup(101, 7);
  link.SetBottleneckGroup(102, 7);
  TransportConfig c;
  c.initial_max_data = 32ull << 20;
  c.recv_window_bytes = 16ull << 20;
  c.cc.copa_mode_switch = mode_switch;
  TransportConfig c0 = c, c1 = c;
  c0.cc.algo = a_algo;
  c1.cc.algo = b_algo;
  Transport t0(link, 1, 101, c0), r0(link, 101, 1, c0);
  Transport t1(link, 2, 102, c1), r1(link, 102, 2, c1);
  t0.OpenStream();
  t1.OpenStream();
  static const std::vector<u8> chunk(1u << 20, 0x5a);
  u64 w0 = 0, w1 = 0, d0 = 0, d1 = 0;
  u64 samples = 0, comp = 0;
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
    if (now >= 10'000'000 && now % 1000 == 0) {
      ++samples;
      comp += t0.copa().InCompetitiveMode() ? 1 : 0;
    }
  }
  TwoFlowResult r;
  r.a_mbps = double(r0.DeliveredBytes() - d0) * 8 / 20e6;
  r.b_mbps = double(r1.DeliveredBytes() - d1) * 8 / 20e6;
  r.a_switches = t0.copa().ModeSwitches();
  r.a_competitive_frac = samples ? double(comp) / double(samples) : 0;
  return r;
}

}  // namespace

// §2.2 的目的：与 CUBIC 共存时切到竞争模式，拿回有意义的份额。
// 不开模式切换时份额只有几个百分点（实验记录 2026-11-16）。
TEST(CopaSimCompetitiveModeHoldsShareAgainstCubic) {
  const TwoFlowResult off = RunTwo(CcAlgo::kCopa, CcAlgo::kCubic, false, 2.0);
  const TwoFlowResult on = RunTwo(CcAlgo::kCopa, CcAlgo::kCubic, true, 2.0);
  const double share_off = off.a_mbps / (off.a_mbps + off.b_mbps);
  const double share_on = on.a_mbps / (on.a_mbps + on.b_mbps);
  std::printf("    copa vs cubic, 2 BDP: default-only share %.3f; with mode switch share %.3f "
              "(%.1f / %.1f Mbps, %llu switches, competitive %.0f%% of time)\n",
              share_off, share_on, on.a_mbps, on.b_mbps, (unsigned long long)on.a_switches,
              100.0 * on.a_competitive_frac);
  CHECK(share_on > 3.0 * share_off);
  CHECK(share_on > 0.2);
  CHECK(on.a_competitive_frac > 0.5);  // 有 CUBIC 在时应当大部分时间处于竞争模式
}

// 只有 Copa 流时队列会周期性排空，判据应基本留在默认模式，公平性不变。
// 流进出时会短暂误切几个 RTT（论文 §5.1 自己也观察到），所以断言时间占比而不是某一瞬间的状态。
TEST(CopaSimTwoCopaFlowsStayFairWithModeSwitch) {
  const TwoFlowResult r = RunTwo(CcAlgo::kCopa, CcAlgo::kCopa, true, 2.0);
  const double jain = (r.a_mbps + r.b_mbps) * (r.a_mbps + r.b_mbps) /
                      (2.0 * (r.a_mbps * r.a_mbps + r.b_mbps * r.b_mbps));
  std::printf("    two copa flows: %.1f / %.1f Mbps, Jain %.3f, %llu switches, competitive %.0f%% of time\n",
              r.a_mbps, r.b_mbps, jain, (unsigned long long)r.a_switches,
              100.0 * r.a_competitive_frac);
  CHECK(jain >= 0.95);
  CHECK(r.a_competitive_frac < 0.2);
  CHECK(r.a_mbps + r.b_mbps > 0.9 * 40.0);
}
