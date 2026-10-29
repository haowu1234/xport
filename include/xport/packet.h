#pragma once
// 报文结构与编解码。
//
// 设计要点（详见技术方案 §3）：
//  * 20 字节定长头，一条 cache line 内完成解析
//  * pkt_num 为 64 位定长，与 stream offset 严格分离：重传用新 pn，ACK 只认 pn
//  * 头部 CRC16（取自 CRC32C 低 16 位）用于丢弃链路损坏包
//  * 帧类型语义对齐 RFC 9000，但编码自研，不与标准 QUIC 互通
#include "xport/bytes.h"
#include "xport/types.h"

namespace xport {

constexpr u8 kVersion = 1;

enum class PacketType : u8 {
  kData = 0x01,
  kAck = 0x02,
};

enum class FrameType : u8 {
  kPing = 0x01,
  kAck = 0x02,
  kStream = 0x08,
  kMaxData = 0x10,
};

struct Header {
  PacketType type = PacketType::kData;
  u16 flags = 0;
  u32 dst_cid = kDefaultCid;
  u64 pkt_num = 0;
  u16 payload_len = 0;
  u16 crc = 0;
};

// 返回写入字节数（固定 20），缓冲区不足返回 0。
std::size_t EncodeHeader(const Header& h, MutBytes out);
// 校验长度、版本、CRC；任何一项不满足返回 false。
bool DecodeHeader(Bytes in, Header* h);

constexpr std::size_t kMaxAckRanges = 16;

// ACK 用显式区间描述"收到了哪些包号"，而不是 64 位位图。
// 原因：cwnd 很容易超过 64 个包，位图窗口外的包会被误判为丢失，引发无谓重传。
struct AckFrame {
  u32 delay_us = 0;              // 从收到到发出 ACK 的延迟，供对端算 PTO
  u8 count = 0;                  // 段数，≤ kMaxAckRanges，按包号降序
  u64 lo[kMaxAckRanges] = {};    // 每段的最低包号
  u64 hi[kMaxAckRanges] = {};    // 每段的最高包号
};

inline bool AckCovers(const AckFrame& a, u64 pn) {
  const std::size_t n = a.count < kMaxAckRanges ? a.count : kMaxAckRanges;
  for (std::size_t i = 0; i < n; ++i) {
    if (pn >= a.lo[i] && pn <= a.hi[i]) return true;
  }
  return false;
}

inline u64 AckLargest(const AckFrame& a) { return a.count == 0 ? 0 : a.hi[0]; }

struct StreamFrame {
  u32 sid = 0;
  u64 offset = 0;
  Bytes data{};
  bool fin = false;
};

std::size_t EncodeAck(MutBytes out, const AckFrame& f);
std::size_t EncodeStream(MutBytes out, const StreamFrame& f);
std::size_t EncodeMaxData(MutBytes out, u64 value);
std::size_t EncodePing(MutBytes out);

// 一个报文里可以装多个 STREAM 帧（帧聚合）。
// 这是小消息场景的关键优化：100 B 的 RPC 消息如果独占一个 1400 B 的包，
// 有效载荷只有 7%；聚合 12 条后 pps 直接降一个数量级，而字节数不变。
constexpr std::size_t kMaxStreamFrames = 16;

struct Decoded {
  bool has_ack = false;
  AckFrame ack;
  bool has_max_data = false;
  u64 max_data = 0;
  bool has_ping = false;
  u8 stream_count = 0;                       // 实际存在的 STREAM 帧个数
  StreamFrame streams[kMaxStreamFrames];     // 每个 data 都指向入参缓冲区，不拷贝
};

// 解析整个 payload。任何非法/截断/重复控制帧都返回 false——上层直接丢包。
//
// 注意：为了省掉每次调用都清零 1 KB 结构体的开销，这里只初始化用到的字段，
// 不整体 memset。调用方不要假设未使用的 streams 槽位是零值。
bool DecodeFrames(Bytes payload, Decoded* out);

}  // namespace xport
