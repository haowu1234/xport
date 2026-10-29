// xport_bench —— 批量跑矩阵，输出 CSV 与对齐表格。
//
// 每个用例：新建链路 + 两个端点，传输固定大小的数据，校验逐字节一致，
// 记录吞吐、重传率、平均拥塞窗口、SRTT。
//
// 用法：
//   xport_bench                 # 跑全部矩阵，打印表格
//   xport_bench --csv out.csv   # 同时写 CSV
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xport/hash.h"
#include "xport/sim.h"
#include "xport/transport.h"

using namespace xport;

namespace {

constexpr Time kStepUs = 10;  // 仿真步长：比 demo 更细，高带宽场景才有意义
constexpr Time kLimitUs = 120'000'000;

struct Result {
  double loss = 0;
  double bw = 0;
  int streams = 1;
  double bytes_mb = 0;
  double virtual_ms = 0;
  double goodput_mbps = 0;
  double util_pct = 0;
  unsigned long long sent = 0;
  unsigned long long retr = 0;
  unsigned long long lost = 0;
  unsigned long long spurious = 0;
  unsigned long long qdrop = 0;
  unsigned long long events = 0;
  int algo = 0;
  double retr_pct = 0;
  double cwnd_kb = 0;
  double srtt_ms = 0;
  bool ok = false;
};

std::vector<u8> MakeData(std::size_t n, u32 seed) {
  std::vector<u8> v(n);
  u32 x = seed * 2654435761u + 1u;
  for (auto& b : v) {
    x = x * 1103515245u + 12345u;
    b = u8(x >> 16);
  }
  return v;
}

// 单次传输
Result RunOnce(double loss_pct, double bw_mbps, int streams, std::size_t bytes, u64 seed,
               CcAlgo algo = CcAlgo::kCubic) {
  LinkParams lp;
  lp.loss_pct = loss_pct;
  lp.bandwidth_mbps = bw_mbps;
  lp.delay_us = 1000;  // RTT 2ms
  lp.seed = seed;

  SimLink link(lp);
  TransportConfig tcfg;
  tcfg.cc.algo = algo;
  Transport a(link, 1, 2, tcfg);
  Transport b(link, 2, 1, tcfg);

  std::vector<std::vector<u8>> payloads;
  std::vector<u32> sids;
  const std::size_t per = bytes / std::size_t(streams);
  std::size_t submitted = 0;
  for (int i = 0; i < streams; ++i) {
    const std::size_t n = (i == streams - 1) ? (bytes - submitted) : per;
    payloads.push_back(MakeData(n, u32(i + 7)));
    sids.push_back(a.OpenStream());
    a.Write(sids.back(), Bytes(payloads.back()));
    submitted += n;
  }

  std::vector<u8> pkt;
  Time now = 0;
  double cwnd_sum = 0;
  std::size_t cwnd_n = 0;
  Time next_sample = 0;
  // 消费式接收：不消费会被流控挡住。校验改用滚动哈希，避免把上百 MB 数据留在内存里。
  std::vector<u64> rx_hash(sids.size(), kFnvInit);
  std::vector<u64> rx_bytes(sids.size(), 0);

  for (; now < kLimitUs; now += kStepUs) {
    a.Tick(now);
    b.Tick(now);
    while (link.Poll(1, &pkt, now)) a.OnPacket(Bytes(pkt), now);
    while (link.Poll(2, &pkt, now)) b.OnPacket(Bytes(pkt), now);

    for (std::size_t i = 0; i < sids.size(); ++i) {
      for (;;) {
        Bytes v = b.Peek(sids[i]);
        if (v.empty()) break;
        rx_hash[i] = Fnv1aUpdate(rx_hash[i], v);
        rx_bytes[i] += v.size();
        b.Consume(sids[i], v.size());
      }
    }

    if (now >= next_sample) {
      cwnd_sum += a.stats().cwnd_kb;
      ++cwnd_n;
      next_sample = now + 50'000;  // 每 50ms 采样一次
    }
    if (a.SendComplete()) break;
  }

  Result r;
  r.loss = loss_pct;
  r.bw = bw_mbps;
  r.streams = streams;
  r.bytes_mb = double(bytes) / (1024.0 * 1024.0);
  r.virtual_ms = double(now) / 1000.0;
  r.goodput_mbps = r.virtual_ms > 0 ? double(bytes) * 8.0 / (r.virtual_ms / 1000.0) / 1e6 : 0;
  r.util_pct = bw_mbps > 0 ? 100.0 * r.goodput_mbps / bw_mbps : 0;
  r.sent = a.stats().pkts_sent;
  r.retr = a.stats().retransmits;
  r.lost = a.stats().lost;
  r.spurious = a.stats().spurious;
  r.qdrop = link.queue_drops();
  r.events = a.cc().CongestionEvents();
  r.algo = int(algo);
  r.retr_pct = r.sent ? 100.0 * double(r.retr) / double(r.sent) : 0;
  r.cwnd_kb = cwnd_n ? cwnd_sum / double(cwnd_n) : 0;
  r.srtt_ms = a.stats().srtt_ms;

  r.ok = true;
  for (std::size_t i = 0; i < sids.size() && r.ok; ++i) {
    if (rx_bytes[i] != payloads[i].size() || rx_hash[i] != Fnv1a(Bytes(payloads[i]))) {
      r.ok = false;
    }
  }
  return r;
}

// 同配置跑三个种子取中位数，抵消单次随机波动
Result RunMedian(double loss, double bw, int streams, std::size_t bytes,
                 CcAlgo algo = CcAlgo::kCubic) {
  Result rs[3];
  for (int i = 0; i < 3; ++i) rs[i] = RunOnce(loss, bw, streams, bytes, u64(11 + i * 7), algo);
  std::sort(rs, rs + 3, [](const Result& x, const Result& y) { return x.goodput_mbps < y.goodput_mbps; });
  return rs[1];
}

double RenoBound(double loss_pct, double rtt_ms) {
  if (loss_pct <= 0) return 0;
  const double p = loss_pct / 100.0;
  return 1.22 * 1368.0 * 8.0 / ((rtt_ms / 1000.0) * std::sqrt(p)) / 1e6;
}

void PrintHeader() {
  std::printf("\n%-8s %-8s %-4s %-8s %-10s %-8s %-8s %-9s %-8s %-7s %s\n", "丢包%", "带宽Mbps",
              "流", "数据MB", "耗时ms", "吞吐Mbps", "利用率%", "重传率%", "cwnd KB", "SRTTms",
              "校验");
  for (int i = 0; i < 104; ++i) std::putchar('-');
  std::putchar('\n');
}

void PrintRow(const Result& r) {
  std::printf("%-8.3f %-8.0f %-4d %-8.1f %-10.1f %-8.1f %-8.1f %-9.2f %-8.1f %-7.2f %s\n",
              r.loss, r.bw, r.streams, r.bytes_mb, r.virtual_ms, r.goodput_mbps, r.util_pct,
              r.retr_pct, r.cwnd_kb, r.srtt_ms, r.ok ? "OK" : "FAIL");
}

}  // namespace

