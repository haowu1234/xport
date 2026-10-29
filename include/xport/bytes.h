#pragma once
// 大端读写 + 带边界检查的游标读写器。
// 所有解析入口都必须经由 Reader，禁止裸指针偏移——这样 fuzz 才有意义。
#include <cstring>

#include "xport/types.h"

namespace xport {

inline void WriteBE16(u8* p, u16 v) {
  p[0] = u8(v >> 8);
  p[1] = u8(v);
}
inline void WriteBE32(u8* p, u32 v) {
  p[0] = u8(v >> 24);
  p[1] = u8(v >> 16);
  p[2] = u8(v >> 8);
  p[3] = u8(v);
}
inline void WriteBE64(u8* p, u64 v) {
  for (int i = 7; i >= 0; --i) {
    p[i] = u8(v);
    v >>= 8;
  }
}
inline u16 ReadBE16(const u8* p) { return u16(u16(p[0]) << 8 | p[1]); }
inline u32 ReadBE32(const u8* p) {
  u32 v = 0;
  for (int i = 0; i < 4; ++i) v = (v << 8) | p[i];
  return v;
}
inline u64 ReadBE64(const u8* p) {
  u64 v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

class Writer {
 public:
  explicit Writer(MutBytes buf) : begin_(buf.data()), p_(buf.data()), end_(buf.data() + buf.size()) {}

  bool PutU8(u8 v) {
    if (p_ + 1 > end_) return false;
    *p_++ = v;
    return true;
  }
  bool PutU16(u16 v) {
    if (p_ + 2 > end_) return false;
    WriteBE16(p_, v);
    p_ += 2;
    return true;
  }
  bool PutU32(u32 v) {
    if (p_ + 4 > end_) return false;
    WriteBE32(p_, v);
    p_ += 4;
    return true;
  }
  bool PutU64(u64 v) {
    if (p_ + 8 > end_) return false;
    WriteBE64(p_, v);
    p_ += 8;
    return true;
  }
  bool PutVarInt(u64 v);
  bool PutBytes(Bytes b) {
    if (p_ + b.size() > end_) return false;
    if (!b.empty()) std::memcpy(p_, b.data(), b.size());
    p_ += b.size();
    return true;
  }

  std::size_t size() const { return std::size_t(p_ - begin_); }
  MutBytes written() const { return MutBytes(begin_, size()); }
  std::size_t remaining() const { return std::size_t(end_ - p_); }

 private:
  u8* begin_;
  u8* p_;
  u8* end_;
};

class Reader {
 public:
  explicit Reader(Bytes buf) : p_(buf.data()), end_(buf.data() + buf.size()) {}

  bool GetU8(u8* v) {
    if (p_ + 1 > end_) return false;
    *v = *p_++;
    return true;
  }
  bool GetU16(u16* v) {
    if (p_ + 2 > end_) return false;
    *v = ReadBE16(p_);
    p_ += 2;
    return true;
  }
  bool GetU32(u32* v) {
    if (p_ + 4 > end_) return false;
    *v = ReadBE32(p_);
    p_ += 4;
    return true;
  }
  bool GetU64(u64* v) {
    if (p_ + 8 > end_) return false;
    *v = ReadBE64(p_);
    p_ += 8;
    return true;
  }
  bool GetVarInt(u64* v);
  Bytes GetBytes(std::size_t n) {
    if (std::size_t(end_ - p_) < n) return {};
    Bytes out(p_, n);
    p_ += n;
    return out;
  }

  bool empty() const { return p_ >= end_; }
  std::size_t remaining() const { return std::size_t(end_ - p_); }

 private:
  const u8* p_;
  const u8* end_;
};

// ---- varint：首字节高 2 位编码长度（1/2/4/8 字节），与 QUIC 一致 ----

inline std::size_t VarIntLen(u64 v) {
  if (v < (1ull << 6)) return 1;
  if (v < (1ull << 14)) return 2;
  if (v < (1ull << 30)) return 4;
  return 8;
}

inline bool Writer::PutVarInt(u64 v) {
  if (v > 0x3FFFFFFFFFFFFFFFull) return false;  // 62 位上界，超出无法编码
  switch (VarIntLen(v)) {
    case 1:
      return PutU8(u8(v));
    case 2:
      return PutU16(u16(v) | 0x4000);
    case 4:
      return PutU32(u32(v) | 0x80000000u);
    default:
      return PutU64(v | 0xC000000000000000ull);
  }
}

inline bool Reader::GetVarInt(u64* v) {
  if (p_ >= end_) return false;
  const std::size_t n = std::size_t{1} << (*p_ >> 6);
  if (std::size_t(end_ - p_) < n) return false;
  u64 out = *p_ & 0x3F;
  for (std::size_t i = 1; i < n; ++i) out = (out << 8) | p_[i];
  p_ += n;
  *v = out;
  return true;
}

}  // namespace xport
