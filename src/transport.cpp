#include "xport/transport.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xport {
namespace {

// 一个满帧留给 STREAM 帧头的余量（type + sid + offset + len + fin 的最大编码长度）
constexpr std::size_t kMaxStreamPayload = kMaxPayload - 32;
// 聚合时保守估计的帧头开销，用来决定"这个包还能不能再塞一段"
constexpr std::size_t kFrameOverhead = 24;

}  // namespace

Transport::Transport(SimLink& link, u32 self, u32 peer, const TransportConfig& cfg)
    : owned_io_(std::make_unique<SimEndpoint>(link, self, peer)),
      io_(*owned_io_),
      self_(self),
      peer_(peer),
      cfg_(cfg),
      cc_(cfg.cc),
      flow_max_(cfg.initial_max_data) {
  InitBuffers();
}

Transport::Transport(IoEndpoint& io, const TransportConfig& cfg)
    : owned_io_(nullptr),
      io_(io),
      cfg_(cfg),
      cc_(cfg.cc),
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

  sent_ring_.resize(kSentRing);
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
  auto& buf = out_[sid].data;
  buf.insert(buf.end(), data.begin(), data.end());
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
  // 1) RTO：最老的未确认包超时，重传它的全部数据段（不做逐包定时器）
  AdvanceOldest();
  if (oldest_pn_ < next_pn_ && InUse(oldest_pn_)) {
    const Sent& s = Slot(oldest_pn_);
    if (now >= s.t && now - s.t > Rto()) {
      const u64 pn = oldest_pn_;
      const Time sent_t = s.t;
      const u32 n = s.seg_count;
      Seg segs[kMaxSegsPerPacket];
      for (u32 i = 0; i < n; ++i) segs[i] = s.segs[i];
#ifdef XPORT_DEBUG
      std::fprintf(stderr, "[rto] t=%llums pn=%llu segs=%u age=%.1fms rto=%.1fms cwnd=%.1fKB\n",
                   (unsigned long long)(now / 1000), (unsigned long long)pn, n,
                   double(now - sent_t) / 1000.0, double(Rto()) / 1000.0,
                   double(cc_.cwnd()) / 1024.0);
#endif
      Forget(pn);
      for (u32 i = 0; i < n; ++i) {
        retransmit_.emplace_back(segs[i].sid, segs[i].off, segs[i].len);
      }
      ++stats_.lost;
      ++stats_.rto_fires;
      if (!cc_.InRecovery(sent_t)) cc_.OnCongestion(now);
    }
  }

  // 2) ACK 合并窗口到期
  if (ack_pending_ && now >= ack_due_) SendAck(now);

  // 3) 流控死锁探测：被流控挡住、且没有任何在途包可触发对端回 ACK 时，
  //    只能主动发 PING 把对方"敲醒"，让对方带着新的 MAX_DATA 回 ACK。
  if (flow_blocked_ && inflight_pkts_ == 0 && now >= flow_blocked_since_ &&
      now - flow_blocked_since_ >= Rto()) {
    SendProbe(now);
    flow_blocked_since_ = now;
    ++stats_.flow_probes;
  }

  // 4) 发送
  SendData(now);

  stats_.srtt_ms = srtt_us_ / 1000.0;
  stats_.cwnd_kb = double(cc_.cwnd()) / 1024.0;
  stats_.ssthresh_kb = double(cc_.ssthresh()) / 1024.0;
  stats_.rto_ms = double(Rto()) / 1000.0;
  stats_.pacing_mbps =
      double(cc_.PacingRateBps(has_rtt_ ? u64(srtt_us_) : 1000)) * 8.0 / 1e6;
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
  io_.Send(Bytes(buf, hn + n), now);
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
  io_.Send(Bytes(buf, hn + n), now);
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
  // 发送失败不是异常，而是又一个丢包源：登记在途，交给 RTO 重传。
  // 但必须计数——静默的本地丢弃和链路丢包在现象上一模一样，没有计数器就查不出来。
  if (!io_.Send(Bytes(buf, hn + plen), now)) ++stats_.send_fail;

  Sent& s = Slot(h.pkt_num);
  s.pn = h.pkt_num;
  s.t = now;
  s.bytes = bytes;
  s.seg_count = u8(nseg);
  s.used = true;
  for (u32 i = 0; i < nseg; ++i) s.segs[i] = segs[i];

  ++inflight_pkts_;
  inflight_ += bytes;
  ++stats_.pkts_sent;
  stats_.bytes_sent += bytes;
}

