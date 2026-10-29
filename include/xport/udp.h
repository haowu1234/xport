#pragma once
// 真实 UDP socket 后端（Winsock / BSD socket 双平台）。
//
// 这里有意只做**单个 socket、单个线程、非阻塞轮询**：
//   * 单流永远落在一个核上（SO_REUSEPORT 按四元组哈希），多线程帮不上单流；
//   * 轮询（而不是阻塞/中断）才能测出协议栈本身的成本，不被唤醒延迟污染。
//
// 平台差异都收在本文件里，上层只看到 IoEndpoint：
//   * Windows 必须关掉 SIO_UDP_CONNRESET，否则 ICMP 端口不可达会变成
//     WSAECONNRESET，而 UDP 本应无连接状态；
//   * Windows 的 SO_RCVBUF 会被静默截断，必须回读实际值并如实报告；
//   * Linux 的 SO_RCVBUF 内核会翻倍（一份用于簿记），回读值才是真实可用量。
#include <memory>
#include <string>

#include "xport/io.h"

namespace xport {

struct UdpConfig {
  std::string bind_addr = "127.0.0.1";  // 只绑回环：不触发防火墙，也不过网卡
  u16 bind_port = 0;                    // 0 = 让系统分配
  std::string peer_addr = "127.0.0.1";
  u16 peer_port = 0;
  int rcvbuf_bytes = 4 << 20;
  int sndbuf_bytes = 4 << 20;
  std::size_t batch = 32;  // 一次 Recv 最多取多少个报文
};

class UdpSocket final : public IoEndpoint {
 public:
  ~UdpSocket() override;

  // 只 bind（peer_port 为 0 时不 connect）。回环测试需要先拿到两端的实际端口。
  static std::unique_ptr<UdpSocket> Open(const UdpConfig& cfg, std::string* err);
  // 绑定对端地址。UDP 的 connect 只是给内核记住五元组，不做握手、不产生报文，
  // 但能省掉每次 sendto 的地址解析，也顺带过滤掉其它来源的包。
  bool Connect(const std::string& addr, u16 port, std::string* err);

  bool Send(Bytes pkt, Time now) override;
  bool Send(Bytes pkt) { return Send(pkt, MonotonicUs()); }  // 便利重载
  std::size_t Recv(Batch* out, Time now) override;
  Time Now() override { return MonotonicUs(); }
  bool ok() const override { return fd_ >= 0; }
  const char* last_error() const override { return err_; }

  u16 bound_port() const { return bound_port_; }
  // 内核实际给到的缓冲（回读值），用于如实报告——申请值经常被截断
  int actual_rcvbuf() const { return actual_rcvbuf_; }
  int actual_sndbuf() const { return actual_sndbuf_; }

 private:
  UdpSocket() = default;

  long long fd_ = -1;
  u16 bound_port_ = 0;
  int actual_rcvbuf_ = 0;
  int actual_sndbuf_ = 0;
  std::size_t batch_ = 32;
  std::vector<u8> rxbuf_;  // 复用的单包接收缓冲
  char err_[128] = {};
};

}  // namespace xport
