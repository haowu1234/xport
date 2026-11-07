#include "xport/transport.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xport {
namespace {

std::unique_ptr<Bbr3Cc> MakeBbr3(const CcConfig& cc) {
  if (cc.algo == CcAlgo::kBbr2) return std::make_unique<Bbr3Cc>(cc, Bbr3Params::V2());
  if (cc.algo == CcAlgo::kBbr3) return std::make_unique<Bbr3Cc>(cc, Bbr3Params::V3());
  return nullptr;
}

// 一个满帧留给 STREAM 帧头的余量（type + sid + offset + len + fin 的最大编码长度）
constexpr std::size_t kMaxStreamPayload = kMaxPayload - 32;
// 聚合时保守估计的帧头开销，用来决定"这个包还能不能再塞一段"
constexpr std::size_t kFrameOverhead = 24;

}  // namespace

Transport::Transport(SimLink& link, u32 self, u32 peer, const TransportConfig& cfg)
    : owned_io_(std::make_unique<SimEndpoint>(link, self, peer)),
      io_(*owned_io_),
      cfg_(cfg),
      cc_(cfg.cc),
      bbr_(cfg.cc.algo == CcAlgo::kBbr ? std::make_unique<BbrCc>(cfg.cc) : nullptr),
      use_bbr_(bbr_ != nullptr),
      bbr3_(MakeBbr3(cfg.cc)),
      use_bbr3_(bbr3_ != nullptr),
      flow_max_(cfg.initial_max_data) {
  InitBuffers();
}

Transport::Transport(IoEndpoint& io, const TransportConfig& cfg)
    : owned_io_(nullptr),
      io_(io),
      cfg_(cfg),
      cc_(cfg.cc),
      bbr_(cfg.cc.algo == CcAlgo::kBbr ? std::make_unique<BbrCc>(cfg.cc) : nullptr),
      use_bbr_(bbr_ != nullptr),
      bbr3_(MakeBbr3(cfg.cc)),
      use_bbr3_(bbr3_ != nullptr),
      flow_max_(cfg.initial_max_data) {
  InitBuffers();
}

void Transport::InitBuffers() {
  // 接收窗口：默认跟随流控初值（这样"小窗口会被挡住"的语义保持一致），
  // 显式配置时取两者较大者，保证对端能发出去的量不会被缓冲装不下。
  const u64 want = cfg_.recv_window_bytes == 0
                       ? cfg_.initial_max_data
                       : std::max<u64>(cfg_.recv_window_bytes, cfg_.initial_max_data);
  recv_window_ = 4 * 1024;  // 最小一页：允许刻意配置很小的窗口（流控门控用例要用）
  while (recv_window_ < want && recv_window_ < (1ull << 30)) recv_window_ <<= 1;
  // 上限 1 GB：环形缓冲是按流真实分配的内存，不能让"把流控设成无限大"顺手把内存吃光。
  if (recv_window_ > (1ull << 30)) recv_window_ = 1ull << 30;

  sent_ring_.resize(kSentRingInit);
  sent_mask_ = kSentRingInit - 1;
  sent_ring_max_ = kSentRingInit;
  while (sent_ring_max_ * 2 <= cfg_.max_inflight_packets) sent_ring_max_ *= 2;
  recv_bits_.assign(kRecvWords, 0);
  order_.reserve(64);
}

u32 Transport::OpenStream() {
  const u32 sid = u32(out_.size());
  out_.emplace_back();
  order_.push_back(sid);
  return sid;
}

void Transport::Write(u32 sid, Bytes data) {
  if (sid >= out_.size()) return;
  out_[sid].data.Append(data);
}

std::size_t Transport::SendBufferBytes() const {
  std::size_t n = 0;
  for (const auto& st : out_) n += st.data.ResidentBytes();
  return n;
}

bool Transport::MakeSentRoom() {
  AdvanceOldest();
  while (next_pn_ + kSentSlack - oldest_pn_ > sent_ring_.size()) {
    if (sent_ring_.size() >= sent_ring_max_) return false;
    GrowSentRing();
  }
  return true;
}

void Transport::GrowSentRing() {
  // 翻倍并按新掩码重新放置。只搬包号跨度内的记录（含已确认的：持续拥塞判定要看
  // 丢包区间里有没有被确认过的包）。跨度最多几万，扩容是一次性的，不在稳态路径上。
  std::vector<Sent> bigger(sent_ring_.size() * 2);
  const std::size_t mask = bigger.size() - 1;
  for (u64 pn = oldest_pn_; pn < next_pn_; ++pn) {
    const Sent& s = Slot(pn);
    if (s.pn == pn && (s.used || s.acked)) bigger[std::size_t(pn) & mask] = s;
  }
  sent_ring_.swap(bigger);
  sent_mask_ = mask;
}

void Transport::AdvanceOldest() {
  while (oldest_pn_ < next_pn_ && !InUse(oldest_pn_)) ++oldest_pn_;
}

void Transport::Forget(u64 pn) {
  Sent& s = Slot(pn);
  if (!s.used || s.pn != pn) return;
  inflight_ -= s.bytes;
  s.used = false;
  if (inflight_pkts_ > 0) --inflight_pkts_;
}

