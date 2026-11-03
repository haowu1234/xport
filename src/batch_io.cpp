// sendmmsg/recvmmsg 在 glibc 里要 _GNU_SOURCE 才有原型。必须在任何头文件之前定义。
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "xport/batch_io.h"

#include <cstring>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace xport {
namespace batch {
namespace {

// ============================ macOS ============================
#if defined(__APPLE__)

// xnu bsd/sys/socket.h 里的定义。SDK 没导出，必须照抄——字段顺序和大小要完全一致，
// 错一个字段就是内核读到垃圾指针。msg_datalen 是 _x 版本独有的：
// 发送时由调用方填该报文的总长度，接收时由内核回填实收长度。
struct msghdr_x {
  void* msg_name;
  socklen_t msg_namelen;
  struct iovec* msg_iov;
  int msg_iovlen;
  void* msg_control;
  socklen_t msg_controllen;
  int msg_flags;
  size_t msg_datalen;
};

extern "C" {
ssize_t sendmsg_x(int s, const struct msghdr_x* msgp, unsigned int cnt, int flags);
ssize_t recvmsg_x(int s, const struct msghdr_x* msgp, unsigned int cnt, int flags);
}

constexpr unsigned kMaxBatch = 64;

// 探测：建一对回环 socket，用批量接口真发一个包。
// 只检查符号存在是不够的——弱链接下符号可能解析成 nullptr，调用才会暴露。
bool ProbeOnce() {
  const int tx = ::socket(AF_INET, SOCK_DGRAM, 0);
  const int rx = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (tx < 0 || rx < 0) {
    if (tx >= 0) ::close(tx);
    if (rx >= 0) ::close(rx);
    return false;
  }

  bool ok = false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  socklen_t alen = sizeof a;
  if (::bind(rx, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0 &&
      ::getsockname(rx, reinterpret_cast<sockaddr*>(&a), &alen) == 0 &&
      ::connect(tx, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
    u8 payload[8] = {0};
    iovec iov{payload, sizeof payload};
    msghdr_x m{};
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_datalen = sizeof payload;
    ok = sendmsg_x(tx, &m, 1, 0) == 1;
    if (ok) {
      u8 drain[8];
      (void)::recv(rx, drain, sizeof drain, 0);  // 别把探测包留在队列里
    }
  }
  ::close(tx);
  ::close(rx);
  return ok;
}

int SendImpl(long long fd, const Slot* slots, unsigned count) {
  if (count > kMaxBatch) count = kMaxBatch;
  iovec iov[kMaxBatch];
  msghdr_x msg[kMaxBatch];
  std::memset(msg, 0, sizeof(msghdr_x) * count);
  for (unsigned i = 0; i < count; ++i) {
    iov[i].iov_base = slots[i].data;
    iov[i].iov_len = slots[i].len;
    msg[i].msg_iov = &iov[i];
    msg[i].msg_iovlen = 1;
    msg[i].msg_datalen = slots[i].len;
  }
  const ssize_t n = sendmsg_x(int(fd), msg, count, 0);
  if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  return int(n);
}

int RecvImpl(long long fd, Slot* slots, unsigned count) {
  if (count > kMaxBatch) count = kMaxBatch;
  iovec iov[kMaxBatch];
  msghdr_x msg[kMaxBatch];
  std::memset(msg, 0, sizeof(msghdr_x) * count);
  for (unsigned i = 0; i < count; ++i) {
    iov[i].iov_base = slots[i].data;
    iov[i].iov_len = slots[i].capacity;
    msg[i].msg_iov = &iov[i];
    msg[i].msg_iovlen = 1;
    msg[i].msg_datalen = slots[i].capacity;
  }
  const ssize_t n = recvmsg_x(int(fd), msg, count, 0);
  if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  for (ssize_t i = 0; i < n; ++i) slots[i].len = msg[i].msg_datalen;
  return int(n);
}

constexpr const char* kBackend = "sendmsg_x";

// ============================ Linux ============================
#elif defined(__linux__)

constexpr unsigned kMaxBatch = 64;

bool ProbeOnce() { return true; }  // sendmmsg 自 2.6.39 起就是标准接口

int SendImpl(long long fd, const Slot* slots, unsigned count) {
  if (count > kMaxBatch) count = kMaxBatch;
  iovec iov[kMaxBatch];
  mmsghdr msg[kMaxBatch];
  std::memset(msg, 0, sizeof(mmsghdr) * count);
  for (unsigned i = 0; i < count; ++i) {
    iov[i].iov_base = slots[i].data;
    iov[i].iov_len = slots[i].len;
    msg[i].msg_hdr.msg_iov = &iov[i];
    msg[i].msg_hdr.msg_iovlen = 1;
  }
  const int n = ::sendmmsg(int(fd), msg, count, 0);
  if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  return n;
}

int RecvImpl(long long fd, Slot* slots, unsigned count) {
  if (count > kMaxBatch) count = kMaxBatch;
  iovec iov[kMaxBatch];
  mmsghdr msg[kMaxBatch];
  std::memset(msg, 0, sizeof(mmsghdr) * count);
  for (unsigned i = 0; i < count; ++i) {
    iov[i].iov_base = slots[i].data;
    iov[i].iov_len = slots[i].capacity;
    msg[i].msg_hdr.msg_iov = &iov[i];
    msg[i].msg_hdr.msg_iovlen = 1;
  }
  // MSG_DONTWAIT：socket 本身已是非阻塞，但显式带上更稳妥（也便于以后复用在阻塞 fd 上）
  const int n = ::recvmmsg(int(fd), msg, count, MSG_DONTWAIT, nullptr);
  if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
  for (int i = 0; i < n; ++i) slots[i].len = msg[i].msg_len;
  return n;
}

constexpr const char* kBackend = "recvmmsg";

// ============================ 其它平台 ============================
#else

// Windows 的 IOCP 预投模型和这里的同步批量语义差别较大（要维护预投缓冲池和完成队列），
// 单独实现才划算。先回退逐包，接口已经留好。
bool ProbeOnce() { return false; }
int SendImpl(long long, const Slot*, unsigned) { return -1; }
int RecvImpl(long long, Slot*, unsigned) { return -1; }
constexpr const char* kBackend = "single";

#endif

}  // namespace

const Caps& Probe() {
  // 函数内 static 的初始化由标准保证线程安全（C++11 起），且只跑一次
  static const Caps caps = [] {
    Caps c;
    const bool ok = ProbeOnce();
    c.send = ok;
    c.recv = ok;
    c.backend = ok ? kBackend : "single";
    return c;
  }();
  return caps;
}

int Send(long long fd, const Slot* slots, unsigned count) {
  if (count == 0) return 0;
  if (!Probe().send) return -1;
  return SendImpl(fd, slots, count);
}

int Recv(long long fd, Slot* slots, unsigned count) {
  if (count == 0) return 0;
  if (!Probe().recv) return -1;
  return RecvImpl(fd, slots, count);
}

}  // namespace batch
}  // namespace xport
