// 真实 socket 回环基准。
//
// 目的：把"仿真里的数字"换成"真机上的数字"，并且明确测出**天花板在哪里**——
// 所以除了协议本身，还测两项基线：
//   1. 裸 UDP 单包成本（不做任何协议处理，分 1 深/32 深两种在途深度）
//   2. 回环 RTT（64 B ping-pong）→ 时间尺度的参照物
//
// 设计取舍（都会影响结论，必须写清楚）：
//   * 单线程忙轮询：发送端和接收端在同一个线程里，单核要同时干两份活。
//     这比"两端各占一核"悲观，但换来了零调度噪声和完全可复现的时序。
//   * 不用 SO_REUSEPORT、不做多线程：单流永远落在一个核上，加核帮不上忙。
//   * 校验用分块不变的 64 位混合哈希，而不是逐字节 FNV——后者在 1.4 KB 包上
//     要 1400 ns，和接收路径本身同量级，测出来的就不是协议性能了。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "xport/sim.h"
#include "xport/transport.h"
#include "xport/udp.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

using namespace xport;

namespace {

// ---- 进程 CPU 时间（毫秒）。粒度受系统时钟中断限制（Windows 约 15.6 ms），
// 所以耗时短于 30 ms 的运行不要看这个数字。
double CpuMs() {
#ifdef _WIN32
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
  ULARGE_INTEGER kk, uu;
  kk.LowPart = k.dwLowDateTime;
  kk.HighPart = k.dwHighDateTime;
  uu.LowPart = u.dwLowDateTime;
  uu.HighPart = u.dwHighDateTime;
  return double(kk.QuadPart + uu.QuadPart) / 10000.0;
#else
  timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1e6;
#endif
}

inline u64 MixWord(u64 h, u64 w) {
  h ^= w * 0xff51afd7ed558ccdull;
  h = (h << 27) | (h >> 37);
  return h * 0x9E3779B97F4A7C15ull + 0x165667B19E3779F9ull;
}

// 分块不变的滚动哈希。
//
// 第一版直接把每段数据按 8 字节切块喂进去——**分块边界一换，结果就变**。
// 接收端每次 Peek 拿到的段长取决于包边界，和发送端"整块算一次"必然不同，
// 于是数据完全正确却报校验失败。这类 bug 只在"分段长度与预期不一致"时暴露。
// 正确做法是用一个最多 7 字节的尾巴，把不足 8 字节的部分带到下一次。
struct Hash {
  u64 h = 0x9E3779B97F4A7C15ull;
  u8 tail[8] = {};
  int tail_n = 0;

  void Update(Bytes b) {
    std::size_t i = 0;
    if (tail_n > 0) {
      while (tail_n < 8 && i < b.size()) tail[tail_n++] = b[i++];
      if (tail_n < 8) return;
      u64 w = 0;
      std::memcpy(&w, tail, 8);
      h = MixWord(h, w);
      tail_n = 0;
    }
    for (; i + 8 <= b.size(); i += 8) {
      u64 w = 0;
      std::memcpy(&w, b.data() + i, 8);
      h = MixWord(h, w);
    }
    while (i < b.size()) tail[tail_n++] = b[i++];
  }

  u64 Final() const {
    u64 hh = h;
    if (tail_n > 0) {
      u64 w = 0;
      std::memcpy(&w, tail, std::size_t(tail_n));
      hh = MixWord(hh, w ^ (u64(tail_n) << 56));
    }
    return hh ^ (hh >> 29);
  }
};

std::vector<u8> MakeData(std::size_t n, u32 seed) {
  std::vector<u8> v(n);
  u64 s = u64(seed) * 0x9E3779B97F4A7C15ull + 1;
  for (std::size_t i = 0; i < n; ++i) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    v[i] = u8(s);
  }
  return v;
}

const char* kLocal = "127.0.0.1";