void Transport::Tick(Time now) {
  // 1) PTO：最后一个数据包发出后 PtoBackoff() 内没有任何确认 → 发探测包。
  //    计时从"最后一次发出数据包"算起，持续发送时它会不断后移，所以只在发送停顿
  //    （尾部丢包、整窗丢失、ACK 全丢）时才会触发。
  if (inflight_pkts_ > 0 && now >= last_send_t_ && now - last_send_t_ >= PtoBackoff()) {
    OnPtoTimeout();
  }

  // 2) ACK 合并窗口到期
  if (ack_pending_ && now >= ack_due_) SendAck(now);

  // 3) 流控死锁探测：被流控挡住、且没有任何在途包可触发对端回 ACK 时，
  //    只能主动发 PING 把对方"敲醒"，让对方带着新的 MAX_DATA 回 ACK。
  if (flow_blocked_ && inflight_pkts_ == 0 && now >= flow_blocked_since_ &&
      now - flow_blocked_since_ >= PtoBackoff()) {
    SendProbe(now);
    flow_blocked_since_ = now;
    ++stats_.flow_probes;
  }

  // 4) 发送
  SendData(now);

  // 5) 把排队的报文真正发出去。批量后端靠这一步把多个包合成一次 syscall；
  //    不 flush 的话尾部报文会一直压在队列里，表现为传输莫名停住。
  io_.FlushSend(now);

  stats_.srtt_ms = srtt_us_ / 1000.0;
  stats_.cwnd_kb = double(Cwnd()) / 1024.0;
  stats_.ssthresh_kb = double(Ssthresh()) / 1024.0;
  stats_.pto_ms = double(PtoBackoff()) / 1000.0;
  stats_.pacing_mbps = double(PacingRateBps()) * 8.0 / 1e6;
}

u64 Transport::PacingRateBps() const {
  // CUBIC 由 cwnd/SRTT 推出速率；BBR 直接给出 pacing_gain × BtlBw
  if (use_bbr3_) return bbr3_->PacingRateBps();
  return use_bbr_ ? bbr_->PacingRateBps() : cc_.PacingRateBps(has_rtt_ ? u64(srtt_us_) : 1000);
}

void Transport::SendAck(Time now) {
  AckFrame a;
  a.delay_us = cfg_.ack_delay_us;

  // 把位图压缩成降序区间（连续包号合并成一段，最多 16 段）。
  // 按 64 位字跳跃，全零字一次跳过——否则窗口里几千个空洞会把扫描拖成线性。
  if (has_recv_) {
    long long bit = static_cast<long long>(recv_hi_ - recv_base_);
    while (bit >= 0 && a.count < kMaxAckRanges) {
      while (bit >= 0 && !RecvBit(std::size_t(bit))) {
        const std::size_t w = std::size_t(bit) >> 6;
        const unsigned off = unsigned(bit) & 63;
        const u64 mask = (off == 63) ? ~0ull : ((1ull << (off + 1)) - 1ull);
        if ((recv_bits_[w] & mask) == 0) {
          bit = static_cast<long long>(w) * 64 - 1;  // 整个字都是 0，一次跳过
        } else {
          --bit;
        }
      }
      if (bit < 0) break;
      const u64 hi = recv_base_ + u64(bit);
      long long lo = bit;
      while (lo > 0 && RecvBit(std::size_t(lo - 1))) --lo;
      a.lo[a.count] = recv_base_ + u64(lo);
      a.hi[a.count] = hi;
      ++a.count;
      bit = lo - 1;
    }
  }

  // 16 段区间最坏情况下要 (4+1+1) + 16*(8+8) ≈ 262 字节，缓冲区按此上界分配。
  // 早期版本用 64 字节，区间一多编码就失败并静默丢 ACK，导致对端永久停摆。
  u8 payload[320];
  std::size_t n = EncodeAck(MutBytes(payload, sizeof payload), a);
  if (n == 0) {  // 编码失败：丢弃本次 ACK，避免原地空转（对端会重传并触发新 ACK）
    ack_pending_ = false;
    ack_due_ = 0;
    return;
  }

  // MAX_DATA 随每个 ACK 捎带，而不是"只在窗口增长时发一次"。
  // 代价是每包多 5 字节（约 0.2%），换来的是丢掉一个窗口更新也不会双向死等；
  // 真正需要省这点带宽时，才值得引入独立的更新重传机制。
  // 广告基于"已消费"而不是"已收到"：应用不消费，窗口就不前进，对端被背压挡住。
  // 这正是消费式生命周期的核心——缓冲区不会被无限增长的数据撑爆。
  const u64 advertise = recv_consumed_ + recv_window_;
  const std::size_t m = EncodeMaxData(MutBytes(payload + n, sizeof payload - n), advertise);
  if (m > 0) n += m;

  Header h;
  h.type = PacketType::kAck;
  h.pkt_num = next_pn_++;
  h.payload_len = u16(n);

  u8 buf[kMtu];
  const std::size_t hn = EncodeHeader(h, MutBytes(buf, sizeof buf));
  if (hn == 0) return;
  std::memcpy(buf + hn, payload, n);

  cc_.OnPacketSent(h.pkt_num);
  io_.SendQueued(Bytes(buf, hn + n), now);
  ack_pending_ = false;
  ack_due_ = 0;
  recv_since_ack_ = 0;
  ++stats_.ack_sent;
}

void Transport::SendProbe(Time now) {
  u8 payload[8];
  const std::size_t n = EncodePing(MutBytes(payload, sizeof payload));
  if (n == 0) return;

  Header h;
  h.type = PacketType::kData;  // 用 kData 承载，接收端才会回 ACK
  h.pkt_num = next_pn_++;
  h.payload_len = u16(n);

  u8 buf[kMtu];
  const std::size_t hn = EncodeHeader(h, MutBytes(buf, sizeof buf));
  if (hn == 0) return;
  std::memcpy(buf + hn, payload, n);

  cc_.OnPacketSent(h.pkt_num);
  io_.SendQueued(Bytes(buf, hn + n), now);
}

