#include "xport/copa.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace xport {

CopaCc::CopaCc(const CcConfig& cfg) : cfg_(cfg) {
  cwnd_ = double(cfg_.initial_cwnd);
  delta_ = kDefaultDelta;
  target_cwnd_ = double(cfg_.max_cwnd);
}

bool CopaCc::IsCwndLimited(u64 inflight) const {
  const double room = cwnd_ - double(inflight);
  if (room <= double(PacingBurstBytes())) return true;
  return slow_start_ && double(inflight) * 2.0 > cwnd_;
}

u64 CopaCc::PacingRateBps(u64 srtt_us) const {
  // 有常驻 RTT 估计就用它，否则退回调用方给的平滑 RTT。两倍增益是论文 §2.1 的要求。
  const double rtt_s = std::max(
      double(has_standing_ ? standing_rtt_us_ : srtt_us) / 1e6, 1e-4);
  return u64(2.0 * cwnd_ / rtt_s);
}

void CopaCc::UpdateMode(Time now, u64 srtt_us) {
  if (!cfg_.copa_mode_switch || srtt_us == 0 || !has_min_rtt_) return;
  const double span = double(recent_max_rtt_us_) - double(min_rtt_us_);
  // span 为 0（RTT 完全不抖）时队列显然是空的；"接近空"的门槛跟着近期抖动幅度走，
  // 这样同一个判据在抖动大的网络里也不会一直误判成"有竞争流"
  const bool saw_near_empty = span <= 0.0 ||
                              double(recent_min_rtt_us_) - double(min_rtt_us_) < 0.1 * span;
  const bool want_competitive = !saw_near_empty;
  if (want_competitive != competitive_) {
    competitive_ = want_competitive;
    ++mode_switches_;
    // 切回默认模式时 δ 复位；切入竞争模式时从当前 δ（即 0.5）开始做 AIMD
    if (!competitive_) delta_ = kDefaultDelta;
    cubic_epoch_ = false;
    competitive_since_ = now;
    last_delta_inc_t_ = now;
  }
  // CUBIC 曲线延迟 5 个 RTT 再启动：刚切入竞争模式时还不知道对手多强，
  // 让每 RTT +1 先把 1/δ 推到几个包再交给 CUBIC 曲线。如果不延迟，浅缓冲
  // （0.5 BDP）里 CUBIC 曲线的 +1 下限会把 4 条 Copa 流的队列推到 7.8 ms。
  if (competitive_ && cfg_.copa_cubic_competitive &&
      srtt_us > 0 && now - competitive_since_ >= 5 * srtt_us) {
    if (!cubic_epoch_) {
      cubic_epoch_ = true;
      cubic_epoch_t_ = now;
      cubic_wmax_ = 1.0 / delta_;
      cubic_k_s_ = 0.0;
    }
    const double t = double(now - cubic_epoch_t_) / 1e6;
    const double inv = cfg_.cubic_c * std::pow(t - cubic_k_s_, 3.0) + cubic_wmax_;
    if (inv > 1.0 / delta_) delta_ = 1.0 / inv;  // 只抬高；下面每 RTT +1 是下限
  }
  // 每 RTT +1 只在 CUBIC 曲线尚未启动时使用。曲线一旦启动，它自己的增长速度
  // 远超 +1/RTT，+1 下限反而成为噪声——在浅缓冲里它会把 Copa 流之间的队列推高。
  if (competitive_ && !cubic_epoch_ && now - last_delta_inc_t_ >= srtt_us) {
    delta_ = 1.0 / (1.0 / delta_ + 1.0);
    last_delta_inc_t_ = now;
  }
}

Time CopaCc::PushMin(std::deque<RttSample>& w, Time now, Time rtt, Time window_us) {
  // 单调队列：队尾起所有不小于新样本的都丢掉，新样本入队后队首就是窗口最小值。
  while (!w.empty() && w.back().rtt >= rtt) w.pop_back();
  w.push_back(RttSample{now, rtt});
  const Time cutoff = now > window_us ? now - window_us : 0;
  while (w.size() > 1 && w.front().t < cutoff) w.pop_front();
  return w.front().rtt;
}

void CopaCc::OnPacketSent(u64 pn) {
  if (pn > max_pn_sent_) max_pn_sent_ = pn;
}