std::unique_ptr<UdpSocket> OpenSocket(int buf, std::string* err) {
  UdpConfig c;
  c.bind_addr = kLocal;
  c.bind_port = 0;
  c.peer_port = 0;
  c.rcvbuf_bytes = buf;
  c.sndbuf_bytes = buf;
  return UdpSocket::Open(c, err);
}

// ---- 丢包代理：在回环上制造真实丢包 ----
// Windows 没有 tc netem，而"重传路径在真实 socket 上能不能工作"必须验证。
// 用两个 socket 做一次转发，按概率丢包。转发本身要加 2 次 syscall/包，
// 所以这组数字只能和"同条件开代理"的对照组比，不能直接和直连比。
class Relay {
 public:
  bool Open(u16 tx_port, u16 rx_port, double loss_pct, u64 seed, std::string* err) {
    loss_ = loss_pct;
    rng_ = Rng(seed);
    sa_ = OpenSocket(4 << 20, err);  // 面向发送端
    if (!sa_) return false;
    sb_ = OpenSocket(4 << 20, err);  // 面向接收端
    if (!sb_) return false;
    if (!sa_->Connect(kLocal, tx_port, err)) return false;
    if (!sb_->Connect(kLocal, rx_port, err)) return false;
    return true;
  }
  u16 tx_side_port() const { return sa_->bound_port(); }
  u16 rx_side_port() const { return sb_->bound_port(); }

  void Pump(Time now) {
    PumpOne(sa_.get(), sb_.get(), now);
    PumpOne(sb_.get(), sa_.get(), now);
  }

  u64 dropped() const { return dropped_; }

 private:
  void PumpOne(UdpSocket* from, UdpSocket* to, Time now) {
    const std::size_t n = from->Recv(&batch_, now);
    for (std::size_t i = 0; i < n; ++i) {
      if (loss_ > 0 && rng_.NextUnit() * 100.0 < loss_) {
        ++dropped_;
        continue;
      }
      to->Send(Bytes(batch_[i].data), now);
    }
  }

  std::unique_ptr<UdpSocket> sa_, sb_;
  Batch batch_;
  double loss_ = 0;
  Rng rng_{1};
  u64 dropped_ = 0;
};

struct RunOpts {
  double mb = 8.0;
  int streams = 1;
  u64 window = 4ull << 20;
  u32 ack_delay_us = 500;
  bool coalesce = true;
  bool verify = true;
  double limit_s = 60.0;
  const char* trace = nullptr;
  std::string tag;
  // 小消息模式：每轮每流写 msg 字节，共 rounds 轮，轮间留 gap_us。
  // 这才是 RPC 的真实形态——大块数据里帧聚合没有意义，小消息才有。
  std::size_t msg = 0;
  int rounds = 0;
  u32 gap_us = 500;
  double relay_loss = 0.0;
};

struct Result {
  std::string tag;
  double mb = 0;
  int streams = 0;
  bool coalesce = true;
  double wall_ms = 0;
  double cpu_ms = 0;
  double mbps = 0;
  double pps = 0;
  u64 sent = 0, acks = 0, retr = 0, lost = 0, rto = 0, sfail = 0, rxdrop = 0;
  u64 send_sys = 0, recv_sys = 0;
  u64 relay_drop = 0;
  double srtt_ms = 0;
  double cwnd_kb = 0;      // 全程均值（末次采样毫无意义：可能正好落在一次丢包之后）
  double cwnd_min_kb = 0;
  double inflight_avg_kb = 0;
  bool rpc = false;
  bool ok = false;
  bool complete = false;
  double mbps_min = 0;     // 多次运行的最小/最大吞吐：极差本身也是结论
  double mbps_max = 0;
  std::string err;
};

