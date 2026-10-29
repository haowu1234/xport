#include "xport/cc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xport {

const char* CcPhaseName(CcPhase p) {
  switch (p) {
    case CcPhase::kSlowStart:
      return "slow-start";
    case CcPhase::kConsSlowStart:
      return "hystart-css";
    case CcPhase::kCongAvoid:
      return "cong-avoid";
  }
  return "?";
}

const char* CcAlgoName(CcAlgo a) { return a == CcAlgo::kCubic ? "cubic" : "reno"; }

CubicCc::CubicCc(const CcConfig& cfg) : cfg_(cfg) {
  cwnd_ = double(cfg_.initial_cwnd);
  ssthresh_ = double(cfg_.max_cwnd);
  w_max_seg_ = CwndSeg();
}

u64 CubicCc::PacingRateBps(u64 srtt_us) const {
  const double srtt_s = std::max(double(srtt_us) / 1e6, 1e-4);
  const double gain =
      phase_ == CcPhase::kSlowStart ? cfg_.pacing_gain_ss : cfg_.pacing_gain_ca;
  return u64(cwnd_ / srtt_s * gain);
}

double CubicCc::CubicCurveBytes(Time now) const {
  const double t = double(now - epoch_start_) / 1e6;
  const double seg = cfg_.cubic_c * std::pow(t - k_s_, 3.0) + w_max_seg_;
  return seg * double(cfg_.mss);
}

double CubicCc::RenoFriendlyBytes(Time now, u64 srtt_us) const {
  const double rtt_s = std::max(double(srtt_us) / 1e6, 1e-4);
  const double t = double(now - epoch_start_) / 1e6;
  const double b = cfg_.cubic_beta;
  const double seg = w_max_seg_ * b + (3.0 * (1.0 - b) / (1.0 + b)) * (t / rtt_s);
  return seg * double(cfg_.mss);
}

double CubicCc::TargetBytes(Time now, u64 srtt_us) const {
  if (phase_ != CcPhase::kCongAvoid) return cwnd_;
  if (cfg_.algo == CcAlgo::kReno) return cwnd_;
  // 取三次曲线与 Reno 友好估计的较大者——低 BDP / 短 RTT 时由后者兜底，
  // 否则 CUBIC 会长期停在平台期，比 Reno 还慢（RFC 9438 §4.3 的 TCP-friendly 区域）。
  const double target = std::max(CubicCurveBytes(now), RenoFriendlyBytes(now, srtt_us));
  return std::max(target, cwnd_);  // 拥塞避免阶段只升不降
}

void CubicCc::OnPacketSent(u64 pn) {
  if (pn > max_pn_sent_) max_pn_sent_ = pn;
}

void CubicCc::EnterCongAvoid(Time now) {
  phase_ = CcPhase::kCongAvoid;
  // 从慢启动直接进入拥塞避免（HyStart 判定，没有发生丢包）：
  // 以当前窗口为原点重新开始三次曲线，K=0，曲线从平台期缓慢抬升。
  w_max_seg_ = CwndSeg();
  k_s_ = 0.0;
  epoch_start_ = now;
}

void CubicCc::EndRound(Time now) {
#ifdef XPORT_DEBUG
  if (cwnd_ > 30000) {
    std::fprintf(stderr, "[round] r=%llu n=%u cwnd=%.1fKB delay_min=%.1fms round_min=%.1fms ph=%d\n",
                 (unsigned long long)round_, round_samples_, cwnd_ / 1024.0, delay_min_us_ / 1000.0,
                 round_min_us_ / 1000.0, int(phase_));
  }
#endif
  if (cfg_.hystart && cfg_.algo == CcAlgo::kCubic && round_samples_ >= cfg_.hystart_min_samples &&
      cwnd_ >= double(cfg_.hystart_min_cwnd) && delay_min_us_ < 1e17) {
    // 阈值 clamp 在 [4ms, 16ms]：低于 4ms 会被抖动噪声误触发，高于 16ms 反应太慢
    double thr = delay_min_us_ / 8.0;
    thr = std::max(thr, cfg_.hystart_thresh_lo_us);
    thr = std::min(thr, cfg_.hystart_thresh_hi_us);

    if (round_min_us_ >= delay_min_us_ + thr) {
      if (phase_ == CcPhase::kSlowStart) {
        phase_ = CcPhase::kConsSlowStart;
        css_exit_cwnd_ = cwnd_ * 2.0;  // 再涨一倍仍未丢包就走拥塞避免
        ssthresh_ = css_exit_cwnd_;
      } else if (phase_ == CcPhase::kConsSlowStart) {
        EnterCongAvoid(now);
      }
    }
  }
  round_min_us_ = 1e18;
  round_samples_ = 0;
  ++round_;
}