void CopaCc::OnAck(const CopaAck& a) {
  if (a.rtt_us > 0) {
    if (!has_min_rtt_) first_rtt_t_ = a.now;
    // min_rtt 的窗口是 min(10 s, 流存活时长)。比流更老的样本本来就不存在，
    // 所以直接用 10 s 窗口就等价于论文的定义。
    min_rtt_us_ = PushMin(min_window_, a.now, a.rtt_us, kMinRttWindowUs);
    has_min_rtt_ = true;

    // 常驻 RTT：最近 srtt/2 内的最小样本。PushMin 至少保留一个样本，不需要再给窗口兜底。
    // 曾经把窗口下限设成本次 RTT 样本：那等于把窗口拉长到一整个 RTT，队列开始堆积时
    // 窗口里仍留着空队列时期的旧样本，排队时延被算成 0，慢启动永远退不出去。
    const Time standing_win = a.srtt_us > 0 ? a.srtt_us / 2 : a.rtt_us / 2;
    standing_rtt_us_ = PushMin(standing_window_, a.now, a.rtt_us, standing_win);
    has_standing_ = true;

    // 模式判据用的两个窗口：最近 5 RTT 的最小值、最近 4 RTT 的最大值。
    // 没有平滑 RTT 时用本次样本代替 RTT 长度。
    const Time rtt_len = a.srtt_us > 0 ? Time(a.srtt_us) : a.rtt_us;
    // genericCC 用 4·srtt 窗口，论文写 5 RTT。4 RTT 在 0.5 BDP 下对 CUBIC 的份额
    // 0.38 对比 5 RTT 的 0.24（3 个种子均值），其余缓冲相当，Copa 之间排队不变。
    recent_min_rtt_us_ = PushMin(near_empty_window_, a.now, a.rtt_us, 4 * rtt_len);
    // 最大值：把样本取反存进同一种单调队列
    const Time neg = ~Time(0) - a.rtt_us;
    recent_max_rtt_us_ = ~Time(0) - PushMin(recent_max_window_, a.now, neg, 4 * rtt_len);
  }
  if (a.srtt_us > 0) last_srtt_us_ = a.srtt_us;
  UpdateMode(a.now, a.srtt_us);

  if (a.acked == 0 || !has_min_rtt_ || !has_standing_) return;

  // 排队时延。常驻 RTT 比历史最小还小（min_rtt 窗口刚滑走一个旧样本）时夹到 0：
  // 负的排队时延没有意义，按"队列为空"处理，目标速率无穷大。
  queuing_delay_us_ =
      standing_rtt_us_ > min_rtt_us_ ? double(standing_rtt_us_ - min_rtt_us_) : 0.0;
  // 噪声下限：低于它的排队时延当作抖动（见 CcConfig::copa_dq_floor_us）
  const double floor_us = double(cfg_.copa_dq_floor_us);
  queuing_delay_us_ = queuing_delay_us_ > floor_us ? queuing_delay_us_ - floor_us : 0.0;

  const double cwnd_pkts = cwnd_ / double(cfg_.mss);
  // 目标窗口 = 目标速率 × RTT_standing = cwnd_pkts / (δ · d_q_秒) × (d_q 与速率的比较
  // 等价于窗口比较)：λ = cwnd/RTT_standing ≤ 1/(δ·d_q) ⇔ cwnd ≤ RTT_standing/(δ·d_q)。
  // d_q = 0 时目标无穷大，用窗口上限代替。
  double target_pkts = std::numeric_limits<double>::infinity();
  if (queuing_delay_us_ > 0.0) {
    target_pkts = (double(standing_rtt_us_) / queuing_delay_us_) / delta_;
  }
  target_cwnd_ = std::min(target_pkts * double(cfg_.mss), double(cfg_.max_cwnd));
  const bool increase = cwnd_pkts <= target_pkts;

  // 速度参数：每过一个 srtt 结算一次方向（论文 §2.1 第 4 步，"once per window"）。
  // 慢启动期间不做——慢启动的增长不是 ±v 这一套，混进去会让 v 在退出时已经很大。
  if (!slow_start_ && a.srtt_us > 0) {
    if (!round_open_) {
      round_open_ = true;
      round_start_t_ = a.now;
      round_start_cwnd_ = cwnd_;
      cur_dir_sign_ = 0;
    } else if (a.now - round_start_t_ >= a.srtt_us) {
      const int dir = cwnd_ > round_start_cwnd_ ? +1 : -1;
      if (dir == direction_) {
        ++same_dir_rounds_;
        // 连续三个 RTT 同向之后才翻倍。稳态下方向每 2.5 RTT 变一次，到不了 3，
        // 所以 v 保持 1——这是论文要的稳态行为。
        if (same_dir_rounds_ >= 3) velocity_ *= 2.0;
      } else {
        velocity_ = 1.0;
        same_dir_rounds_ = 0;
      }
      direction_ = dir;
      ++rounds_;
      round_start_t_ = a.now;
      round_start_cwnd_ = cwnd_;
      cur_dir_sign_ = 0;
    }
  }

  if (increase) {
    // 应用受限：窗口没被用满，ACK 不能证明网络扛得住更大的窗口，不增窗。
    if (!a.cwnd_limited) return;
    if (slow_start_) {
      // 每 RTT 翻倍一次，而不是每个 ACK 翻倍（那样一个 RTT 内会翻 2^N 倍）。
      if (!has_doubled_) {
        has_doubled_ = true;
        last_double_t_ = a.now;
      } else if (a.srtt_us > 0 && a.now - last_double_t_ >= a.srtt_us) {
        cwnd_ *= 2.0;
        last_double_t_ = a.now;
      }
    } else {
      // 方向突然反转而 v 还停在很大的值上：那个 v 是为相反方向积累的，先清掉。
      // mvfst 的做法，论文没有写这一步；不加的话一次超调会把窗口打穿。
      if (direction_ < 0 && velocity_ > 1.0) {
        velocity_ = 1.0;
        same_dir_rounds_ = 0;
        direction_ = +1;
      }
      cwnd_ += velocity_ / (delta_ * std::max(cwnd_pkts, 1.0)) * double(a.acked);
    }
  } else {
    // 越过目标：慢启动到此结束，之后只做 ±v 的调整。
    slow_start_ = false;
    if (direction_ > 0 && velocity_ > 1.0) {
      velocity_ = 1.0;
      same_dir_rounds_ = 0;
      direction_ = -1;
    }
    const double dec =
        velocity_ / (delta_ * std::max(cwnd_pkts, 1.0)) * double(a.acked);
    cwnd_ = std::max(cwnd_ - dec, double(cfg_.min_cwnd));
  }

  cwnd_ = std::min(cwnd_, double(cfg_.max_cwnd));
  cwnd_ = std::max(cwnd_, double(cfg_.min_cwnd));
}