int main(int argc, char** argv) {
  std::string csv_path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--csv" && i + 1 < argc) {
      csv_path = argv[++i];
    } else {
      std::printf("用法: xport_bench [--csv <path>]\n");
      return 2;
    }
  }

  std::vector<Result> all;
  std::vector<Result> loss_sweep;

  // 矩阵 A：丢包率扫描（固定 200 Mbps，单流，8 MB）
  std::printf("\n=== A. 丢包率扫描 | 200 Mbps | 单流 | 8 MB | RTT 2ms ===\n");
  PrintHeader();
  for (double loss : {0.0, 0.001, 0.01, 0.05, 0.1, 0.5, 1.0, 2.0, 5.0}) {
    Result r = RunMedian(loss, 200.0, 1, 8u << 20);
    PrintRow(r);
    all.push_back(r);
    loss_sweep.push_back(r);
  }

  // 矩阵 B：带宽扫描（固定 0.01% 丢包，单流；数据量按带宽缩放，保持约 0.3~0.6s）
  std::printf("\n=== B. 带宽扫描 | 0.01%% 丢包 | 单流 | RTT 2ms ===\n");
  PrintHeader();
  for (auto [bw, mb] : {std::pair<double, double>{50, 4}, {200, 8}, {1000, 32}, {5000, 128}}) {
    Result r = RunMedian(0.01, bw, 1, std::size_t(mb * 1024 * 1024));
    PrintRow(r);
    all.push_back(r);
  }

  // 矩阵 C：并发流数（固定 200 Mbps，0.01% 丢包，16 MB）
  std::printf("\n=== C. 并发流数 | 0.01%% 丢包 | 200 Mbps | 16 MB | RTT 2ms ===\n");
  PrintHeader();
  for (int s : {1, 2, 4, 8}) {
    Result r = RunMedian(0.01, 200.0, s, 16u << 20);
    PrintRow(r);
    all.push_back(r);
  }

  // 矩阵 D：高丢包下的稳定性（验证不会重传风暴）
  std::printf("\n=== D. 极端丢包 | 200 Mbps | 单流 | 8 MB | RTT 2ms ===\n");
  PrintHeader();
  // 高丢包下数据量按比例缩小：RTO 恢复每 200ms 才能推一个包，
  // 8 MB 在 20% 丢包下要跑上千秒虚拟时间，那不是协议问题是场景超纲
  for (auto [loss, mb] : {std::pair<double, double>{5.0, 2}, {10.0, 1}, {20.0, 0.25}}) {
    Result r = RunMedian(loss, 200.0, 1, std::size_t(mb * 1024 * 1024));
    PrintRow(r);
    all.push_back(r);
  }

  // 丢包率 vs Reno 理论上限
  // 矩阵 G：同一链路只换控制器，证明 CUBIC 在高 BDP 下的收益
  std::printf("\n=== G. CUBIC vs AIMD(Reno) | 0.01%% 丢包 | 单流 | RTT 2ms ===\n");
  std::printf("%-10s %-6s %-10s %-9s %-9s %-8s %s\n", "带宽Mbps", "算法", "吞吐Mbps", "利用率%",
              "cwnd KB", "事件数", "校验");
  for (int i = 0; i < 108; ++i) std::putchar('-');
  std::putchar('\n');
  for (auto [bw, mb] : {std::pair<double, double>{50, 4}, {200, 8}, {1000, 32}, {5000, 128}}) {
    for (CcAlgo algo : {CcAlgo::kCubic, CcAlgo::kReno}) {
      Result r = RunMedian(0.01, bw, 1, std::size_t(mb * 1024 * 1024), algo);
      std::printf("%-10.0f %-6s %-10.1f %-9.1f %-9.1f %-8llu %s\n", r.bw, CcAlgoName(algo),
                  r.goodput_mbps, r.util_pct, r.cwnd_kb, (unsigned long long)r.events,
                  r.ok ? "OK" : "FAIL");
      all.push_back(r);
    }
  }

  // 矩阵 H：小消息（RPC）场景的帧聚合 A/B。
  // 每条消息 200 B，8 条流轮流向对方各发一条。关闭聚合时每条消息独占一个 1400 B 的包，
  // 有效载荷只有 14%——这正是 RPC 场景 pps 虚高的根源。
  std::printf("\n=== H. 帧聚合 A/B | 小消息 200B | 8 条流 | RTT 2ms | 100 Mbps ===\n");
  std::printf("%-12s %-10s %-10s %-12s %-8s %s\n", "聚合", "消息总数", "发送包数", "每包帧数",
              "校验", "说明");
  for (int i = 0; i < 100; ++i) std::putchar('-');
  std::putchar('\n');
  {
    const int kStreams = 8, kRounds = 200, kMsg = 200;
    for (bool coalesce : {true, false}) {
      LinkParams lp;
      lp.bandwidth_mbps = 100;
      lp.delay_us = 1000;
      lp.seed = 5;
      SimLink link(lp);
      TransportConfig tcfg;
      tcfg.coalesce = coalesce;
      Transport a(link, 1, 2, tcfg);
      Transport b(link, 2, 1, tcfg);

      std::vector<std::vector<u8>> msgs;
      std::vector<u32> sids;
      for (int k = 0; k < kStreams; ++k) {
        msgs.push_back(MakeData(kMsg, u32(k + 11)));
        sids.push_back(a.OpenStream());
      }
      // 消费式接收：每 tick 取走并滚动哈希（不保留全量数据）
      std::vector<u64> hh(kStreams, kFnvInit);
      std::vector<std::size_t> hb(kStreams, 0);
      const auto drain = [&]() {
        for (int k = 0; k < kStreams; ++k) {
          for (;;) {
            Bytes v = b.Peek(sids[k]);
            if (v.empty()) break;
            hh[k] = Fnv1aUpdate(hh[k], v);
            hb[k] += v.size();
            b.Consume(sids[k], v.size());
          }
        }
      };

      std::vector<u8> pkt;
      Time now = 0;
      for (int r = 0; r < kRounds; ++r) {
        for (int k = 0; k < kStreams; ++k) a.Write(sids[k], Bytes(msgs[k]));
        const Time until = now + 5000;  // 每轮给 5 ms 虚拟时间（≈2.5 个 RTT）
        for (; now < until; now += 10) {
          a.Tick(now);
          b.Tick(now);
          while (link.Poll(1, &pkt, now)) a.OnPacket(Bytes(pkt), now);
          while (link.Poll(2, &pkt, now)) b.OnPacket(Bytes(pkt), now);
          drain();
        }
      }
      for (int i = 0; i < 4000; ++i) {
        now += 10;
        a.Tick(now);
        b.Tick(now);
        while (link.Poll(1, &pkt, now)) a.OnPacket(Bytes(pkt), now);
        while (link.Poll(2, &pkt, now)) b.OnPacket(Bytes(pkt), now);
        drain();
      }
      bool ok = true;
      for (int k = 0; k < kStreams; ++k) {
        std::vector<u8> expect;  // 同一消息重复 kRounds 次
        expect.reserve(std::size_t(kRounds) * kMsg);
        for (int r = 0; r < kRounds; ++r) expect.insert(expect.end(), msgs[k].begin(), msgs[k].end());
        if (hb[k] != expect.size() || hh[k] != Fnv1a(Bytes(expect))) ok = false;
      }
      const unsigned long long pkts = a.stats().pkts_sent;
      std::printf("%-12s %-10d %-10llu %-12.2f %-8s %s\n", coalesce ? "开" : "关",
                  kStreams * kRounds, pkts, double(kStreams * kRounds) / double(pkts ? pkts : 1),
                  ok ? "OK" : "FAIL", coalesce ? "多流合并进同一报文" : "一帧一包");
    }
  }

  std::printf("\n=== E. 与 AIMD 理论上限对比（200 Mbps，单流）===\n");
  std::printf("%-8s %-14s %-14s %s\n", "丢包%", "实测Mbps", "Reno上限Mbps", "实测/理论");
  for (int i = 0; i < 100; ++i) std::putchar('-');
  std::putchar('\n');
  for (const auto& r : loss_sweep) {
    if (r.loss <= 0) continue;
    const double bound = RenoBound(r.loss, 2.0);
    // 丢包很低时 Reno 上限远高于链路容量，比值没有意义（链路才是瓶颈），跳过
    if (bound > 200.0) continue;
    std::printf("%-8.3f %-14.1f %-14.1f %.0f%%\n", r.loss, r.goodput_mbps, bound,
                bound > 0 ? 100.0 * r.goodput_mbps / bound : 0.0);
  }

  // 矩阵 F：同配置多种子，量化随机波动
  std::printf("\n=== F. 重复性（1%% 丢包 / 200 Mbps / 8 MB，7 个种子）===\n");
  {
    Result rs[7];
    for (int i = 0; i < 7; ++i) rs[i] = RunOnce(1.0, 200.0, 1, 8u << 20, u64(101 + i * 13));
    std::sort(rs, rs + 7,
              [](const Result& x, const Result& y) { return x.goodput_mbps < y.goodput_mbps; });
    std::printf("%-14s %-14s %-14s %s\n", "最小Mbps", "中位Mbps", "最大Mbps", "极差%");
    for (int i = 0; i < 60; ++i) std::putchar('-');
    std::putchar('\n');
    const double lo = rs[0].goodput_mbps;
    const double hi = rs[6].goodput_mbps;
    std::printf("%-14.1f %-14.1f %-14.1f %.1f%%\n", lo, rs[3].goodput_mbps, hi,
                lo > 0 ? 100.0 * (hi - lo) / lo : 0.0);
    all.push_back(rs[3]);
  }

  if (!csv_path.empty()) {
    if (FILE* f = std::fopen(csv_path.c_str(), "w")) {
      std::fprintf(f, "loss_pct,bandwidth_mbps,streams,data_mb,virtual_ms,goodput_mbps,util_pct,"
                      "pkts_sent,retransmits,lost,spurious,qdrop,retrans_pct,cwnd_kb,srtt_ms,"
                      "events,algo,verified\n");
      for (const auto& r : all) {
        std::fprintf(f,
                     "%.4f,%.0f,%d,%.2f,%.2f,%.2f,%.2f,%llu,%llu,%llu,%llu,%llu,%.3f,%.2f,%.2f,"
                     "%llu,%s,%d\n",
                     r.loss, r.bw, r.streams, r.bytes_mb, r.virtual_ms, r.goodput_mbps, r.util_pct,
                     r.sent, r.retr, r.lost, r.spurious, r.qdrop, r.retr_pct, r.cwnd_kb, r.srtt_ms,
                     (unsigned long long)r.events, CcAlgoName(CcAlgo(r.algo)), r.ok ? 1 : 0);
      }
      std::fclose(f);
      std::printf("\nCSV 已写入: %s（%zu 行）\n", csv_path.c_str(), all.size());
    }
  }
  return 0;
}