void Transport::EmitPacket(const Seg* segs, u32 nseg, u32 bytes, const u8* payload,
                           std::size_t plen, Time now) {
  Header h;
  h.type = PacketType::kData;
  h.pkt_num = next_pn_++;
  h.dst_cid = kDefaultCid;
  h.payload_len = u16(plen);

  u8 buf[kMtu];
  const std::size_t hn = EncodeHeader(h, MutBytes(buf, sizeof buf));
  if (hn == 0) return;
  std::memcpy(buf + hn, payload, plen);

  cc_.OnPacketSent(h.pkt_num);
  // 速率采样：在 inflight 增长之前拍快照，拍完再增长——快照记录的是"发出本包时"的状态
  const RateSnapshot snap = sampler_.OnPacketSent(now, inflight_, bytes);
  // 发送失败不是异常，而是又一个丢包源：登记在途，交给丢包检测 / PTO 重传。
  // 但必须计数——静默的本地丢弃和链路丢包在现象上一模一样，没有计数器就查不出来。
  if (!io_.SendQueued(Bytes(buf, hn + plen), now)) ++stats_.send_fail;

  Sent& s = Slot(h.pkt_num);
  s.pn = h.pkt_num;
  s.t = now;
  s.bytes = bytes;
  s.seg_count = u8(nseg);
  s.used = true;
  s.acked = false;
  s.StoreRate(snap);  // 必须在 s.t 赋值之后：差值相对 s.t 存放
  last_send_t_ = now;
  if (probe_budget_ > 0) --probe_budget_;
  for (u32 i = 0; i < nseg; ++i) s.segs[i] = segs[i];

  ++inflight_pkts_;
  inflight_ += bytes;
  ++stats_.pkts_sent;
  stats_.bytes_sent += bytes;
}

void Transport::SendStreamPacket(u32 sid, u64 off, u32 len, Time now, bool retransmit) {
  if (sid >= out_.size()) return;
  OutStream& st = out_[sid];
  if (off + len > st.data.end() || off < st.data.base()) return;

  u8 scratch[kMaxStreamPayload];
  StreamFrame f;
  f.sid = sid;
  f.offset = off;
  f.data = st.data.View(off, len, scratch);
  f.fin = false;

  u8 payload[kMaxPayload];
  const std::size_t n = EncodeStream(MutBytes(payload, sizeof payload), f);
  if (n == 0) return;

  const Seg seg{sid, len, off};
  EmitPacket(&seg, 1, len, payload, n, now);

  if (retransmit) {
    ++stats_.retransmits;
  } else {
    st.next_off = off + len;
    st.bytes_sent += len;
  }
}

void Transport::RefillPacer(Time now) {
  const double burst = double(CcBurstBytes());

  if (!pace_init_) {
    pace_init_ = true;
    last_pace_ = now;
    tokens_ = burst;  // 冷启动给一个突发额度
    return;
  }
  if (now <= last_pace_) return;

  const double rate = double(PacingRateBps());
  const double dt = double(now - last_pace_) / 1e6;
  tokens_ = std::min(tokens_ + rate * dt, burst);
  last_pace_ = now;
}

