// 单包 CPU 成本基准：回答"协议本身能吃掉多少带宽"。
//
// 仿真器里的 cwnd 曲线只能证明协议逻辑对，证明不了性能——虚拟时钟不计
// syscall、不计 memcpy、不计 cache miss。这里直接测协议栈自身每包要花多少
// 纳秒，再乘包长换算成带宽上限。
//
// 取"多轮最小值"而不是平均值：调度器抢占是外部噪声，我们量的是能力上限。
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <new>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xport/packet.h"
#include "xport/sim.h"
#include "xport/transport.h"

using namespace xport;
using Clock = std::chrono::steady_clock;

// 全局 new 计数：用来定位"每处理一个包要 malloc 几次"。
// 堆分配在热路径上是最贵的一类操作（调用 + 指针追逐 + cache miss），
// 比 memcpy 贵一到两个数量级，而且会随并发放大成锁竞争。
static std::atomic<unsigned long long> g_allocs{0};
static bool g_count = false;
void* operator new(std::size_t n) {
  if (g_count) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static double NsPerIter(Clock::time_point a, Clock::time_point b, long long n) {
  return double(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()) / double(n);
}

// 重复 R 轮取最小值
template <typename F>
static double BestOf(int rounds, long long iters, F&& f) {
  double best = 1e30;
  for (int r = 0; r < rounds; ++r) {
    const auto t0 = Clock::now();
    f();
    const auto t1 = Clock::now();
    best = std::min(best, NsPerIter(t0, t1, iters));
  }
  return best;
}

// 用一条"无限带宽、零延迟"的链路，让 SimLink 只做最少的搬运
static LinkParams FastLink() {
  LinkParams p;
  p.delay_us = 0;
  p.bandwidth_mbps = 0;
  p.queue_us = 0;
  return p;
}

int main(int argc, char** argv) {
  const bool csv = argc > 1 && std::string(argv[1]) == "--csv";
  constexpr std::size_t kPayload = kMaxPayload - 32;  // 1368 B，与 transport 的流帧留白一致
  constexpr double kGhz = 3.0;                          // 换算 cycles 用

  // ---- 1. 编码：固定头 + STREAM 帧 ----
  const long long kEncIters = 1'000'000;
  u8 buf[kMtu];
  const double enc_ns = BestOf(5, kEncIters, [&] {
    for (long long i = 0; i < kEncIters; ++i) {
      Header h;
      h.type = PacketType::kData;
      h.pkt_num = u64(i);
      h.payload_len = u16(kPayload + 12);
      const std::size_t hn = EncodeHeader(h, MutBytes(buf, sizeof buf));
      const StreamFrame f{u32(i % 4), u64(i) * kPayload, Bytes(buf + hn, kPayload), false};
      h.payload_len = u16(EncodeStream(MutBytes(buf + hn, sizeof buf - hn), f) + 0);
    }
  });

  // ---- 2. 解码：头校验 + 帧解析 ----
  Header h0;
  h0.type = PacketType::kData;
  h0.pkt_num = 42;
  const std::size_t hn = EncodeHeader(h0, MutBytes(buf, sizeof buf));
  const StreamFrame f0{0, 0, Bytes(buf + hn + 32, kPayload), false};
  const std::size_t fn = EncodeStream(MutBytes(buf + hn, sizeof buf - hn), f0);
  {
    Header hh;
    hh.type = PacketType::kData;
    hh.pkt_num = 42;
    hh.payload_len = u16(fn);
    EncodeHeader(hh, MutBytes(buf, sizeof buf));
  }
  const std::size_t pkt_len = hn + fn;

  const long long kDecIters = 1'000'000;
  const double dec_ns = BestOf(5, kDecIters, [&] {
    u64 sink = 0;
    for (long long i = 0; i < kDecIters; ++i) {
      Header h;
      if (!DecodeHeader(Bytes(buf, pkt_len), &h)) continue;
      Decoded d;
      if (DecodeFrames(Bytes(buf + hn, h.payload_len), &d)) sink += d.streams[0].data.size();
    }
    if (sink == 0xdeadbeefull) std::printf("");
  });

  // ---- 3. 接收路径：OnPacket（重组长 + ACK 记账）----
  // ACK 合并窗口设到极大，把"发 ACK"从这条路径里摘出去单独计。
  const long long kRxIters = 40'000;
  std::vector<std::vector<u8>> pkts;
  pkts.reserve(kRxIters);
  {
    u8 tmp[kMtu];
    for (long long i = 0; i < kRxIters; ++i) {
      Header h;
      h.type = PacketType::kData;
      h.pkt_num = u64(i + 1);
      h.payload_len = u16(EncodeStream(MutBytes(tmp, sizeof tmp), StreamFrame{
          0, u64(i) * kPayload, Bytes(tmp + 64, kPayload), false}));
      const std::size_t n = EncodeHeader(h, MutBytes(tmp, sizeof tmp));
      // 头写入会覆盖 payload 区，重新写一次帧到头的后面
      h.payload_len = u16(EncodeStream(MutBytes(tmp + n, sizeof tmp - n),
                                       StreamFrame{0, u64(i) * kPayload,
                                                   Bytes(tmp + n + 64, kPayload), false}));
      pkts.emplace_back(tmp, tmp + n + h.payload_len);
    }
  }
  double rx_ns = 1e30;
  double rx_alloc = 0.0;
  for (int r = 0; r < 3; ++r) {
    SimLink link(FastLink());
    TransportConfig cfg;
    cfg.ack_delay_us = 1'000'000'000u;  // 不主动发 ACK，隔离发送成本
    cfg.initial_max_data = 8ull << 20;
    cfg.recv_window_bytes = 8ull << 20;  // 环形缓冲也是 8 MB：够装下 40k 包的消费间隙
    Transport b(link, 2, 1, cfg);
    const auto t0 = Clock::now();
    g_allocs.store(0);
    g_count = true;
    for (long long i = 0; i < kRxIters; ++i) {
      b.OnPacket(Bytes(pkts[i]), Time(i + 1));
      // 消费式接收：不消费会撞上 2 MB 环形窗口，后面全被丢弃，测出来的就不是真实路径
      Bytes v = b.Peek(0);
      if (!v.empty()) b.Consume(0, v.size());
    }
    g_count = false;
    const auto t1 = Clock::now();
    rx_alloc = double(g_allocs.load()) / double(kRxIters);
    rx_ns = std::min(rx_ns, NsPerIter(t0, t1, kRxIters));
  }

  // ---- 4. 发送路径：Tick 推包（含 SimLink 搬运）----
  const long long kTxPkts = 40'000;
  double tx_ns = 1e30;
  for (int r = 0; r < 3; ++r) {
    SimLink link(FastLink());
    TransportConfig cfg;
    cfg.initial_max_data = 1ull << 40;
    cfg.ack_every_n = 1'000'000;   // 不因 ACK 触发额外发送
    cfg.cc.initial_cwnd = 1ull << 30;
    cfg.cc.max_cwnd = 1ull << 30;
    Transport a(link, 1, 2, cfg);
    const u32 sid = a.OpenStream();
    std::vector<u8> blob(std::size_t(kTxPkts) * kPayload, 0x5a);
    a.Write(sid, Bytes(blob));

    const auto t0 = Clock::now();
    Time now = 0;
    u64 sent = 0;
    while (sent < u64(kTxPkts) && now < Time(1) * 60'000'000) {
      const u64 before = a.stats().pkts_sent;
      a.Tick(now);
      sent += a.stats().pkts_sent - before;
      now += 1000;
    }
    const auto t1 = Clock::now();
    tx_ns = std::min(tx_ns, NsPerIter(t0, t1, static_cast<long long>(sent ? sent : 1)));
  }

  // ---- 5. SimLink 自身开销（用于从 tx 中扣除）----
  const long long kLinkIters = 40'000;  // 与发送路径测试同规模，队列增长带来的偏差才一致
  double link_ns = 1e30;
  {
    SimLink link(FastLink());
    std::vector<u8> pkt(buf, buf + pkt_len);
    link_ns = BestOf(5, kLinkIters, [&] {
      // 与发送路径测试保持同一条件（都不排空），这样"扣除仿真开销"才是同一口径。
      // 注意：仿真器的 Send 每包都会 new 一个 vector，本身就有几百 ns，
      // 真实实现里没有这部分，所以只看扣除后的差值，不要拿绝对值当净值。
      for (long long i = 0; i < kLinkIters; ++i) link.Send(1, 2, Bytes(pkt), Time(i));
    });
  }

  // ---- 6. memcpy 基线 ----
  const long long kCopyIters = 400'000;
  std::vector<u8> src(kPayload, 1), dst(kPayload, 0);
  const double copy_ns = BestOf(5, kCopyIters, [&] {
    for (long long i = 0; i < kCopyIters; ++i) {
      std::memcpy(dst.data(), src.data() + (i & 7), kPayload - 8);
      // 空 asm + memory clobber：告诉编译器"这块内存被外部读走了"，
      // 阻止它把整段 memcpy 优化成死代码。比 volatile 写便宜得多。
      asm volatile("" : : "r"(dst.data()) : "memory");
    }
  });

  // ---- 6b. 重组写缓冲基线：把 1368 B 追加进一个不断增长的 vector ----
  // 接收路径必然要把载荷落地一次。这个基线含"首次触碰新页"的缺页成本，
  // 用它去分解 OnPacket，才能分清哪些是协议开销、哪些是内存带宽/缺页。
  double append_ns = 1e30;
  for (int r = 0; r < 3; ++r) {
    std::vector<u8> v;
    const auto t0 = Clock::now();
    for (long long i = 0; i < kRxIters; ++i) {
      v.insert(v.end(), src.begin(), src.end());
    }
    const auto t1 = Clock::now();
    append_ns = std::min(append_ns, NsPerIter(t0, t1, kRxIters));
    asm volatile("" : : "r"(v.data()) : "memory");
  }

  // ---- 6c. 载荷落地成本 vs 包长 ----
  // 接收路径里"把载荷写进重组缓冲"是与**字节数**成正比的部分，
  // 而协议逻辑是与**包数**成正比的固定成本。两者的比例决定了包长该怎么选：
  // 小包被固定成本拖死，大包被内存带宽拖死，中间有个最优点。
  std::vector<u8> big(64 * 1024, 0x77);
  std::printf("\n=== 载荷落地成本（单核，1 字节写入要多少 ns）===\n");
  std::printf("%-10s %14s %14s %12s %12s\n", "包长", "增长缓冲 ns/B", "预分配 ns/B",
              "增长 GB/s", "预分配 GB/s");
  for (int i = 0; i < 92; ++i) std::putchar('-');
  std::putchar('\n');
  for (std::size_t sz : {std::size_t(256), std::size_t(1368), std::size_t(4096),
                         std::size_t(8192), std::size_t(16384)}) {
    const long long iters = 120'000;
    double grow = 1e30, reuse = 1e30;

    for (int r = 0; r < 3; ++r) {
      std::vector<u8> v;
      const auto t0 = Clock::now();
      for (long long i = 0; i < iters; ++i) {
        v.insert(v.end(), big.data(), big.data() + sz);
      }
      const auto t1 = Clock::now();
      grow = std::min(grow, NsPerIter(t0, t1, iters) / double(sz));
      asm volatile("" : : "r"(v.data()) : "memory");
    }

    for (int r = 0; r < 3; ++r) {
      std::vector<u8> ring(8u << 20);  // 预分配：模拟环形接收缓冲，无缺页
      std::size_t off = 0;
      const auto t0 = Clock::now();
      for (long long i = 0; i < iters; ++i) {
        std::memcpy(ring.data() + off, big.data(), sz);
        off += sz;
        if (off + sz > ring.size()) off = 0;
        asm volatile("" : : "r"(ring.data() + off) : "memory");
      }
      const auto t1 = Clock::now();
      reuse = std::min(reuse, NsPerIter(t0, t1, iters) / double(sz));
    }

    std::printf("%-10zu %14.3f %14.3f %12.1f %12.1f\n", sz, grow, reuse, 1.0 / grow,
                1.0 / reuse);
  }
  std::printf("(GB/s = 单核 100%% 用于写内存时能承载的带宽；实际要和协议固定成本叠加)\n");

  // ---- 7. ACK 处理成本 vs 在途包数 ----
  // 这一项决定"窗口能开多大"。若成本随在途包数线性增长，那么在大 BDP 链路上
  // 首个瓶颈不是带宽而是 ACK 处理——窗口开得越大越慢，形成自锁。
  std::vector<int> depths = {16, 64, 256, 1024, 4096};
  std::vector<double> ack_ns;    // 一次 ACK 覆盖整个窗口（窗口整体推进）
  std::vector<double> ack2_ns;   // 一次 ACK 只确认最新 2 包（稳态主流情况）
  double ack2_alloc = 0;
  for (int d : depths) {
    std::vector<double> samples;
    for (int rep = 0; rep < 5; ++rep) {
      SimLink link(FastLink());
      TransportConfig cfg;
      cfg.initial_max_data = 1ull << 40;
      cfg.ack_every_n = 1'000'000;
      cfg.cc.initial_cwnd = 1ull << 30;
      cfg.cc.max_cwnd = 1ull << 30;
      Transport a(link, 1, 2, cfg);
      const u32 sid = a.OpenStream();
      std::vector<u8> blob(std::size_t(d) * kPayload, 0x33);
      a.Write(sid, Bytes(blob));
      Time now = 0;
      while (a.stats().pkts_sent < u64(d) && now < Time(1) * 60'000'000) {
        a.Tick(now);
        now += 1000;
      }
      // 构造覆盖全部在途包的单个 ACK
      AckFrame af;
      af.count = 1;
      af.lo[0] = 0;
      af.hi[0] = u64(d) - 1;
      u8 ab[kMtu];
      const std::size_t an = EncodeAck(MutBytes(ab + 32, sizeof ab - 32), af);
      Header ah;
      ah.type = PacketType::kAck;
      ah.pkt_num = 1'000'000;
      ah.payload_len = u16(an);
      const std::size_t ahn = EncodeHeader(ah, MutBytes(ab, sizeof ab));
      std::memcpy(ab + ahn, ab + 32, an);
      std::vector<u8> ackpkt(ab, ab + ahn + an);

      const auto t0 = Clock::now();
      a.OnPacket(Bytes(ackpkt), now);
      const auto t1 = Clock::now();
      samples.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    std::sort(samples.begin(), samples.end());
    ack_ns.push_back(samples[samples.size() / 2]);

    // 稳态：每 2 个包回一次 ACK，只推进最新两包
    std::vector<double> s2;
    for (int rep = 0; rep < 5; ++rep) {
      SimLink link(FastLink());
      TransportConfig cfg;
      cfg.initial_max_data = 1ull << 40;
      cfg.ack_every_n = 1'000'000;
      cfg.cc.initial_cwnd = 1ull << 30;
      cfg.cc.max_cwnd = 1ull << 30;
      Transport a(link, 1, 2, cfg);
      const u32 sid = a.OpenStream();
      std::vector<u8> blob(std::size_t(2 * d) * kPayload, 0x33);
      a.Write(sid, Bytes(blob));
      Time now = 0;
      while (a.stats().pkts_sent < u64(2 * d) && now < Time(1) * 60'000'000) {
        a.Tick(now);
        now += 1000;
      }
      // 预热：把前 d 个包全部确认掉。此时在途还剩 d 个包，
      // 且 largest_acked 已经推进到 d-1——这才是稳态，不是"第一个 ACK 就宣判半个窗口丢包"。
      {
        AckFrame warm;
        warm.count = 1;
        warm.lo[0] = 0;
        warm.hi[0] = u64(d) - 1;
        u8 wb[kMtu];
        const std::size_t wn = EncodeAck(MutBytes(wb + 32, sizeof wb - 32), warm);
        Header wh;
        wh.type = PacketType::kAck;
        wh.pkt_num = 999'999;
        wh.payload_len = u16(wn);
        const std::size_t whn = EncodeHeader(wh, MutBytes(wb, sizeof wb));
        std::memcpy(wb + whn, wb + 32, wn);
        a.OnPacket(Bytes(wb, whn + wn), now);
      }
      AckFrame af;
      af.count = 1;
      af.lo[0] = u64(d);
      af.hi[0] = u64(d) + 1;
      u8 ab[kMtu];
      const std::size_t an = EncodeAck(MutBytes(ab + 32, sizeof ab - 32), af);
      Header ah;
      ah.type = PacketType::kAck;
      ah.pkt_num = 1'000'000;
      ah.payload_len = u16(an);
      const std::size_t ahn = EncodeHeader(ah, MutBytes(ab, sizeof ab));
      std::memcpy(ab + ahn, ab + 32, an);
      std::vector<u8> ackpkt(ab, ab + ahn + an);

      g_allocs.store(0);
      g_count = true;
      const auto t0 = Clock::now();
      a.OnPacket(Bytes(ackpkt), now);
      const auto t1 = Clock::now();
      g_count = false;
      ack2_alloc = std::max(ack2_alloc, double(g_allocs.load()));
      s2.push_back(double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    std::sort(s2.begin(), s2.end());
    ack2_ns.push_back(s2[s2.size() / 2]);
  }

  std::printf("\n=== ACK 处理成本 vs 在途包数 ===\n");
  std::printf("%-12s %16s %16s %18s\n", "在途包数", "整窗口 ACK (ns)", "稳态 ACK (ns)",
              "稳态占单核@690kpps");
  for (int i = 0; i < 108; ++i) std::putchar('-');
  std::putchar('\n');
  for (std::size_t i = 0; i < depths.size(); ++i) {
    // 690 kpps 数据包 → 每 2 包回一次 ACK = 345k ACK/s
    const double load = ack2_ns[i] * 345'000.0 / 1e9 * 100.0;
    std::printf("%-12d %16.0f %16.0f %17.1f%%\n", depths[i], ack_ns[i], ack2_ns[i], load);
  }

  // ---- 换算 ----
  const double proto_tx = std::max(0.0, tx_ns - link_ns);  // 扣掉仿真器的搬运
  const double per_pkt = proto_tx + rx_ns;                 // 一台机器两端的合计（参考值）
  struct Row {
    const char* name;
    double ns;
  } rows[] = {
      {"编码（头 + STREAM 帧）", enc_ns},
      {"解码（头校验 + 帧解析）", dec_ns},
      {"接收路径 OnPacket", rx_ns},
      {"发送路径 Tick（含仿真链路）", tx_ns},
      {"SimLink::Send（仿真开销）", link_ns},
      {"发送路径（扣除仿真开销）", proto_tx},
      {"memcpy 1368 B 基线", copy_ns},
      {"（旧方案）写入增长缓冲", append_ns},
  };

  std::printf("\n=== 单包 CPU 成本（best-of-N，%.1f GHz 折算）===\n", kGhz);
  std::printf("%-32s %10s %10s\n", "阶段", "ns/包", "cycles");
  for (int i = 0; i < 108; ++i) std::putchar('-');
  std::putchar('\n');
  for (const auto& r : rows) std::printf("%-32s %10.1f %10.0f\n", r.name, r.ns, r.ns * kGhz);

  std::printf("\n每处理 1 个数据包发生 %.1f 次堆分配；一次稳态 ACK 发生 %.0f 次\n",
              rx_alloc, ack2_alloc);

  std::printf("\n=== 折算成带宽上限（单核独占，1448 B/包 = %.1f kpps @1Gbps）===\n", 1e9 / 8.0 / 1448.0 / 1000.0);
  std::printf("%-14s %12s %12s %12s\n", "链路速率", "需要 pps", "发送侧占用", "接收侧占用");
  for (int i = 0; i < 108; ++i) std::putchar('-');
  std::putchar('\n');
  const double rates[] = {1.0, 2.0, 5.0, 8.0, 10.0, 25.0};
  for (double gbps : rates) {
    const double pps = gbps * 1e9 / 8.0 / 1448.0;
    std::printf("%-14.1f %12.0f %11.1f%% %11.1f%%\n", gbps, pps, pps * proto_tx / 1e9 * 100.0,
                pps * rx_ns / 1e9 * 100.0);
  }
  std::printf("\n单核 100%% 占满时的上限：发送 %.2f Gbps，接收 %.2f Gbps（按当前实现）\n",
              1e9 / proto_tx * 1448.0 * 8.0 / 1e9, 1e9 / rx_ns * 1448.0 * 8.0 / 1e9);
  std::printf("扣除仿真开销后，协议栈每包合计 %.0f ns（约 %.0f cycles）\n", per_pkt, per_pkt * kGhz);

  if (csv) {
    std::printf("\nCSV\nstage,ns_per_pkt\n");
    for (const auto& r : rows) std::printf("%s,%.2f\n", r.name, r.ns);
    std::printf("proto_tx+rx,%.2f\n", per_pkt);
  }
  return 0;
}
