#pragma once
// 基础类型与常量。全部使用定宽整数，避免不同平台上协议布局漂移。
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xport {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using Bytes = std::span<const u8>;
using MutBytes = std::span<u8>;

// 虚拟微秒时钟。仿真器与实机共用同一类型，切换时只替换时钟源。
using Time = std::uint64_t;
constexpr Time kNever = ~Time{0};

constexpr std::size_t kMtu = 1400;
constexpr std::size_t kHeaderSize = 20;
constexpr std::size_t kMaxPayload = kMtu - kHeaderSize;

// 32 位短连接 ID。面向数据中心内网，不做 QUIC 那种 8~20 字节可变长。
constexpr u32 kDefaultCid = 0x58504F52;  // "XPOR"

struct Stats {
  u64 pkts_sent = 0;
  u64 pkts_recv = 0;
  u64 bytes_sent = 0;
  u64 bytes_recv = 0;
  u64 retransmits = 0;
  u64 spurious = 0;
  u64 lost = 0;
  u64 ack_sent = 0;
  u64 dropped_by_link = 0;
  u64 flow_blocked = 0;   // 因流控窗口被挡住的次数（每次进入阻塞记一次）
  u64 flow_probes = 0;    // 流控探测包数量（防 MAX_DATA 丢失导致双向等待）
  u64 ack_overflow = 0;   // 确认区间表溢出、改走重传的次数
  u64 rx_overflow = 0;    // 收到的数据越过接收窗口（对端越界，包被丢弃）
  u64 rto_fires = 0;      // RTO 触发次数（区别于"判定丢失"：这是真超时）
  u64 send_fail = 0;      // 本地 socket 发送失败（缓冲满/无路由），等价于链路丢包
  double srtt_ms = 0.0;
  double cwnd_kb = 0.0;
  double ssthresh_kb = 0.0;
  double rto_ms = 0.0;
  double pacing_mbps = 0.0;
};

}  // namespace xport
