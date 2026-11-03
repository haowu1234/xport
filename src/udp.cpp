#include "xport/udp.h"

#include "xport/batch_io.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#if defined(__has_include)
#if __has_include(<mstcpip.h>)
#include <mstcpip.h>
#endif
#endif
// 老的/MinGW 头文件里可能没有这个 ioctl 码，按 Winsock 的宏定义补上
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace xport {

Time MonotonicUs() {
#ifdef _WIN32
  static const double kInv = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1e6 / double(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return Time(double(c.QuadPart) * kInv);
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return Time(ts.tv_sec) * 1'000'000 + Time(ts.tv_nsec) / 1000;
#endif
}

namespace {

#ifdef _WIN32
// Winsock 初始化必须是进程级一次性的。用函数内静态对象保证只跑一次，
// 也顺带保证在 DLL 卸载前完成清理。
bool WsaReady() {
  static const bool ok = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  return ok;
}
#endif

}  // namespace

UdpSocket::~UdpSocket() {
#ifdef _WIN32
  if (fd_ >= 0) closesocket((SOCKET)fd_);
#else
  if (fd_ >= 0) ::close(int(fd_));
#endif
}

std::unique_ptr<UdpSocket> UdpSocket::Open(const UdpConfig& cfg, std::string* err) {
  auto self = std::unique_ptr<UdpSocket>(new UdpSocket());
  self->batch_ = cfg.batch ? cfg.batch : 32;
  // 比 MTU 略大：真实链路上偶尔会有对方发的超大包（比如误配 jumbo），
  // 缓冲太小会被截断成"看似合法的短包"，那是最难查的一类问题。
  self->rxbuf_.resize(kMtu * 4);
  self->want_batch_ = cfg.use_batch;
  // 批量缓冲一次性分配好：热路径上只做 memcpy，不碰 malloc
  if (self->want_batch_ && batch::Probe().send) {
    self->txpool_.resize(kMtu * self->batch_);
    self->txlen_.resize(self->batch_, 0);
  }
  if (self->want_batch_ && batch::Probe().recv) {
    self->rxpool_.resize(kMtu * self->batch_);
  }

  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_port = htons(cfg.bind_port);
  if (inet_pton(AF_INET, cfg.bind_addr.c_str(), &local.sin_addr) != 1) {
    if (err) *err = "bind 地址非法: " + cfg.bind_addr;
    return nullptr;
  }

#ifdef _WIN32
  if (!WsaReady()) {
    if (err) *err = "WSAStartup 失败";
    return nullptr;
  }
  SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) {
    if (err) *err = "socket() 失败: " + std::to_string(WSAGetLastError());
    return nullptr;
  }
  self->fd_ = (long long)s;

  // UDP 本应无连接状态，但 Windows 默认把 ICMP 端口不可达报成 WSAECONNRESET，
  // 下一次 recv 就会失败。发往一个还没起来的对端时必然遇到，必须关掉。
  {
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
  }

  int val = cfg.rcvbuf_bytes;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&val, sizeof val);
  val = cfg.sndbuf_bytes;
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&val, sizeof val);
  {
    int got = 0, len = sizeof got;
    getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&got, &len);
    self->actual_rcvbuf_ = got;
    len = sizeof got;
    getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&got, &len);
    self->actual_sndbuf_ = got;
  }

  if (::bind(s, (sockaddr*)&local, sizeof local) == SOCKET_ERROR) {
    if (err) *err = "bind() 失败: " + std::to_string(WSAGetLastError());
    return nullptr;
  }
  {
    sockaddr_in got{};
    int len = sizeof got;
    if (getsockname(s, (sockaddr*)&got, &len) == 0) self->bound_port_ = ntohs(got.sin_port);
  }

  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
