#pragma once
// 拥塞控制：BBR v1（draft-cardwell-iccrg-bbr-congestion-control-00，细节对照 Linux tcp_bbr.c）。
//
// 与 CUBIC 的根本区别：不把丢包当拥塞信号，而是显式估计两个量——
//   BtlBw   瓶颈带宽：最近 10 轮投递速率样本的最大值（rate_sample.h 提供样本）
//   RTprop  传播时延：最近 10 s 的 RTT 最小值
// 发送速率 = pacing_gain × BtlBw，在途上限 = cwnd_gain × BtlBw × RTprop。
// 于是窗口只需要比 BDP 略大，队列不会被填满——这也是它在随机丢包和深缓冲下的优势来源。
//
// 状态机：
//   Startup   pacing_gain = 2/ln2，每轮带宽翻倍；连续 3 轮涨幅不到 25% 判定"管道已满"
//   Drain     pacing_gain = ln2/2，把 Startup 冲进队列的数据排掉，直到在途 ≤ 1 BDP
//   ProbeBW   8 相增益循环 {1.25, 0.75, 1×6}：先探更高带宽，再排掉探测造成的队列
//   ProbeRTT  RTprop 10 s 没刷新时进入：cwnd 压到 4 包至少 200 ms + 1 轮，重新测最小 RTT
//
// 本实现刻意没做的（v1 草案里也是可选或后加的）：
//   * 长期带宽采样（lt_bw，针对令牌桶限速器）；
//   * 空闲重启（idle restart）的 pacing 特殊处理；
//   * ACK 聚合补偿（extra_acked，v2 才进主线）。
// 这些都不影响仿真里的基本行为，需要时再加，并各自配用例。
#include <algorithm>

#include "xport/cc.h"
#include "xport/rate_sample.h"
#include "xport/types.h"

namespace xport {

// 窗口极大值过滤器：Kathleen Nichols 的三样本算法，移植自 Linux lib/minmax.c。
// 时间轴用"轮数"，不用墙钟：一轮内的样本高度相关，按时间开窗会被 ACK 聚合噪声主导。
// 用三个样本（最好、次好、第三好且更新）近似整个窗口，O(1) 更新、零分配。
class WindowedMax {
 public:
  u64 Get() const { return s_[0].v; }
  void Reset(u64 t, u64 v) { s_[0] = s_[1] = s_[2] = Sample{t, v}; }
  // 返回更新后的最大值
  u64 Update(u64 win, u64 t, u64 v);

 private:
  struct Sample {
    u64 t = 0;
    u64 v = 0;
  };
  void SubwinUpdate(u64 win, const Sample& val);
  Sample s_[3];
};

enum class BbrState : u8 {
  kStartup = 0,
  kDrain = 1,
  kProbeBw = 2,
  kProbeRtt = 3,
};

const char* BbrStateName(BbrState s);

// 一次 ACK 处理（含随后的丢包检测）的全部输入。
// 做成结构体而不是一长串参数：字段多、语义相近，位置参数极易传错。
struct BbrAck {
  Time now = 0;
  u64 acked = 0;           // 本次新确认的字节
  u64 lost = 0;            // 本次判丢的字节
  u64 prior_inflight = 0;  // 处理本 ACK 之前的在途字节（ProbeBW 换相判据要用）
  u64 inflight = 0;        // 处理完本 ACK 与丢包之后的在途字节
  u64 delivered = 0;       // 连接累计确认字节（RateSampler::delivered）
  Time rtt_us = 0;         // 本次 RTT 样本，0 表示没有
  bool has_newest_acked = false;
  Time newest_acked_sent_t = 0;  // 本次确认的包里最晚发出的那个的发送时刻（判定退出恢复期）
  // 处理本 ACK 之前窗口是否被用满（v2/v3 只在被窗口卡住时才抬高 inflight_hi；v1 不用）
  bool cwnd_limited = false;
  RateSample rs;
};

class BbrCc {
 public:
  static constexpr double kHighGain = 2.885;  // 2/ln2：每轮翻倍所需的最小增益
  static constexpr double kDrainGain = 1.0 / 2.885;
  static constexpr double kCwndGain = 2.0;    // 容忍 ACK 延迟/聚合，在途留一倍余量
  static constexpr int kCycleLen = 8;
  static constexpr u64 kBwWindowRounds = kCycleLen + 2;
  static constexpr Time kMinRttWindowUs = 10'000'000;
  static constexpr Time kProbeRttDurationUs = 200'000;
  static constexpr double kFullBwThresh = 1.25;
  static constexpr u32 kFullBwRounds = 3;
  // pacing 比估计带宽略低 1%（Linux bbr_pacing_margin_percent）：
  // 正好等于瓶颈速率时，任何估计误差都会让队列缓慢累积。
  static constexpr double kPacingMargin = 0.99;

