#pragma once
// 传输层：可靠、有序、多流复用的连接。
//
// 本期实现范围：
//   * 包级可靠传输：SACK 区间 ACK + 包阈值/时间阈值丢包检测 + RTO
//   * 恢复期（recovery period）：同一窗口内只降一次窗，避免被重复砍
//   * 拥塞控制：CUBIC + HyStart++（见 cc.h），可切 Reno 做对照
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

#include "xport/cc.h"
#include "xport/io.h"
#include "xport/packet.h"
#include "xport/ranges.h"
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
  Time min_rto_us = 200'000;    // RTO 下限：低于 200ms 会把排队抖动误判成丢包
  bool coalesce = true;         // 帧聚合开关（A/B 对照用）
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

  // 推进协议：RTO、流控探测、发 ACK、发新数据
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
  const CubicCc& cc() const { return cc_; }
  u64 Cwnd() const { return cc_.cwnd(); }
  u64 Ssthresh() const { return cc_.ssthresh(); }
  u64 InFlight() const { return inflight_; }
  u64 SrttUs() const { return has_rtt_ ? u64(srtt_us_) : 0; }
  u64 FlowLimit() const { return flow_max_; }
  u64 SentHighWater() const { return sent_high_water_; }
  u64 StreamSentBytes(u32 sid) const;
  std::size_t StreamCount() const { return out_.size(); }
  std::size_t InFlightPackets() const { return inflight_pkts_; }

 private:
  // 一个包内最多携带多少段数据（可跨流）。上限决定了环形数组元素的体积：
  // 8 段 × 16 B + 头部 ≈ 152 B，8192 槽 = 1.2 MB，单连接可接受。
  static constexpr u32 kMaxSegsPerPacket = 8;
  // 在途包环形数组容量，必须是 2 的幂。它同时是"窗口能开到多大"的硬上限：
  // 8192 包 × 1368 B ≈ 11 MB，覆盖 100 Gbps × 1 ms 的 BDP。
  static constexpr std::size_t kSentRing = 8192;
  static constexpr std::size_t kSentMask = kSentRing - 1;
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
    Seg segs[kMaxSegsPerPacket];
  };
  struct OutStream {
    std::vector<u8> data;   // 用户已提交的全部字节
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
  Sent& Slot(u64 pn) { return sent_ring_[std::size_t(pn) & kSentMask]; }
  const Sent& Slot(u64 pn) const { return sent_ring_[std::size_t(pn) & kSentMask]; }
  bool InUse(u64 pn) const {
    const Sent& s = Slot(pn);
    return s.used && s.pn == pn;
  }
  void AdvanceOldest();
  void Forget(u64 pn);

  void InitBuffers();

  void SendData(Time now);
  void SendAck(Time now);
  void SendProbe(Time now);
  void RefillPacer(Time now);
  // 把已编码的 payload 封装成一个包发出去，并登记在途记录
  void EmitPacket(const Seg* segs, u32 nseg, u32 bytes, const u8* payload, std::size_t plen,
                  Time now);
  void SendStreamPacket(u32 sid, u64 off, u32 len, Time now, bool retransmit);
  void OnAck(const AckFrame& a, Time now);
  void OnStream(const StreamFrame& f);
  void RecordRecv(u64 pn);
  void SlideRecvWindow(u64 shift);
  bool RecvBit(std::size_t idx) const {
    return (recv_bits_[idx >> 6] >> (idx & 63)) & 1ull;
  }
  void MarkRecv(std::size_t idx) { recv_bits_[idx >> 6] |= 1ull << (idx & 63); }
  void DetectLoss(Time now);
  void UpdateRtt(Time sample_us);
  Time Rto() const;

  // 仿真路径独占的适配器；真实路径下为 nullptr。
  // 声明顺序必须在 io_ 之前——引用绑定的对象要先构造出来。
  std::unique_ptr<SimEndpoint> owned_io_;
  IoEndpoint& io_;
  u32 self_ = 0;
  u32 peer_ = 0;
  TransportConfig cfg_;

  std::deque<OutStream> out_;  // sid 即下标；deque 保证扩容时引用不失效
  std::deque<InStream> in_;
  std::vector<u32> order_;
  std::size_t rr_ = 0;        // DRR 轮转游标

  // 在途包：按包号索引的环形数组 + 惰性推进的最老包号游标
  std::vector<Sent> sent_ring_;
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

  // 流控：recv_high_water_ 是本端收到的"各流最高偏移之和"，
  // 对端据此发 MAX_DATA；sent_high_water_ 是对应方向的已发送量。
  u64 recv_consumed_ = 0;   // Σ consumed，流控广告的基准
  u64 recv_window_ = 0;     // 每流环形缓冲大小（2 的幂）
  u64 sent_high_water_ = 0;
  u64 flow_max_ = 0;
  bool flow_blocked_ = false;
  Time flow_blocked_since_ = 0;

  // pacing 令牌桶：没有它，一次 Tick 会把整个 cwnd 倒进队列，尾部成批丢弃
  double tokens_ = 0;
  Time last_pace_ = 0;
  bool pace_init_ = false;

  Stats stats_;
};

}  // namespace xport
