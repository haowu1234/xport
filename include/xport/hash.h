#pragma once
// 极简增量哈希：用来在"不保存全部数据"的前提下校验传输正确性。
//
// 流式消费之后，接收端不能再保留全量数据做 memcmp——那正是这次要删掉的东西。
// 维持一个滚动哈希，收一段算一段，既能校验又不需要额外内存。
#include "xport/types.h"

namespace xport {

inline u64 Fnv1a(Bytes data) {
  u64 h = 1469598103934665603ull;
  for (u8 b : data) {
    h ^= b;
    h *= 1099511628211ull;
  }
  return h;
}

inline u64 Fnv1aUpdate(u64 h, Bytes data) {
  for (u8 b : data) {
    h ^= b;
    h *= 1099511628211ull;
  }
  return h;
}

constexpr u64 kFnvInit = 1469598103934665603ull;

}  // namespace xport
