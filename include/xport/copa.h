#pragma once
// 拥塞控制：Copa（Arun & Balakrishnan, NSDI 2018）。
//
// 与 CUBIC / BBR 的根本区别：Copa 不把丢包当拥塞信号，也不估计瓶颈带宽，
// 而是直接把"排队时延"当成唯一的反馈。论文从效用函数 U = log λ − δ·log d 出发，
// 推导出每个发送端应当瞄准的目标速率（§2.1，公式 (1)）：
//
//     λ_target = 1 / (δ · d_q)        单位：包/秒，1/δ 以 MTU 计
//
// 其中 d_q 是排队时延。窗口按"当前速率离目标有多远"逐 ACK 调整，每 RTT 大约
// 变化 v/δ 个包：
//
//     cwnd += ± v / (δ · cwnd) · 本次确认的字节数     （cwnd 以字节计）
//
// 两个时延估计（§2.1）：
//   * RTT_min       最近 min(10 s, 流存活时长) 内的最小 RTT，当作传播时延。
//                   10 s 窗口是为了路由切换后能忘掉旧路径的最小值。
//   * RTT_standing  最近 srtt/2 内的最小 RTT，当作"常驻队列"的时延。
//                   用窗口最小值而不是最新样本，是为了不被 ACK 压缩和抖动骗到。
//   * d_q = RTT_standing − RTT_min
//
// 速度参数 v（§2.1 第 4 步）：每过一个 srtt 比较一次窗口方向，方向连续保持
// 3 个 RTT 之后才开始每次翻倍，方向一变就回到 1。稳态下方向每 2.5 RTT 变一次，
// 所以 v 恒为 1，窗口只做幅度约 1/δ 个包的小振荡。
//
// 慢启动：窗口每 RTT 翻倍，直到当前速率超过目标速率（d_q = 0 时目标为无穷大，
// 所以空队列上会一直翻倍）。论文没有写退出后的行为，这里采用 mvfst 的做法：
// 一旦速率越过目标就永久离开慢启动，之后只走 ±v 的调整。
//
// 两种模式（§2.2）：
//   * 默认模式：δ = 0.5，丢包不降窗。随机丢包不该降速，这正是 Copa 相对 CUBIC 的卖点。
//   * 竞争模式：有长时间填满缓冲的流（CUBIC / Reno）同在瓶颈时，队列永远排不空，
//     δ 固定的 Copa 会被饿死（实测份额 0.3–6%）。这时对 1/δ 做 AIMD：每 RTT +1，
//     丢包时减半（每 RTT 至多一次），δ 上限 0.5。
// 判据利用 Copa 自己的性质：只有 Copa 流时队列至少每 5 RTT 排空一次。最近 5 个 RTT
// 里见过"接近空"的队列（d_q < 0.1·(RTT_max − RTT_min)，RTT_max 取最近 4 个 RTT）
// 就留在默认模式，否则切到竞争模式；切回默认模式时 δ 复位到 0.5。
// 每 RTT 一步的细节（+1 与减半的节拍）照作者实现 genericCC 的 update_delta。
//
// 竞争模式默认不用 AIMD，而是让 1/δ 沿 CUBIC 曲线走（论文 §2.2："varies 1/δ according to
// whatever buffer-filling algorithm one wishes to emulate"）：
//     1/δ(t) = C·(t − K)³ + W_max，  丢包时 W_max = 1/δ，1/δ ×= β，K = ∛(W_max·(1−β)/C)
// 每 RTT +1 仍作为下限。原因：AIMD 的 1/δ 从 2 起步，在深缓冲里追不上已经把队列填满的
// CUBIC（16 BDP 份额 0.089）；CUBIC 曲线下是 0.51，Copa 之间的公平不变。数据见
// research/udp-transport/notes/2026-11-16-copa.md。
//
// 持续拥塞（整段路径不通）两种模式都一样：窗口回到最小值重新慢启动。
//
// 单位：窗口对外一律字节，公式内部先换成"段"（MSS 数）再算——和 cc.h 里 CUBIC
// 的约定同一条理由：论文的 δ 与 1/δ 都是按包定义的，直接拿字节代入量纲就错了。
#include <deque>

#include "xport/cc.h"

namespace xport {

// 一次 ACK 处理的输入。字段和 BbrAck 对齐，Copa 只用得到其中几个，
// 但 transport 已经把这一组数都算好了，复用同一份避免再开一条调用路径。
struct CopaAck {
  Time now = 0;
  u64 acked = 0;   // 本次新确认的字节
  Time rtt_us = 0;  // 本次 RTT 样本，0 表示没有
  u64 srtt_us = 0;  // 平滑 RTT（transport 维护的那份），0 表示还没有样本
  bool cwnd_limited = false;  // 窗口是否被用满；应用受限时不增窗
};

class CopaCc {
 public:
  // 论文 §2.2：默认模式 δ = 0.5。
  static constexpr double kDefaultDelta = 0.5;
  // RTT_min 的观察窗口：min(10 s, 流存活时长)。10 s 是论文给的上限。
  static constexpr Time kMinRttWindowUs = 10'000'000;

  explicit CopaCc(const CcConfig& cfg = CcConfig{});