Result RunXport(const RunOpts& o) {
  Result r;
  r.tag = o.tag;
  r.mb = o.mb;
  r.streams = o.streams;
  r.coalesce = o.coalesce;

  std::string err;
  auto tx_sock = OpenSocket(8 << 20, &err);
  if (!tx_sock) {
    r.err = "打开 socket A 失败: " + err;
    return r;
  }
  auto rx_sock = OpenSocket(8 << 20, &err);
  if (!rx_sock) {
    r.err = "打开 socket B 失败: " + err;
    return r;
  }

  Relay relay;
  const bool use_relay = o.relay_loss > 0;
  if (use_relay) {
    // 两端先 bind 好拿到端口，再让代理分别连过去，最后两端连代理
    if (!relay.Open(tx_sock->bound_port(), rx_sock->bound_port(), o.relay_loss, 0x5EED, &err)) {
      r.err = "启动丢包代理失败: " + err;
      return r;
    }
    if (!tx_sock->Connect(kLocal, relay.tx_side_port(), &err) ||
        !rx_sock->Connect(kLocal, relay.rx_side_port(), &err)) {
      r.err = "connect 失败: " + err;
      return r;
    }
  } else {
    if (!tx_sock->Connect(kLocal, rx_sock->bound_port(), &err) ||
        !rx_sock->Connect(kLocal, tx_sock->bound_port(), &err)) {
      r.err = "connect 失败: " + err;
      return r;
    }
  }

  TransportConfig tc;
  tc.initial_max_data = o.window;
  tc.recv_window_bytes = o.window;
  tc.ack_delay_us = o.ack_delay_us;
  tc.coalesce = o.coalesce;

  Transport tx(*tx_sock, tc);
  Transport rx(*rx_sock, tc);

  std::vector<std::vector<u8>> payloads;
  std::vector<u32> sids;
  std::vector<Hash> hashes;
  std::vector<Hash> expect;
  std::vector<std::size_t> counts;

  if (o.msg > 0) {
    const std::vector<u8> block = MakeData(o.msg, 7);
    for (int k = 0; k < o.streams; ++k) {
      payloads.push_back(block);  // 每轮写同一块，校验时重复计入
      sids.push_back(tx.OpenStream());
      hashes.emplace_back();
      expect.emplace_back();
      counts.push_back(0);
    }
  } else {
    const std::size_t total = std::size_t(o.mb * 1024.0 * 1024.0);
    const std::size_t share = total / std::size_t(o.streams);
    for (int k = 0; k < o.streams; ++k) {
      // 余数全给最后一条流，保证 Σ = total（否则"完成度"判定会差几个字节）
      const std::size_t n =
          (k + 1 == o.streams) ? (total - share * std::size_t(o.streams - 1)) : share;
      payloads.push_back(MakeData(n, u32(k + 1)));
      sids.push_back(tx.OpenStream());
      hashes.emplace_back();
      expect.emplace_back();
      expect.back().Update(Bytes(payloads.back()));
      counts.push_back(0);
    }
    for (int k = 0; k < o.streams; ++k) {
      tx.Write(sids[std::size_t(k)], Bytes(payloads[std::size_t(k)]));
    }
  }

  std::FILE* tr = nullptr;
  if (o.trace) {
    tr = std::fopen(o.trace, "w");
    if (tr) {
      std::fprintf(tr, "t_ms,cwnd_kb,inflight_kb,srtt_ms,in_flight_pkts,retrans,lost,delivered_kb\n");
    }
  }

  Batch ba, bb;
  ba.reserve(64);
  bb.reserve(64);

  const Time t0 = MonotonicUs();
  const double cpu0 = CpuMs();
  const Time limit = Time(o.limit_s * 1e6);
  Time next_sample = 0;
  Time now = 0;
  double cwnd_sum = 0, inflight_sum = 0;
  double cwnd_min = 1e18;
  u64 samples = 0;

  const auto step = [&]() {
    now = MonotonicUs() - t0;
    if (use_relay) relay.Pump(now);

    tx.Tick(now);
    rx.Tick(now);

    const std::size_t n1 = tx_sock->Recv(&ba, now);
    for (std::size_t i = 0; i < n1; ++i) tx.OnPacket(Bytes(ba[i].data), now);
    const std::size_t n2 = rx_sock->Recv(&bb, now);
    for (std::size_t i = 0; i < n2; ++i) rx.OnPacket(Bytes(bb[i].data), now);

    // 消费式接收：不消费，流控窗口就不前进，传输会停在中途
    for (int k = 0; k < o.streams; ++k) {
      for (;;) {
        Bytes v = rx.Peek(sids[std::size_t(k)]);
        if (v.empty()) break;
        if (o.verify) hashes[std::size_t(k)].Update(v);
        counts[std::size_t(k)] += v.size();
        rx.Consume(sids[std::size_t(k)], v.size());
      }
    }

    if (now >= next_sample) {
      const Stats& st = tx.stats();
      const double cw = double(tx.Cwnd()) / 1024.0;
      cwnd_sum += cw;
      inflight_sum += double(tx.InFlight()) / 1024.0;
      if (cw < cwnd_min) cwnd_min = cw;
      ++samples;
      if (tr) std::fprintf(tr, "%.3f,%.1f,%.1f,%.3f,%zu,%llu,%llu,%.1f\n", double(now) / 1000.0,
                   double(tx.Cwnd()) / 1024.0, double(tx.InFlight()) / 1024.0,
                   double(tx.SrttUs()) / 1000.0, tx.InFlightPackets(),
                   (unsigned long long)st.retransmits, (unsigned long long)st.lost,
                   double(rx.DeliveredBytes()) / 1024.0);
      next_sample = now + 1000;  // 1 ms 采样
    }
  };

  if (o.msg > 0) {
    for (int round = 0; round < o.rounds; ++round) {
      for (int k = 0; k < o.streams; ++k) {
        tx.Write(sids[std::size_t(k)], Bytes(payloads[std::size_t(k)]));
        if (o.verify) expect[std::size_t(k)].Update(Bytes(payloads[std::size_t(k)]));
      }
      const Time until = MonotonicUs() - t0 + o.gap_us;
      while (MonotonicUs() - t0 < until) step();
      if (MonotonicUs() - t0 > limit) break;
    }
    while (!tx.SendComplete() && MonotonicUs() - t0 < limit) step();
  } else {
    while (!tx.SendComplete()) {
      step();
      if (now >= limit) break;
    }
  }

  const Time wall = MonotonicUs() - t0;
  const double cpu = CpuMs() - cpu0;
  if (tr) std::fclose(tr);

  r.wall_ms = double(wall) / 1000.0;
  r.cpu_ms = cpu;

  std::size_t delivered = 0;
  for (std::size_t c : counts) delivered += c;
  r.mbps = wall > 0 ? double(delivered) * 8.0 / double(wall) : 0.0;

  const Stats& st = tx.stats();
  const Stats& rs = rx.stats();
  r.sent = st.pkts_sent;
  r.acks = rs.ack_sent;
  r.retr = st.retransmits;
  r.lost = st.lost;
  r.rto = st.rto_fires;
  r.sfail = st.send_fail;
  r.rxdrop = rs.rx_overflow;
  r.pps = wall > 0 ? double(st.pkts_sent) * 1e6 / double(wall) : 0.0;
  r.srtt_ms = double(tx.SrttUs()) / 1000.0;
  r.cwnd_kb = samples ? cwnd_sum / double(samples) : 0.0;
  r.cwnd_min_kb = samples ? cwnd_min : 0.0;
  r.inflight_avg_kb = samples ? inflight_sum / double(samples) : 0.0;
  r.rpc = o.msg > 0;
  r.send_sys = tx_sock->send_syscalls() + rx_sock->send_syscalls();
  r.recv_sys = tx_sock->recv_syscalls() + rx_sock->recv_syscalls();
  r.relay_drop = relay.dropped();

  bool all_arrived = true;
  bool hash_ok = true;
  for (int k = 0; k < o.streams; ++k) {
    const std::size_t want =
        o.msg > 0 ? std::size_t(o.rounds) * o.msg : payloads[std::size_t(k)].size();
    if (counts[std::size_t(k)] != want) all_arrived = false;
    if (o.verify && hashes[std::size_t(k)].Final() != expect[std::size_t(k)].Final()) {
      hash_ok = false;
    }
  }
  r.ok = o.verify ? hash_ok : true;
  r.complete = all_arrived && tx.SendComplete();
  return r;
}

