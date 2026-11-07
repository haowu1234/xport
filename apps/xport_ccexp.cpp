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
  // 复刻外部实验时用：覆盖默认的链路速率 / 基础单程时延 / 缓冲（queue_us > 0 时忽略 buf_bdp）
  double bw_mbps = kBwMbps;
  u32 one_way_us = kBaseOneWayUs;
  u32 queue_us = 0;
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
  double srtt_p50_ms = 0;  // 所有流 SRTT 每 1 ms 采样一次的中位数（Google IETF 105 的时延口径）
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
  const double bdp_bytes = rs.bw_mbps * 1e6 / 8.0 * (2.0 * rs.one_way_us / 1e6);
  LinkParams lp;
  lp.bandwidth_mbps = rs.bw_mbps;
  lp.delay_us = rs.one_way_us;
  // 缓冲按时间计（SimLink 的约定）：k × BDP 字节 = k × 基础 RTT 的串行化时间。
  // 下限一个 MTU 的串行化时间，否则极浅缓冲（0.02 BDP）会连单个包都放不进
  lp.queue_us = rs.queue_us ? rs.queue_us : u32(rs.buf_bdp * bdp_bytes * 8.0 / rs.bw_mbps);
  lp.queue_us = std::max<u32>(lp.queue_us, u32(double(kMtu) * 8.0 / rs.bw_mbps) + 1);
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
  std::vector<double> srtts;
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
      for (auto& f : flows) {
        if (f.tx->SrttUs() > 0) srtts.push_back(double(f.tx->SrttUs()) / 1000.0);
      }
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
  out.srtt_p50_ms = Percentile(srtts, 0.50);
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

// ---- 6. 复刻已发表的实验配置（与 Google / MPI / KIT 的结果对照用）----
//
// 每组都按原文的拓扑参数搭，原文参数超出仿真器能力的地方按比例缩小并在 CSV 的
// note 列注明。来源见 docs/bbr-report.md 的"与已发表结果对照"一节。
void WriteRep(FILE* f, const char* exp, CcAlgo algo, const std::string& param, int seed,
              const RunResult& r, const std::string& note) {
  std::fprintf(f, "%s,%s,%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%s\n", exp, CcAlgoName(algo),
               param.c_str(), seed, r.flows[0].goodput_mbps, r.total_mbps, r.retx_pct, r.q_avg_ms,
               r.q_p95_ms, r.srtt_p50_ms, r.jain, note.c_str());
}