  void OnPacketSent(u64 pn);
  // 一次 ACK 处理后调用。acked 是本次新确认的字节（已聚合，不必逐包调用）。
  void OnAck(const CopaAck& a);
  // 默认模式不响应丢包。保留这个入口是为了和 transport 的分派对称：
  // 恢复期判定仍要登记，否则同一次拥塞里的持续拥塞判定会被重复触发。
  void OnCongestion(Time now);
  // 持续拥塞：路径整段不通，窗口回到最小值并重新慢启动。
  void OnPersistentCongestion(Time now);

  bool InRecovery(Time sent_time) const { return has_congestion_ && sent_time <= recovery_start_; }

  u64 cwnd() const { return u64(cwnd_); }
  // pacing 速率 = 2 · cwnd / RTT_standing（§2.1）。论文明确要求两倍：
  // 正好等于 cwnd/RTT 时，pacing 的任何误差都会让实际发送慢于窗口允许的速率。
  // 还没有 RTT 样本时退回到按 srtt 估算，和 CubicCc::PacingRateBps 同一条兜底。
  u64 PacingRateBps(u64 srtt_us) const;
  u64 PacingBurstBytes() const { return u64(cfg_.pacing_burst_pkts * double(cfg_.mss)); }
  // 判据与 CubicCc::IsCwndLimited 相同：剩余额度不到一个 pacing 突发就算被用满，
  // 慢启动放宽到"在途超过窗口一半"（慢启动每 RTT 翻倍，在途天然只有窗口的一半左右）。
  bool IsCwndLimited(u64 inflight) const;

  // ---- 诊断接口（图表 / 测试用）----

  bool InSlowStart() const { return slow_start_; }
  bool InCompetitiveMode() const { return competitive_; }
  u64 ModeSwitches() const { return mode_switches_; }
  double Delta() const { return delta_; }
  double Velocity() const { return velocity_; }
  // 方向：+1 增、−1 减、0 尚未判定。
  int Direction() const { return direction_; }
  Time MinRttUs() const { return has_min_rtt_ ? min_rtt_us_ : 0; }
  Time StandingRttUs() const { return has_standing_ ? standing_rtt_us_ : 0; }
  // 最近一次 ACK 算出的排队时延，µs。没有估计时为 0。
  double QueuingDelayUs() const { return queuing_delay_us_; }
  // 最近一次 ACK 的目标窗口（字节）。d_q = 0 时为 +inf，返回 max_cwnd。
  double TargetCwndBytes() const { return target_cwnd_; }
  u64 RoundCount() const { return rounds_; }
  u64 CongestionEvents() const { return events_; }

 private:
  // (时刻, RTT 样本)。单调递减队列：队首即窗口内最小值，新样本入队时丢掉所有更大的旧样本。
  struct RttSample {
    Time t = 0;
    Time rtt = 0;
  };
  // 喂一个样本并丢掉窗口之外的，返回窗口内最小值。窗口内还没有样本时返回 rtt 本身。
  static Time PushMin(std::deque<RttSample>& w, Time now, Time rtt, Time window_us);

  CcConfig cfg_;
  double delta_ = kDefaultDelta;
  double cwnd_ = 0;

  bool slow_start_ = true;
  Time last_double_t_ = 0;  // 慢启动最近一次翻倍的时刻
  bool has_doubled_ = false;

  // 速度参数。direction_ 是上一个整轮的判定，cur_dir_sign_ 是本轮内逐 ACK 累加的符号和。
  double velocity_ = 1.0;
  int direction_ = 0;
  int same_dir_rounds_ = 0;  // 方向连续保持的轮数，满 3 轮才开始翻倍
  Time round_start_t_ = 0;
  double round_start_cwnd_ = 0;
  bool round_open_ = false;
  int cur_dir_sign_ = 0;
  u64 rounds_ = 0;

  // RTT 估计。min_rtt 用 10 s 窗口，standing 用 srtt/2 窗口。
  std::deque<RttSample> min_window_;
  std::deque<RttSample> standing_window_;
  Time min_rtt_us_ = 0;
  Time standing_rtt_us_ = 0;
  bool has_min_rtt_ = false;
  bool has_standing_ = false;
  Time first_rtt_t_ = 0;  // 第一个样本的时刻：min_rtt 窗口 = min(10 s, 流存活时长)

  // 模式切换（§2.2）。near_empty_ 是最近 5 个 RTT 内的 RTT 最小值，recent_max_ 是最近
  // 4 个 RTT 内的最大值，两者都是单调队列。
  void UpdateMode(Time now, u64 srtt_us);
  std::deque<RttSample> near_empty_window_;
  std::deque<RttSample> recent_max_window_;  // 存的是 RTT 的相反数，复用 PushMin 求最大
  Time recent_min_rtt_us_ = 0;
  Time recent_max_rtt_us_ = 0;
  bool competitive_ = false;
  u64 mode_switches_ = 0;
  Time last_delta_inc_t_ = 0;   // 竞争模式下 1/δ 上一次 +1 的时刻
  Time last_delta_loss_t_ = 0;  // 上一次因丢包把 1/δ 减半的时刻
  bool has_delta_loss_ = false;
  // CUBIC 曲线的状态（单位：1/δ，即包）
  bool cubic_epoch_ = false;
  Time cubic_epoch_t_ = 0;
  double cubic_wmax_ = 0;
  double cubic_k_s_ = 0;
  u64 last_srtt_us_ = 0;

  double queuing_delay_us_ = 0;
  double target_cwnd_ = 0;

  Time recovery_start_ = 0;
  bool has_congestion_ = false;
  u64 events_ = 0;

  u64 max_pn_sent_ = 0;
};

}  // namespace xport
