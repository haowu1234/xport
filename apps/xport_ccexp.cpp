// xport_ccexp —— 拥塞控制对比实验：吞吐 / 时延 / 公平性。
//
// 拓扑是哑铃：N 条流的数据方向共享同一条瓶颈（SimLink::SetBottleneckGroup），
// 瓶颈缓冲按"基础 RTT 的倍数"给，即 buffer = k × BDP。ACK 方向不设共享瓶颈。
//
// 五组实验，各写一个 CSV 到 --out 目录：
//   single.csv     单流：算法 × 缓冲 × 随机丢包 → 吞吐、排队时延分位数、重传率
//   single_ts.csv  单流时间序列（深缓冲、无丢包）：cwnd、排队时延、吞吐
//   intra.csv      同算法 4 流错峰启动 → Jain 公平指数、每流份额；intra_ts.csv 每流吞吐曲线
//   inter.csv      1 条 BBRx 对 1 条 CUBIC，扫缓冲大小 → 各自份额
//   rtt.csv        同算法 2 流，RTT 20 ms 对 80 ms → 短 RTT 流的份额
//
// 所有随机性（链路丢包、BBR 探测相位）都由固定种子决定，逐位可复现。
//
// 用法：
//   xport_ccexp --out build/exp                 # 全部实验
//   xport_ccexp --out build/exp --only single   # 只跑一组（可并行起多个进程）
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "xport/sim.h"
#include "xport/transport.h"

using namespace xport;

