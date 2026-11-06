#pragma once
// 拥塞控制：BBR v2 / v3（draft-ietf-ccwg-bbr-06，参数对照 Linux google/bbr 的 v2alpha 与 v3 分支）。
//
// 相对 v1（bbr.h）的核心变化：模型从"带宽 + 时延"两个量扩展成带上下界的区间——
//   上界（长期）  inflight_hi   探测时丢包率超过 2% 的在途量，ProbeBW 的硬天花板
//   下界（短期）  bw_lo / inflight_lo   非探测阶段遇到丢包后每轮乘 0.7 收缩，探测前清除
// v1 对丢包完全无感，浅缓冲下会持续 5%+ 的丢包、把 CUBIC 挤死；v2/v3 用这两个界把丢包率
// 控制在 LossThresh 附近，并在 CRUISE 阶段给别的流留 15% 余量（Headroom）。
//
// ProbeBW 从 v1 的 8 相增益循环换成 4 个子状态：
//   DOWN（降速排队）→ CRUISE（巡航，留余量）→ REFILL（按估计带宽填满管道 1 轮）→ UP（1.25 探测）
// 探测间隔 = min(2~3 s 随机, BDP 包数轮)——后者让 BBR 与 Reno/CUBIC 的增长周期同量级，
// 这是它和 loss-based 流共存的关键（draft §5.3.3.8）。
//
// v2 与 v3 共用这一个实现，差别只在 Bbr3Params：
//   参数                v2alpha        v3（draft-06）
//   Startup pacing     2.885          2.77
//   Startup cwnd       2.885          2.0
//   Drain pacing       1/2.885        0.5
//   DOWN pacing        0.75           0.90
//   UP cwnd_gain       2.0            2.25
//   Startup 丢包退出    8 次丢包事件    6 次
//   Drain 最长          不限           3 轮
//   UP 退出条件         在途达 1.25 BDP  带宽平台（3 轮涨幅 < 25%）
// 两版共同的：LossThresh 2%、Beta 0.7、Headroom 0.15、ProbeRTT 每 5 s 一次、cwnd 压到 0.5 BDP。
//
// 没做的：ECN（仿真链路不打标）、spurious loss undo（本传输层没有 undo 机制）、
// idle restart（实验都是持续发送）。
#include "xport/bbr.h"
#include "xport/cc.h"
#include "xport/rate_sample.h"
#include "xport/types.h"

namespace xport {

enum class Bbr3State : u8 {
  kStartup = 0,
  kDrain = 1,
  kProbeBwDown = 2,
  kProbeBwCruise = 3,
  kProbeBwRefill = 4,
  kProbeBwUp = 5,
  kProbeRtt = 6,
};

const char* Bbr3StateName(Bbr3State s);

struct Bbr3Params {
  int version = 3;
  double startup_pacing_gain = 2.77;
  double startup_cwnd_gain = 2.0;
  double drain_pacing_gain = 0.5;
  double down_pacing_gain = 0.9;
  double up_cwnd_gain = 2.25;
  u32 full_loss_cnt = 6;
  u32 drain_max_rounds = 3;    // 0 表示不限
  bool up_exit_on_plateau = true;

  static Bbr3Params V2();
  static Bbr3Params V3();
};

class Bbr3Cc {
 public:
  static constexpr double kLossThresh = 0.02;
  static constexpr double kBeta = 0.7;
  static constexpr double kHeadroom = 0.15;
  static constexpr double kPacingMargin = 0.99;
  static constexpr double kDefaultCwndGain = 2.0;
  static constexpr double kProbeRttCwndGain = 0.5;
  static constexpr double kUpPacingGain = 1.25;
  static constexpr double kFullBwThresh = 1.25;
  static constexpr u32 kFullBwCount = 3;
  static constexpr Time kProbeRttIntervalUs = 5'000'000;
  static constexpr Time kMinRttFilterUs = 10'000'000;
  static constexpr Time kProbeRttDurationUs = 200'000;
  static constexpr u64 kExtraAckedRounds = 10;
  static constexpr u64 kMaxRenoRounds = 63;
  // extra_acked 的上限：最多补偿 100 ms 的带宽（Linux bbr_extra_acked_max_us），
  // 防止一次异常的 ACK 聚合把窗口撑到离谱
  static constexpr Time kExtraAckedMaxUs = 100'000;
  static constexpr u64 kInf = ~u64(0);

  Bbr3Cc(const CcConfig& cfg, const Bbr3Params& p);

