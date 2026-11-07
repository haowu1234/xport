#pragma once
// 传输层：可靠、有序、多流复用的连接。
//
// 本期实现范围：
//   * 包级可靠传输：SACK 区间 ACK + 包阈值/时间阈值丢包检测 + PTO（RFC 9002）
//   * 批量收发：SendQueued + FlushSend，后端可把多包合成一次 syscall
//   * 恢复期（recovery period）：同一窗口内只降一次窗，避免被重复砍
//   * 拥塞控制：CUBIC + HyStart++（见 cc.h），可切 Reno 做对照，或切 BBR v1（见 bbr.h）
//   * pacing：令牌桶按 cwnd/SRTT 放行，杜绝"一次 Tick 倒空整个窗口"
//   * 流控：MAX_DATA 连接级窗口 + 半窗更新，附带流控探测防死锁
//   * 多流调度：DRR（deficit round robin），按字节公平
//   * 帧聚合：一个报文可携带多条流的多段数据，小消息场景把 pps 降一个数量级
//
// 性能相关的数据结构选择（都有实测依据，见 docs/scaling.md）：
//   * 在途包表用**按包号索引的环形数组**，ACK 按区间直接定位，O(确认包数)
//   * 区间合并用**定长数组**（RangeSet），热路径零堆分配
//   * 接收端包号用**滑动位图**，替代 std::set
//   * 流表用 deque，替代 std::map<u32, ...>
#include <deque>
#include <memory>
#include <tuple>
#include <vector>

#include "xport/bbr.h"
#include "xport/bbr3.h"
#include "xport/cc.h"
#include "xport/io.h"
#include "xport/packet.h"
#include "xport/ranges.h"
#include "xport/rate_sample.h"
#include "xport/send_buffer.h"
#include "xport/sim.h"
#include "xport/types.h"

namespace xport {

struct TransportConfig {
  u32 ack_delay_us = 2000;      // ACK 合并窗口上限（稀疏流量兜底）
  u32 ack_every_n = 2;          // 每收到 N 个数据包立即回 ACK
  u64 initial_max_data = 2ull << 20;  // 初始流控窗口；无握手，两端静态约定
  // 接收窗口（字节，内部向上取到 2 的幂）。它同时是每流环形缓冲的大小：
  // 未消费数据永远不会超过这个值——因为流控广告的就是"已消费 + 窗口"。
  // 0 表示跟随 initial_max_data（默认行为）；想提高吞吐就显式调大。
  u64 recv_window_bytes = 0;
  u64 drr_quantum = 4 * 1368;   // DRR 每轮每流额度（字节）
  // 定时器粒度（RFC 9002 kGranularity）。PTO = SRTT + max(4·RTTVAR, 粒度) + ack_delay。
  // 不再有 200 ms 下限：PTO 超时只发探测包、不降窗，误触发的代价是两个多余的包，
  // 而不是一次 200 ms 的停顿（回环 RTT 45 µs 时那相当于 4400 个 RTT）。
  Time timer_granularity_us = 1000;
  // 持续拥塞判定倍数（RFC 9002 kPersistentCongestionThreshold）
  u32 persistent_congestion_threshold = 3;
  bool coalesce = true;         // 帧聚合开关（A/B 对照用）
  // 接收端接受的最大流数（sid ∈ [0, max_streams)）。sid 来自线上，不设上限时
  // 对端一个 sid 很大的包就能让本端 resize 出 GB 级的流表。越界帧丢弃并计 stream_limit。
  // 最坏内存 ≈ max_streams × recv_window（每流环形缓冲在首个字节到达时分配）。
  u32 max_streams = 256;
  // 在途包表（按包号索引的环形数组）的容量上限，按 2 的幂向下取整，最小 8192。
  // 表从 8192 槽起步，包号跨度不够时翻倍扩容。它限制的是"同时未确认的包号跨度"，
  // 默认 131072 包 ≈ 179 MB 在途数据，覆盖 10 Gbps × 100 ms；扩满时表本身约 22 MB。
  u32 max_inflight_packets = 1u << 17;
  CcConfig cc;
};

class Transport {
 public:
  // 仿真路径：内部把 SimLink 包成 SimEndpoint，调用方代码与之前完全一致。
  Transport(SimLink& link, u32 self, u32 peer, const TransportConfig& cfg = TransportConfig{});
  // 真实路径：直接绑定到一个已经 connect() 好的端点。
  Transport(IoEndpoint& io, const TransportConfig& cfg = TransportConfig{});