void Transport::SendData(Time now) {
  RefillPacer(now);

  const u64 cwnd = Cwnd();

  // 流控状态只在"额度确实不足"时置位，避免把 cwnd 阻塞误判成流控阻塞
  if (sent_high_water_ + kMaxStreamPayload > flow_max_) {
    if (!flow_blocked_) {
      flow_blocked_ = true;
      flow_blocked_since_ = now;
      ++stats_.flow_blocked;
    }
  } else {
    flow_blocked_ = false;
  }

  // 当前正在拼装的包
  u8 payload[kMaxPayload];
  std::size_t plen = 0;
  Seg segs[kMaxSegsPerPacket];
  u32 nseg = 0;
  u32 batch_bytes = 0;
  bool out_of_data = false;  // 循环是否因 DRR 找不到可发数据而结束

  const auto flush = [&]() {
    if (nseg == 0) return;
    EmitPacket(segs, nseg, batch_bytes, payload, plen, now);
    nseg = 0;
    plen = 0;
    batch_bytes = 0;
  };

  for (int guard = 0; guard < 8192; ++guard) {
    // 拥塞窗口：留一个满包的余量
    // PTO 探测包不受 cwnd 约束（RFC 9002 §7.5）：窗口满了也必须发出去
    if (inflight_ + batch_bytes + kMaxStreamPayload > cwnd && probe_budget_ == 0) break;
    if (!HasSentRoom()) break;  // 在途包表扩到上限仍不够：必须先等 ACK

    // 1) 重传优先：恢复期若被新数据挤占窗口，丢包会长时间补不上
    if (!retransmit_.empty()) {
      flush();
      auto [rsid, roff, rlen] = retransmit_.front();
      const bool invalid = rsid >= out_.size() || roff + rlen > out_[rsid].data.end();
      // 低于 base 的数据已经释放，只可能是因为它落在已确认的连续前缀里
      const bool already = !invalid && (roff < out_[rsid].data.base() ||
                                        out_[rsid].acked.Contains(roff, roff + rlen));
      if (invalid || already) {
        if (already) ++stats_.spurious;
        retransmit_.pop_front();
        continue;
      }
      if (tokens_ < double(rlen) && probe_budget_ == 0) break;
      tokens_ -= double(rlen);
      retransmit_.pop_front();
      SendStreamPacket(rsid, roff, rlen, now, true);
      continue;
    }

    // 2) 包空间不够再塞一段就先发出去（关了聚合则每段一包）
    if (!cfg_.coalesce) {
      flush();
    } else if (kMaxPayload - plen < kFrameOverhead + 64) {
      flush();
    }
    std::size_t room = kMaxPayload - plen;

    // 3) DRR：按 deficit 轮转。每个流一次最多发 quantum 字节，
    //    小流不会被大流饿死（轮询是按字节记账，不是按包数）。
    bool found = false;
    u32 sid = 0;
    u64 off = 0;
    u32 len = 0;
    for (std::size_t i = 0; i < order_.size(); ++i) {
      const std::size_t idx = (rr_ + i) % order_.size();
      const u32 cand = order_[idx];
      OutStream& st = out_[cand];
      if (st.next_off >= st.data.end()) continue;

      const u64 remain = st.data.end() - st.next_off;
      const std::size_t cap = std::min<std::size_t>(kMaxStreamPayload, room - kFrameOverhead);
      const u32 n = u32(std::min<u64>(u64(cap), remain));
      if (n == 0) continue;

      // 流控：MAX_DATA 约束的是"各流最高偏移之和"
      if (sent_high_water_ + n > flow_max_) continue;

      if (st.deficit < (long long)n) st.deficit += (long long)cfg_.drr_quantum;
      if (st.deficit < (long long)n) continue;  // quantum >= 单包上限，理论上不会走到

      sid = cand;
      off = st.next_off;
      len = n;
      st.deficit -= (long long)n;
      rr_ = (idx + 1) % order_.size();
      found = true;
      break;
    }
    if (!found) {
#ifdef XPORT_DEBUG
      // 停滞探针：数据发完了、没在途包、也没有待重传，但确认区间没覆盖全部数据
      if (retransmit_.empty() && inflight_pkts_ == 0) {
        for (std::size_t i = 0; i < out_.size(); ++i) {
          const OutStream& st = out_[i];
          if (st.next_off >= st.data.end() && st.acked.PrefixEnd() < st.data.end()) {
            std::fprintf(stderr,
                         "[stall] t=%llums sid=%zu next=%llu acked_prefix=%llu size=%llu "
                         "segments=%zu dropped=%llu next_pn=%llu\n",
                         (unsigned long long)(now / 1000), i,
                         (unsigned long long)st.next_off,
                         (unsigned long long)st.acked.PrefixEnd(),
                         (unsigned long long)st.data.end(), st.acked.size(),
                         (unsigned long long)st.acked.dropped(), (unsigned long long)next_pn_);
          }
        }
      }
#endif
      out_of_data = true;
      break;
    }

    // 令牌不足：等下一轮 Tick，不逐包定时（PTO 探测包除外）
    if (tokens_ < double(len) && probe_budget_ == 0) break;
    tokens_ -= double(len);

    // 4) 编进当前包。装得下就继续拼下一段，装不下就下一轮 flush。
    OutStream& st = out_[sid];
    u8 scratch[kMaxStreamPayload];  // 段跨 64 KB 块边界时才用到
    StreamFrame f;
    f.sid = sid;
    f.offset = off;
    f.data = st.data.View(off, len, scratch);
    f.fin = false;

    const std::size_t n = EncodeStream(MutBytes(payload + plen, kMaxPayload - plen), f);
    if (n == 0) {  // 估算偏小导致放不下：先把当前包发掉，下一轮重来
      flush();
      continue;
    }

    segs[nseg++] = Seg{sid, len, off};
    plen += n;
    batch_bytes += len;
    st.next_off = off + len;
    st.bytes_sent += len;
    sent_high_water_ += len;

    if (nseg >= kMaxSegsPerPacket) flush();
  }
  flush();

  // 应用受限标记：发送循环因为"没有新数据可发"而停下（不是被 cwnd/pacing/流控挡住），
  // 说明带宽没有被用满。标记后直到当前在途的数据全部被确认之前，所有发出的包都带
  // app-limited 标签——BBR 看到这个标签会拒绝用它们的样本来抬高 max_bw。
  // 直接复用 DRR 找不到数据时的那次判断，而不是循环后再调 HasUnsentData() 扫一遍
  // 流表：后者每个 Tick 都是 O(流数)。流因流控被跳过时 flow_blocked_ 必然已置位
  // （单段长度不超过 kMaxStreamPayload），所以这里排除它就够了。
  if (out_of_data && !flow_blocked_ && inflight_ + kMaxStreamPayload <= cwnd) {
    sampler_.OnAppLimited(inflight_);
  }
  // ProbeRTT 主动把在途压到 4 包，这期间的低速样本同样不代表网络变慢（Linux 进入
  // ProbeRTT 时也会置 app_limited），否则 10 轮窗口里会混进几个很低的值
  if ((use_bbr_ && bbr_->InProbeRtt()) || (use_bbr3_ && bbr3_->InProbeRtt())) {
    sampler_.OnAppLimited(inflight_);
  }
}