  void OnAck(const BbrAck& a);
  // 每判丢一个包调用一次（在 OnCongestion 之前）。c_lost 是包含本包在内的累计判丢字节，
  // c_delivered 是当前累计确认字节。探测期的丢包在这里逐包判断是否越过 2%。
  void OnPacketLost(const RateSnapshot& p, u64 bytes, u64 c_lost, u64 c_delivered, Time now);
  void OnCongestion(Time now);
  void OnPersistentCongestion(Time now);
  bool InRecovery(Time sent_time) const { return has_congestion_ && sent_time <= recovery_start_; }

  u64 cwnd() const { return u64(cwnd_); }
  u64 PacingRateBps() const { return u64(pacing_rate_); }
  u64 PacingBurstBytes() const { return u64(cfg_.pacing_burst_pkts * double(cfg_.mss)); }
  bool InProbeRtt() const { return state_ == Bbr3State::kProbeRtt; }

  // ---- 诊断接口 ----
  int version() const { return p_.version; }
  Bbr3State state() const { return state_; }
  u64 MaxBwBps() const { return bw_hi_[0] > bw_hi_[1] ? bw_hi_[0] : bw_hi_[1]; }
  u64 BwBps() const { return bw_; }
  u64 BwLoBps() const { return bw_lo_; }
  u64 InflightHi() const { return inflight_hi_; }
  u64 InflightLo() const { return inflight_lo_; }
  Time MinRttUs() const { return has_min_rtt_ ? min_rtt_us_ : 0; }
  u64 BdpBytes() const { return u64(BdpMultiple(1.0)); }
  u64 ExtraAcked() const { return extra_acked_; }
  double PacingGain() const { return pacing_gain_; }
  double CwndGain() const { return cwnd_gain_; }
  bool FullBwReached() const { return full_bw_reached_; }
  u64 RoundCount() const { return round_count_; }
  u64 CongestionEvents() const { return events_; }
  u64 ProbeRttCount() const { return probe_rtt_count_; }
  u64 InflightTooHighEvents() const { return too_high_events_; }
  u64 BwProbes() const { return bw_probes_; }
  bool StartupExitedOnLoss() const { return startup_loss_exit_; }
  u64 MinPipeCwnd() const { return 4 * cfg_.mss; }

 private:
  enum class AckPhase : u8 { kInit, kRefilling, kProbeStarting, kProbeFeedback, kProbeStopping };

  bool IsInProbeBw() const {
    return state_ == Bbr3State::kProbeBwDown || state_ == Bbr3State::kProbeBwCruise ||
           state_ == Bbr3State::kProbeBwRefill || state_ == Bbr3State::kProbeBwUp;
  }
  bool IsProbingBw() const {
    return state_ == Bbr3State::kStartup || state_ == Bbr3State::kProbeBwRefill ||
           state_ == Bbr3State::kProbeBwUp;
  }
  static bool IsInflightTooHigh(u64 lost, u64 tx_in_flight) {
    return tx_in_flight > 0 && double(lost) > double(tx_in_flight) * kLossThresh;
  }

  // ---- 模型 ----
  void UpdateRound(const BbrAck& a);
  void StartRound() { next_round_delivered_ = cur_delivered_; }
  void UpdateLatestDeliverySignals(const BbrAck& a, u64 bw);
  void UpdateCongestionSignals(const BbrAck& a, u64 bw);
  void UpdateMaxBw(const BbrAck& a, u64 bw);
  void AdvanceMaxBwFilter();
  void CheckStartupHighLoss(const BbrAck& a);
  void AdaptLowerBoundsFromCongestion();
  void AdvanceLatestDeliverySignals(const BbrAck& a, u64 bw);
  void ResetCongestionSignals();
  void ResetShortTermModel();
  void UpdateAckAggregation(const BbrAck& a);
  void UpdateMinRtt(const BbrAck& a);
  void BoundBwForModel();

  // ---- 状态机 ----
  void ResetFullBw();
  void CheckFullBwReached(const BbrAck& a, u64 bw);
  void EnterStartup();
  void EnterDrain();
  void CheckDrainDone(const BbrAck& a);
  void EnterProbeBw();
  void UpdateProbeBwCyclePhase(const BbrAck& a, u64 bw);
  bool AdaptLongTermModel(const BbrAck& a);
  bool HandleInflightTooHigh(bool app_limited, u64 tx_in_flight);
  void ProbeInflightHiUpward(const BbrAck& a);
  void RaiseInflightHiSlope();
  bool IsTimeToProbeBw();
  bool IsRenoCoexistenceProbeTime() const;
  bool IsTimeToCruise(const BbrAck& a) const;
  bool IsTimeToGoDown(const BbrAck& a, u64 bw);
  bool HasElapsedInPhase(Time interval) const { return now_ > cycle_stamp_ + interval; }
  void PickProbeWait();
  void StartProbeBwDown();
  void StartProbeBwCruise() { state_ = Bbr3State::kProbeBwCruise; }
  void StartProbeBwRefill();
  void StartProbeBwUp(u64 bw);
  void CheckProbeRtt(const BbrAck& a);
  void HandleProbeRtt(const BbrAck& a);
  void ExitProbeRtt();
  void UpdateGains();