#else
  int s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) {
    if (err) *err = std::string("socket() 失败: ") + std::strerror(errno);
    return nullptr;
  }
  self->fd_ = s;

  int val = cfg.rcvbuf_bytes;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, &val, sizeof val);
  val = cfg.sndbuf_bytes;
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, &val, sizeof val);
  {
    int got = 0;
    socklen_t len = sizeof got;
    getsockopt(s, SOL_SOCKET, SO_RCVBUF, &got, &len);
    self->actual_rcvbuf_ = got;
    len = sizeof got;
    getsockopt(s, SOL_SOCKET, SO_SNDBUF, &got, &len);
    self->actual_sndbuf_ = got;
  }

  if (::bind(s, (sockaddr*)&local, sizeof local) != 0) {
    if (err) *err = std::string("bind() 失败: ") + std::strerror(errno);
    return nullptr;
  }
  {
    sockaddr_in got{};
    socklen_t len = sizeof got;
    if (getsockname(s, (sockaddr*)&got, &len) == 0) self->bound_port_ = ntohs(got.sin_port);
  }

  const int fl = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif

  if (cfg.peer_port != 0 && !self->Connect(cfg.peer_addr, cfg.peer_port, err)) return nullptr;
  return self;
}

bool UdpSocket::Connect(const std::string& addr, u16 port, std::string* err) {
  sockaddr_in peer{};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(port);
  if (inet_pton(AF_INET, addr.c_str(), &peer.sin_addr) != 1) {
    if (err) *err = "peer 地址非法: " + addr;
    return false;
  }
#ifdef _WIN32
  if (::connect((SOCKET)fd_, (sockaddr*)&peer, sizeof peer) == SOCKET_ERROR) {
    if (err) *err = "connect() 失败: " + std::to_string(WSAGetLastError());
    return false;
  }
#else
  if (::connect(int(fd_), (sockaddr*)&peer, sizeof peer) != 0) {
    if (err) *err = std::string("connect() 失败: ") + std::strerror(errno);
    return false;
  }
#endif
  return true;
}

bool UdpSocket::batch_enabled() const {
  return want_batch_ && batch::Probe().send && !txpool_.empty();
}

const char* UdpSocket::batch_backend() const {
  return (want_batch_ && batch::Probe().send) ? batch::Probe().backend : "single";
}

bool UdpSocket::SendQueued(Bytes pkt, Time now) {
  if (!batch_enabled() || pkt.size() > kMtu) return Send(pkt, now);

  std::memcpy(txpool_.data() + qcount_ * kMtu, pkt.data(), pkt.size());
  txlen_[qcount_] = pkt.size();
  ++qcount_;
  // 攒满就发。不等下一次 Flush，省掉调用方记状态的负担。
  if (qcount_ >= batch_) return Flush(now);
  return true;
}

bool UdpSocket::Flush(Time now) {
  if (qcount_ == 0) return true;
  if (!batch_enabled()) {  // 理论上进不来（SendQueued 已经分流），兜底防状态不一致
    qcount_ = 0;
    return true;
  }

  batch::Slot slots[64];
  const unsigned n = unsigned(qcount_ < 64 ? qcount_ : 64);
  for (unsigned i = 0; i < n; ++i) {
    slots[i].data = txpool_.data() + i * kMtu;
    slots[i].len = txlen_[i];
    slots[i].capacity = kMtu;
  }

  ++send_syscalls_;
  const int done = batch::Send(fd_, slots, n);
  if (done < 0) {
    // 批量失败就逐包补发：发送失败是正常的丢包源，但不能整批静默丢掉
    ++send_fail_;
    std::snprintf(err_, sizeof err_, "批量发送失败，已回退逐包");
    bool all = true;
    for (unsigned i = 0; i < n; ++i) {
      if (!Send(Bytes(txpool_.data() + i * kMtu, txlen_[i]), now)) all = false;
    }
    qcount_ = 0;
    return all;
  }

  sent_ += u64(done);
  // 内核只收了前 done 个：把剩下的往前挪，下次再发。
  // 不挪的话这些报文会被下一批覆盖，表现为随机丢包，极难查。
  const unsigned left = n - unsigned(done);
  if (left > 0) {
    std::memmove(txpool_.data(), txpool_.data() + unsigned(done) * kMtu, left * kMtu);
    for (unsigned i = 0; i < left; ++i) txlen_[i] = txlen_[unsigned(done) + i];
    send_fail_ += left;  // 没发出去的等价于本地丢弃，交给上层的丢包恢复
  }
  qcount_ = left;
  return done > 0;
}

