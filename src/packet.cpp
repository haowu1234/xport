#include "xport/packet.h"

#include "xport/crc32c.h"

namespace xport {
namespace {
constexpr std::size_t kCrcField = 18;  // CRC 覆盖前 18 字节
}

std::size_t EncodeHeader(const Header& h, MutBytes out) {
  if (out.size() < kHeaderSize) return 0;
  out[0] = kVersion;
  out[1] = u8(h.type);
  WriteBE16(out.data() + 2, h.flags);
  WriteBE32(out.data() + 4, h.dst_cid);
  WriteBE64(out.data() + 8, h.pkt_num);
  WriteBE16(out.data() + 16, h.payload_len);
  const u16 crc = u16(Crc32c(Bytes(out.data(), kCrcField)) & 0xFFFFu);
  WriteBE16(out.data() + kCrcField, crc);
  return kHeaderSize;
}

bool DecodeHeader(Bytes in, Header* h) {
  if (in.size() < kHeaderSize) return false;
  if (in[0] != kVersion) return false;

  const u16 want = ReadBE16(in.data() + kCrcField);
  const u16 got = u16(Crc32c(Bytes(in.data(), kCrcField)) & 0xFFFFu);
  if (want != got) return false;

  const u16 payload_len = ReadBE16(in.data() + 16);
  if (kHeaderSize + payload_len > in.size()) return false;

  h->type = static_cast<PacketType>(in[1]);
  h->flags = ReadBE16(in.data() + 2);
  h->dst_cid = ReadBE32(in.data() + 4);
  h->pkt_num = ReadBE64(in.data() + 8);
  h->payload_len = payload_len;
  h->crc = want;

  if (h->type != PacketType::kData && h->type != PacketType::kAck) return false;
  return true;
}

std::size_t EncodeAck(MutBytes out, const AckFrame& f) {
  Writer w(out);
  const std::size_t n = f.count < kMaxAckRanges ? f.count : kMaxAckRanges;
  if (!w.PutU8(u8(FrameType::kAck))) return 0;
  if (!w.PutU32(f.delay_us)) return 0;
  if (!w.PutU8(u8(n))) return 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (!w.PutVarInt(f.lo[i])) return 0;
    if (!w.PutVarInt(f.hi[i])) return 0;
  }
  return w.size();
}

std::size_t EncodeStream(MutBytes out, const StreamFrame& f) {
  Writer w(out);
  if (!w.PutU8(u8(FrameType::kStream))) return 0;
  if (!w.PutVarInt(f.sid)) return 0;
  if (!w.PutVarInt(f.offset)) return 0;
  if (!w.PutVarInt(f.data.size())) return 0;
  if (!w.PutU8(f.fin ? 1 : 0)) return 0;
  if (!w.PutBytes(f.data)) return 0;
  return w.size();
}

std::size_t EncodeMaxData(MutBytes out, u64 value) {
  Writer w(out);
  if (!w.PutU8(u8(FrameType::kMaxData))) return 0;
  if (!w.PutVarInt(value)) return 0;
  return w.size();
}

std::size_t EncodePing(MutBytes out) {
  Writer w(out);
  if (!w.PutU8(u8(FrameType::kPing))) return 0;
  return 1;
}

bool DecodeFrames(Bytes payload, Decoded* out) {
  Reader r(payload);
  // 只复位用到的字段：整体 Decoded{} 要清零约 1 KB，在 690 kpps 下是纯浪费
  out->has_ack = false;
  out->has_max_data = false;
  out->has_ping = false;
  out->stream_count = 0;

  while (!r.empty()) {
    u8 type = 0;
    if (!r.GetU8(&type)) return false;

    switch (static_cast<FrameType>(type)) {
      case FrameType::kPing:
        out->has_ping = true;
        break;

      case FrameType::kAck: {
        if (out->has_ack) return false;  // 一包一个 ACK 帧
        u8 n = 0;
        if (!r.GetU32(&out->ack.delay_us)) return false;
        if (!r.GetU8(&n)) return false;
        if (n > kMaxAckRanges) return false;
        for (u8 i = 0; i < n; ++i) {
          if (!r.GetVarInt(&out->ack.lo[i])) return false;
          if (!r.GetVarInt(&out->ack.hi[i])) return false;
          if (out->ack.lo[i] > out->ack.hi[i]) return false;
        }
        out->ack.count = n;
        out->has_ack = true;
        break;
      }

      case FrameType::kStream: {
        // 一个报文可以携带多个 STREAM 帧（帧聚合）。上限既防病态输入，
        // 也保证 Decoded 大小可控——超出上限的包直接判非法。
        if (out->stream_count >= kMaxStreamFrames) return false;
        u64 sid = 0, off = 0, len = 0;
        u8 fin = 0;
        if (!r.GetVarInt(&sid)) return false;
        if (!r.GetVarInt(&off)) return false;
        if (!r.GetVarInt(&len)) return false;
        if (!r.GetU8(&fin)) return false;
        if (len > r.remaining()) return false;  // 长度必须自洽
        StreamFrame& f = out->streams[out->stream_count++];
        f.sid = u32(sid);
        f.offset = off;
        f.fin = (fin != 0);
        f.data = r.GetBytes(std::size_t(len));
        break;
      }

      case FrameType::kMaxData:
        if (!r.GetVarInt(&out->max_data)) return false;
        out->has_max_data = true;
        break;

      default:
        return false;  // 未知帧类型一律视为非法包
    }
  }
  return true;
}

}  // namespace xport
