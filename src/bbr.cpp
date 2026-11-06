#include "xport/bbr.h"

#include <algorithm>

namespace xport {
namespace {

// ProbeBW 的 8 相增益。第 0 相探测（1.25），第 1 相排队（0.75），其余 6 相巡航。
constexpr double kCycleGain[BbrCc::kCycleLen] = {1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

}  // namespace

const char* BbrStateName(BbrState s) {
  switch (s) {
    case BbrState::kStartup:
      return "startup";
    case BbrState::kDrain:
      return "drain";
    case BbrState::kProbeBw:
      return "probe-bw";
    case BbrState::kProbeRtt:
      return "probe-rtt";
  }
  return "?";
}

// ---- WindowedMax（Linux lib/minmax.c 的直接移植）----

void WindowedMax::SubwinUpdate(u64 win, const Sample& val) {
  const u64 dt = val.t - s_[0].t;
  if (dt > win) {
    // 最好的样本过期了：整体左移，新样本补到末尾。左移一次后若仍过期就再移一次
    s_[0] = s_[1];
    s_[1] = s_[2];
    s_[2] = val;
    if (val.t - s_[0].t > win) {
      s_[0] = s_[1];
      s_[1] = s_[2];
      s_[2] = val;
    }
  } else if (s_[1].t == s_[0].t && dt > win / 4) {
    // 窗口已过 1/4 而次好样本还和最好的是同一个：用新样本占住次好位，
    // 否则最好的一过期，窗口里就没有任何候选
    s_[2] = s_[1] = val;
  } else if (s_[2].t == s_[1].t && dt > win / 2) {
    s_[2] = val;
  }
}

u64 WindowedMax::Update(u64 win, u64 t, u64 v) {
  const Sample val{t, v};
  if (v >= s_[0].v || t - s_[2].t > win) {
    Reset(t, v);
    return s_[0].v;
  }
  if (v >= s_[1].v) {
    s_[2] = s_[1] = val;
  } else if (v >= s_[2].v) {
    s_[2] = val;
  }
  SubwinUpdate(win, val);
  return s_[0].v;
}

// ---- BbrCc ----

BbrCc::BbrCc(const CcConfig& cfg) : cfg_(cfg) {
  cwnd_ = double(cfg_.initial_cwnd);
  // 还没有 RTT 时按 1 ms 估（Linux bbr_init_pacing_rate_from_rtt 的默认值）
  pacing_rate_ = kHighGain * cwnd_ / 1e-3 * kPacingMargin;
  if (cfg_.rng_seed != 0) rng_ = cfg_.rng_seed;
  EnterStartup();
}

void BbrCc::EnterStartup() {
  state_ = BbrState::kStartup;
  UpdateGains();
}

void BbrCc::EnterProbeBw(Time now) {
  state_ = BbrState::kProbeBw;
  // 随机起始相位，但不从第 1 相（0.75）开始：刚排完队又立刻降速，白白损失一段吞吐。
  // 取值落在 {2..7, 0}，与 Linux 的 CYCLE_LEN - 1 - rand(7) 再 advance 一次等价。
  cycle_index_ = kCycleLen - 1 - int(NextRand() % u32(kCycleLen - 1));
  AdvanceCyclePhase(now);
}

u32 BbrCc::NextRand() {
  // xorshift32：只用来选相位，够用且逐位可复现
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return rng_;
}

double BbrCc::Bdp(double gain) const {
  const u64 bw = btlbw_.Get();
  if (!has_min_rtt_ || bw == 0) return double(cfg_.initial_cwnd);
  return gain * double(bw) * double(min_rtt_us_) / 1e6;
}

u64 BbrCc::BdpBytes() const { return u64(Bdp(1.0)); }

void BbrCc::SaveCwnd() {
  // 已在恢复期或 ProbeRTT 时，当前窗口是被压低过的，不能拿它覆盖"压低前"的值
  if (!in_recovery_ && state_ != BbrState::kProbeRtt) {
    prior_cwnd_ = cwnd_;
  } else {
    prior_cwnd_ = std::max(prior_cwnd_, cwnd_);
  }
}

void BbrCc::OnCongestion(Time now) {
  ++events_;
  SaveCwnd();
  has_congestion_ = true;
  recovery_start_ = now;
  enter_recovery_pending_ = true;
}

void BbrCc::OnPersistentCongestion(Time now) {
  SaveCwnd();
  has_congestion_ = true;
  recovery_start_ = now;
  in_recovery_ = true;
  enter_recovery_pending_ = false;
  packet_conservation_ = false;
  cwnd_ = double(cfg_.min_cwnd);
  // 路径整体不通过一段时间，旧的"管道已满"判定不再可信：重新开始累计（Linux 进 Loss 时同样清零）
  full_bw_ = 0;
  full_bw_count_ = 0;
}

void BbrCc::OnAck(const BbrAck& a) {
  UpdateBtlBw(a);
  CheckCyclePhase(a);
  CheckFullPipe(a);
  CheckDrain(a);
  UpdateMinRttAndProbeRtt(a);
  UpdateGains();

  if (!has_seen_rtt_ && has_min_rtt_) {
    // 首个 RTT 样本到了：用它重算一次 pacing。否则会一直沿用 1 ms 的猜测值，
    // 长 RTT 路径上那是几十倍的高估，Startup 第一轮就把队列冲爆
    has_seen_rtt_ = true;
    pacing_rate_ = kHighGain * cwnd_ / (double(min_rtt_us_) / 1e6) * kPacingMargin;
  }
  SetPacingRate();
  SetCwnd(a);
}

void BbrCc::UpdateBtlBw(const BbrAck& a) {
  round_start_ = false;
  if (a.acked == 0) return;

  // 轮次以投递量计：本次定界包发出时的 delivered 越过了上一轮的终点，说明上一轮
  // 发出的数据都已被确认。用投递量而不是包号，是因为重传和 ACK 包也占包号
  if (a.rs.prior_delivered >= next_round_delivered_) {
    next_round_delivered_ = a.delivered;
    ++round_;
    round_start_ = true;
    packet_conservation_ = false;  // 包守恒只维持一轮
  }

  if (!a.rs.valid) return;
  const u64 bw = a.rs.RateBps();
  // app-limited 样本只能往上推：发送端自己没数据时测到的低速率不代表网络变慢
  if (!a.rs.is_app_limited || bw >= btlbw_.Get()) {
    btlbw_.Update(kBwWindowRounds, round_, bw);
  }
}

void BbrCc::CheckCyclePhase(const BbrAck& a) {
  if (state_ != BbrState::kProbeBw) return;
  const bool full_length = a.now - cycle_stamp_ > min_rtt_us_;
  bool next = false;
  if (pacing_gain_ == 1.0) {
    next = full_length;
  } else if (pacing_gain_ > 1.0) {
    // 探测相：至少一个 RTprop，并且要么在途已经涨到 1.25 BDP，要么出现了丢包
    // （涨不上去还丢包，说明这就是上限，没必要再坚持）
    next = full_length && (a.lost > 0 || double(a.prior_inflight) >= Inflight(pacing_gain_));
  } else {
    // 排队相：探测造成的队列排空了就提前结束，不必等满一个 RTprop
    next = full_length || double(a.inflight) <= Inflight(1.0);
  }
  if (next) AdvanceCyclePhase(a.now);
}

void BbrCc::AdvanceCyclePhase(Time now) {
  cycle_index_ = (cycle_index_ + 1) % kCycleLen;
  cycle_stamp_ = now;
}

void BbrCc::CheckFullPipe(const BbrAck& a) {
  if (filled_pipe_ || !round_start_ || a.rs.is_app_limited) return;
  const u64 bw = btlbw_.Get();
  if (double(bw) >= double(full_bw_) * kFullBwThresh) {
    full_bw_ = bw;
    full_bw_count_ = 0;
    return;
  }
  // 连续 3 轮涨幅不到 25%：Startup 每轮本该翻倍，涨不动说明已经到瓶颈
  if (++full_bw_count_ >= kFullBwRounds) filled_pipe_ = true;
}

void BbrCc::CheckDrain(const BbrAck& a) {
  if (state_ == BbrState::kStartup && filled_pipe_) state_ = BbrState::kDrain;
  if (state_ == BbrState::kDrain && double(a.inflight) <= Inflight(1.0)) EnterProbeBw(a.now);
}

void BbrCc::UpdateMinRttAndProbeRtt(const BbrAck& a) {
  const bool expired = has_min_rtt_ && a.now > min_rtt_stamp_ + kMinRttWindowUs;
  if (a.rtt_us > 0 && (!has_min_rtt_ || a.rtt_us < min_rtt_us_ || expired)) {
    min_rtt_us_ = a.rtt_us;
    min_rtt_stamp_ = a.now;
    has_min_rtt_ = true;
  }

  if (expired && state_ != BbrState::kProbeRtt) {
    SaveCwnd();
    state_ = BbrState::kProbeRtt;
    probe_rtt_done_armed_ = false;
    ++probe_rtt_count_;
  }
  if (state_ != BbrState::kProbeRtt) return;

  // 等在途真正降到 4 包之后才开始计时：计时的目的是"在空队列下停留 200 ms"，
  // 在途还很高时开始计时，最后测到的仍然是排队后的 RTT
  if (!probe_rtt_done_armed_ && a.inflight <= MinPipeCwnd()) {
    probe_rtt_done_armed_ = true;
    probe_rtt_done_stamp_ = a.now + kProbeRttDurationUs;
    probe_rtt_round_done_ = false;
    next_round_delivered_ = a.delivered;
  } else if (probe_rtt_done_armed_) {
    if (round_start_) probe_rtt_round_done_ = true;
    if (probe_rtt_round_done_ && a.now > probe_rtt_done_stamp_) {
      min_rtt_stamp_ = a.now;
      RestoreCwnd();
      if (filled_pipe_) {
        EnterProbeBw(a.now);
      } else {
        EnterStartup();
      }
    }
  }
}

void BbrCc::UpdateGains() {
  switch (state_) {
    case BbrState::kStartup:
      pacing_gain_ = kHighGain;
      cwnd_gain_ = kHighGain;
      break;
    case BbrState::kDrain:
      pacing_gain_ = kDrainGain;
      cwnd_gain_ = kHighGain;  // 只降速不收窗：窗口一收，Drain 期间就测不到带宽
      break;
    case BbrState::kProbeBw:
      pacing_gain_ = kCycleGain[cycle_index_];
      cwnd_gain_ = kCwndGain;
      break;
    case BbrState::kProbeRtt:
      pacing_gain_ = 1.0;
      cwnd_gain_ = 1.0;
      break;
  }
}

void BbrCc::SetPacingRate() {
  const double rate = pacing_gain_ * double(btlbw_.Get()) * kPacingMargin;
  // 管道没满之前只升不降：Startup 早期样本少、波动大，跟着降会把自己限死
  if (filled_pipe_ || rate > pacing_rate_) pacing_rate_ = rate;
}

void BbrCc::SetCwnd(const BbrAck& a) {
  if (a.acked == 0) return;
  const double mss = double(cfg_.mss);
  double cwnd = cwnd_;

  // ---- 恢复期（Linux bbr_set_cwnd_to_recover_or_restore）----
  // 本实现没有 TCP 那样的 CA 状态机，用 RFC 9002 的恢复期代替：从降窗时刻起，
  // 直到确认了一个降窗之后才发出的包，都算在恢复期内。
  if (a.lost > 0) cwnd = std::max(cwnd - double(a.lost), mss);
  if (enter_recovery_pending_) {
    enter_recovery_pending_ = false;
    in_recovery_ = true;
    // 包守恒：本轮只按"确认多少就发多少"放行，等新的一轮再按模型增长。
    // 新的一轮从现在起算，否则包守恒可能只维持了几个 ACK
    packet_conservation_ = true;
    next_round_delivered_ = a.delivered;
    cwnd = double(a.inflight + a.acked);
  } else if (in_recovery_ && a.has_newest_acked && a.newest_acked_sent_t > recovery_start_) {
    in_recovery_ = false;
    packet_conservation_ = false;
    cwnd = std::max(cwnd, prior_cwnd_);
  }

  if (packet_conservation_) {
    cwnd = std::max(cwnd, double(a.inflight + a.acked));
  } else {
    const double target = Inflight(cwnd_gain_);
    if (filled_pipe_) {
      cwnd = std::min(cwnd + double(a.acked), target);
    } else if (cwnd < target || a.delivered < cfg_.initial_cwnd) {
      // Startup 只增不减，即便目标暂时低于当前窗口：早期样本还不可信
      cwnd += double(a.acked);
    }
    cwnd = std::max(cwnd, double(MinPipeCwnd()));
  }

  cwnd = std::min(cwnd, double(cfg_.max_cwnd));
  if (state_ == BbrState::kProbeRtt) cwnd = std::min(cwnd, double(MinPipeCwnd()));
  cwnd_ = cwnd;
}

}  // namespace xport
