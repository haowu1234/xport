#include "xport/bbr3.h"

#include <algorithm>

namespace xport {

const char* Bbr3StateName(Bbr3State s) {
  switch (s) {
    case Bbr3State::kStartup:
      return "startup";
    case Bbr3State::kDrain:
      return "drain";
    case Bbr3State::kProbeBwDown:
      return "bw-down";
    case Bbr3State::kProbeBwCruise:
      return "bw-cruise";
    case Bbr3State::kProbeBwRefill:
      return "bw-refill";
    case Bbr3State::kProbeBwUp:
      return "bw-up";
    case Bbr3State::kProbeRtt:
      return "probe-rtt";
  }
  return "?";
}

Bbr3Params Bbr3Params::V3() { return Bbr3Params{}; }

Bbr3Params Bbr3Params::V2() {
  // Linux google/bbr v2alpha 分支 tcp_bbr2.c 的默认值
  Bbr3Params p;
  p.version = 2;
  p.startup_pacing_gain = 2.885;
  p.startup_cwnd_gain = 2.885;
  p.drain_pacing_gain = 1.0 / 2.885;
  p.down_pacing_gain = 0.75;
  p.up_cwnd_gain = 2.0;
  p.full_loss_cnt = 8;
  p.drain_max_rounds = 0;
  p.up_exit_on_plateau = false;
  return p;
}

Bbr3Cc::Bbr3Cc(const CcConfig& cfg, const Bbr3Params& p) : cfg_(cfg), p_(p) {
  cwnd_ = double(cfg_.initial_cwnd);
  // 还没有 RTT 时按 1 ms 估（draft InitPacingRate）
  pacing_rate_ = p_.startup_pacing_gain * cwnd_ / 1e-3 * kPacingMargin;
  extra_acked_filter_.Reset(0, 0);
  if (cfg_.rng_seed != 0) rng_ = cfg_.rng_seed;
  ResetCongestionSignals();
  ResetShortTermModel();
  PickProbeWait();
  EnterStartup();
}

u32 Bbr3Cc::NextRand() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

// ---------------------------------------------------------------- 主流程

void Bbr3Cc::OnAck(const BbrAck& a) {
  now_ = a.now;
  cur_delivered_ = a.delivered;
  cwnd_limited_ = a.cwnd_limited;
  const u64 bw = a.rs.valid ? a.rs.RateBps() : 0;

  // UpdateModelAndState（draft §5.2.3，顺序照抄——好几处判据依赖前一步刚更新的量）
  UpdateRound(a);
  UpdateLatestDeliverySignals(a, bw);
  UpdateCongestionSignals(a, bw);
  UpdateAckAggregation(a);
  CheckFullBwReached(a, bw);
  if (state_ == Bbr3State::kStartup && full_bw_reached_) EnterDrain();
  CheckDrainDone(a);
  UpdateProbeBwCyclePhase(a, bw);
  UpdateMinRtt(a);
  CheckProbeRtt(a);
  AdvanceLatestDeliverySignals(a, bw);
  BoundBwForModel();
  UpdateGains();

  if (!has_seen_rtt_ && has_min_rtt_) {
    // 首个 RTT 样本到了：按它重算初始 pacing，原因同 v1（1 ms 猜测在长 RTT 下高估几十倍）
    has_seen_rtt_ = true;
    pacing_rate_ = p_.startup_pacing_gain * cwnd_ / (double(min_rtt_us_) / 1e6) * kPacingMargin;
  }
  SetPacingRate();
  SetCwnd(a);
}

void Bbr3Cc::UpdateRound(const BbrAck& a) {
  round_start_ = false;
  if (a.acked == 0) return;
  if (a.rs.prior_delivered >= next_round_delivered_) {
    StartRound();
    ++round_count_;
    ++rounds_since_probe_up_;
    round_start_ = true;
  }
}

// ---------------------------------------------------------------- 带宽与丢包信号

void Bbr3Cc::UpdateLatestDeliverySignals(const BbrAck& a, u64 bw) {
  loss_round_start_ = false;
  if (a.acked == 0) return;
  bw_latest_ = std::max(bw_latest_, bw);
  inflight_latest_ = std::max(inflight_latest_, a.rs.delivered);
  if (a.rs.prior_delivered >= loss_round_delivered_) {
    loss_round_delivered_ = cur_delivered_;
    loss_round_start_ = true;
  }
}

void Bbr3Cc::UpdateMaxBw(const BbrAck& a, u64 bw) {
  if (bw == 0) return;
  // app-limited 样本只能往上推（同 v1）
  if (bw >= MaxBwBps() || !a.rs.is_app_limited) {
    u64& slot = bw_hi_[cycle_count_ & 1];
    if (bw > slot) slot = bw;
  }
}

void Bbr3Cc::AdvanceMaxBwFilter() {
  // 两槽窗口：每个 ProbeBW 周期结束时滚动一次，max_bw 只记得"本周期 + 上周期"。
  // v1 用 10 轮时间窗；改成按探测周期计，带宽下降后两个周期（约 4~6 s）就能遗忘旧峰值
  ++cycle_count_;
  bw_hi_[cycle_count_ & 1] = 0;
}

void Bbr3Cc::UpdateCongestionSignals(const BbrAck& a, u64 bw) {
  UpdateMaxBw(a, bw);
  if (a.lost > 0 && loss_events_in_round_ < 0xffff) ++loss_events_in_round_;
  if (!loss_round_start_) return;  // 每个丢包轮只在结束时响应一次
  CheckStartupHighLoss(a);
  AdaptLowerBoundsFromCongestion();
  is_loss_in_round_ = false;
  loss_events_in_round_ = 0;
}

void Bbr3Cc::CheckStartupHighLoss(const BbrAck& a) {
  if (full_bw_reached_ || state_ != Bbr3State::kStartup) return;
  // 三个条件同时满足才退出（draft §5.3.1.3）：在恢复期内、本轮丢包事件够多、丢包率超阈值。
  // 只看丢包率会被单次突发丢包误触发；只看事件数会在低 BDP 下永远凑不够
  if (!in_recovery_ || loss_events_in_round_ < p_.full_loss_cnt) return;
  if (!IsInflightTooHigh(a.rs.lost, a.rs.tx_in_flight)) return;
  full_bw_reached_ = true;
  startup_loss_exit_ = true;
  inflight_hi_ = std::max<u64>(BdpBytes(), inflight_latest_);
  EnterDrain();
}

void Bbr3Cc::AdaptLowerBoundsFromCongestion() {
  // 探测阶段的丢包由 inflight_hi 处理（那是在"试探上限"，丢包是预期中的），
  // 不能再拿它压低短期模型，否则一次探测就把自己压回去
  if (IsProbingBw() || !is_loss_in_round_) return;
  if (bw_lo_ == kInf) bw_lo_ = MaxBwBps();
  if (inflight_lo_ == kInf) inflight_lo_ = u64(cwnd_);
  bw_lo_ = std::max<u64>(bw_latest_, u64(kBeta * double(bw_lo_)));
  inflight_lo_ = std::max<u64>(inflight_latest_, u64(kBeta * double(inflight_lo_)));
}

void Bbr3Cc::AdvanceLatestDeliverySignals(const BbrAck& a, u64 bw) {
  if (!loss_round_start_) return;
  bw_latest_ = bw;
  inflight_latest_ = a.rs.delivered;
}

void Bbr3Cc::ResetCongestionSignals() {
  is_loss_in_round_ = false;
  loss_events_in_round_ = 0;
  bw_latest_ = 0;
  inflight_latest_ = 0;
}

void Bbr3Cc::ResetShortTermModel() {
  bw_lo_ = kInf;
  inflight_lo_ = kInf;
}

void Bbr3Cc::BoundBwForModel() { bw_ = std::min(MaxBwBps(), bw_lo_); }

void Bbr3Cc::UpdateAckAggregation(const BbrAck& a) {
  if (a.acked == 0) return;
  const Time interval = now_ > extra_acked_interval_start_ ? now_ - extra_acked_interval_start_ : 0;
  u64 expected = u64(double(bw_) * double(interval) / 1e6);
  // ACK 速率低于估计带宽：说明没有聚合，区间从头开始
  if (extra_acked_delivered_ <= expected) {
    extra_acked_delivered_ = 0;
    extra_acked_interval_start_ = now_;
    expected = 0;
  }
  extra_acked_delivered_ += a.acked;
  u64 extra = extra_acked_delivered_ - expected;
  extra = std::min<u64>(extra, u64(cwnd_));
  const u64 cap = u64(double(bw_) * double(kExtraAckedMaxUs) / 1e6);
  if (bw_ > 0) extra = std::min(extra, cap);
  const u64 win = full_bw_reached_ ? kExtraAckedRounds : 1;
  extra_acked_ = extra_acked_filter_.Update(win, round_count_, extra);
}

void Bbr3Cc::UpdateMinRtt(const BbrAck& a) {
  // 两个最小值过滤器（draft §5.3.4.3）：probe_rtt_min 窗口 5 s，决定何时进 ProbeRTT；
  // min_rtt 窗口 10 s，是 BDP 用的值。拆开是为了让 ProbeRTT 更频繁、但 BDP 不随之抖动
  probe_rtt_expired_ = has_probe_rtt_min_ && now_ > probe_rtt_min_stamp_ + kProbeRttIntervalUs;
  if (a.rtt_us > 0 &&
      (!has_probe_rtt_min_ || a.rtt_us < probe_rtt_min_delay_ || probe_rtt_expired_)) {
    probe_rtt_min_delay_ = a.rtt_us;
    probe_rtt_min_stamp_ = now_;
    has_probe_rtt_min_ = true;
  }
  if (!has_probe_rtt_min_) return;
  const bool min_rtt_expired = has_min_rtt_ && now_ > min_rtt_stamp_ + kMinRttFilterUs;
  if (!has_min_rtt_ || probe_rtt_min_delay_ < min_rtt_us_ || min_rtt_expired) {
    min_rtt_us_ = probe_rtt_min_delay_;
    min_rtt_stamp_ = probe_rtt_min_stamp_;
    has_min_rtt_ = true;
  }
}

// ---------------------------------------------------------------- Startup / Drain

void Bbr3Cc::ResetFullBw() {
  full_bw_ = 0;
  full_bw_count_ = 0;
  full_bw_now_ = false;
}

void Bbr3Cc::CheckFullBwReached(const BbrAck& a, u64 bw) {
  if (full_bw_now_ || !round_start_ || a.rs.is_app_limited || !a.rs.valid) return;
  if (double(bw) >= double(full_bw_) * kFullBwThresh) {
    ResetFullBw();
    full_bw_ = bw;
    return;
  }
  ++full_bw_count_;
  full_bw_now_ = full_bw_count_ >= kFullBwCount;
  if (full_bw_now_) full_bw_reached_ = true;
}

void Bbr3Cc::EnterStartup() {
  state_ = Bbr3State::kStartup;
  UpdateGains();
}

void Bbr3Cc::EnterDrain() {
  state_ = Bbr3State::kDrain;
  drain_start_round_ = round_count_;
  UpdateGains();
}

void Bbr3Cc::CheckDrainDone(const BbrAck& a) {
  if (state_ != Bbr3State::kDrain) return;
  // v3 最多排 3 轮：ACK 聚合严重时在途可能永远降不到 1 BDP，v2 会卡在 Drain 里低速发送
  const bool timeout = p_.drain_max_rounds != 0 &&
                       round_count_ > drain_start_round_ + p_.drain_max_rounds;
  if (double(a.inflight) <= Inflight(1.0) || timeout) EnterProbeBw();
}

void Bbr3Cc::EnterProbeBw() {
  cwnd_gain_ = kDefaultCwndGain;
  StartProbeBwDown();
}

// ---------------------------------------------------------------- ProbeBW

void Bbr3Cc::PickProbeWait() {
  // 随机化探测时机：多条 BBR 流同步探测会一起把队列打满、一起降速，收敛变慢
  rounds_since_probe_up_ = NextRand() & 1;
  bw_probe_wait_ = 2'000'000 + Time(NextRand() % 1'000'001);
}

void Bbr3Cc::StartProbeBwDown() {
  ResetCongestionSignals();
  probe_up_acked_per_inc_ = kInf;
  PickProbeWait();
  cycle_stamp_ = now_;
  ack_phase_ = AckPhase::kProbeStopping;
  StartRound();
  state_ = Bbr3State::kProbeBwDown;
  UpdateGains();
}

void Bbr3Cc::StartProbeBwRefill() {
  ResetShortTermModel();
  bw_probe_up_rounds_ = 0;
  bw_probe_up_acked_ = 0;
  prev_probe_precautionary_ = false;
  ack_phase_ = AckPhase::kRefilling;
  StartRound();
  state_ = Bbr3State::kProbeBwRefill;
  ++bw_probes_;
  UpdateGains();
}

void Bbr3Cc::StartProbeBwUp(u64 bw) {
  ack_phase_ = AckPhase::kProbeStarting;
  StartRound();
  ResetFullBw();
  full_bw_ = bw;
  state_ = Bbr3State::kProbeBwUp;
  RaiseInflightHiSlope();
  UpdateGains();
}

void Bbr3Cc::UpdateProbeBwCyclePhase(const BbrAck& a, u64 bw) {
  if (!full_bw_reached_) return;
  if (AdaptLongTermModel(a)) return;
  if (!IsInProbeBw()) return;
  switch (state_) {
    case Bbr3State::kProbeBwDown:
      if (IsTimeToProbeBw()) return;
      if (IsTimeToCruise(a)) StartProbeBwCruise();
      break;
    case Bbr3State::kProbeBwCruise:
      IsTimeToProbeBw();
      break;
    case Bbr3State::kProbeBwRefill:
      // REFILL 满一轮后才开始 UP：此后的样本才反映"填满管道之后"的状态
      if (round_start_) {
        is_bw_probe_sample_ = true;
        StartProbeBwUp(bw);
      }
      break;
    case Bbr3State::kProbeBwUp:
      if (IsTimeToGoDown(a, bw)) {
        prev_probe_too_high_ = false;
        StartProbeBwDown();
      }
      break;
    default:
      break;
  }
}

bool Bbr3Cc::AdaptLongTermModel(const BbrAck& a) {
  if (ack_phase_ == AckPhase::kProbeStarting && round_start_) {
    ack_phase_ = AckPhase::kProbeFeedback;
  }
  if (ack_phase_ == AckPhase::kProbeStopping && round_start_) {
    // 探测期发出的包都已确认：本周期的带宽样本收齐，滚动 max_bw 窗口
    is_bw_probe_sample_ = false;
    ack_phase_ = AckPhase::kInit;
    if (IsInProbeBw() && !a.rs.is_app_limited) AdvanceMaxBwFilter();
    // 上次探测因"碰到 inflight_hi"而提前收手（预防性），且没真的丢包：立刻再探一次
    if (IsInProbeBw() && prev_probe_precautionary_ && !prev_probe_too_high_) {
      StartProbeBwRefill();
      return true;
    }
  }
  if (IsInflightTooHigh(a.rs.lost, a.rs.tx_in_flight)) {
    if (is_bw_probe_sample_) return HandleInflightTooHigh(a.rs.is_app_limited, a.rs.tx_in_flight);
    return false;
  }
  // 丢包率安全：上界可以往上抬
  if (inflight_hi_ == kInf) return false;
  if (a.rs.tx_in_flight > inflight_hi_) inflight_hi_ = a.rs.tx_in_flight;
  if (state_ == Bbr3State::kProbeBwUp) ProbeInflightHiUpward(a);
  return false;
}

bool Bbr3Cc::HandleInflightTooHigh(bool app_limited, u64 tx_in_flight) {
  prev_probe_too_high_ = true;
  is_bw_probe_sample_ = false;  // 每次探测只响应一次
  ++too_high_events_;
  // Beta 下限：不比 CUBIC 的 0.7 倍降窗更激进
  if (!app_limited) {
    inflight_hi_ = std::max<u64>(tx_in_flight, u64(TargetInflight() * kBeta));
  }
  if (state_ == Bbr3State::kProbeBwUp) {
    StartProbeBwDown();
    return true;
  }
  return false;
}

void Bbr3Cc::RaiseInflightHiSlope() {
  // UP 期间 inflight_hi 的增长速度每轮翻倍：第 n 轮每确认 cwnd/2^n 字节就加 1 MSS
  const u64 growth = u64(1) << bw_probe_up_rounds_;
  bw_probe_up_rounds_ = std::min<u32>(bw_probe_up_rounds_ + 1, 30);
  probe_up_acked_per_inc_ = std::max<u64>(u64(cwnd_) / growth, cfg_.mss);
}

void Bbr3Cc::ProbeInflightHiUpward(const BbrAck& a) {
  // 只有真被 inflight_hi 卡住时才抬它：没用满的上界抬了也测不出东西
  if (!cwnd_limited_ || cwnd_ < double(inflight_hi_)) return;
  bw_probe_up_acked_ += a.acked;
  if (bw_probe_up_acked_ >= probe_up_acked_per_inc_) {
    const u64 delta = bw_probe_up_acked_ / probe_up_acked_per_inc_;
    bw_probe_up_acked_ -= delta * probe_up_acked_per_inc_;
    inflight_hi_ += delta * cfg_.mss;
  }
  if (round_start_) RaiseInflightHiSlope();
}

double Bbr3Cc::TargetInflight() const { return std::min(BdpMultiple(1.0), cwnd_); }

bool Bbr3Cc::IsRenoCoexistenceProbeTime() const {
  // 探测间隔不超过"Reno 把窗口从 0.7 BDP 涨回 BDP 所需的轮数"量级（约 BDP 包数），
  // 否则与 Reno/CUBIC 共享瓶颈时，BBR 探测太稀、份额被对方慢慢吃掉
  const u64 reno_rounds = u64(TargetInflight() / double(cfg_.mss));
  return rounds_since_probe_up_ >= std::min<u64>(reno_rounds, kMaxRenoRounds);
}

bool Bbr3Cc::IsTimeToProbeBw() {
  if (HasElapsedInPhase(bw_probe_wait_) || IsRenoCoexistenceProbeTime()) {
    StartProbeBwRefill();
    return true;
  }
  return false;
}

bool Bbr3Cc::IsTimeToCruise(const BbrAck& a) const {
  if (double(a.inflight) > InflightWithHeadroom()) return false;
  // 和 max_bw 下的 BDP 比（不是被 bw_lo 截断后的 bw），与 draft 一致
  const double bdp_max =
      has_min_rtt_ ? double(MaxBwBps()) * double(min_rtt_us_) / 1e6 : double(cfg_.initial_cwnd);
  return double(a.inflight) <= QuantizationBudget(bdp_max);
}

bool Bbr3Cc::IsTimeToGoDown(const BbrAck& a, u64 bw) {
  // 上次探测碰到过上限：这次一到上限就收手（预防性），不再重复撞墙
  if (prev_probe_too_high_ && double(a.inflight) >= double(inflight_hi_)) {
    prev_probe_precautionary_ = true;
    return true;
  }
  if (p_.up_exit_on_plateau) {
    // v3：带宽连续 3 轮没涨 25% 就收手。被 inflight_hi 卡住时重置平台计数，
    // 因为那时带宽不涨是窗口不够，不是管道满了
    if (cwnd_limited_ && cwnd_ >= double(inflight_hi_)) {
      ResetFullBw();
      full_bw_ = bw;
    } else if (full_bw_now_) {
      return true;
    }
    return false;
  }
  // v2alpha：至少一个 min_rtt，且在途涨到 1.25 BDP（说明 1.25 倍速探测已经在排队）
  return HasElapsedInPhase(min_rtt_us_) &&
         double(a.prior_inflight) >= Inflight(kUpPacingGain);
}

// ---------------------------------------------------------------- 丢包

void Bbr3Cc::OnPacketLost(const RateSnapshot& p, u64 bytes, u64 c_lost, u64 c_delivered,
                          Time now) {
  now_ = now;
  cur_delivered_ = c_delivered;
  if (!is_loss_in_round_) loss_round_delivered_ = c_delivered;
  is_loss_in_round_ = true;
  if (!is_bw_probe_sample_) return;  // 只有探测期发出的包才用来修正上界

  // 逐包判断（draft §5.5.10.2）：RFC 9002 的判丢会延迟到 1.125 RTT 之后，
  // 等到 ACK 侧的样本算出 2% 时，tx_in_flight 已经是后来发的、更小的在途量
  const u64 tx = p.tx_in_flight;
  const u64 lost = u32(u32(c_lost) - u32(p.lost));  // 与 Sent 里的 32 位存储一致，按模减
  if (!IsInflightTooHigh(lost, tx)) return;
  // 在这个包的哪个前缀处丢包率恰好越过 2%：上界设在那里，而不是整包之后
  const double inflight_prev = double(tx > bytes ? tx - bytes : 0);
  const double lost_prev = double(lost > bytes ? lost - bytes : 0);
  const double num = kLossThresh * inflight_prev - lost_prev;
  const double at_loss = num > 0 ? inflight_prev + num / (1.0 - kLossThresh) : inflight_prev;
  HandleInflightTooHigh(p.is_app_limited, u64(at_loss));
}

void Bbr3Cc::OnCongestion(Time now) {
  ++events_;
  if (!in_recovery_) SaveCwnd();
  has_congestion_ = true;
  in_recovery_ = true;
  recovery_start_ = now;
}

void Bbr3Cc::OnPersistentCongestion(Time now) {
  SaveCwnd();
  has_congestion_ = true;
  in_recovery_ = true;
  recovery_start_ = now;
  // 等价于 TCP 的 RTO（draft OnEnterRTO 把 cwnd 压到 inflight + 1；这里按 RFC 9002 压到最小窗口）
  cwnd_ = double(cfg_.min_cwnd);
}

void Bbr3Cc::SaveCwnd() {
  if (!in_recovery_ && state_ != Bbr3State::kProbeRtt) {
    prior_cwnd_ = cwnd_;
  } else {
    prior_cwnd_ = std::max(prior_cwnd_, cwnd_);
  }
}

// ---------------------------------------------------------------- ProbeRTT

void Bbr3Cc::CheckProbeRtt(const BbrAck& a) {
  if (state_ != Bbr3State::kProbeRtt && probe_rtt_expired_) {
    SaveCwnd();
    state_ = Bbr3State::kProbeRtt;
    probe_rtt_done_stamp_ = 0;
    ack_phase_ = AckPhase::kProbeStopping;
    StartRound();
    ++probe_rtt_count_;
    UpdateGains();
  }
  if (state_ == Bbr3State::kProbeRtt) HandleProbeRtt(a);
}

void Bbr3Cc::HandleProbeRtt(const BbrAck& a) {
  // 低速样本的屏蔽由 transport 负责（InProbeRtt 时把采样器标成 app-limited）
  if (probe_rtt_done_stamp_ == 0 && double(a.inflight) <= ProbeRttCwnd()) {
    probe_rtt_done_stamp_ = now_ + kProbeRttDurationUs;
    probe_rtt_round_done_ = false;
    StartRound();
  } else if (probe_rtt_done_stamp_ != 0) {
    if (round_start_) probe_rtt_round_done_ = true;
    if (probe_rtt_round_done_ && now_ > probe_rtt_done_stamp_) {
      probe_rtt_min_stamp_ = now_;
      RestoreCwnd();
      ExitProbeRtt();
    }
  }
}

void Bbr3Cc::ExitProbeRtt() {
  ResetShortTermModel();
  if (full_bw_reached_) {
    // 在途已经低于 BDP，可以直接巡航；但探测时钟要从头算（DOWN 里 PickProbeWait）
    StartProbeBwDown();
    StartProbeBwCruise();
    UpdateGains();
  } else {
    EnterStartup();
  }
}

// ---------------------------------------------------------------- 控制量

void Bbr3Cc::UpdateGains() {
  switch (state_) {
    case Bbr3State::kStartup:
      pacing_gain_ = p_.startup_pacing_gain;
      cwnd_gain_ = p_.startup_cwnd_gain;
      break;
    case Bbr3State::kDrain:
      pacing_gain_ = p_.drain_pacing_gain;
      cwnd_gain_ = p_.startup_cwnd_gain;
      break;
    case Bbr3State::kProbeBwDown:
      pacing_gain_ = p_.down_pacing_gain;
      cwnd_gain_ = kDefaultCwndGain;
      break;
    case Bbr3State::kProbeBwCruise:
    case Bbr3State::kProbeBwRefill:
      pacing_gain_ = 1.0;
      cwnd_gain_ = kDefaultCwndGain;
      break;
    case Bbr3State::kProbeBwUp:
      pacing_gain_ = kUpPacingGain;
      cwnd_gain_ = p_.up_cwnd_gain;
      break;
    case Bbr3State::kProbeRtt:
      pacing_gain_ = 1.0;
      cwnd_gain_ = kProbeRttCwndGain;
      break;
  }
}

double Bbr3Cc::BdpMultiple(double gain) const {
  if (!has_min_rtt_ || bw_ == 0) return double(cfg_.initial_cwnd);
  return gain * double(bw_) * double(min_rtt_us_) / 1e6;
}

double Bbr3Cc::QuantizationBudget(double cap) const {
  // QUIC 的 offload budget 就是一个发送量子（draft §5.5.8.2）：这里是 pacing 突发
  cap = std::max(cap, double(PacingBurstBytes()));
  cap = std::max(cap, double(MinPipeCwnd()));
  if (state_ == Bbr3State::kProbeBwUp) cap += 2.0 * double(cfg_.mss);
  return cap;
}

double Bbr3Cc::InflightWithHeadroom() const {
  if (inflight_hi_ == kInf) return 1e300;
  const double hi = double(inflight_hi_);
  const double headroom = std::max(double(cfg_.mss), kHeadroom * hi);
  return std::max(hi - headroom, double(MinPipeCwnd()));
}

double Bbr3Cc::ProbeRttCwnd() const {
  return std::max(BdpMultiple(kProbeRttCwndGain), double(MinPipeCwnd()));
}

void Bbr3Cc::SetPacingRate() {
  const double rate = pacing_gain_ * double(bw_) * kPacingMargin;
  if (full_bw_reached_ || rate > pacing_rate_) pacing_rate_ = rate;
}

void Bbr3Cc::SetCwnd(const BbrAck& a) {
  // 退出恢复期：确认了降窗之后才发出的包
  if (in_recovery_ && a.has_newest_acked && a.newest_acked_sent_t > recovery_start_) {
    in_recovery_ = false;
    RestoreCwnd();
  }

  const double max_inflight = QuantizationBudget(BdpMultiple(cwnd_gain_) + double(extra_acked_));
  double cwnd = cwnd_;
  if (full_bw_reached_) {
    cwnd = std::min(cwnd + double(a.acked), max_inflight);
  } else if (cwnd < max_inflight || a.delivered < cfg_.initial_cwnd) {
    cwnd += double(a.acked);
  }
  cwnd = std::max(cwnd, double(MinPipeCwnd()));
  if (state_ == Bbr3State::kProbeRtt) cwnd = std::min(cwnd, ProbeRttCwnd());

  // BoundCwndForModel：探测相受长期上界约束，巡航相再留 15% 余量，短期下界始终生效
  double cap = 1e300;
  if (IsInProbeBw() && state_ != Bbr3State::kProbeBwCruise) {
    if (inflight_hi_ != kInf) cap = double(inflight_hi_);
  } else if (state_ == Bbr3State::kProbeRtt || state_ == Bbr3State::kProbeBwCruise) {
    cap = InflightWithHeadroom();
  }
  if (inflight_lo_ != kInf) cap = std::min(cap, double(inflight_lo_));
  cap = std::max(cap, double(MinPipeCwnd()));
  cwnd = std::min(cwnd, cap);
  cwnd_ = std::min(cwnd, double(cfg_.max_cwnd));
}

}  // namespace xport