  // ---- 控制量 ----
  double BdpMultiple(double gain) const;
  double QuantizationBudget(double cap) const;
  double Inflight(double gain) const { return QuantizationBudget(BdpMultiple(gain)); }
  double InflightWithHeadroom() const;
  double TargetInflight() const;
  double ProbeRttCwnd() const;
  void SetPacingRate();
  void SetCwnd(const BbrAck& a);
  void SaveCwnd();
  void RestoreCwnd() { cwnd_ = cwnd_ > prior_cwnd_ ? cwnd_ : prior_cwnd_; }
  u32 NextRand();

  CcConfig cfg_;
  Bbr3Params p_;
  Bbr3State state_ = Bbr3State::kStartup;
  double pacing_gain_ = 0;
  double cwnd_gain_ = 0;
  double cwnd_ = 0;
  double pacing_rate_ = 0;
  double prior_cwnd_ = 0;
  bool has_seen_rtt_ = false;

  // 当前 ACK 的上下文
  Time now_ = 0;
  u64 cur_delivered_ = 0;
  bool cwnd_limited_ = false;

  // 带宽：max_bw 是两个 ProbeBW 周期的最大值（draft MaxBwFilterLen = 2），
  // bw 是它再被 bw_lo 截断后的值，pacing 和 BDP 都用 bw
  u64 bw_hi_[2] = {0, 0};
  u64 cycle_count_ = 0;
  u64 bw_ = 0;
  u64 bw_lo_ = kInf;
  u64 inflight_lo_ = kInf;
  u64 inflight_hi_ = kInf;
  u64 bw_latest_ = 0;
  u64 inflight_latest_ = 0;

  // 以丢包为单位的轮次（与包计时的轮次分开：丢包轮从首个丢包开始算）
  u64 loss_round_delivered_ = 0;
  bool loss_round_start_ = false;
  bool is_loss_in_round_ = false;
  u32 loss_events_in_round_ = 0;

  // RTT
  bool has_min_rtt_ = false;
  Time min_rtt_us_ = 0;
  Time min_rtt_stamp_ = 0;
  bool has_probe_rtt_min_ = false;
  Time probe_rtt_min_delay_ = 0;
  Time probe_rtt_min_stamp_ = 0;
  bool probe_rtt_expired_ = false;
  Time probe_rtt_done_stamp_ = 0;
  bool probe_rtt_round_done_ = false;

  // 包计时的轮次
  u64 round_count_ = 0;
  u64 next_round_delivered_ = 0;
  bool round_start_ = false;
  u64 rounds_since_probe_up_ = 0;

  // Startup
  u64 full_bw_ = 0;
  u32 full_bw_count_ = 0;
  bool full_bw_now_ = false;
  bool full_bw_reached_ = false;
  u64 drain_start_round_ = 0;

  // ProbeBW
  Time cycle_stamp_ = 0;
  Time bw_probe_wait_ = 0;
  AckPhase ack_phase_ = AckPhase::kInit;
  bool is_bw_probe_sample_ = false;
  bool prev_probe_too_high_ = false;
  bool prev_probe_precautionary_ = false;
  u64 probe_up_acked_per_inc_ = kInf;
  u64 bw_probe_up_acked_ = 0;
  u32 bw_probe_up_rounds_ = 0;

  // ACK 聚合
  WindowedMax extra_acked_filter_;
  Time extra_acked_interval_start_ = 0;
  u64 extra_acked_delivered_ = 0;
  u64 extra_acked_ = 0;

  // 恢复期
  bool has_congestion_ = false;
  bool in_recovery_ = false;
  Time recovery_start_ = 0;

  // 计数器（诊断 / 实验报告用）
  u64 events_ = 0;
  u64 probe_rtt_count_ = 0;
  u64 too_high_events_ = 0;
  u64 bw_probes_ = 0;
  bool startup_loss_exit_ = false;

  u32 rng_ = 0x2545f491u;
};

}  // namespace xport
