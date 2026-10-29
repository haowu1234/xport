// xport_ccprobe —— 拥塞控制轨迹探针。
//
// 目的不是压测吞吐，而是把 cwnd / inflight / RTT / 相位逐毫秒记下来，
// 用来回答一个问题：**实现出来的拥塞控制，行为是不是和 RFC 9438 写的一致**。
//
// 三个场景各有分工：
//   cubic    固定带宽、无随机丢包、只在一个时间窗内整段丢包 → 制造"孤立的单次拥塞事件"，
//            于是可以先看降窗比例、再看三次曲线回涨，不受其它事件干扰。
//   hystart  深队列、零丢包 → 检验 HyStart++ 能否在队列打满前退出慢启动（判定依据：队列零丢弃）
//   sawtooth 稳态随机丢包 → 检验连续事件下的锯齿形状与恢复期去重
//
// 用法：
//   xport_ccprobe --scenario cubic --out build/traces/cubic.csv
//   xport_ccprobe --scenario sawtooth --algo reno --out build/traces/sawtooth_reno.csv
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xport/cc.h"
#include "xport/hash.h"
#include "xport/sim.h"
#include "xport/transport.h"

using namespace xport;

namespace {

struct Scenario {
  const char* name;
  double bw_mbps;
  double data_mb;
  u32 delay_us;      // 单程
  u32 queue_us;      // 出口队列深度（按带宽折算）
  double loss_pct;
  double drop_at_ms;  // 确定性丢包窗口起点，<0 表示不使用
  double drop_len_ms;
  Time limit_us;
  Time sample_us;
  Time step_us;  // 仿真步长：RTT 越大可以放得越粗
};

const Scenario kScenarios[] = {
    // 20 Mbps × RTT 100ms → BDP 250 KB ≈ 183 段，K ≈ 5 s，一段完整三次曲线装得下
    {"cubic", 20.0, 30.0, 20'000, 30'000, 0.0, 4000.0, 80.0, 20'000'000, 5'000, 10},
    {"hystart", 20.0, 6.0, 20'000, 500'000, 0.0, -1.0, 0.0, 4'000'000, 1'000, 10},
    {"sawtooth", 20.0, 20.0, 20'000, 30'000, 0.05, -1.0, 0.0, 20'000'000, 5'000, 10},
    // 长肥管道：5 Mbps × RTT 800 ms → BDP 500 KB，与 2 Gbps×2 ms 的管道等价，
    // 但传输 68 MB 只要 110 s，内存占用可控——这样才跑得完一个完整的三次曲线。
    {"longhaul", 5.0, 60.0, 400'000, 600'000, 0.0, 10000.0, 800.0, 110'000'000, 100'000, 200},
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

double Kb(u64 bytes) { return double(bytes) / 1024.0; }

}  // namespace

int main(int argc, char** argv) {
  std::string scenario = "cubic";
  std::string algo = "cubic";
  std::string out;
  std::string hystart = "on";

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--scenario" && i + 1 < argc) {
      scenario = argv[++i];
    } else if (a == "--algo" && i + 1 < argc) {
      algo = argv[++i];
    } else if (a == "--hystart" && i + 1 < argc) {
      hystart = argv[++i];
    } else if (a == "--out" && i + 1 < argc) {
      out = argv[++i];
    } else {
      std::printf("用法: xport_ccprobe [--scenario cubic|hystart|sawtooth] [--algo cubic|reno] "
                  "[--out <csv>]\n");
      return 2;
    }
  }

  const Scenario* sc = nullptr;
  for (const auto& s : kScenarios) {
    if (scenario == s.name) sc = &s;
  }
  if (sc == nullptr) {
    std::printf("未知场景: %s\n", scenario.c_str());
    return 2;
  }

  TransportConfig tcfg;
  tcfg.cc.algo = (algo == "reno") ? CcAlgo::kReno : CcAlgo::kCubic;
  tcfg.cc.hystart = (hystart != "off");

  LinkParams lp;
  lp.bandwidth_mbps = sc->bw_mbps;
  lp.delay_us = sc->delay_us;
  lp.queue_us = sc->queue_us;
  lp.loss_pct = sc->loss_pct;
  lp.seed = 2026;
  if (sc->drop_at_ms >= 0) {
    lp.loss_from_us = Time(sc->drop_at_ms * 1000.0);
    lp.loss_to_us = lp.loss_from_us + Time(sc->drop_len_ms * 1000.0);
  }

  SimLink link(lp);
  Transport a(link, 1, 2, tcfg);
  Transport b(link, 2, 1, tcfg);

  const std::size_t bytes = std::size_t(sc->data_mb * 1024.0 * 1024.0);
  auto payload = MakeData(bytes, 3);
  const u32 sid = a.OpenStream();
  a.Write(sid, Bytes(payload));

  FILE* f = out.empty() ? nullptr : std::fopen(out.c_str(), "w");
  if (f != nullptr) {
    std::fprintf(f,
                 "t_ms,cwnd_kb,ssthresh_kb,target_kb,inflight_kb,srtt_ms,pacing_mbps,phase,round,"
                 "w_max_kb,k_s,epoch_ms,queue_pkts,retrans,lost,delivered_kb,bw_mbps,rtt_ms,"
                 "pre_cong_kb\n");
  }

  const double bw_bdp_kb = sc->bw_mbps * 1e6 / 8.0 * (2.0 * double(sc->delay_us) / 1e6) / 1024.0;

  std::vector<u8> pkt;
  Time next_sample = 0;
  u64 last_events = 0;
  Time now = 0;

  // 消费式接收：边收边比对前缀，不保留全量数据（长肥管道场景一跑就是 60 MB）
  std::size_t rx_off = 0;
  bool prefix_ok = true;
  const auto drain = [&]() {
    for (;;) {
      Bytes v = b.Peek(sid);
      if (v.empty()) break;
      if (rx_off < payload.size()) {
        const std::size_t n = std::min(v.size(), payload.size() - rx_off);
        if (std::memcmp(v.data(), payload.data() + rx_off, n) != 0) prefix_ok = false;
      }
      rx_off += v.size();
      b.Consume(sid, v.size());
    }
  };

  for (; now < sc->limit_us; now += sc->step_us) {
    a.Tick(now);
    b.Tick(now);
    while (link.Poll(1, &pkt, now)) a.OnPacket(Bytes(pkt), now);
    while (link.Poll(2, &pkt, now)) b.OnPacket(Bytes(pkt), now);
    drain();
    if (a.SendComplete()) break;

    if (now >= next_sample) {
      next_sample = now + sc->sample_us;
      const CubicCc& cc = a.cc();
      if (f != nullptr) {
        std::fprintf(f, "%.3f,%.2f,%.2f,%.2f,%.2f,%.3f,%.2f,%d,%llu,%.2f,%.4f,%.3f,%zu,%llu,%llu,"
                        "%.2f,%.1f,%.3f,%.2f\n",
                     double(now) / 1000.0, Kb(cc.cwnd()), Kb(cc.ssthresh()),
                     Kb(u64(cc.TargetBytes(now, a.SrttUs()))), Kb(a.InFlight()),
                     double(a.SrttUs()) / 1000.0,
                     double(cc.PacingRateBps(a.SrttUs())) * 8.0 / 1e6, int(cc.phase()),
                     (unsigned long long)cc.RoundCount(), Kb(u64(cc.WMaxSegments() * tcfg.cc.mss)),
                     cc.KSeconds(), double(cc.EpochStart()) / 1000.0,
                     link.queue_depth(2, now), (unsigned long long)a.stats().retransmits,
                     (unsigned long long)a.stats().lost, Kb(b.DeliveredBytes()), sc->bw_mbps,
                     2.0 * double(sc->delay_us) / 1000.0, Kb(cc.LastPreCongestionCwnd()));
      }
      last_events = cc.CongestionEvents();
      (void)last_events;
    }
  }

  if (f != nullptr) std::fclose(f);

  const auto& sa = a.stats();
  const double secs = double(now) / 1e6;
  const double mbps = secs > 0 ? double(b.DeliveredBytes()) * 8.0 / secs / 1e6 : 0.0;
  // 时间窗结束时传输可能还没跑完：只要已交付部分是**逐字节正确的前缀**，
  // 就说明协议没有出错，只是窗口把时间截断了。前缀比对在 drain 里逐段完成。
  const std::size_t cmp = std::min(rx_off, payload.size());
  const bool complete = rx_off == payload.size();
  const bool all_ok = prefix_ok;

  std::printf("\n场景 %-9s 算法 %-5s  链路 %.0f Mbps · RTT %.0f ms · 丢包 %.2f%%\n", sc->name,
              CcAlgoName(tcfg.cc.algo), sc->bw_mbps, 2.0 * double(sc->delay_us) / 1000.0,
              sc->loss_pct);
  std::printf("  BDP 理论值        %.1f KB\n", bw_bdp_kb);
  std::printf("  虚拟时长          %.2f s\n", secs);
  std::printf("  交付吞吐          %.2f Mbps  (链路 %.0f Mbps 的 %.1f%%)\n", mbps, sc->bw_mbps,
              sc->bw_mbps > 0 ? 100.0 * mbps / sc->bw_mbps : 0.0);
  std::printf("  拥塞事件          %llu 次（平均间隔 %.0f ms）\n",
              (unsigned long long)a.cc().CongestionEvents(),
              a.cc().CongestionEvents() ? secs * 1000.0 / double(a.cc().CongestionEvents()) : 0.0);
  std::printf("  丢包/重传         %llu / %llu  (重传率 %.2f%%)\n", (unsigned long long)sa.lost,
              (unsigned long long)sa.retransmits,
              sa.pkts_sent ? 100.0 * double(sa.retransmits) / double(sa.pkts_sent) : 0.0);
  std::printf("  队列丢弃          %llu 包（尾丢弃 = 慢启动冲过头）\n",
              (unsigned long long)link.queue_drops());
  std::printf("  最终 cwnd         %.1f KB  (BDP 的 %.0f%%)\n", Kb(a.cc().cwnd()),
              bw_bdp_kb > 0 ? 100.0 * Kb(a.cc().cwnd()) / bw_bdp_kb : 0.0);
  std::printf("  交付进度          %.1f%%  (%s)\n",
              100.0 * double(cmp) / double(payload.size()),
              complete ? "已完成" : "时间窗结束，未跑完");
  std::printf("  校验              %s\n",
              !prefix_ok ? "[FAIL] 数据不一致"
                         : (complete ? "[OK] 逐字节一致" : "[OK] 已交付部分逐字节一致"));
  if (!out.empty()) std::printf("  轨迹              %s\n", out.c_str());
  std::printf("\n");
  return all_ok ? 0 : 1;
}
