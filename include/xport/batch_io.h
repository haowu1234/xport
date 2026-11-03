#pragma once
// 批量收发的平台抽象。
//
// 为什么要这一层：回环实测每包 2.23 次 syscall、约 2.8 µs，协议自身只占 113 ns。
// 批量化能把 syscall 降到约 0.06 次/包（实测减少 96.9%），这是单流吞吐的主要杠杆。
// 但三个平台的批量接口完全不同：
//
//   Linux   sendmmsg / recvmmsg          标准接口，有 man page
//   macOS   sendmsg_x / recvmsg_x        xnu 私有：libSystem 导出符号、syscall.h 有号，
//                                        但 SDK 头文件既没有原型也没有 struct msghdr_x
//   Windows WSASendMsg + IOCP 预投       语义不同：一次调用取回 N 个"完成"，
//                                        每个报文背后仍是各自一次 WSARecvMsg
//
// 所以这里只承诺一件事：**一次调用处理一批报文**。不承诺"一次调用 = 一次 syscall"——
// 那是 Linux/macOS 的实现细节，Windows 对不上，按它设计上层就得为平台改代码。
//
// 另一个约束：能力必须**运行时**探测，不能只靠编译期宏。macOS 用的是私有 syscall，
// Apple 没有任何兼容性承诺，随时可能移除；探测失败就回退到逐包路径，不能直接崩。
#include <cstddef>

#include "xport/types.h"

namespace xport {
namespace batch {

// 一个报文槽。发送时读 [data, data+len)；接收时写入 [data, data+capacity)，len 为实收长度。
struct Slot {
  u8* data = nullptr;
  std::size_t len = 0;
  std::size_t capacity = 0;
};

// 当前平台的批量能力。首次调用时探测（会真的发一个包试），结果缓存。
struct Caps {
  bool send = false;         // 批量发可用
  bool recv = false;         // 批量收可用
  const char* backend = "";  // "sendmmsg" / "sendmsg_x" / "single"，用于如实报告
};

// 探测结果全局缓存一份：探测本身要建 socket、发包，不能每次收发都来一遍。
const Caps& Probe();

// 批量发送。返回成功发出的报文数；0 或负数表示本次调用失败，调用方应回退逐包路径。
// 与单包 send 一样，返回值小于 count 不是错误——剩下的留给下次。
int Send(long long fd, const Slot* slots, unsigned count);

// 批量接收。返回收到的报文数；0 表示暂无数据（非阻塞）；负数表示出错。
// would-block 归一化成 0，调用方不需要再判平台相关的 errno。
int Recv(long long fd, Slot* slots, unsigned count);

}  // namespace batch
}  // namespace xport