void Transport::SendStreamPacket(u32 sid, u64 off, u32 len, Time now, bool retransmit) {
  if (sid >= out_.size()) return;
  OutStream& st = out_[sid];
  if (off + len > st.data.size()) return;

  StreamFrame f;
  f.sid = sid;
  f.offset = off;
  f.data = Bytes(st.data.data() + off, len);
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
  const double burst = double(cc_.PacingBurstBytes());

  if (!pace_init_) {
    pace_init_ = true;
    last_pace_ = now;
    tokens_ = burst;  // 冷启动给一个突发额度
    return;
  }
  if (now <= last_pace_) return;

  const double rate = double(cc_.PacingRateBps(has_rtt_ ? u64(srtt_us_) : 1000));
  const double dt = double(now - last_pace_) / 1e6;
  tokens_ = std::min(tokens_ + rate * dt, burst);
  last_pace_ = now;
}

void Transport::SendData(Time now) {
  RefillPacer(now);

  const u64 cwnd = cc_.cwnd();

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

  const auto flush = [&]() {
    if (nseg == 0) return;
    EmitPacket(segs, nseg, batch_bytes, payload, plen, now);
    nseg = 0;
    plen = 0;
    batch_bytes = 0;
  };

  for (int guard = 0; guard < 8192; ++guard) {
    // 拥塞窗口：留一个满包的余量
    if (inflight_ + batch_bytes + kMaxStreamPayload > cwnd) break;
    if (inflight_pkts_ >= kSentRing) break;  // 环形数组满，必须先等 ACK

    // 1) 重传优先：恢复期若被新数据挤占窗口，丢包会长时间补不上
    if (!retransmit_.empty()) {
      flush();
      auto [rsid, roff, rlen] = retransmit_.front();
      const bool invalid = rsid >= out_.size() || roff + rlen > out_[rsid].data.size();
      const bool already = !invalid && out_[rsid].acked.Contains(roff, roff + rlen);
      if (invalid || already) {
        if (already) ++stats_.spurious;
        retransmit_.pop_front();
        continue;
      }
      if (tokens_ < double(rlen)) break;
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
      if (st.next_off >= st.data.size()) continue;

      const u64 remain = st.data.size() - st.next_off;
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
          if (st.next_off >= st.data.size() && st.acked.PrefixEnd() < st.data.size()) {
            std::fprintf(stderr,
                         "[stall] t=%llums sid=%zu next=%llu acked_prefix=%llu size=%llu "
                         "segments=%zu dropped=%llu next_pn=%llu\n",
                         (unsigned long long)(now / 1000), i,
                         (unsigned long long)st.next_off,
                         (unsigned long long)st.acked.PrefixEnd(),
                         (unsigned long long)st.data.size(), st.acked.size(),
                         (unsigned long long)st.acked.dropped(), (unsigned long long)next_pn_);
          }
        }
      }
#endif
      break;
    }

    if (tokens_ < double(len)) break;  // 令牌不足：等下一轮 Tick，不逐包定时
    tokens_ -= double(len);

    // 4) 编进当前包。装得下就继续拼下一段，装不下就下一轮 flush。
    OutStream& st = out_[sid];
    StreamFrame f;
    f.sid = sid;
    f.offset = off;
    f.data = Bytes(st.data.data() + off, len);
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
}