void ExpReplica(const std::string& dir, const std::string& which) {
  const std::string fname = "rep_" + which + ".csv";
  FILE* f = Open(dir, fname.c_str());
  if (!f) return;
  std::fprintf(f, "exp,algo,param,seed,flow0_mbps,total_mbps,retx_pct,q_avg_ms,q_p95_ms,"
                  "srtt_p50_ms,jain,note\n");
  const auto mb = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%g", v); return std::string(b); };

  if (which == "loss") {
    // Google IETF 105 p6：1 Gbps / min_rtt 100 ms / 1 BDP / 2 min。
    // 按 50 Mbps 缩放，RTT 与缓冲倍数不变。当初是因为 1 Gbps × 100 ms 的 BDP（12.5 MB）超过了
    // 固定 8192 槽的在途包表；表已改为按需扩容，保留缩放是为了与已发表的复刻结果保持可比
    for (CcAlgo algo : kAlgos) {
      for (double loss : {0.001, 0.01, 0.1, 0.5, 1.0, 1.5, 2.0, 3.0, 5.0, 10.0}) {
        for (int s = 0; s < 3; ++s) {
          RunSpec sp;
          sp.flows = {FlowSpec{algo, 0, 0}};
          sp.bw_mbps = 50.0;
          sp.one_way_us = 50'000;
          sp.buf_bdp = 1.0;
          sp.loss_pct = loss;
          sp.seed = u64(1 + s);
          sp.duration_us = 60'000'000;
          sp.measure_from_us = 10'000'000;
          WriteRep(f, "g105_loss", algo, mb(loss), s, Run(sp), "bw scaled 1000->50 Mbps");
        }
        std::printf("rep loss %s %.3f done\n", CcAlgoName(algo), loss);
        std::fflush(stdout);
      }
    }
  } else if (which == "qdelay") {
    // Google IETF 105 p7：2 条同算法流，第 2 条 t=2 s 启动，50 Mbps / 30 ms，缓冲 {1,10,100} BDP，2 min，p50 srtt。
    // 只跑 1 与 10 BDP：当初 100 BDP（18.75 MB）超过了旧的 8 MB max_cwnd 和 8192 槽在途包表，
    // 两者都已放开，但 100 BDP × 2 min 单组要跑十几分钟，暂不加入
    for (CcAlgo algo : kAlgos) {
      for (double buf : {1.0, 10.0}) {
        for (int s = 0; s < 3; ++s) {
          RunSpec sp;
          sp.flows = {FlowSpec{algo, 0, 0}, FlowSpec{algo, 2'000'000, 0}};
          sp.bw_mbps = 50.0;
          sp.one_way_us = 15'000;
          sp.buf_bdp = buf;
          sp.seed = u64(1 + s);
          sp.duration_us = 120'000'000;
          sp.measure_from_us = 2'000'000;
          WriteRep(f, "g105_qdelay", algo, mb(buf), s, Run(sp), "");
        }
        std::printf("rep qdelay %s %.0f done\n", CcAlgoName(algo), buf);
        std::fflush(stdout);
      }
    }
  } else if (which == "coexist") {
    // Google IETF 105 p10：CUBIC t=0、X t=2 s，50 Mbps / 30 ms，缓冲 {0.1,1,2,4,8,16} BDP，3 min
    for (CcAlgo algo : {CcAlgo::kBbr, CcAlgo::kBbr2, CcAlgo::kBbr3}) {
      for (double buf : {0.1, 1.0, 2.0, 4.0, 8.0, 16.0}) {
        for (int s = 0; s < 3; ++s) {
          RunSpec sp;
          sp.flows = {FlowSpec{algo, 2'000'000, 0}, FlowSpec{CcAlgo::kCubic, 0, 0}};
          sp.bw_mbps = 50.0;
          sp.one_way_us = 15'000;
          sp.buf_bdp = buf;
          sp.seed = u64(1 + s);
          sp.duration_us = 180'000'000;
          sp.measure_from_us = 2'000'000;
          WriteRep(f, "g105_coexist", algo, mb(buf), s, Run(sp), "");
        }
        std::printf("rep coexist %s %.1f done\n", CcAlgoName(algo), buf);
        std::fflush(stdout);
      }
    }
  } else if (which == "retx") {
    // Google IETF 105 p11：N 条同算法流，1 Gbps / 100 ms / 0.02 BDP，5 min，重传率。
    // 按 50 Mbps 缩放（0.02 BDP = 12.5 KB ≈ 9 包），N ∈ {1, 10}，60 s
    for (CcAlgo algo : kAlgos) {
      for (int n : {1, 10}) {
        for (int s = 0; s < 3; ++s) {
          RunSpec sp;
          for (int i = 0; i < n; ++i) sp.flows.push_back(FlowSpec{algo, 0, 0});
          sp.bw_mbps = 50.0;
          sp.one_way_us = 50'000;
          sp.buf_bdp = 0.02;
          sp.seed = u64(1 + s);
          sp.duration_us = 60'000'000;
          sp.measure_from_us = 5'000'000;
          WriteRep(f, "g105_retx", algo, std::to_string(n), s, Run(sp), "bw scaled 1000->50 Mbps");
        }
        std::printf("rep retx %s n=%d done\n", CcAlgoName(algo), n);
        std::fflush(stdout);
      }
    }
  } else if (which == "rtt") {
    // MPI IETF 121 p37：2 条同算法流，RTT 160 对 10 ms、160 对 80 ms，100 Mbps，缓冲 2 BDP。
    // 原文没写 BDP 按哪条流的 RTT 算，这里两种都跑（ref=short / ref=long）
    for (CcAlgo algo : kAlgos) {
      for (int pair : {10, 80}) {
        for (int ref_long : {0, 1}) {
          for (int s = 0; s < 3; ++s) {
            RunSpec sp;
            sp.bw_mbps = 100.0;
            sp.one_way_us = 5'000;  // 基础 RTT 10 ms，长 RTT 流在数据方向额外加时延
            const u32 extra_short = u32((pair - 10) * 1000);
            sp.flows = {FlowSpec{algo, 0, extra_short}, FlowSpec{algo, 0, 150'000}};
            const double ref_rtt_s = (ref_long ? 160 : pair) / 1000.0;
            sp.queue_us = u32(2.0 * ref_rtt_s * 1e6);  // 2 × BDP 换算成时间就是 2 × RTT
            sp.seed = u64(1 + s);
            sp.duration_us = 60'000'000;
            sp.measure_from_us = 20'000'000;
            const std::string param = std::to_string(pair) + "v160/ref=" + (ref_long ? "160" : std::to_string(pair));
            WriteRep(f, "mpi121_rtt", algo, param, s, Run(sp), "");
          }
        }
        std::printf("rep rtt %s %d done\n", CcAlgoName(algo), pair);
        std::fflush(stdout);
      }
    }
  } else if (which == "long") {
    // 本报告 3.2 节的深缓冲共存（同时启动）跑到 3 min，看 25 s 窗口是不是没到稳态
    for (CcAlgo algo : {CcAlgo::kBbr, CcAlgo::kBbr2, CcAlgo::kBbr3}) {
      for (double buf : {8.0, 16.0}) {
        for (int s = 0; s < 3; ++s) {
          RunSpec sp;
          sp.flows = {FlowSpec{algo, 0, 0}, FlowSpec{CcAlgo::kCubic, 0, 0}};
          sp.buf_bdp = buf;
          sp.seed = u64(1 + s);
          sp.duration_us = 180'000'000;
          sp.measure_from_us = 120'000'000;  // 只看最后 60 s
          WriteRep(f, "long_coexist", algo, mb(buf), s, Run(sp), "measure 120-180 s");
        }
        std::printf("rep long %s %.0f done\n", CcAlgoName(algo), buf);
        std::fflush(stdout);
      }
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
      std::printf("用法: xport_ccexp [--out <dir>] [--only single|single_ts|intra|inter|rtt|"
                  "rep_loss|rep_qdelay|rep_coexist|rep_retx|rep_rtt|rep_long]\n");
      return 2;
    }
  }
  const auto want = [&](const char* name) { return only.empty() || only == name; };
  if (want("single")) ExpSingle(dir);
  if (want("single_ts")) ExpSingleTs(dir);
  if (want("intra")) ExpIntra(dir);
  if (want("inter")) ExpInter(dir);
  if (want("rtt")) ExpRtt(dir);
  // 复刻实验不在默认集合里（单组最长约数分钟），用 --only rep_<name> 单独跑
  for (const char* r : {"loss", "qdelay", "coexist", "retx", "rtt", "long"}) {
    if (only == std::string("rep_") + r) ExpReplica(dir, r);
  }
  return 0;
}
