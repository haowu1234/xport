#pragma once
// IO 端点抽象。
//
// 设计上刻意按「**完成批量**」定义接口，而不是按 recvmmsg 的 syscall 语义：
//   * Linux 的 recvmmsg 一次 syscall 收 N 个报文；
//   * Windows 的 GetQueuedCompletionStatusEx 一次调用取 N 个完成，
//     但每个报文背后仍然各自一次 WSARecvMsg（IOCP 预投模型）。
// 如果按"一次调用 = 一次 syscall"来设计，两个平台的语义就对不上，
// 移植时上层必须重写——所以这里只承诺"一次调用拿回一批已到达的报文"。
//
// 另一个约束：Send 必须允许失败。真实 socket 会因发送缓冲满、路由缺失而失败，
// 这不是异常而是正常的丢包源，上层应当把它当作"链路上丢了"交给 RTO 处理。
#include <vector>

#include "xport/types.h"

namespace xport {

struct Datagram {
  std::vector<u8> data;
  Time at = 0;  // 到达时刻（微秒，单调时钟）；批量收包时同一批可共享同一时刻
};

// 复用的接收缓冲：避免每个报文一次分配。
using Batch = std::vector<Datagram>;

class IoEndpoint {
 public:
  virtual ~IoEndpoint() = default;

  // 发送一个已编码好的报文。返回 false 表示本地丢弃。
  // now 必须由调用方显式传入，不能让端点自己"记住当前时间"——
  // 仿真里时间是虚拟的、由驱动方推进，端点缓存一份就必然与协议看到的时间不一致
  // （这个错误的表现是所有包的时间戳都停在 0，RTO 立刻成批触发）。
  virtual bool Send(Bytes pkt, Time now) = 0;

  // 收取一批已到达的报文，返回条数；无数据时返回 0（**非阻塞**）。
  // out 会被复用：调用方通常预分配好容量，实现只覆盖 data 的内容。
  virtual std::size_t Recv(Batch* out, Time now) = 0;

  // 单调时钟（微秒）。仿真后端返回虚拟时间，真实后端返回硬件时钟。
  virtual Time Now() = 0;

  virtual bool ok() const = 0;
  virtual const char* last_error() const { return ""; }

  u64 sent() const { return sent_; }
  u64 recv() const { return recv_; }
  u64 send_fail() const { return send_fail_; }
  u64 recv_dropped() const { return recv_dropped_; }
  // 系统调用次数。这是真实后端最值钱的指标：每包成本几乎完全由它决定，
  // 而它又直接对应"单核能跑多少 pps"。批量收发的收益必须在这里体现。
  u64 send_syscalls() const { return send_syscalls_; }
  u64 recv_syscalls() const { return recv_syscalls_; }
  u64 poll_calls() const { return poll_calls_; }      // Recv 被调用的次数
  u64 empty_polls() const { return empty_polls_; }    // 其中一次都没收到的次数（空转）

  virtual void ResetCounters() {
    sent_ = recv_ = send_fail_ = recv_dropped_ = 0;
    send_syscalls_ = recv_syscalls_ = poll_calls_ = empty_polls_ = 0;
  }

 protected:
  u64 sent_ = 0;
  u64 recv_ = 0;
  u64 send_fail_ = 0;     // 本地发送失败（缓冲满/无路由），等价于链路丢包
  u64 recv_dropped_ = 0;  // 收到但没装进批量（缓冲区耗尽等）
  u64 send_syscalls_ = 0;
  u64 recv_syscalls_ = 0;
  u64 poll_calls_ = 0;
  u64 empty_polls_ = 0;
};

// 进程级单调时钟，微秒。Windows 用 QPC，POSIX 用 CLOCK_MONOTONIC。
Time MonotonicUs();

}  // namespace xport