void CubicCc::OnAck(u64 acked, u64 largest_pn, Time now, u64 srtt_us, u64 sample_us) {
  if (sample_us > 0) {
    const double s = double(sample_us);
    delay_min_us_ = std::min(delay_min_us_, s);
    round_min_us_ = std::min(round_min_us_, s);
    ++round_samples_;
  }

  // 轮次边界：本轮开始时发出的包都被 ACK 覆盖了
  if (largest_pn >= round_end_pn_) {
    EndRound(now);
    round_end_pn_ = max_pn_sent_;
  }

  if (acked == 0) return;

  switch (phase_) {
    case CcPhase::kSlowStart:
      cwnd_ += double(acked);
      if (cwnd_ >= ssthresh_) {
        EnterCongAvoid(now);
        ssthresh_ = cwnd_;
      }
      break;

    case CcPhase::kConsSlowStart:
      // 每 RTT 只涨 1/4：足以在 HyStart 误判时继续试探，又不会瞬间打爆队列
      cwnd_ += double(acked) * 0.25;
      if (cwnd_ >= css_exit_cwnd_) {
        EnterCongAvoid(now);
      }
      break;

    case CcPhase::kCongAvoid: {
      if (cfg_.algo == CcAlgo::kReno) {
        cwnd_ += double(acked) * double(cfg_.mss) / std::max(cwnd_, 1.0);
        break;
      }
      const double target = TargetBytes(now, srtt_us);
      if (target > cwnd_) {
        // RFC 9438 §4.1：每 ACK 增长 (target - cwnd) / cwnd 个 MSS
        cwnd_ += (target - cwnd_) * double(acked) / std::max(cwnd_, 1.0);
      }
      break;
    }
  }

  cwnd_ = std::min(cwnd_, double(cfg_.max_cwnd));
  cwnd_ = std::max(cwnd_, double(cfg_.min_cwnd));
}

void CubicCc::OnCongestion(Time now) {
  ++events_;
  last_pre_cwnd_ = cwnd_;
  const bool had_event = has_congestion_;  // 首轮没有历史 W_max，不能走快速收敛
  has_congestion_ = true;
  recovery_start_ = now;

  if (cfg_.algo == CcAlgo::kReno) {
    cwnd_ = std::max(cwnd_ * 0.5, double(cfg_.min_cwnd));
    ssthresh_ = cwnd_;
    phase_ = CcPhase::kCongAvoid;
    epoch_start_ = now;
    return;
  }

  const double w = CwndSeg();
  // 快速收敛：本次窗口仍低于历史 W_max，说明网络容量估计偏大，把 W_max 压到
  // 新旧窗口的中间值，让曲线更快逼近真实容量（RFC 9438 §4.6）
  if (had_event && w < w_max_seg_) {
    w_max_seg_ = w * (1.0 + cfg_.cubic_beta) / 2.0;
  } else {
    w_max_seg_ = w;
  }

  cwnd_ = std::max(w * cfg_.cubic_beta * double(cfg_.mss), double(cfg_.min_cwnd));
  ssthresh_ = cwnd_;
  // K 的定义：从当前窗口涨回 W_max 所需的时间
  k_s_ = std::cbrt(std::max((w_max_seg_ - CwndSeg()) / cfg_.cubic_c, 0.0));
  epoch_start_ = now;
  phase_ = CcPhase::kCongAvoid;
}

}  // namespace xport