  u32 OpenStream();
  void Write(u32 sid, Bytes data);

  // 推进协议：PTO、流控探测、发 ACK、发新数据
  void Tick(Time now);
  // 收到一个完整 UDP 包
  void OnPacket(Bytes pkt, Time now);

  // 发送端视角：所有流的字节都已发出且被确认
  bool SendComplete() const;
  // 应用已消费的字节数（不是"已收到"——收到但不消费不计入）
  u64 DeliveredBytes() const { return recv_consumed_; }
  const Stats& stats() const { return stats_; }

  // ---- 接收侧：消费式生命周期 ----
  // 协议不再替应用保存全部数据，而是把收到的字节写进每流的**预分配环形缓冲**，
  // 应用取走（Peek）并归还（Consume）之后，这段空间才会被后续数据覆盖。
  //
  // 这样做的两个直接后果，都是有意的：
  //   1. 收包热路径零缺页、零零填充，落地成本从 0.49 ns/B 降到 0.03 ns/B；
  //   2. 应用不消费，流控窗口就不前进，对端会被背压挡住——这是正确行为，
  //      但调用方**必须**消费，否则传输会停在中途。
  std::size_t ReadableBytes(u32 sid) const;
  // 返回当前可读的连续视图（零拷贝）。回绕时只返回到缓冲末尾的那一段，
  // 调用方循环取即可。视图在对应数据被 Consume 之前保持有效。
  Bytes Peek(u32 sid);
  // 归还 n 字节缓冲。n 会被夹到当前可读长度之内。
  void Consume(u32 sid, std::size_t n);
  // 当前环形缓冲容量（诊断用）
  std::size_t RecvBufferSize(u32 sid) const;

  // ---- 诊断接口（bench / 轨迹探针 / 图表用）----
  // CUBIC/Reno 的内部状态。algo 为 kBbr 时这个对象不参与控制，读 bbr()
  const CubicCc& cc() const { return cc_; }
  // 非 BBR 连接返回一个静态的初始状态对象，方便诊断代码无条件调用
  const BbrCc& bbr() const {
    static const BbrCc kIdle;
    return bbr_ ? *bbr_ : kIdle;
  }
  CcAlgo Algo() const { return cfg_.cc.algo; }
  const Bbr3Cc& bbr3() const {
    static const Bbr3Cc kIdle(CcConfig{}, Bbr3Params::V3());
    return bbr3_ ? *bbr3_ : kIdle;
  }
  bool UsingBbr() const { return use_bbr_; }
  bool UsingBbr3() const { return use_bbr3_; }  // v2 或 v3
  u64 CongestionEvents() const {
    return use_bbr3_ ? bbr3_->CongestionEvents()
                     : (use_bbr_ ? bbr_->CongestionEvents() : cc_.CongestionEvents());
  }
  u64 PacingRateBps() const;
  const RateSampler& rate_sampler() const { return sampler_; }
  const RateSample& LastRateSample() const { return last_rate_sample_; }
  u64 Cwnd() const {
    return use_bbr3_ ? bbr3_->cwnd() : (use_bbr_ ? bbr_->cwnd() : cc_.cwnd());
  }
  u64 Ssthresh() const { return use_bbr_ || use_bbr3_ ? 0 : cc_.ssthresh(); }  // BBR 没有 ssthresh
  u64 InFlight() const { return inflight_; }
  u64 SrttUs() const { return has_rtt_ ? u64(srtt_us_) : 0; }
  u64 FlowLimit() const { return flow_max_; }
  u64 SentHighWater() const { return sent_high_water_; }
  u64 StreamSentBytes(u32 sid) const;
  std::size_t StreamCount() const { return out_.size(); }
  std::size_t InFlightPackets() const { return inflight_pkts_; }
  // 发送缓冲实际占用的内存（所有流合计，按 64 KB 块计）
  std::size_t SendBufferBytes() const;
  std::size_t SentRingCapacity() const { return sent_ring_.size(); }