bool UdpSocket::Send(Bytes pkt, Time /*now*/) {
  ++send_syscalls_;
#ifdef _WIN32
  const int n = ::send((SOCKET)fd_, (const char*)pkt.data(), (int)pkt.size(), 0);
  if (n == SOCKET_ERROR) {
    ++send_fail_;
    std::snprintf(err_, sizeof err_, "send() 失败: %d", WSAGetLastError());
    return false;
  }
#else
  const ssize_t n = ::send(int(fd_), pkt.data(), pkt.size(), 0);
  if (n < 0) {
    ++send_fail_;
    std::snprintf(err_, sizeof err_, "send() 失败: %s", std::strerror(errno));
    return false;
  }
#endif
  ++sent_;
  return true;
}

std::size_t UdpSocket::Recv(Batch* out, Time now) {
  std::size_t n = 0;
  ++poll_calls_;

  // 批量路径：一次 syscall 取回最多 batch_ 个报文。
  // 失败（含平台不支持）直接落到下面的逐包循环，不返回错误。
  if (want_batch_ && batch::Probe().recv && !rxpool_.empty()) {
    batch::Slot slots[64];
    const unsigned want = unsigned(batch_ < 64 ? batch_ : 64);
    for (unsigned i = 0; i < want; ++i) {
      slots[i].data = rxpool_.data() + i * kMtu;
      slots[i].len = 0;
      slots[i].capacity = kMtu;
    }
    ++recv_syscalls_;
    const int got = batch::Recv(fd_, slots, want);
    if (got > 0) {
      for (int i = 0; i < got; ++i) {
        if (slots[i].len == 0) continue;  // 0 长度报文对协议无意义
        if (out->size() < n + 1) out->emplace_back();
        (*out)[n].data.assign(slots[i].data, slots[i].data + slots[i].len);
        (*out)[n].at = now;
        ++n;
        ++recv_;
      }
      return n;  // 批量成功就不再走逐包，避免同一轮里混用两条路径
    }
    if (got == 0) {  // 非阻塞下的"暂无数据"，和逐包路径的 EWOULDBLOCK 等价
      ++empty_polls_;
      return 0;
    }
    // got < 0：批量出错，继续往下走逐包兜底
  }

  while (n < batch_) {
    int r = 0;
    ++recv_syscalls_;
#ifdef _WIN32
    r = ::recv((SOCKET)fd_, (char*)rxbuf_.data(), (int)rxbuf_.size(), 0);
    if (r < 0) {
      const int e = WSAGetLastError();
      if (e == WSAEWOULDBLOCK) break;
      if (e == WSAECONNRESET) continue;  // 已关 SIO_UDP_CONNRESET，这里只是兜底
      ++recv_dropped_;
      break;
    }
#else
    r = int(::recv(int(fd_), rxbuf_.data(), rxbuf_.size(), 0));
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      ++recv_dropped_;
      break;
    }
#endif
    if (r <= 0) break;  // 0 长度报文对协议无意义

    if (out->size() < n + 1) out->emplace_back();
    // assign 在容量足够时不重新分配，所以稳态下这条路径零 malloc
    (*out)[n].data.assign(rxbuf_.data(), rxbuf_.data() + r);
    (*out)[n].at = now;
    ++n;
    ++recv_;
  }
  // 最后那一次返回 WSAEWOULDBLOCK 的 recv 也是真金白银的 syscall，
  // 已经计入 recv_syscalls_——这正是"轮询比批量更贵"的量化来源。
  if (n == 0) ++empty_polls_;
  return n;
}

}  // namespace xport
