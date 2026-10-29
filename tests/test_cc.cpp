// CUBIC / HyStart++ / pacing 的纯数学用例。
//
// 这些用例不跑仿真器，直接把"公式应该等于什么"写成断言。
// 好处：曲线算错时立刻定位到 cc.cpp，而不是在千万步的仿真里猜。
#include <cmath>

#include "test_framework.h"
#include "xport/cc.h"

using namespace xport;

namespace {

constexpr u64 kMss = 1368;  // CcConfig 默认 MSS

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// 模拟"在途窗口领先 ACK 200 个包"的连接：每个 ACK 覆盖 2 个包。
// 这样一轮里能积累约 100 个 RTT 采样，与真实大窗口链路的形态一致；
// 否则每轮只有 1 个样本，HyStart 的最小样本数条件永远不满足。
struct Feeder {
  CubicCc cc;
  Time now = 0;
  u64 frontier = 0;

  void Round(double rtt_us, int acks = 150) {
    for (int i = 0; i < acks; ++i) {
      frontier += 2;
      cc.OnPacketSent(frontier + 200);  // 发送端持续发包，窗口前沿始终领先
      cc.OnAck(kMss, frontier, now, u64(rtt_us), u64(rtt_us));
      now += 1000;
    }
  }
};

}  // namespace

TEST(CubicStartsInSlowStart) {
  CubicCc cc;
  CHECK_EQ(cc.cwnd(), u64(10 * kMss));
  CHECK(cc.phase() == CcPhase::kSlowStart);
  CHECK_EQ(cc.CongestionEvents(), u64(0));
}

TEST(CubicSlowStartGrowsByAckedBytes) {
  CubicCc cc;
  cc.OnAck(100 * kMss, 1, 0, 40'000, 40'000);
  CHECK_EQ(cc.cwnd(), u64(110 * kMss));
}

TEST(CubicCongestionMultipliesByBeta) {
  CubicCc cc;
  cc.OnAck(490 * kMss, 1, 0, 40'000, 40'000);  // 窗口推到 500 段
  CHECK_EQ(cc.cwnd(), u64(500 * kMss));

  const u64 before = cc.cwnd();
  cc.OnCongestion(1'000'000);

  // RFC 9438 §4.6：cwnd = cwnd * beta，beta = 0.7
  CHECK(Near(double(cc.cwnd()), double(before) * 0.7, 2.0));
  CHECK(cc.phase() == CcPhase::kCongAvoid);
  CHECK_EQ(cc.CongestionEvents(), u64(1));
  CHECK_EQ(cc.ssthresh(), cc.cwnd());       // 降窗后 ssthresh 必须同步
  CHECK_EQ(cc.WMaxSegments(), 500.0);       // 首次事件：W_max 就是降窗前的窗口
}

TEST(CubicKIsTimeToRegrowToWMax) {
  CcConfig cfg;
  CubicCc cc(cfg);
  cc.OnAck(490 * kMss, 1, 0, 40'000, 40'000);
  cc.OnCongestion(0);

  // K = cbrt((W_max - cwnd) / C)，窗口与 K 都以"段/秒"计
  const double expect = std::cbrt((500.0 - 350.0) / cfg.cubic_c);
  CHECK(Near(cc.KSeconds(), expect, 1e-9));
  CHECK(Near(cc.KSeconds(), 7.2112, 1e-3));

  // 按定义，t = K 时三次曲线正好回到 W_max
  const double at_k = cc.CubicCurveBytes(cc.EpochStart() + Time(cc.KSeconds() * 1e6));
  CHECK(Near(at_k, 500.0 * double(kMss), 2.0));
}

TEST(CubicCurveIsConcaveThenConvex) {
  CubicCc cc;
  cc.OnAck(490 * kMss, 1, 0, 40'000, 40'000);
  cc.OnCongestion(0);
  const Time epoch = cc.EpochStart();
  const double k = cc.KSeconds();

  const auto at = [&](double t) { return cc.CubicCurveBytes(epoch + Time(t * 1e6)); };

  double prev = at(0);
  for (double t = 0.25; t <= k * 2.0; t += 0.25) {
    const double cur = at(t);
    CHECK(cur >= prev - 1e-6);
    prev = cur;
  }

  // 拐点在 t = K：之前增速下降（凹），之后增速上升（凸）
  const double d1 = at(k * 0.5) - at(k * 0.5 - 0.5);
  const double d2 = at(k * 0.5 + 0.5) - at(k * 0.5);
  const double d3 = at(k * 1.5) - at(k * 1.5 - 0.5);
  const double d4 = at(k * 1.5 + 0.5) - at(k * 1.5);
  CHECK(d2 < d1);
  CHECK(d4 > d3);
  CHECK(at(k * 2.0) > at(k) * 1.1);
}