struct RawStats {
  double ns_pair = 0, mbps = 0, pps = 0;            // 1 包在途：延迟受限
  double pipe_ns = 0, pipe_mbps = 0, pipe_pps = 0;  // 32 包在途：吞吐受限
  double rtt_us = 0;
  bool ok = false;
  std::string err;
};

RawStats RunRaw(int iters, int rtt_iters) {
  RawStats rs;
  std::string err;
  auto sa = OpenSocket(8 << 20, &err);
  if (!sa) {
    rs.err = err;
    return rs;
  }
  auto sb = OpenSocket(8 << 20, &err);
  if (!sb) {
    rs.err = err;
    return rs;
  }
  if (!sa->Connect(kLocal, sb->bound_port(), &err) || !sb->Connect(kLocal, sa->bound_port(), &err)) {
    rs.err = err;
    return rs;
  }

  std::vector<u8> pkt(1368, 0xA5);
  Batch b;
  b.reserve(64);

  for (int i = 0; i < 3000; ++i) {  // 预热：路由、缓冲、分支预测进稳态
    sa->Send(Bytes(pkt));
    while (sb->Recv(&b, 0) == 0) {
    }
  }

  const Time t0 = MonotonicUs();
  int done = 0;
  for (int i = 0; i < iters; ++i) {
    if (!sa->Send(Bytes(pkt))) continue;
    while (sb->Recv(&b, 0) == 0) {
    }
    ++done;
  }
  const Time wall1 = MonotonicUs() - t0;
  if (done == 0) {
    rs.err = "raw 基准没有完成任何一次收发";
    return rs;
  }
  rs.ns_pair = double(wall1) * 1000.0 / double(done);
  rs.mbps = double(pkt.size()) * 8.0 * 1000.0 / rs.ns_pair;  // bits/ns × 1000 = Mbps
  rs.pps = 1e9 / rs.ns_pair;

  // 32 包在途：延迟被流水线掩盖，这才是吞吐天花板。
  // 拿 1 深的结果当"内核上限"会严重低估，于是协议看起来"只跑到上限的 40%"。
  // 跑三次取中位数：这个数字决定后面所有百分比，单次会漂 20% 以上。
  constexpr int kDepth = 32;
  {
    std::vector<double> ns;
    for (int rep = 0; rep < 3; ++rep) {
      int sent_n = 0, got_n = 0;
      const Time s = MonotonicUs();
      while (sent_n < iters) {
        while (sent_n - got_n < kDepth && sent_n < iters) {
          sa->Send(Bytes(pkt));
          ++sent_n;
        }
        got_n += int(sb->Recv(&b, 0));
      }
      const Time w = MonotonicUs() - s;
      if (got_n > 0) ns.push_back(double(w) * 1000.0 / double(got_n));
    }
    if (!ns.empty()) {
      std::sort(ns.begin(), ns.end());
      rs.pipe_ns = ns[ns.size() / 2];
      rs.pipe_mbps = double(pkt.size()) * 8.0 * 1000.0 / rs.pipe_ns;
      rs.pipe_pps = 1e9 / rs.pipe_ns;
    }
  }

  std::vector<u8> tiny(64, 0x11);
  std::vector<double> samples;
  samples.reserve(std::size_t(rtt_iters));
  for (int i = 0; i < rtt_iters; ++i) {
    const Time s = MonotonicUs();
    if (!sa->Send(Bytes(tiny))) continue;
    while (sb->Recv(&b, 0) == 0) {
    }
    samples.push_back(double(MonotonicUs() - s));
  }
  if (!samples.empty()) {
    std::sort(samples.begin(), samples.end());
    rs.rtt_us = samples[samples.size() / 2];
  }
  rs.ok = true;
  return rs;
}