void Transport::OnPacket(Bytes pkt, Time now) {
  Header h;
  if (!DecodeHeader(pkt, &h)) return;  // 长度/版本/CRC 任一不符，静默丢弃

  Decoded d;
  if (!DecodeFrames(Bytes(pkt.data() + kHeaderSize, h.payload_len), &d)) return;

  ++stats_.pkts_recv;

  if (h.type == PacketType::kData) {
    RecordRecv(h.pkt_num);
    bool recorded = true;
    for (u8 i = 0; i < d.stream_count; ++i) recorded = OnStream(d.streams[i]) && recorded;
    // 只 ACK 数据确实登记下来的包。以前登记失败（区间表满）时数据被丢、包却照样被确认，
    // 发送端认为已送达、永不重传，接收端的连续前缀从此卡死。深缓冲下 BBR Startup 一次
    // 冲出上千个空洞时必现（50 Mbps / RTT 30 ms / 8 BDP，与 CUBIC 共享瓶颈，交付停在 6.38 MB）。
    // 失败时把刚置上的接收位清掉，ACK 就不会覆盖这个包，发送端会判丢重传；重传填洞会合并
    // 区间、腾出空位，所以不会活锁。
    // 先 RecordRecv、失败再撤销，而不是成功后才 RecordRecv：后一种写法实测让接收路径
    // 慢约 5 ns/包（cpubench OnPacket 69.5 → 75.2 ns），失败分支极少走到，撤销几乎没有成本。
    if (!recorded) {
      ++stats_.rx_range_full;
      if (h.pkt_num >= recv_base_ && h.pkt_num < recv_base_ + kRecvBits) {
        ClearRecv(std::size_t(h.pkt_num - recv_base_));
      }
    }

    // ACK 触发：每 N 个包立即回，否则等合并窗口到期
    ++recv_since_ack_;
    if (!ack_pending_) {
      ack_pending_ = true;
      ack_due_ = now + cfg_.ack_delay_us;
    }
    if (recv_since_ack_ >= cfg_.ack_every_n) ack_due_ = now;
  }
  if (d.has_max_data && d.max_data > flow_max_) flow_max_ = d.max_data;
  if (d.has_ack) OnAck(d.ack, now);
}

void Transport::RecordRecv(u64 pn) {
  if (!has_recv_) {
    has_recv_ = true;
    std::fill(recv_bits_.begin(), recv_bits_.end(), 0);
    recv_base_ = pn > (kRecvBits - 1) ? pn - (kRecvBits - 1) : 0;
    recv_hi_ = pn;
    MarkRecv(std::size_t(pn - recv_base_));
    return;
  }
  if (pn < recv_base_) return;  // 过老：对端会重传，接收端按 offset 幂等去重
  if (pn >= recv_base_ + kRecvBits) {
    const u64 new_base = pn - (kRecvBits - 1);
    SlideRecvWindow(new_base - recv_base_);
    recv_base_ = new_base;
  }
  MarkRecv(std::size_t(pn - recv_base_));
  if (pn > recv_hi_) recv_hi_ = pn;
}

// 位图整体右移 shift 位（时间推进）。shift 通常只有个位数，走位运算；
// 大于一个字长时先按字搬移，避免逐位移 64 次。
void Transport::SlideRecvWindow(u64 shift) {
  const std::size_t words = recv_bits_.size();
  if (shift >= kRecvBits) {
    std::fill(recv_bits_.begin(), recv_bits_.end(), 0);
    return;
  }
  const std::size_t wshift = std::size_t(shift / 64);
  const unsigned bshift = unsigned(shift % 64);
  if (wshift) {
    for (std::size_t i = 0; i + wshift < words; ++i) recv_bits_[i] = recv_bits_[i + wshift];
    for (std::size_t i = words - wshift; i < words; ++i) recv_bits_[i] = 0;
  }
  if (bshift) {
    for (std::size_t i = 0; i + 1 < words; ++i) {
      recv_bits_[i] = (recv_bits_[i] >> bshift) | (recv_bits_[i + 1] << (64 - bshift));
    }
    recv_bits_[words - 1] >>= bshift;
  }
}

bool Transport::OnStream(const StreamFrame& f) {
  if (f.data.empty()) return true;
  if (f.sid >= cfg_.max_streams) {  // 先于 resize：sid 是对端可控的输入
    ++stats_.stream_limit;
    return true;  // 对端越界的帧永久丢弃：确认它，免得对端无限重传
  }
  if (f.sid >= in_.size()) in_.resize(f.sid + 1);  // 对端 sid 从 0 连续分配
  InStream& st = in_[f.sid];
  if (st.ring.empty()) st.ring.resize(std::size_t(recv_window_), 0);  // 一次性预分配

  const std::size_t cap = st.ring.size();  // 2 的幂
  const u64 end = f.offset + f.data.size();
  if (end <= st.consumed) return true;  // 整段都已消费过：纯重复包

  // 未消费量不能超过环形缓冲，否则会覆盖还没被取走的数据。
  // 正常情况下流控保证不会发生；真发生了说明对端越界，丢包并计数。
  if (end - st.consumed > cap) {
    ++stats_.rx_overflow;
    return false;  // 不确认：窗口腾出来之后对端的重传还能送达
  }

  // 裁掉已消费的前缀
  u64 off = f.offset;
  Bytes src = f.data;
  if (off < st.consumed) {
    const std::size_t skip = std::size_t(st.consumed - off);
    if (skip >= src.size()) return true;
    src = src.subspan(skip);
    off = st.consumed;
  }

  // 写环形缓冲：回绕时拆成两段
  const std::size_t pos = std::size_t(off & (cap - 1));
  const std::size_t first = std::min(src.size(), cap - pos);
  std::memcpy(st.ring.data() + pos, src.data(), first);
  if (first < src.size()) std::memcpy(st.ring.data(), src.data() + first, src.size() - first);

  if (end > st.highest_end) st.highest_end = end;

  // 数据已经写进环形缓冲，但区间没登记就等于没收到：返回 false 让这个包不被确认
  if (!st.ranges.Add(off, end)) return false;
  const u64 contiguous = st.ranges.PrefixEndFrom(st.consumed);
  if (contiguous > st.contiguous) st.contiguous = contiguous;
  return true;
}