 private:
  // 一个包内最多携带多少段数据（可跨流）。上限决定了环形数组元素的体积：
  // 8 段 × 16 B + 头部 ≈ 168 B，初始 8192 槽 ≈ 1.4 MB。
  static constexpr u32 kMaxSegsPerPacket = 8;
  // 在途包环形数组的初始容量（2 的幂）。以前它是固定的硬上限：8192 包 × 1368 B ≈ 11 MB，
  // 1 Gbps × 100 ms 的 BDP（12.5 MB）就装不下。现在按需翻倍，上限见 max_inflight_packets。
  static constexpr std::size_t kSentRingInit = 8192;
  // 接收包号位图容量（位）。同时决定 ACK 能描述多长的历史。
  static constexpr std::size_t kRecvBits = 16384;
  static constexpr std::size_t kRecvWords = kRecvBits / 64;

  struct Seg {
    u32 sid = 0;
    u32 len = 0;
    u64 off = 0;
  };
  struct Sent {
    u64 pn = 0;
    Time t = 0;
    u32 bytes = 0;   // 包内数据总字节（不含帧头）
    u8 seg_count = 0;
    bool used = false;
    bool acked = false;  // 因被确认而释放（区别于判丢释放），持续拥塞判定要用
    bool rs_app_limited = false;  // 以下四个字段是压缩存放的 RateSnapshot
    // 两个时刻都不晚于发送时刻 t，存成相对 t 的 u32 差值（µs，上限约 71 分钟）。
    // 原样存 RateSnapshot 要 32 B，Sent 从 152 B 涨到 184 B，实测发送路径慢约 5 ns/包；
    // 压缩后只多 16 B，app_limited 标记塞进原来的对齐空洞。
    u32 rs_delivered_dt = 0;
    u32 rs_first_sent_dt = 0;
    u64 rs_delivered = 0;
    // BBRv2/v3 的丢包模型用：发送后的在途量（上限受环形数组约束，远小于 4 GB）与累计判丢的低 32 位
    u32 rs_tx_in_flight = 0;
    u32 rs_lost = 0;
    Seg segs[kMaxSegsPerPacket];

    void StoreRate(const RateSnapshot& r) {
      rs_delivered = r.delivered;
      rs_delivered_dt = SatDelta(t, r.delivered_t);
      rs_first_sent_dt = SatDelta(t, r.first_sent_t);
      rs_app_limited = r.is_app_limited;
      rs_tx_in_flight = u32(r.tx_in_flight);
      rs_lost = u32(r.lost);
    }
    RateSnapshot LoadRate() const {
      return RateSnapshot{rs_delivered, t - rs_delivered_dt, t - rs_first_sent_dt,
                          rs_app_limited,     rs_tx_in_flight,      rs_lost};
    }
    // 超过 u32 时饱和：区间被算得更长、速率偏低，只会让带宽估计保守，不会虚高
    static u32 SatDelta(Time hi, Time lo) {
      const Time d = hi > lo ? hi - lo : 0;
      return d > 0xffffffffull ? 0xffffffffu : u32(d);
    }
  };
  struct OutStream {
    // 用户已提交、尚未被确认释放的字节。偏移是流内绝对偏移，低于 data.base() 的已释放
    SendBuffer data;
    u64 next_off = 0;       // 下一个待发送偏移
    // 已确认的 offset 区间，与接收端的重组结构对称。
    // 注意：不能用单一 watermark 代替——重传换新包号，包号连续性不代表字节连续性。
    RangeSet acked;
    long long deficit = 0;  // DRR 额度
    u64 bytes_sent = 0;     // 已发出的原始字节（不含重传），用于公平性度量
  };
  struct InStream {
    // 预分配环形缓冲，大小 = recv_window_（2 的幂）。offset 直接取模定位，
    // 不做分段管理、不做链式分配——连续字节流最适合这种结构。
    std::vector<u8> ring;
    u64 consumed = 0;       // 应用已消费到的 offset
    u64 contiguous = 0;     // 连续已收到的最高 offset（可读上界）
    u64 highest_end = 0;    // 收到的最高 offset（含空洞之后的）
    RangeSet ranges;        // [consumed, highest_end) 内已收到的区间
  };