namespace {

constexpr Time kStepUs = 10;
constexpr u32 kBaseOneWayUs = 20'000;  // 基础 RTT 40 ms
constexpr double kBwMbps = 50.0;
constexpr u32 kBottleneck = 1000;
// 多流实验对 BBR 探测相位的随机种子很敏感（谁先探测、谁晚到撞上已有队列），
// 单个种子的份额能差出一倍。每个配置跑这么多个种子，CSV 逐行记录，报告取中位数和范围。
constexpr int kFairSeeds = 3;

const CcAlgo kAlgos[] = {CcAlgo::kCubic, CcAlgo::kBbr, CcAlgo::kBbr2, CcAlgo::kBbr3};

struct FlowSpec {
  CcAlgo algo = CcAlgo::kCubic;
  Time start_us = 0;
  u32 extra_one_way_us = 0;  // 在基础时延之上加的单程时延（RTT 公平性实验）
};

struct RunSpec {
  std::vector<FlowSpec> flows;
  double buf_bdp = 2.0;  // 缓冲 = buf_bdp × (kBwMbps × 基础 RTT)
  double loss_pct = 0.0;
  u64 seed = 1;
  Time duration_us = 20'000'000;
  Time measure_from_us = 5'000'000;
  Time ts_interval_us = 0;  // >0 时记录时间序列
};

struct FlowResult {
  double goodput_mbps = 0;  // 测量窗口内的交付速率
  u64 pkts_sent = 0;
  u64 retransmits = 0;
};

struct TsPoint {
  double t_s = 0;
  std::vector<double> flow_mbps;
  std::vector<double> cwnd_kb;
  double qdelay_ms = 0;
};

struct RunResult {
  std::vector<FlowResult> flows;
  double total_mbps = 0;
  double q_avg_ms = 0, q_p50_ms = 0, q_p95_ms = 0, q_p99_ms = 0;
  double retx_pct = 0;
  double jain = 0;
  std::vector<TsPoint> ts;
};

double Jain(const std::vector<double>& x) {
  double s = 0, s2 = 0;
  for (double v : x) {
    s += v;
    s2 += v * v;
  }
  return s2 > 0 ? s * s / (double(x.size()) * s2) : 0;
}

double Percentile(std::vector<double> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const double idx = p * double(v.size() - 1);
  const std::size_t lo = std::size_t(idx);
  const std::size_t hi = std::min(lo + 1, v.size() - 1);
  return v[lo] + (v[hi] - v[lo]) * (idx - double(lo));
}

struct Flow {
  std::unique_ptr<Transport> tx;
  std::unique_ptr<Transport> rx;
  u32 sid = 0;
  u64 written = 0;
  bool started = false;
  u64 delivered_at_measure = 0;
  u64 last_ts_delivered = 0;
};

RunResult Run(const RunSpec& rs) {
  const double bdp_bytes = kBwMbps * 1e6 / 8.0 * (2.0 * kBaseOneWayUs / 1e6);
  LinkParams lp;
  lp.bandwidth_mbps = kBwMbps;
  lp.delay_us = kBaseOneWayUs;
  // 缓冲按时间计（SimLink 的约定）：k × BDP 字节 = k × 基础 RTT 的串行化时间
  lp.queue_us = u32(rs.buf_bdp * bdp_bytes * 8.0 / kBwMbps);
  lp.loss_pct = rs.loss_pct;
  lp.seed = rs.seed;
  SimLink link(lp);

  const std::size_t n = rs.flows.size();
  std::vector<Flow> flows(n);
  for (std::size_t i = 0; i < n; ++i) {
    const u32 tx_id = u32(1 + i);
    const u32 rx_id = u32(101 + i);
    link.SetBottleneckGroup(rx_id, kBottleneck);
    if (rs.flows[i].extra_one_way_us) link.SetExtraDelay(rx_id, rs.flows[i].extra_one_way_us);
    TransportConfig c;
    c.cc.algo = rs.flows[i].algo;
    c.cc.rng_seed = u32(0x9e3779b1u * (i + 1) + u32(rs.seed));
    // 流控和接收窗口都放到远大于 BDP，实验测的是拥塞控制而不是窗口
    c.initial_max_data = 32ull << 20;
    c.recv_window_bytes = 16ull << 20;
    flows[i].tx = std::make_unique<Transport>(link, tx_id, rx_id, c);
    flows[i].rx = std::make_unique<Transport>(link, rx_id, tx_id, c);
    flows[i].sid = flows[i].tx->OpenStream();
  }

  static const std::vector<u8> kChunk(1u << 20, 0x5a);
  std::vector<u8> pkt;
  std::vector<double> qdelay;
  qdelay.reserve(std::size_t(rs.duration_us / 1000));
  RunResult out;
  out.flows.resize(n);
  Time next_q = 0;
  Time next_ts = rs.ts_interval_us;
  bool measuring = false;

  for (Time now = 0; now < rs.duration_us; now += kStepUs) {
    for (std::size_t i = 0; i < n; ++i) {
      Flow& f = flows[i];
      if (!f.started && now >= rs.flows[i].start_us) f.started = true;
      if (!f.started) continue;
      // 持续有数据可发：未发量低于 1 MB 就补 1 MB（发送端保留全部已写数据，所以按需补）
      if (f.written - f.tx->StreamSentBytes(f.sid) < kChunk.size()) {
        f.tx->Write(f.sid, Bytes(kChunk));
        f.written += kChunk.size();
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      flows[i].tx->Tick(now);
      flows[i].rx->Tick(now);
    }
    for (std::size_t i = 0; i < n; ++i) {
      while (link.Poll(u32(1 + i), &pkt, now)) flows[i].tx->OnPacket(Bytes(pkt), now);
      while (link.Poll(u32(101 + i), &pkt, now)) flows[i].rx->OnPacket(Bytes(pkt), now);
      Transport& r = *flows[i].rx;
      for (Bytes v = r.Peek(flows[i].sid); !v.empty(); v = r.Peek(flows[i].sid)) {
        r.Consume(flows[i].sid, v.size());
      }
    }

    if (!measuring && now >= rs.measure_from_us) {
      measuring = true;
      for (auto& f : flows) f.delivered_at_measure = f.rx->DeliveredBytes();
    }
    if (measuring && now >= next_q) {
      next_q = now + 1000;
      qdelay.push_back(double(link.backlog_us(101, now)) / 1000.0);
    }
    if (rs.ts_interval_us > 0 && now >= next_ts) {
      TsPoint p;
      p.t_s = double(now) / 1e6;
      for (auto& f : flows) {
        const u64 d = f.rx->DeliveredBytes();
        p.flow_mbps.push_back(double(d - f.last_ts_delivered) * 8.0 /
                              double(rs.ts_interval_us));
        f.last_ts_delivered = d;
        p.cwnd_kb.push_back(f.started ? double(f.tx->Cwnd()) / 1024.0 : 0.0);
      }
      p.qdelay_ms = double(link.backlog_us(101, now)) / 1000.0;
      out.ts.push_back(std::move(p));
      next_ts += rs.ts_interval_us;
    }
  }

  const double window_s = double(rs.duration_us - rs.measure_from_us) / 1e6;
  std::vector<double> shares;
  u64 sent = 0, retx = 0;
  for (std::size_t i = 0; i < n; ++i) {
    FlowResult& fr = out.flows[i];
    const u64 d = flows[i].rx->DeliveredBytes() - flows[i].delivered_at_measure;
    fr.goodput_mbps = double(d) * 8.0 / window_s / 1e6;
    fr.pkts_sent = flows[i].tx->stats().pkts_sent;
    fr.retransmits = flows[i].tx->stats().retransmits;
    out.total_mbps += fr.goodput_mbps;
    shares.push_back(fr.goodput_mbps);
    sent += fr.pkts_sent;
    retx += fr.retransmits;
  }
  out.jain = Jain(shares);
  out.retx_pct = sent ? 100.0 * double(retx) / double(sent) : 0;
  double qs = 0;
  for (double q : qdelay) qs += q;
  out.q_avg_ms = qdelay.empty() ? 0 : qs / double(qdelay.size());
  out.q_p50_ms = Percentile(qdelay, 0.50);
  out.q_p95_ms = Percentile(qdelay, 0.95);
  out.q_p99_ms = Percentile(qdelay, 0.99);
  return out;
}

FILE* Open(const std::string& dir, const char* name) {
  const std::string p = dir + "/" + name;
  FILE* f = std::fopen(p.c_str(), "w");
  if (f == nullptr) std::fprintf(stderr, "无法写入 %s\n", p.c_str());
  return f;
}

// ---- 1. 单流矩阵 ----
void ExpSingle(const std::string& dir) {
  FILE* f = Open(dir, "single.csv");
  if (!f) return;
  std::fprintf(f, "algo,buf_bdp,loss_pct,goodput_mbps,util_pct,q_avg_ms,q_p50_ms,q_p95_ms,"
                  "q_p99_ms,retx_pct,seeds\n");
  std::printf("\n=== 单流 | %.0f Mbps | RTT %.0f ms ===\n", kBwMbps, 2.0 * kBaseOneWayUs / 1000);
  std::printf("%-6s %-6s %-6s %-10s %-9s %-9s %-9s\n", "算法", "缓冲", "丢包%", "吞吐Mbps",
              "排队avg", "排队p95", "重传%");
  for (double buf : {0.5, 1.0, 2.0, 8.0}) {
    for (double loss : {0.0, 0.1, 1.0, 2.0}) {
      for (CcAlgo algo : kAlgos) {
        // 有随机丢包时取 3 个种子的中位数（按吞吐排序），无丢包时只有一个确定结果
        const int seeds = loss > 0 ? 3 : 1;
        std::vector<RunResult> rr;
        for (int s = 0; s < seeds; ++s) {
          RunSpec spec;
          spec.flows = {FlowSpec{algo, 0, 0}};
          spec.buf_bdp = buf;
          spec.loss_pct = loss;
          spec.seed = u64(11 + 7 * s);
          spec.duration_us = 20'000'000;
          spec.measure_from_us = 5'000'000;
          rr.push_back(Run(spec));
        }
        std::sort(rr.begin(), rr.end(),
                  [](const RunResult& x, const RunResult& y) { return x.total_mbps < y.total_mbps; });
        const RunResult& r = rr[rr.size() / 2];
        std::fprintf(f, "%s,%.1f,%.2f,%.3f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", CcAlgoName(algo),
                     buf, loss, r.total_mbps, 100.0 * r.total_mbps / kBwMbps, r.q_avg_ms,
                     r.q_p50_ms, r.q_p95_ms, r.q_p99_ms, r.retx_pct, seeds);
        std::printf("%-6s %-6.1f %-6.2f %-10.2f %-9.2f %-9.2f %-9.3f\n", CcAlgoName(algo), buf, loss,
                    r.total_mbps, r.q_avg_ms, r.q_p95_ms, r.retx_pct);
        std::fflush(stdout);
      }
    }
  }
  std::fclose(f);
}

// ---- 2. 单流时间序列（深缓冲 8 BDP、无丢包：bufferbloat 场景）----
void ExpSingleTs(const std::string& dir) {
  FILE* f = Open(dir, "single_ts.csv");
  if (!f) return;
  std::fprintf(f, "algo,t_s,mbps,cwnd_kb,qdelay_ms\n");
  for (CcAlgo algo : kAlgos) {
    RunSpec spec;
    spec.flows = {FlowSpec{algo, 0, 0}};
    spec.buf_bdp = 8.0;
    spec.duration_us = 20'000'000;
    spec.measure_from_us = 2'000'000;
    spec.ts_interval_us = 50'000;
    const RunResult r = Run(spec);
    for (const auto& p : r.ts) {
      std::fprintf(f, "%s,%.3f,%.3f,%.2f,%.3f\n", CcAlgoName(algo), p.t_s, p.flow_mbps[0],
                   p.cwnd_kb[0], p.qdelay_ms);
    }
    std::printf("单流时序 %-6s 完成：平均排队 %.1f ms\n", CcAlgoName(algo), r.q_avg_ms);
  }
  std::fclose(f);
}

// ---- 3. 同算法多流公平性 ----
void ExpIntra(const std::string& dir) {
  FILE* f = Open(dir, "intra.csv");
  FILE* ts = Open(dir, "intra_ts.csv");
  if (!f || !ts) return;
  std::fprintf(f, "algo,buf_bdp,seed,flows,jain,total_mbps,q_avg_ms,q_p95_ms,retx_pct,share0,"
                  "share1,share2,share3\n");
  std::fprintf(ts, "algo,buf_bdp,t_s,f0,f1,f2,f3,qdelay_ms\n");
  std::printf("\n=== 同算法 4 流（每 2 s 启动一条，测量 10~30 s）===\n");
  std::printf("%-6s %-6s %-7s %-10s %-9s %s\n", "算法", "缓冲", "Jain", "总吞吐", "重传%",
              "各流 Mbps");
  for (double buf : {0.5, 2.0, 8.0}) {
    for (CcAlgo algo : kAlgos) {
      for (int seed = 0; seed < kFairSeeds; ++seed) {
        RunSpec spec;
        for (int i = 0; i < 4; ++i) spec.flows.push_back(FlowSpec{algo, Time(i) * 2'000'000, 0});
        spec.buf_bdp = buf;
        spec.seed = u64(1 + seed);
        spec.duration_us = 30'000'000;
        spec.measure_from_us = 10'000'000;
        spec.ts_interval_us = seed == 0 ? 200'000 : 0;  // 时间序列只记第一个种子
        const RunResult r = Run(spec);
        std::fprintf(f, "%s,%.1f,%d,4,%.4f,%.3f,%.3f,%.3f,%.3f", CcAlgoName(algo), buf, seed,
                     r.jain, r.total_mbps, r.q_avg_ms, r.q_p95_ms, r.retx_pct);
        for (const auto& fl : r.flows) std::fprintf(f, ",%.3f", fl.goodput_mbps);
        std::fprintf(f, "\n");
        for (const auto& p : r.ts) {
          std::fprintf(ts, "%s,%.1f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f\n", CcAlgoName(algo), buf, p.t_s,
                       p.flow_mbps[0], p.flow_mbps[1], p.flow_mbps[2], p.flow_mbps[3], p.qdelay_ms);
        }
        std::printf("%-6s %-6.1f s%d %-7.3f %-10.2f %-9.3f %.1f / %.1f / %.1f / %.1f\n",
                    CcAlgoName(algo), buf, seed, r.jain, r.total_mbps, r.retx_pct,
                    r.flows[0].goodput_mbps, r.flows[1].goodput_mbps, r.flows[2].goodput_mbps,
                    r.flows[3].goodput_mbps);
        std::fflush(stdout);
      }
    }
  }
  std::fclose(f);
  std::fclose(ts);
}

// ---- 4. 与 CUBIC 共存 ----
void ExpInter(const std::string& dir) {
  FILE* f = Open(dir, "inter.csv");
  if (!f) return;
  std::fprintf(f, "algo,buf_bdp,seed,algo_mbps,cubic_mbps,algo_share,jain,q_avg_ms,retx_pct\n");
  std::printf("\n=== 1 条 X 对 1 条 CUBIC（同时启动，测量 5~30 s）===\n");
  std::printf("%-6s %-6s %-10s %-10s %-8s\n", "算法", "缓冲", "X Mbps", "CUBIC Mbps", "X 份额");
  for (CcAlgo algo : kAlgos) {
    for (double buf : {0.5, 1.0, 2.0, 4.0, 8.0, 16.0}) {
      for (int seed = 0; seed < kFairSeeds; ++seed) {
        RunSpec spec;
        spec.flows = {FlowSpec{algo, 0, 0}, FlowSpec{CcAlgo::kCubic, 0, 0}};
        spec.buf_bdp = buf;
        spec.seed = u64(1 + seed);
        spec.duration_us = 30'000'000;
        spec.measure_from_us = 5'000'000;
        const RunResult r = Run(spec);
        const double x = r.flows[0].goodput_mbps, c = r.flows[1].goodput_mbps;
        const double share = x + c > 0 ? x / (x + c) : 0;
        std::fprintf(f, "%s,%.1f,%d,%.3f,%.3f,%.4f,%.4f,%.3f,%.3f\n", CcAlgoName(algo), buf, seed,
                     x, c, share, r.jain, r.q_avg_ms, r.retx_pct);
        std::printf("%-6s %-6.1f s%d %-10.2f %-10.2f %-8.3f\n", CcAlgoName(algo), buf, seed, x, c,
                    share);
        std::fflush(stdout);
      }
    }
  }
  std::fclose(f);
}

// ---- 5. RTT 公平性 ----
void ExpRtt(const std::string& dir) {
  FILE* f = Open(dir, "rtt.csv");
  if (!f) return;
  std::fprintf(f, "algo,seed,rtt_short_ms,rtt_long_ms,short_mbps,long_mbps,short_share,jain\n");
  std::printf("\n=== RTT 公平性：同算法 2 流，RTT 40 ms 对 160 ms，缓冲 2 BDP(40ms) ===\n");
  for (CcAlgo algo : kAlgos) {
    for (int seed = 0; seed < kFairSeeds; ++seed) {
      RunSpec spec;
      // 长 RTT 流的数据方向多 120 ms 单程时延 → RTT 160 ms
      spec.flows = {FlowSpec{algo, 0, 0}, FlowSpec{algo, 0, 120'000}};
      spec.buf_bdp = 2.0;
      spec.seed = u64(1 + seed);
      spec.duration_us = 40'000'000;
      spec.measure_from_us = 10'000'000;
      const RunResult r = Run(spec);
      const double s = r.flows[0].goodput_mbps, l = r.flows[1].goodput_mbps;
      std::fprintf(f, "%s,%d,40,160,%.3f,%.3f,%.4f,%.4f\n", CcAlgoName(algo), seed, s, l,
                   s + l > 0 ? s / (s + l) : 0, r.jain);
      std::printf("%-6s s%d 短 RTT %.2f Mbps · 长 RTT %.2f Mbps · 短 RTT 份额 %.3f\n",
                  CcAlgoName(algo), seed, s, l, s + l > 0 ? s / (s + l) : 0);
      std::fflush(stdout);
    }
  }
  std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
  std::string dir = "build/exp";
  std::string only;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) {
      dir = argv[++i];
    } else if (a == "--only" && i + 1 < argc) {
      only = argv[++i];
    } else {
      std::printf("用法: xport_ccexp [--out <dir>] [--only single|single_ts|intra|inter|rtt]\n");
      return 2;
    }
  }
  const auto want = [&](const char* name) { return only.empty() || only == name; };
  if (want("single")) ExpSingle(dir);
  if (want("single_ts")) ExpSingleTs(dir);
  if (want("intra")) ExpIntra(dir);
  if (want("inter")) ExpInter(dir);
  if (want("rtt")) ExpRtt(dir);
  return 0;
}
