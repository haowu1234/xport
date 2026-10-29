#include "xport/crc32c.h"

#include <array>

namespace xport {
namespace {

constexpr u32 kPoly = 0x82F63B78u;  // 反射形式的 Castagnoli 多项式

struct Table {
  std::array<u32, 256> v{};
  constexpr Table() {
    for (u32 i = 0; i < 256; ++i) {
      u32 c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1u) ? ((c >> 1) ^ kPoly) : (c >> 1);
      v[i] = c;
    }
  }
};

constexpr Table kTable{};

}  // namespace

u32 Crc32c(Bytes data, u32 seed) {
  u32 c = seed ^ 0xFFFFFFFFu;
  for (u8 byte : data) c = (c >> 8) ^ kTable.v[(c ^ byte) & 0xFFu];
  return c ^ 0xFFFFFFFFu;
}

}  // namespace xport
