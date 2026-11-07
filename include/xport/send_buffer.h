#pragma once
// 发送缓冲：按固定大小分块存放应用写入的字节，已确认的整块随时释放。
//
// 为什么不是一个 std::vector：
//   旧实现把整条流的历史都留在一个 vector 里，Write 只追加、从不截断，长连接内存会一直涨。
//   改成"截掉已确认前缀"也不行：前缀删除要把剩余数据整体前移，稳态下每发一个包就要搬
//   约一个包的字节（1368 B memmove ≈ 30–70 ns），和整条收发路径的开销同一个量级。
//   分块后释放是 pop_front，零搬移；块大小固定、按偏移对齐，定位是一次移位。
//
// 分段与块边界无关：一个 STREAM 段跨两块时，View 把它拷进调用方给的临时缓冲（每 64 KB
// 才发生一次），所以分包方式与旧实现逐字节一致——仿真结果不受影响。
#include <cstring>
#include <deque>
#include <memory>

#include "xport/types.h"

namespace xport {

class SendBuffer {
 public:
  static constexpr std::size_t kChunk = 64 * 1024;  // 2 的幂
  static constexpr std::size_t kShift = 16;
  static_assert(kChunk == std::size_t(1) << kShift, "kChunk must be 2^kShift");

  void Append(Bytes data) {
    const u8* p = data.data();
    std::size_t left = data.size();
    while (left > 0) {
      const u64 rel = end_ - base_;
      const std::size_t idx = std::size_t(rel >> kShift);
      const std::size_t pos = std::size_t(rel & (kChunk - 1));
      if (idx == chunks_.size()) {
        // 不清零：块里只有 [base, end) 范围内的字节会被读到
        chunks_.emplace_back(new u8[kChunk]);
      }
      const std::size_t n = left < kChunk - pos ? left : kChunk - pos;
      std::memcpy(chunks_[idx].get() + pos, p, n);
      p += n;
      left -= n;
      end_ += n;
    }
  }

  // 已写入的总字节数（流的逻辑末尾）
  u64 end() const { return end_; }
  // 仍保留在内存里的最低偏移（块对齐）。低于它的数据都已被确认并释放。
  u64 base() const { return base_; }

  // [off, off + len) 必须落在 [base, end) 内，且 len ≤ kChunk。
  // 不跨块时直接返回块内视图；跨块时拷进 scratch（至少 len 字节）再返回。
  Bytes View(u64 off, std::size_t len, u8* scratch) const {
    const u64 rel = off - base_;
    const std::size_t idx = std::size_t(rel >> kShift);
    const std::size_t pos = std::size_t(rel & (kChunk - 1));
    if (pos + len <= kChunk) return Bytes(chunks_[idx].get() + pos, len);
    const std::size_t first = kChunk - pos;
    std::memcpy(scratch, chunks_[idx].get() + pos, first);
    std::memcpy(scratch + first, chunks_[idx + 1].get(), len - first);
    return Bytes(scratch, len);
  }

  // 释放完全位于 off 之前的整块。off 通常是"已确认的连续前缀"。
  // 常见情况是一次比较就返回，可以放在每个 ACK 的热路径上。
  void ReleaseBefore(u64 off) {
    while (!chunks_.empty() && base_ + kChunk <= off) {
      chunks_.pop_front();
      base_ += kChunk;
    }
  }

  // 实际占用的内存（按块计）
  std::size_t ResidentBytes() const { return chunks_.size() * kChunk; }

 private:
  std::deque<std::unique_ptr<u8[]>> chunks_;
  u64 base_ = 0;  // chunks_[0][0] 对应的流偏移，始终是 kChunk 的整数倍
  u64 end_ = 0;
};

}  // namespace xport