  explicit BbrCc(const CcConfig& cfg = CcConfig{});

  void OnAck(const BbrAck& a);
  // 拥塞事件（判丢且不在恢复期内）。真正的降窗推迟到随后的 OnAck，
  // 因为包守恒要用到"本 ACK 确认了多少"。
  void OnCongestion(Time now);
  void OnPersistentCongestion(Time now);
  bool InRecovery(Time sent_time) const { return has_congestion_ && sent_time <= recovery_start_; }

  u64 cwnd() const { return u64(cwnd_); }
  u64 PacingRateBps() const { return u64(pacing_rate_); }
  u64 PacingBurstBytes() const { return u64(cfg_.pacing_burst_pkts * double(cfg_.mss)); }
  // ProbeRTT 期间发送端要把自己标成 app-limited，否则低速样本会把 BtlBw 拉下来
  bool InProbeRtt() const { return state_ == BbrState::kProbeRtt; }

  // ---- 诊断接口 ----
  BbrState state() const { return state_; }
  u64 BtlBwBps() const { return btlbw_.Get(); }
  Time MinRttUs() const { return has_min_rtt_ ? min_rtt_us_ : 0; }
  u64 BdpBytes() const;
  double PacingGain() const { return pacing_gain_; }
  double CwndGain() const { return cwnd_gain_; }
  bool FilledPipe() const { return filled_pipe_; }
  int CycleIndex() const { return cycle_index_; }
  u64 RoundCount() const { return round_; }
  u64 CongestionEvents() const { return events_; }
  u64 ProbeRttCount() const { return probe_rtt_count_; }
  bool InPacketConservation() const { return packet_conservation_; }
  u64 MinPipeCwnd() const { return 4 * cfg_.mss; }

 private:
  // gain × BtlBw × RTprop；还没有 RTT 样本时返回初始窗口（与 Linux bbr_bdp 一致）
  double Bdp(double gain) const;
  // Bdp 再加上量化余量：pacing 按突发放行，窗口要能容纳在途的那几个突发，
  // 否则高增益阶段会被 cwnd 卡住、测不到更高的带宽
  double Inflight(double gain) const { return Bdp(gain) + 3.0 * double(cfg_.mss); }

  void UpdateBtlBw(const BbrAck& a);
  void CheckCyclePhase(const BbrAck& a);
  void AdvanceCyclePhase(Time now);
  void CheckFullPipe(const BbrAck& a);
  void CheckDrain(const BbrAck& a);
  void UpdateMinRttAndProbeRtt(const BbrAck& a);
  void UpdateGains();
  void SetPacingRate();
  void SetCwnd(const BbrAck& a);

  void EnterStartup();
  void EnterProbeBw(Time now);
  void SaveCwnd();
  void RestoreCwnd() { cwnd_ = std::max(cwnd_, prior_cwnd_); }
  u32 NextRand();

  CcConfig cfg_;
  BbrState state_ = BbrState::kStartup;
  double pacing_gain_ = kHighGain;
  double cwnd_gain_ = kHighGain;
  double cwnd_ = 0;
  double pacing_rate_ = 0;
  double prior_cwnd_ = 0;
  bool has_seen_rtt_ = false;  // 拿到首个 RTT 后按它重新初始化 pacing

  WindowedMax btlbw_;
  Time min_rtt_us_ = 0;
  Time min_rtt_stamp_ = 0;
  bool has_min_rtt_ = false;

  // 轮次：以"投递量"为时钟，发出时的 delivered 越过上一轮末尾就算新的一轮
  u64 round_ = 0;
  u64 next_round_delivered_ = 0;
  bool round_start_ = false;

  // Startup 退出判定
  bool filled_pipe_ = false;
  u64 full_bw_ = 0;
  u32 full_bw_count_ = 0;

  // ProbeBW
  int cycle_index_ = 0;
  Time cycle_stamp_ = 0;

  // ProbeRTT
  bool probe_rtt_done_armed_ = false;
  Time probe_rtt_done_stamp_ = 0;
  bool probe_rtt_round_done_ = false;
  u64 probe_rtt_count_ = 0;

  // 恢复期（包守恒）
  bool has_congestion_ = false;
  Time recovery_start_ = 0;
  bool enter_recovery_pending_ = false;
  bool in_recovery_ = false;
  bool packet_conservation_ = false;
  u64 events_ = 0;

  // 确定性伪随机：ProbeBW 的起始相位。固定种子 → 仿真逐位可复现
  u32 rng_ = 0x9e3779b9u;
};

}  // namespace xport
