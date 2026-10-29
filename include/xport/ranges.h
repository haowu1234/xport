#pragma once
// 扁平区间表：按起点升序的定长数组，用来替代 std::map<u64,u64>。
//
// 为什么不用 map：区间合并是**每包都要做**的操作。std::map 每次插入要一次
// 堆分配 + 红黑树旋转 + 指针追逐，实测单包成本 1.4 µs 里有相当一部分花在这里。
// 而实际的区间数量极少——数据连续到达时永远只有 1 段，乱序时也只有几个空洞。
// 定长数组 + 线性搬移在 n ≤ 8 时比红黑树快一个数量级，且零分配。
#include <algorithm>
#include <cstddef>

#include "xport/types.h"

namespace xport {

class RangeSet {
 public:
  // 容量按"同时存在的空洞数"设。重传会在任意 offset 上产生孤立区间，
  // 高丢包下碎片会明显变多，所以给到 512 段（8 KB/流），并在溢出时**返回失败**，
  // 由调用方决定怎么办——绝不能默默丢弃，否则那一包的数据就永远补不回来了。
  static constexpr std::size_t kCap = 512;

  void Clear() {
    n_ = 0;
    dropped_ = 0;
  }

  bool Empty() const { return n_ == 0; }
  std::size_t size() const { return n_; }

  // 从 0 开始的连续覆盖长度。发送端用它判断"整条流已送达"。
  u64 PrefixEnd() const { return (n_ > 0 && s_[0] == 0) ? e_[0] : 0; }

  // 从 base 开始的连续覆盖长度。接收端消费掉前 base 字节后，用它算"还能读多少"。
  u64 PrefixEndFrom(u64 base) const {
    if (n_ == 0 || s_[0] > base) return base;
    return e_[0] > base ? e_[0] : base;
  }

  // 丢弃 base 之前的部分（应用已消费）。已消费的数据不再需要记录，
  // 这样区间表不会随传输时长无限增长。
  void TrimBefore(u64 base) {
    std::size_t drop = 0;
    while (drop < n_ && e_[drop] <= base) ++drop;
    if (drop > 0) {
      for (std::size_t k = 0; k + drop < n_; ++k) {
        s_[k] = s_[k + drop];
        e_[k] = e_[k + drop];
      }
      n_ -= drop;
    }
    if (n_ > 0 && s_[0] < base) s_[0] = base;
  }

  // [s, e) 是否被**单段**完整覆盖（不跨空洞）
  bool Contains(u64 s, u64 e) const {
    if (e <= s) return true;
    std::size_t i = 0;
    while (i < n_ && s_[i] <= s) ++i;
    if (i == 0) return false;
    --i;
    return s_[i] <= s && e <= e_[i];
  }

  // 插入并合并相邻/重叠区间。全部操作在定长数组内完成，无堆分配。
  // 返回 false 表示表已满、本次区间没有被记录下来。
  bool Add(u64 s, u64 e) {
    if (e <= s) return true;

    std::size_t i = 0;
    while (i < n_ && e_[i] < s) ++i;  // 第一段可能相接的区间

    std::size_t j = i;
    while (j < n_ && s_[j] <= e) {  // 向后吞并所有重叠/相邻段
      s = std::min(s, s_[j]);
      e = std::max(e, e_[j]);
      ++j;
    }

    if (j > i) {  // 有合并：写回第一段，删掉其余
      s_[i] = s;
      e_[i] = e;
      const std::size_t rm = j - i - 1;
      for (std::size_t k = i + 1; k + rm < n_; ++k) {
        s_[k] = s_[k + rm];
        e_[k] = e_[k + rm];
      }
      n_ -= rm;
      return true;
    }

    if (n_ == kCap) {  // 满了：报告失败，让调用方走保守路径（重传而不是当成已确认）
      ++dropped_;
      return false;
    }
    for (std::size_t k = n_; k > i; --k) {  // 腾位后插入
      s_[k] = s_[k - 1];
      e_[k] = e_[k - 1];
    }
    s_[i] = s;
    e_[i] = e;
    ++n_;
    return true;
  }

  u64 dropped() const { return dropped_; }

 private:
  u64 s_[kCap] = {};
  u64 e_[kCap] = {};
  std::size_t n_ = 0;
  u64 dropped_ = 0;
};

}  // namespace xport