// 同一配置跑多次取中位数：单次运行的极差能到 15%，直接报一次等于报噪声。
Result RunMedian(const RunOpts& o, int times = 3) {
  std::vector<Result> rs;
  for (int i = 0; i < times; ++i) rs.push_back(RunXport(o));
  std::sort(rs.begin(), rs.end(),
            [](const Result& a, const Result& b) { return a.mbps < b.mbps; });
  Result mid = rs[rs.size() / 2];
  mid.mbps_min = rs.front().mbps;
  mid.mbps_max = rs.back().mbps;
  return mid;
}

void PrintHeader() {
  std::printf("%-12s %6s %4s %9s %9s %8s %17s %6s %4s %7s %5s %s\n", "场景", "MB", "流", "耗时ms",
              "吞吐Mbps", "包/秒", "吞吐范围Mbps", "重传", "RTO", "SRTTms", "校验", "说明");
  for (int i = 0; i < 138; ++i) std::putchar('-');
  std::putchar('\n');
}

void PrintRow(const Result& r) {
  char range[32] = "-";
  if (r.mbps_max > 0) {
    std::snprintf(range, sizeof range, "%.0f~%.0f", r.mbps_min, r.mbps_max);
  }
  std::printf("%-12s %6.2f %4d %9.2f %9.1f %8.0f %17s %6llu %4llu %7.3f %5s %s\n", r.tag.c_str(),
              r.mb, r.streams, r.wall_ms, r.mbps, r.pps, range, (unsigned long long)r.retr,
              (unsigned long long)r.rto, r.srtt_ms, r.ok ? "OK" : "FAIL",
              !r.complete ? "未跑完" : (r.coalesce ? "" : "无聚合"));
  if (!r.err.empty()) std::printf("   !! %s\n", r.err.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  RunOpts base;
  bool sweep = false;
  std::string csv;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--mb" && i + 1 < argc) base.mb = std::atof(argv[++i]);
    else if (a == "--streams" && i + 1 < argc) base.streams = std::atoi(argv[++i]);
    else if (a == "--window" && i + 1 < argc) base.window = u64(std::atof(argv[++i]) * 1048576.0);
    else if (a == "--no-coalesce") base.coalesce = false;
    else if (a == "--noverify") base.verify = false;
    else if (a == "--sweep") sweep = true;
    else if (a == "--limit" && i + 1 < argc) base.limit_s = std::atof(argv[++i]);
    else if (a == "--trace" && i + 1 < argc) base.trace = argv[++i];
    else if (a == "--csv" && i + 1 < argc) csv = argv[++i];
    else if (a == "--msg" && i + 1 < argc) base.msg = std::size_t(std::atoi(argv[++i]));
    else if (a == "--rounds" && i + 1 < argc) base.rounds = std::atoi(argv[++i]);
    else if (a == "--gap" && i + 1 < argc) base.gap_us = u32(std::atoi(argv[++i]));
    else if (a == "--relay-loss" && i + 1 < argc) base.relay_loss = std::atof(argv[++i]);
    else if (a == "--help") {
      std::printf(
          "用法: xport_udp_bench [选项]\n"
          "  --mb N           传输数据量（MB），默认 8\n"
          "  --streams K      并发流数，默认 1\n"
          "  --window MB      流控/接收窗口，默认 4\n"
          "  --no-coalesce    关闭帧聚合（A/B 对照）\n"
          "  --noverify       不做数据校验（测纯吞吐）\n"
          "  --sweep          跑完整扫描矩阵\n"
          "  --msg N          小消息模式：每轮每流写 N 字节\n"
          "  --rounds R       小消息轮数\n"
          "  --gap US         轮间隔（微秒），默认 500\n"
          "  --relay-loss PCT 用进程内转发代理注入丢包（百分数）\n"
          "  --limit S        单次超时（秒），默认 60\n"
          "  --trace FILE     导出 cwnd/RTT 轨迹 CSV\n"
          "  --csv FILE       导出结果 CSV\n");
      return 0;
    }
  }

  std::printf("=== 真实 socket 回环基准（127.0.0.1，单线程忙轮询）===\n\n");
  std::printf("[基线] 裸 UDP（无协议处理）\n");
  const RawStats raw = RunRaw(30000, 5000);
  if (!raw.ok) {
    std::printf("  raw 基准失败: %s\n", raw.err.c_str());
  } else {
    std::printf("  1 包在途（延迟受限）：%6.0f ns/包 → %7.1f Mbps（%6.0f 包/秒）\n", raw.ns_pair,
                raw.mbps, raw.pps);
    std::printf("  32 包在途（吞吐受限）：%5.0f ns/包 → %7.1f Mbps（%6.0f 包/秒）  ← 天花板\n",
                raw.pipe_ns, raw.pipe_mbps, raw.pipe_pps);
    std::printf("  64 B 回环 RTT 中位数：%.1f µs\n", raw.rtt_us);
  }
  std::printf("\n");

  std::vector<Result> all;

  if (sweep) {
    std::printf("=== A. 数据量扫描 | 单流 | 窗口 4 MB ===\n");
    PrintHeader();
    for (double m : {0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0}) {
      RunOpts o = base;
      o.mb = m;
      o.streams = 1;
      o.tag = "单流";
      Result r = RunMedian(o);
      PrintRow(r);
      all.push_back(r);
    }

    std::printf("\n=== B. 并发流扫描 | 16 MB ===\n");
    PrintHeader();
    for (int k : {1, 2, 4, 8}) {
      RunOpts o = base;
      o.mb = 16.0;
      o.streams = k;
      o.tag = "多流";
      Result r = RunMedian(o);
      PrintRow(r);
      all.push_back(r);
    }

    std::printf("\n=== C. 小消息 RPC（200 B × 8 流 × 200 轮，轮间 500 µs）帧聚合 A/B ===\n");
    PrintHeader();
    for (bool co : {true, false}) {
      RunOpts o = base;
      o.msg = 200;
      o.rounds = 200;
      o.streams = 8;
      o.coalesce = co;
      o.mb = double(200.0 * 200.0 * 8.0) / 1048576.0;
      o.tag = co ? "聚合开" : "聚合关";
      Result r = RunMedian(o);
      PrintRow(r);
      all.push_back(r);
    }

    std::printf("\n=== D. 真实丢包（进程内转发代理注入，4 MB 单流）===\n");
    PrintHeader();
    for (double loss : {0.0, 0.5, 1.0, 2.0}) {
      RunOpts o = base;
      o.mb = 4.0;
      o.streams = 1;
      if (loss == 0.0) {
        o.relay_loss = 0.0;  // 直连对照组
        o.tag = "直连";
      } else {
        o.relay_loss = loss;
        char buf[32];
        std::snprintf(buf, sizeof buf, "丢包%.1f%%", loss);
        o.tag = buf;
      }
      Result r = RunMedian(o);
      PrintRow(r);
      all.push_back(r);
    }
    {
      // 代理 0% 丢包的对照：把"代理自身的开销"和"丢包的代价"分开，
      // 否则会误以为 1% 丢包让吞吐掉了 40%，其实是多了一跳。
      RunOpts o = base;
      o.mb = 4.0;
      o.streams = 1;
      o.relay_loss = 1e-9;  // 概率为 0，但仍走代理
      o.tag = "代理0%丢包";
      Result r = RunMedian(o);
      PrintRow(r);
      all.push_back(r);
    }
  } else {
    std::printf("=== 单次运行 ===\n");
    PrintHeader();
    RunOpts o = base;
    o.tag = base.msg > 0 ? "小消息" : "单次";
    if (o.msg > 0 && o.rounds == 0) o.rounds = 200;
    if (o.msg > 0) o.mb = double(o.msg) * double(o.rounds) * o.streams / 1048576.0;
    Result r = RunXport(o);
    PrintRow(r);
    all.push_back(r);
    if (!r.err.empty()) return 1;
  }

  // ---- 系统调用解剖 ----
  std::printf("\n=== 系统调用解剖（解释为什么达不到裸 UDP 上限）===\n");
  std::printf("%-12s %10s %10s %10s %12s\n", "场景", "数据包", "ACK包", "syscall", "syscall/包");
  for (int i = 0; i < 60; ++i) std::putchar('-');
  std::putchar('\n');
  for (const auto& r : all) {
    const u64 datagrams = r.sent + r.acks;
    const u64 sys = r.send_sys + r.recv_sys;
    const double per = datagrams > 0 ? double(sys) / double(datagrams) : 0.0;
    if (r.rpc) {
      // 小消息场景每轮之间有 500 µs 空档，空档里的轮询也会产生 syscall，
      // 这个比值不反映协议开销，只反映"人为注入的空转"。
      std::printf("%-12s %10llu %10llu %10llu %12s\n", r.tag.c_str(), (unsigned long long)r.sent,
                  (unsigned long long)r.acks, (unsigned long long)sys, "轮间空转");
      continue;
    }
    std::printf("%-12s %10llu %10llu %10llu %12.2f\n", r.tag.c_str(), (unsigned long long)r.sent,
                (unsigned long long)r.acks, (unsigned long long)sys, per);
  }

  if (raw.ok) {
    std::printf("\n=== 相对裸 UDP 天花板 ===\n");
    for (const auto& r : all) {
      if (r.pps > 0) {
        std::printf("  %-12s %7.0f 包/秒 = 天花板 %.0f 包/秒 的 %.0f%%   吞吐 %.1f / %.1f Mbps\n",
                    r.tag.c_str(), r.pps, raw.pipe_pps, 100.0 * r.pps / raw.pipe_pps, r.mbps,
                    raw.pipe_mbps);
      }
    }
  }

  if (!csv.empty()) {
    std::FILE* f = std::fopen(csv.c_str(), "w");
    if (f) {
      std::fprintf(f, "tag,mb,streams,coalesce,wall_ms,cpu_ms,mbps,pps,sent,acks,retr,lost,"
                      "send_fail,rx_overflow,rto,send_sys,recv_sys,relay_drop,srtt_ms,cwnd_kb,"
                      "cwnd_min_kb,inflight_avg_kb,mbps_min,mbps_max,complete,verified,rpc\n");
      for (const auto& r : all) {
        std::fprintf(f, "%s,%.3f,%d,%d,%.3f,%.3f,%.1f,%.0f,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                        "%llu,%llu,%llu,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%d\n",
                     r.tag.c_str(), r.mb, r.streams, r.coalesce ? 1 : 0, r.wall_ms, r.cpu_ms,
                     r.mbps, r.pps, (unsigned long long)r.sent, (unsigned long long)r.acks,
                     (unsigned long long)r.retr, (unsigned long long)r.lost,
                     (unsigned long long)r.sfail, (unsigned long long)r.rxdrop,
                     (unsigned long long)r.rto, (unsigned long long)r.send_sys,
                     (unsigned long long)r.recv_sys,
                     (unsigned long long)r.relay_drop, r.srtt_ms, r.cwnd_kb, r.cwnd_min_kb,
                     r.inflight_avg_kb, r.mbps_min, r.mbps_max, r.complete ? 1 : 0,
                     r.ok ? 1 : 0, r.rpc ? 1 : 0);
      }
      std::fclose(f);
      std::printf("\nCSV 已写入 %s\n", csv.c_str());
    }
  }

  std::printf("\n注：本进程是忙轮询单线程，发送与接收共用一核；且回环没有队列限制、没有随机丢包，\n");
  std::printf("    因此 cwnd 会一路涨到窗口上限——回环能验证\"正确性\"和\"单核处理能力\"，\n");
  std::printf("    但验证不了拥塞控制，那一部分仍然要靠确定性仿真。\n");
  return 0;
}