std::size_t Transport::ReadableBytes(u32 sid) const {
  if (sid >= in_.size()) return 0;
  const InStream& st = in_[sid];
  return std::size_t(st.contiguous > st.consumed ? st.contiguous - st.consumed : 0);
}

Bytes Transport::Peek(u32 sid) {
  if (sid >= in_.size()) return {};
  InStream& st = in_[sid];
  if (st.ring.empty() || st.contiguous <= st.consumed) return {};
  const std::size_t cap = st.ring.size();
  const std::size_t pos = std::size_t(st.consumed & (cap - 1));
  std::size_t n = std::size_t(st.contiguous - st.consumed);
  if (n > cap - pos) n = cap - pos;  // 不跨回绕，调用方循环取
  return Bytes(st.ring.data() + pos, n);
}

void Transport::Consume(u32 sid, std::size_t n) {
  if (sid >= in_.size()) return;
  InStream& st = in_[sid];
  if (st.ring.empty()) return;
  const std::size_t avail = ReadableBytes(sid);
  if (n > avail) n = avail;
  if (n == 0) return;

  st.consumed += n;
  recv_consumed_ += n;
  stats_.bytes_recv = recv_consumed_;

  // 已消费的区间不再需要记录，区间表因此不会随传输时长增长
  st.ranges.TrimBefore(st.consumed);
  st.contiguous = st.ranges.PrefixEndFrom(st.consumed);
}

std::size_t Transport::RecvBufferSize(u32 sid) const {
  return sid >= in_.size() ? 0 : in_[sid].ring.size();
}

void Transport::OnAck(const AckFrame& a, Time now) {
  const u64 largest = AckLargest(a);
  // 窗口是否被用满，要看这批包被确认**之前**的在途量——确认之后在途必然变小
  const bool cwnd_limited = cc_.IsCwndLimited(inflight_);
  const u64 prior_inflight = inflight_;
  u64 acked_bytes = 0;
  Time newest_sent_t = 0;  // 本 ACK 确认的包里最晚发出的那个（BBR 判定退出恢复期）
  u64 max_acked_pn = 0;
  bool newly_acked = false;
  u64 sample = 0;

  // 只为"最大被确认包"采样。每个重传用新包号，所以样本不会像 TCP 那样有歧义。
  // 有了按包号索引的环形数组，这一步是 O(1)，不再是全表扫描。
  if (!has_largest_acked_ || largest > largest_acked_) {
    // 已被判丢（槽位释放但 pn 未被复用、也没被确认过）的包，确认到达时同样是有效的
    // RTT 样本。只认在途包会形成死锁：排队把 RTT 拉长到超过时间阈值后，所有包都在
    // ACK 到达前被判丢 → 永远拿不到新样本 → SRTT 冻结 → 阈值不放宽 → 重传也被判丢。
    // 深缓冲下 BBR Startup 冲出 ~100 ms 队列时实测必现（10 Mbps / RTT 20 ms）。
    const Sent& ls = Slot(largest);
    if (ls.pn == largest && !ls.acked && (ls.used || largest < next_pn_)) {
      sample = now > ls.t ? now - ls.t : 0;
    }
    largest_acked_ = largest;
    has_largest_acked_ = true;
  }

  // 按 ACK 给出的区间直接定位，成本 O(被确认的包数)。
  // 原实现要遍历整张在途表，在途 1024 包时单次 ACK 12.5 µs——这是大 BDP 下的天花板。
  const std::size_t nr = std::min<std::size_t>(a.count, kMaxAckRanges);
  for (std::size_t i = 0; i < nr; ++i) {
    u64 lo = a.lo[i];
    u64 hi = a.hi[i];
    if (next_pn_ == 0) break;
    if (hi >= next_pn_) hi = next_pn_ - 1;
    if (lo < oldest_pn_) lo = oldest_pn_;  // 低于游标的包早已释放
    for (u64 pn = lo; pn <= hi; ++pn) {
      Sent& s = Slot(pn);
      if (!s.used || s.pn != pn) continue;
      bool recorded = true;
      for (u32 k = 0; k < s.seg_count; ++k) {
        OutStream& st = out_[s.segs[k].sid];
        if (!st.acked.Add(s.segs[k].off, s.segs[k].off + s.segs[k].len)) {
          recorded = false;
        } else {
          // 已确认的连续前缀覆盖了整块就释放（通常一次比较就返回）。段的边界在重传时不变，
          // 所以前缀之下的任何段都已整段确认，不会再被读到
          st.data.ReleaseBefore(st.acked.PrefixEnd());
        }
      }
      if (!recorded) {
        // 确认区间表满了。绝不能"记不下就当已确认"——那一包已经从环形数组里消失，
        // 结果就是数据永久缺失、两端互等到超时（实测在 1 Gbps + 1% 丢包下必现）。
        // 保守做法：不释放这个包，立刻重新入队重传，让副本再去撞一次。
        ++stats_.ack_overflow;
        for (u32 k = 0; k < s.seg_count; ++k) {
          retransmit_.emplace_back(s.segs[k].sid, s.segs[k].off, s.segs[k].len);
        }
        Forget(pn);
        continue;
      }
      acked_bytes += s.bytes;
      if (pn > max_acked_pn) max_acked_pn = pn;
      if (s.t > newest_sent_t) newest_sent_t = s.t;
      // 速率采样：喂给采样器，用最晚发出的那个包定界
      sampler_.OnPacketAcked(s.LoadRate(), s.t, s.bytes, now);
      s.acked = true;
      newly_acked = true;
      Forget(pn);
    }
  }

  if (sample > 0) {
    if (!has_rtt_) first_rtt_t_ = now;
    UpdateRtt(sample);
  }
  // 有新包被确认，说明路径通了：退避清零（RFC 9002 §6.2.1）
  if (newly_acked) pto_count_ = 0;
  if (!use_bbr_ && !use_bbr3_) {
    // 一次 ACK 只调一次，避免逐包 pow()——10G 场景下这是实打实的 CPU 开销
    cc_.OnAck(acked_bytes, std::max(max_acked_pn, largest), now,
              has_rtt_ ? u64(srtt_us_) : 0, sample, cwnd_limited);

    DetectLoss(now);

    // 速率采样：ACK 处理和丢包检测都结束后完成本轮采样。
    // min_rtt 过滤用 delay_min（CubicCc 维护的全程最小 RTT），没有样本时传 0 跳过过滤。
    last_rate_sample_ = sampler_.Finish(
        cc_.DelayMinUs() < 1e17 ? Time(cc_.DelayMinUs()) : 0);
    return;
  }

  // BBR：顺序和 CUBIC 不同——先判丢再交给 CC。BBR 要在同一次调用里看到本 ACK 的
  // 确认量和丢失量（包守恒、ProbeBW 探测相的退出条件都依赖两者），而 OnCongestion
  // 只是登记恢复期，真正的窗口调整发生在随后的 OnAck 里。
  const u64 lost_bytes = DetectLoss(now);
  last_rate_sample_ = sampler_.Finish(use_bbr3_ ? bbr3_->MinRttUs() : bbr_->MinRttUs());

  BbrAck ba;
  ba.now = now;
  ba.acked = acked_bytes;
  ba.lost = lost_bytes;
  ba.prior_inflight = prior_inflight;
  ba.inflight = inflight_;
  ba.delivered = sampler_.delivered();
  ba.rtt_us = sample;
  ba.has_newest_acked = newly_acked;
  ba.newest_acked_sent_t = newest_sent_t;
  ba.rs = last_rate_sample_;
  // 窗口是否被用满：按 BBR 自己的 cwnd 判（上面的 cwnd_limited 是按 CUBIC 的窗口算的）
  ba.cwnd_limited = prior_inflight + kMaxStreamPayload >= Cwnd();
  if (use_bbr3_) {
    bbr3_->OnAck(ba);
  } else {
    bbr_->OnAck(ba);
  }
}