TEST(CubicTargetNeverShrinksInCongAvoid) {
  CubicCc cc;
  cc.OnAck(490 * kMss, 1, 0, 40'000, 40'000);
  cc.OnCongestion(0);
  const u64 cwnd = cc.cwnd();
  // 刚降窗时 t=0，三次项为负；目标窗口不得低于当前窗口，否则会出现"增长为负"
  CHECK(cc.TargetBytes(0, 40'000) >= double(cwnd));
}

TEST(CubicFastConvergenceReducesWMax) {
  CubicCc cc;
  cc.OnAck(490 * kMss, 1, 0, 40'000, 40'000);
  cc.OnCongestion(0);
  CHECK_EQ(cc.WMaxSegments(), 500.0);

  // 当前窗口低于历史 W_max 时再丢一次：说明 500 段并非真实容量，
  // W_max 压到"新旧窗口中点" = cwnd * (1+beta)/2（RFC 9438 §4.6）
  const double w_now = double(cc.cwnd()) / double(kMss);
  cc.OnCongestion(2'000'000);
  CHECK(Near(cc.WMaxSegments(), w_now * 0.85, 1e-6));  // (1+beta)/2 = 0.85
}

TEST(CubicRecoveryPeriodSuppressesDuplicateCongestion) {
  CubicCc cc;
  cc.OnAck(490 * kMss, 1, 0, 40'000, 40'000);
  cc.OnCongestion(1'000'000);

  CHECK(cc.InRecovery(999'999));   // 降窗前发出的包：再丢也不算新事件
  CHECK(cc.InRecovery(1'000'000));
  CHECK(!cc.InRecovery(1'000'001));  // 降窗后发出的包才算
}

TEST(HystartExitsSlowStartOnRttInflation) {
  Feeder f;
  f.Round(40'000);  // 建立 delay_min = 40ms
  f.Round(40'000);
  CHECK(f.cc.phase() == CcPhase::kSlowStart);

  // 队列堆积：RTT 涨到 75ms，超过 40 + clamp(40/8, 4, 16) = 45ms
  f.Round(75'000);
  CHECK(f.cc.phase() == CcPhase::kConsSlowStart);

  // 保守慢启动只涨 1/4；RTT 仍高就转拥塞避免
  f.Round(75'000);
  CHECK(f.cc.phase() == CcPhase::kCongAvoid);
  CHECK_EQ(f.cc.CongestionEvents(), u64(0));  // 全程零丢包，纯靠延迟判定退出
}

TEST(HystartKeepsSlowStartWhenRttStable) {
  Feeder f;
  for (int i = 0; i < 6; ++i) f.Round(40'000);
  CHECK(f.cc.phase() == CcPhase::kSlowStart);
  CHECK(Near(f.cc.DelayMinUs(), 40'000.0, 1e-6));
}

TEST(PacingRateFollowsCwndOverSrtt) {
  CubicCc cc;
  cc.OnAck(490 * kMss, 1, 0, 40'000, 40'000);
  // 慢启动给 2 倍余量，否则会把自己限速
  CHECK_EQ(cc.PacingRateBps(40'000), u64(double(500 * kMss) / 0.04 * 2.0));

  cc.OnCongestion(0);
  CHECK_EQ(cc.PacingRateBps(40'000), u64(double(cc.cwnd()) / 0.04 * 1.25));
}

TEST(RenoModeHalvesAndGrowsLinearly) {
  CcConfig cfg;
  cfg.algo = CcAlgo::kReno;
  CubicCc cc(cfg);

  cc.OnAck(490 * kMss, 1, 0, 40'000, 40'000);
  CHECK_EQ(cc.cwnd(), u64(500 * kMss));

  cc.OnCongestion(0);
  CHECK_EQ(cc.cwnd(), u64(250 * kMss));  // AIMD：减半

  // 拥塞避免：每个 ACK 增加 MSS^2/cwnd（即每 RTT 一个 MSS）
  const u64 before = cc.cwnd();
  cc.OnAck(kMss, 2, 100'000, 40'000, 40'000);
  CHECK_EQ(cc.cwnd(), before + kMss * kMss / before);
}