  // 环形数组访问
  Sent& Slot(u64 pn) { return sent_ring_[std::size_t(pn) & sent_mask_]; }
  const Sent& Slot(u64 pn) const { return sent_ring_[std::size_t(pn) & sent_mask_]; }
  // 保证接下来至少还能登记 kSentSlack 个包而不覆盖在用的槽。SendData 每轮检查一次，
  // 一轮最多发 2 个包（重传前先 flush 拼好的包，或聚合时满 8 段再 flush），循环退出后
  // 收尾的 flush 还可能再发 1 个，所以留 3 个。
  // 判据是包号跨度而不是在途包数：最老的包迟迟未确认时，新包号会绕回到它的槽位上。
  // 跨度不够时先推进 oldest_pn_，仍不够就扩容；扩到上限仍不够返回 false。
  static constexpr u64 kSentSlack = 3;
  bool HasSentRoom() {
    if (next_pn_ + kSentSlack - oldest_pn_ <= sent_ring_.size()) return true;
    return MakeSentRoom();
  }
  bool MakeSentRoom();
  void GrowSentRing();
  bool InUse(u64 pn) const {
    const Sent& s = Slot(pn);
    return s.used && s.pn == pn;
  }
  void AdvanceOldest();
  void Forget(u64 pn);

  void InitBuffers();

  void SendData(Time now);
  bool HasUnsentData() const;  // 流控允许范围内还有没发过的新数据
  void SendAck(Time now);
  void SendProbe(Time now);
  void RefillPacer(Time now);
  // 把已编码的 payload 封装成一个包发出去，并登记在途记录
  void EmitPacket(const Seg* segs, u32 nseg, u32 bytes, const u8* payload, std::size_t plen,
                  Time now);
  void SendStreamPacket(u32 sid, u64 off, u32 len, Time now, bool retransmit);
  void OnAck(const AckFrame& a, Time now);
  // 返回 false 表示这段数据没有被登记（区间表满或越过接收窗口），包不能 ACK
  bool OnStream(const StreamFrame& f);
  void RecordRecv(u64 pn);
  void SlideRecvWindow(u64 shift);
  bool RecvBit(std::size_t idx) const {
    return (recv_bits_[idx >> 6] >> (idx & 63)) & 1ull;
  }
  void MarkRecv(std::size_t idx) { recv_bits_[idx >> 6] |= 1ull << (idx & 63); }
  void ClearRecv(std::size_t idx) { recv_bits_[idx >> 6] &= ~(1ull << (idx & 63)); }
  // 返回本次判丢的字节数（BBR 的包守恒和换相判据要用）
  u64 DetectLoss(Time now);
  // 拥塞控制分派。没用虚函数：两个分支都是内联小函数，分支预测稳定命中，
  // 而虚调用会挡住内联，CUBIC 热路径上的 cwnd()/InRecovery() 都变成间接调用
  bool CcInRecovery(Time sent_t) const {
    if (use_bbr3_) return bbr3_->InRecovery(sent_t);
    return use_bbr_ ? bbr_->InRecovery(sent_t) : cc_.InRecovery(sent_t);
  }
  u64 CcBurstBytes() const {
    if (use_bbr3_) return bbr3_->PacingBurstBytes();
    return use_bbr_ ? bbr_->PacingBurstBytes() : cc_.PacingBurstBytes();
  }
  void UpdateRtt(Time sample_us);
  // 基础 PTO（不含退避）。持续拥塞时长也按它算。
  Time Pto() const;
  // 当前生效的 PTO：基础值 × 2^pto_count，上限 60 s
  Time PtoBackoff() const;
  // PTO 超时：登记要重发的数据，并给 SendData 两个不受 cwnd / pacing 约束的探测包额度
  void OnPtoTimeout();