// 丢包检测：两条 RFC 9002 规则，都改成"只走近真正需要检查的包号"。
// 原实现对每个在途包做一遍判断，等于把 ACK 成本重新变回 O(在途包数)。
u64 Transport::DetectLoss(Time now) {
  if (!has_largest_acked_ || inflight_pkts_ == 0) return 0;
  u64 lost_bytes = 0;

  const double srtt = has_rtt_ ? srtt_us_ : 100000.0;
  const Time time_threshold = std::max<Time>(Time(srtt * 1.125), 2000);
  bool congested = false;
  // 持续拥塞候选：本次判丢的包里，首个 RTT 样本之后发出的最早 / 最晚那个
  bool pc_any = false;
  u64 pc_lo_pn = 0, pc_hi_pn = 0;
  Time pc_lo_t = 0, pc_hi_t = 0;

  const auto declare_lost = [&](u64 pn) {
    Sent& s = Slot(pn);
    const Time t = s.t;
    if (has_rtt_ && t >= first_rtt_t_) {
      if (!pc_any || pn < pc_lo_pn) { pc_lo_pn = pn; pc_lo_t = t; }
      if (!pc_any || pn > pc_hi_pn) { pc_hi_pn = pn; pc_hi_t = t; }
      pc_any = true;
    }
    lost_bytes += s.bytes;
    sampler_.OnPacketLost(s.bytes);
    // v2/v3 逐包判断探测是否越过 2% 丢包率（必须在 OnCongestion 之前，见 bbr3.h）
    if (use_bbr3_) {
      bbr3_->OnPacketLost(s.LoadRate(), s.bytes, sampler_.lost(), sampler_.delivered(), now);
    }
    const u32 n = s.seg_count;
    Seg segs[kMaxSegsPerPacket];
    for (u32 i = 0; i < n; ++i) segs[i] = s.segs[i];
    Forget(pn);
    for (u32 i = 0; i < n; ++i) {
      retransmit_.emplace_back(segs[i].sid, segs[i].off, segs[i].len);
    }
    ++stats_.lost;
    // 恢复期内的包丢失不构成新的拥塞事件：否则一次拥塞会被重复降窗，
    // cwnd 会以 RTT 为周期被连续砍成指数衰减。
    if (!CcInRecovery(t)) congested = true;
  };

  // 规则一：包阈值。pn + 3 <= largest_acked 仍未确认 → 判丢。
  // 用单调游标，每个包号一生只被检查一次，摊还 O(1)。
  if (largest_acked_ >= 3) {
    const u64 limit = std::min<u64>(largest_acked_ - 3, next_pn_ ? next_pn_ - 1 : 0);
    for (u64 pn = loss_scan_pn_; pn <= limit; ++pn) {
      const Sent& s = Slot(pn);
      if (s.used && s.pn == pn) declare_lost(pn);
      if (pn == limit) break;  // 防 pn 溢出成 0 的死循环
    }
    if (limit + 1 > loss_scan_pn_) loss_scan_pn_ = limit + 1;
  }

  // 规则二：时间阈值。包号顺序即发送顺序，所以只需从最老的往前看，
  // 一旦发现年龄未超阈值就停——正常无丢包时是 O(1)。
  while (inflight_pkts_ > 0) {
    AdvanceOldest();
    if (oldest_pn_ >= next_pn_) break;
    const Sent& s = Slot(oldest_pn_);
    if (!s.used || s.pn != oldest_pn_) {
      ++oldest_pn_;
      continue;
    }
    if (now < s.t || now - s.t <= time_threshold) break;
    declare_lost(oldest_pn_);
  }

  if (congested) {
    if (use_bbr3_) {
      bbr3_->OnCongestion(now);
    } else if (use_bbr_) {
      bbr_->OnCongestion(now);
    } else {
      cc_.OnCongestion(now);
    }
  }

  // 持续拥塞（RFC 9002 §7.6）：丢失的包横跨了 threshold × PTO，且中间没有任何包被确认，
  // 说明这段时间路径整体不通。这时一次 β 降窗不够，窗口要回到最小值重新慢启动。
  // 中间有没有被确认的包，按包号区间查 acked 标记；只有跨度已经够长时才扫，平时不花成本。
  if (pc_any && pc_hi_pn > pc_lo_pn &&
      pc_hi_t - pc_lo_t > Time(cfg_.persistent_congestion_threshold) * Pto()) {
    bool gap_acked = false;
    const u64 hi = std::min<u64>(pc_hi_pn, pc_lo_pn + sent_ring_.size());
    for (u64 pn = pc_lo_pn + 1; pn < hi; ++pn) {
      const Sent& s = Slot(pn);
      if (s.pn == pn && s.acked) {
        gap_acked = true;
        break;
      }
    }
    if (!gap_acked) {
      if (use_bbr3_) {
        bbr3_->OnPersistentCongestion(now);
      } else if (use_bbr_) {
        bbr_->OnPersistentCongestion(now);
      } else {
        cc_.OnPersistentCongestion(now);
      }
      ++stats_.persistent_congestion;
    }
  }
  return lost_bytes;
}

