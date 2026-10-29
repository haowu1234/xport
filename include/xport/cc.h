#pragma once
// 拥塞控制：CUBIC（RFC 9438）+ HyStart++（RFC 9406）+ pacing 速率。
//
// 刻意做成不依赖传输层的独立模块，原因是：
//   1) 曲线数学可以用纯用例验证，不必跑仿真器，失败点定位到公式而不是"整条链路的锅"；
//   2) 换成 BBR 时只替换本文件，transport 不动；
//   3) 所有内部状态（W_max、K、epoch、round）都能被测试直接读取。
//
// 单位约定：
//   * 窗口对外一律用字节（与 cwnd 的实际比较对象一致，避免 MSS 换算漂移）；
//   * CUBIC 公式内部用"段"（segments = MSS 数），因为 RFC 9438 的 C 与 K 都是按段定义的，
//     直接拿字节代入会让 K 差一个 MSS 量级——这是移植 CUBIC 最常踩的坑。
#include "xport/types.h"

namespace xport {

enum class CcPhase : u8 {
  kSlowStart = 0,      // 指数增长
  kConsSlowStart = 1,  // HyStart++ 保守慢启动：每 RTT 只涨 1/4
  kCongAvoid = 2,      // CUBIC 拥塞避免
};

enum class CcAlgo : u8 {
  kCubic = 0,
  kReno = 1,  // 对照组：AIMD，用于证明 CUBIC 在高 BDP 下的收益
};

const char* CcPhaseName(CcPhase p);
const char* CcAlgoName(CcAlgo a);

struct CcConfig {
  CcAlgo algo = CcAlgo::kCubic;
  u64 mss = 1368;
  u64 initial_cwnd = 10 * 1368;
  u64 min_cwnd = 2 * 1368;
  u64 max_cwnd = 8ull << 20;

  // RFC 9438：C 决定增长激进度，beta 是乘性降窗因子
  double cubic_c = 0.4;
  double cubic_beta = 0.7;

  // RFC 9406 HyStart++
  bool hystart = true;
  u64 hystart_min_cwnd = 16 * 1368;   // 窗口太小时 RTT 噪声盖过信号，不判
  u32 hystart_min_samples = 8;        // 每轮至少 8 个样本才判
  double hystart_thresh_lo_us = 4000.0;
  double hystart_thresh_hi_us = 16000.0;

  // pacing：慢启动给 2 倍余量（否则自己把自己限速），拥塞避免 1.25 倍
  double pacing_gain_ss = 2.0;
  double pacing_gain_ca = 1.25;
  double pacing_burst_pkts = 8.0;
};

class CubicCc {
 public:
  explicit CubicCc(const CcConfig& cfg = CcConfig{});

  // 每发出一个包调用一次，用于划分"轮"（round），HyStart++ 依赖它。
  void OnPacketSent(u64 pn);

  // 一次 ACK 处理后调用（已聚合本次 ACK 覆盖的全部字节，避免逐包 pow）。
  //   acked      本次新确认的字节数
  //   largest_pn 本次 ACK 覆盖的最大包号
  //   srtt_us    平滑 RTT（0 表示还没有样本）
  //   sample_us  本次的 RTT 采样（0 表示本 ACK 没有有效采样）
  void OnAck(u64 acked, u64 largest_pn, Time now, u64 srtt_us, u64 sample_us);

  // 拥塞事件（丢包或 RTO）。同一恢复期内的重复事件由 InRecovery 过滤。
  void OnCongestion(Time now);

  // 该包是否在"上一次降窗之前"发出——是的话它的丢失不构成新的拥塞事件。
  // 这是 RFC 9002 的恢复期判定，能消除同一窗口内被重复砍两刀的问题。
  bool InRecovery(Time sent_time) const { return has_congestion_ && sent_time <= recovery_start_; }

  u64 cwnd() const { return u64(cwnd_); }
  u64 ssthresh() const { return u64(ssthresh_); }
  CcPhase phase() const { return phase_; }
  CcAlgo algo() const { return cfg_.algo; }
  u64 PacingRateBps(u64 srtt_us) const;
  u64 PacingBurstBytes() const { return u64(cfg_.pacing_burst_pkts * double(cfg_.mss)); }

  // ---- 诊断接口（图表/测试用）----

  // 当前时刻 CUBIC 的目标窗口（字节）。拥塞避免阶段实测 cwnd 应当贴着它走。
  double TargetBytes(Time now, u64 srtt_us) const;
  // 纯三次函数项 C*(t-K)^3 + W_max，单位字节；不含 Reno 友好项
  double CubicCurveBytes(Time now) const;
  // Reno 友好估计 W_est，单位字节
  double RenoFriendlyBytes(Time now, u64 srtt_us) const;

  double WMaxSegments() const { return w_max_seg_; }
  // 最近一次降窗前的窗口（字节）。用于核对 beta，避免用采样点近似带来误差。
  u64 LastPreCongestionCwnd() const { return u64(last_pre_cwnd_); }
  double KSeconds() const { return k_s_; }
  Time EpochStart() const { return epoch_start_; }
  u64 CongestionEvents() const { return events_; }
  u64 RoundCount() const { return round_; }
  double DelayMinUs() const { return delay_min_us_; }

 private:
  double CwndSeg() const { return cwnd_ / double(cfg_.mss); }
  void EnterCongAvoid(Time now);
  void EndRound(Time now);

  CcConfig cfg_;
  double cwnd_ = 0;
  double ssthresh_ = 0;
  CcPhase phase_ = CcPhase::kSlowStart;

  // CUBIC 状态
  double w_max_seg_ = 0;      // W_max，单位段
  double k_s_ = 0;            // K，秒
  Time epoch_start_ = 0;      // 当前拥塞避免阶段的起点
  Time recovery_start_ = 0;   // 最近一次降窗时刻（恢复期上界）
  bool has_congestion_ = false;
  u64 events_ = 0;

  // HyStart++
  double delay_min_us_ = 1e18;
  double round_min_us_ = 1e18;
  u32 round_samples_ = 0;
  u64 round_end_pn_ = 0;
  u64 max_pn_sent_ = 0;
  u64 round_ = 0;
  double css_exit_cwnd_ = 0;
  double last_pre_cwnd_ = 0;
};

}  // namespace xport