void Transport::OnPacket(Bytes pkt, Time now) {
  Header h;
  if (!DecodeHeader(pkt, &h)) return;  // 长度/版本/CRC 任一不符，静默丢弃

  Decoded d;
  if (!DecodeFrames(Bytes(pkt.data() + kHeaderSize, h.payload_len), &d)) return;

  ++stats_.pkts_recv;

  if (h.type == PacketType::kData) {
    RecordRecv(h.pkt_num);
    for (u8 i = 0; i < d.stream_count; ++i) OnStream(d.streams[i]);

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

void Transport::OnStream(const StreamFrame& f) {
  if (f.data.empty()) return;
  if (f.sid >= in_.size()) in_.resize(f.sid + 1);  // 对端 sid 从 0 连续分配
  InStream& st = in_[f.sid];
  if (st.ring.empty()) st.ring.resize(std::size_t(recv_window_), 0);  // 一次性预分配

  const std::size_t cap = st.ring.size();  // 2 的幂
  const u64 end = f.offset + f.data.size();
  if (end <= st.consumed) return;  // 整段都已消费过：纯重复包

  // 未消费量不能超过环形缓冲，否则会覆盖还没被取走的数据。
  // 正常情况下流控保证不会发生；真发生了说明对端越界，丢包并计数。
  if (end - st.consumed > cap) {
    ++stats_.rx_overflow;
    return;
  }

  // 裁掉已消费的前缀
  u64 off = f.offset;
  Bytes src = f.data;
  if (off < st.consumed) {
    const std::size_t skip = std::size_t(st.consumed - off);
    if (skip >= src.size()) return;
    src = src.subspan(skip);
    off = st.consumed;
  }

  // 写环形缓冲：回绕时拆成两段
  const std::size_t pos = std::size_t(off & (cap - 1));
  const std::size_t first = std::min(src.size(), cap - pos);
  std::memcpy(st.ring.data() + pos, src.data(), first);
  if (first < src.size()) std::memcpy(st.ring.data(), src.data() + first, src.size() - first);

  if (end > st.highest_end) st.highest_end = end;

  st.ranges.Add(off, end);
  const u64 contiguous = st.ranges.PrefixEndFrom(st.consumed);
  if (contiguous > st.contiguous) st.contiguous = contiguous;
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
  u64 acked_bytes = 0;
  u64 max_acked_pn = 0;
  u64 sample = 0;

  // 只为"最大被确认包"采样。每个重传用新包号，所以样本不会像 TCP 那样有歧义。
  // 有了按包号索引的环形数组，这一步是 O(1)，不再是全表扫描。
  if (!has_largest_acked_ || largest > largest_acked_) {
    if (InUse(largest)) sample = now - Slot(largest).t;
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
        if (!st.acked.Add(s.segs[k].off, s.segs[k].off + s.segs[k].len)) recorded = false;
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
      Forget(pn);
    }
  }

  if (sample > 0) UpdateRtt(sample);
  // 一次 ACK 只调一次，避免逐包 pow()——10G 场景下这是实打实的 CPU 开销
  cc_.OnAck(acked_bytes, std::max(max_acked_pn, largest), now,
            has_rtt_ ? u64(srtt_us_) : 0, sample);

  DetectLoss(now);
}

// 丢包检测：两条 RFC 9002 规则，都改成"只走近真正需要检查的包号"。
// 原实现对每个在途包做一遍判断，等于把 ACK 成本重新变回 O(在途包数)。
void Transport::DetectLoss(Time now) {
  if (!has_largest_acked_ || inflight_pkts_ == 0) return;

  const double srtt = has_rtt_ ? srtt_us_ : 100000.0;
  const Time time_threshold = std::max<Time>(Time(srtt * 1.125), 2000);
  bool congested = false;

  const auto declare_lost = [&](u64 pn) {
    Sent& s = Slot(pn);
    const Time t = s.t;
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
    if (!cc_.InRecovery(t)) congested = true;
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

  if (congested) cc_.OnCongestion(now);
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

Time Transport::Rto() const {
  // 还没有 RTT 样本时必须用保守的初始值（RFC 6298：1 s）。
  // 早期版本用 90 ms，在 RTT=100 ms 的链路上每个包都会在 ACK 到达前被判超时，
  // 于是每个 RTT 降一次窗，cwnd 三次后就塌到最小值——链路完全跑不动。
  if (!has_rtt_) return 1'000'000;

  // G（时钟粒度）取 4 ms：仿真与真实内核的定时器粒度都远大于 1 ms，
  // 用 1 ms 会让 RTO 贴着实测 RTT，排队抖动一来就误判超时。
  const double rto = srtt_us_ + std::max(4.0 * rttvar_us_, 4000.0) + double(cfg_.ack_delay_us);
  return std::max<Time>(Time(rto), cfg_.min_rto_us);
}

bool Transport::SendComplete() const {
  if (inflight_pkts_ != 0 || !retransmit_.empty()) return false;
  if (flow_blocked_) return false;
  for (const auto& st : out_) {
    if (st.next_off < st.data.size()) return false;
    if (st.acked.PrefixEnd() < st.data.size()) return false;
  }
  return true;
}

u64 Transport::StreamSentBytes(u32 sid) const {
  return sid >= out_.size() ? 0 : out_[sid].bytes_sent;
}

}  // namespace xport