void Transport::OnPtoTimeout() {
  ++pto_count_;
  ++stats_.pto_fires;

  // 探测内容的优先级：已判丢待重传的数据 > 新数据 > 最老在途包的数据。
  // 前两种 SendData 本来就会发，这里只处理"什么都没得发"的尾部丢包：把最老的两个
  // 在途包的数据段排到重传队列最前面。原包仍留在在途表里——它可能只是 ACK 慢了，
  // 哪份先被确认都行，另一份出队时会按 acked 区间去重。
  if (retransmit_.empty() && !HasUnsentData()) {
    u32 picked = 0;
    for (u64 pn = oldest_pn_; pn < next_pn_ && picked < 2; ++pn) {
      if (!InUse(pn)) continue;
      const Sent& s = Slot(pn);
      for (u32 i = s.seg_count; i > 0; --i) {
        retransmit_.emplace_front(s.segs[i - 1].sid, s.segs[i - 1].off, s.segs[i - 1].len);
      }
      ++picked;
    }
  }
  probe_budget_ = 2;  // RFC 9002 §6.2.4：每次 PTO 最多两个探测包
}

bool Transport::HasUnsentData() const {
  for (const auto& st : out_) {
    if (st.next_off < st.data.end() && sent_high_water_ + 1 <= flow_max_) return true;
  }
  return false;
}

void Transport::UpdateRtt(Time sample_us) {
  const double s = double(sample_us);
  if (!has_rtt_) {
    srtt_us_ = s;
    rttvar_us_ = s / 2.0;
    has_rtt_ = true;
    return;
  }
  const double diff = srtt_us_ > s ? srtt_us_ - s : s - srtt_us_;
  rttvar_us_ = 0.75 * rttvar_us_ + 0.25 * diff;
  srtt_us_ = 0.875 * srtt_us_ + 0.125 * s;
}

Time Transport::Pto() const {
  // 还没有 RTT 样本时用保守初值 1 s（RFC 9002 的 kInitialRtt = 333 ms 推出来也是约 1 s）。
  // 早期版本用 90 ms，在 RTT=100 ms 的链路上每个包都会在 ACK 到达前超时。
  if (!has_rtt_) return 1'000'000;
  const double g = double(cfg_.timer_granularity_us);
  return Time(srtt_us_ + std::max(4.0 * rttvar_us_, g) + double(cfg_.ack_delay_us));
}

Time Transport::PtoBackoff() const {
  constexpr Time kMaxPto = 60'000'000;  // 退避上限 60 s，与 RFC 6298 的 RTO 上限一致
  const Time base = Pto();
  const u32 shift = std::min<u32>(pto_count_, 30);
  if (base > (kMaxPto >> shift)) return kMaxPto;
  return base << shift;
}

bool Transport::SendComplete() const {
  if (inflight_pkts_ != 0 || !retransmit_.empty()) return false;
  if (flow_blocked_) return false;
  for (const auto& st : out_) {
    if (st.next_off < st.data.end()) return false;
    if (st.acked.PrefixEnd() < st.data.end()) return false;
  }
  return true;
}

u64 Transport::StreamSentBytes(u32 sid) const {
  return sid >= out_.size() ? 0 : out_[sid].bytes_sent;
}

}  // namespace xport