void CopaCc::OnCongestion(Time now) {
  ++events_;
  has_congestion_ = true;
  recovery_start_ = now;
  // 默认模式不响应丢包。竞争模式下乘性减：AIMD 时 1/δ 减半，CUBIC 曲线时乘 β，δ 上限 0.5。
  // 每个 RTT 至多一次，同一窗口里的连续丢包只算一次拥塞（和 Reno 的恢复期同一道理）。
  if (!competitive_) return;
  if (has_delta_loss_ && last_srtt_us_ > 0 && now - last_delta_loss_t_ < last_srtt_us_) return;
  if (cfg_.copa_cubic_competitive) {
    cubic_wmax_ = 1.0 / delta_;
    delta_ = std::min(delta_ / cfg_.cubic_beta, kDefaultDelta);
    cubic_k_s_ = std::cbrt(cubic_wmax_ * (1.0 - cfg_.cubic_beta) / cfg_.cubic_c);
    cubic_epoch_t_ = now;
    cubic_epoch_ = true;
  } else {
    delta_ = std::min(delta_ * 2.0, kDefaultDelta);
  }
  last_delta_loss_t_ = now;
  has_delta_loss_ = true;
}

void CopaCc::OnPersistentCongestion(Time now) {
  cwnd_ = double(cfg_.min_cwnd);
  slow_start_ = true;
  has_doubled_ = false;
  velocity_ = 1.0;
  direction_ = 0;
  same_dir_rounds_ = 0;
  round_open_ = false;
  has_congestion_ = true;
  recovery_start_ = now;
  delta_ = kDefaultDelta;
  competitive_ = false;
  cubic_epoch_ = false;
}

}  // namespace xport