  // 仿真路径独占的适配器；真实路径下为 nullptr。
  // 声明顺序必须在 io_ 之前——引用绑定的对象要先构造出来。
  std::unique_ptr<SimEndpoint> owned_io_;
  IoEndpoint& io_;
  // 节点号只用于构造 SimEndpoint（它自己保存一份），Transport 本身不需要留。
  TransportConfig cfg_;

  std::deque<OutStream> out_;  // sid 即下标；deque 保证扩容时引用不失效
  std::deque<InStream> in_;
  std::vector<u32> order_;
  std::size_t rr_ = 0;        // DRR 轮转游标

  // 在途包：按包号索引的环形数组 + 惰性推进的最老包号游标
  std::vector<Sent> sent_ring_;
  std::size_t sent_mask_ = 0;
  std::size_t sent_ring_max_ = 0;
  u64 next_pn_ = 0;
  u64 oldest_pn_ = 0;         // 最小的可能仍在途的包号（惰性推进）
  std::size_t inflight_pkts_ = 0;
  u64 loss_scan_pn_ = 0;      // 包阈值丢包检测的单调游标，避免每次重扫
  std::deque<std::tuple<u32, u64, u32>> retransmit_;

  u64 largest_acked_ = 0;
  bool has_largest_acked_ = false;
  u64 inflight_ = 0;

  // 收到的包号：滑动位图。基准 recv_base_ 之下的包号视为过老（靠重传兜底）
  std::vector<u64> recv_bits_;
  u64 recv_base_ = 0;
  u64 recv_hi_ = 0;
  bool has_recv_ = false;
  bool ack_pending_ = false;
  Time ack_due_ = 0;
  u32 recv_since_ack_ = 0;

  bool has_rtt_ = false;
  double srtt_us_ = 0;
  double rttvar_us_ = 0;
  CubicCc cc_;
  // 堆上分配、只在选了 BBR 时分配。内联存放会让 Transport 多出 368 B：实测在未改动的
  // 基线上只加同样大小的填充，CUBIC 发送路径就慢 ~12%（cpubench Tick 136 → 152 ns/包），
  // 原因是热成员之间的布局与栈上对象的缓存行分布变了，与 BBR 逻辑无关。
  // 构造时分配一次，不在热路径上。
  std::unique_ptr<BbrCc> bbr_;
  bool use_bbr_ = false;
  // v2/v3 同理：按需堆分配，不选就不占 Transport 的体积
  std::unique_ptr<Bbr3Cc> bbr3_;
  bool use_bbr3_ = false;
  RateSampler sampler_;
  RateSample last_rate_sample_;  // 最近一次 ACK 产生的速率样本

  // 流控：recv_high_water_ 是本端收到的"各流最高偏移之和"，
  // 对端据此发 MAX_DATA；sent_high_water_ 是对应方向的已发送量。
  u64 recv_consumed_ = 0;   // Σ consumed，流控广告的基准
  u64 recv_window_ = 0;     // 每流环形缓冲大小（2 的幂）
  u64 sent_high_water_ = 0;
  u64 flow_max_ = 0;
  bool flow_blocked_ = false;
  Time flow_blocked_since_ = 0;

  // PTO 状态（RFC 9002 §6.2）
  Time last_send_t_ = 0;    // 最近一次发出数据包的时刻，PTO 从这里起算
  u32 pto_count_ = 0;       // 连续 PTO 次数，决定退避倍数；有新确认就清零
  u32 probe_budget_ = 0;    // 剩余可绕过 cwnd / pacing 的探测包个数
  Time first_rtt_t_ = 0;    // 拿到首个 RTT 样本的时刻：之前发的包不参与持续拥塞判定

  // pacing 令牌桶：没有它，一次 Tick 会把整个 cwnd 倒进队列，尾部成批丢弃
  double tokens_ = 0;
  Time last_pace_ = 0;
  bool pace_init_ = false;

  Stats stats_;
};

}  // namespace xport
