#pragma once
#include "xport/types.h"

namespace xport {

// CRC32C（Castagnoli），软件查表实现。实机上可替换为 SSE4.2 / ARMv8 硬件指令。
u32 Crc32c(Bytes data, u32 seed = 0);

}  // namespace xport
