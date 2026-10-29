// xport demo —— 在确定性仿真链路上跑一次真实的多流可靠传输。
//
// 用法：
//   xport_demo                       # 8 MB，200 Mbps，RTT 2ms，1% 丢包
//   xport_demo --mb 64 --loss 5 --streams 4
//   xport_demo --help
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "xport/hash.h"
#include "xport/sim.h"
#include "xport/transport.h"

using namespace xport;

namespace {

struct Options {
  double mb = 8.0;
  double loss_pct = 1.0;
  double dup_pct = 0.0;
  double bandwidth_mbps = 200.0;
  double delay_ms = 1.0;
  u32 jitter_us = 0;
  int streams = 1;
  u64 seed = 42;
};

void Usage() {
  std::printf(
      "xport demo —— 基于 UDP 的可靠多流传输（确定性链路仿真）\n\n"
      "  --mb <n>        传输数据量，单位 MB（默认 8）\n"
      "  --streams <n>   并发流数（默认 1）\n"
      "  --loss <pct>    链路丢包率，百分比（默认 1.0）\n"
      "  --dup <pct>     链路重复率，百分比（默认 0）\n"
      "  --bw <mbps>     链路带宽，Mbps（默认 200，0 表示不限速）\n"
      "  --delay-ms <n>  单程延迟，毫秒（默认 1.0）\n"
      "  --jitter <us>   抖动，微秒（默认 0）\n"
      "  --seed <n>      随机种子（默认 42，同种子完全可复现）\n"
      "  --help          显示本帮助\n");
}

bool NextValue(int& i, int argc, char** argv, std::string* out) {
  const std::string a = argv[i];
  const auto eq = a.find('=');
  if (eq != std::string::npos) {
    *out = a.substr(eq + 1);
    return true;
  }
  if (i + 1 >= argc) return false;
  *out = argv[++i];
  return true;
}

bool Parse(int argc, char** argv, Options* o, bool* want_help) {
  *want_help = false;
  for (int i = 1; i < argc; ++i) {
    std::string key = argv[i];
    const auto eq = key.find('=');
    if (eq != std::string::npos) key = key.substr(0, eq);

    std::string val;
    if (key == "--help" || key == "-h") {
      *want_help = true;
      return true;
    } else if (key == "--mb") {
      if (!NextValue(i, argc, argv, &val)) return false;
      o->mb = std::stod(val);
    } else if (key == "--streams") {
      if (!NextValue(i, argc, argv, &val)) return false;
      o->streams = std::stoi(val);
    } else if (key == "--loss") {
      if (!NextValue(i, argc, argv, &val)) return false;
      o->loss_pct = std::stod(val);
    } else if (key == "--dup") {
      if (!NextValue(i, argc, argv, &val)) return false;
      o->dup_pct = std::stod(val);
    } else if (key == "--bw") {
      if (!NextValue(i, argc, argv, &val)) return false;
      o->bandwidth_mbps = std::stod(val);
    } else if (key == "--delay-ms") {
      if (!NextValue(i, argc, argv, &val)) return false;
      o->delay_ms = std::stod(val);
    } else if (key == "--jitter") {
      if (!NextValue(i, argc, argv, &val)) return false;
      o->jitter_us = u32(std::stoul(val));
    } else if (key == "--seed") {
      if (!NextValue(i, argc, argv, &val)) return false;
      o->seed = std::stoull(val);
    } else {
      std::printf("未知参数：%s\n\n", argv[i]);
      return false;
    }
  }
  return true;
}

std::vector<u8> MakeData(std::size_t n, u32 seed) {
  std::vector<u8> v(n);
  u32 x = seed * 2654435761u + 1u;
  for (auto& b : v) {
    x = x * 1103515245u + 12345u;
    b = u8(x >> 16);
  }
  return v;
}

void Rule(char c = '-') {
  for (int i = 0; i < 62; ++i) std::putchar(c);
  std::putchar('\n');
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  bool want_help = false;
  if (!Parse(argc, argv, &o, &want_help)) {
    Usage();
    return 2;
  }
  if (want_help || o.streams <= 0 || o.mb <= 0) {
    Usage();
    return want_help ? 0 : 2;
  }

  const std::size_t total_bytes = std::size_t(o.mb * 1024.0 * 1024.0);
  const std::size_t per_stream = total_bytes / std::size_t(o.streams);

  LinkParams lp;
  lp.loss_pct = o.loss_pct;
  lp.dup_pct = o.dup_pct;
  lp.bandwidth_mbps = o.bandwidth_mbps;
  lp.delay_us = u32(o.delay_ms * 1000.0);
  lp.jitter_us = o.jitter_us;
  lp.seed = o.seed;

  SimLink link(lp);
  Transport a(link, 1, 2);
  Transport b(link, 2, 1);

  std::vector<std::vector<u8>> payloads;
  std::vector<u32> sids;
  std::size_t submitted = 0;
  for (int i = 0; i < o.streams; ++i) {
    const std::size_t n = (i == o.streams - 1) ? (total_bytes - submitted) : per_stream;
    payloads.push_back(MakeData(n, u32(i + 1)));
    sids.push_back(a.OpenStream());
    a.Write(sids.back(), Bytes(payloads.back()));
    submitted += n;
  }

  std::printf("\nxport demo · 基于 UDP 的可靠多流传输（确定性链路仿真）\n");
  Rule('=');
  std::printf(" 数据      %.2f MB × %d 流\n", double(total_bytes) / (1024.0 * 1024.0), o.streams);
  if (o.bandwidth_mbps > 0) {
    std::printf(" 链路      %.0f Mbps · RTT %.1f ms · 丢包 %.1f%% · 重复 %.1f%%\n",
                o.bandwidth_mbps, o.delay_ms * 2.0, o.loss_pct, o.dup_pct);
  } else {
    std::printf(" 链路      不限速 · RTT %.1f ms · 丢包 %.1f%% · 重复 %.1f%%\n",
                o.delay_ms * 2.0, o.loss_pct, o.dup_pct);
  }
  std::printf(" 种子      %llu（同一种子结果完全可复现）\n\n", (unsigned long long)o.seed);

  const auto wall_start = std::chrono::steady_clock::now();

  constexpr Time kStepUs = 20;
  constexpr Time kLimitUs = 120'000'000;  // 120 秒虚拟时间
  Time now = 0;
  Time next_report = 500'000;
  std::vector<u8> pkt;

  // 消费式接收：数据取走后缓冲才回收。不消费会被流控挡住（这是设计行为）。
  // 校验用滚动哈希，避免为了"最后比对一次"把几十 MB 全留在内存里。
  std::vector<u64> rx_hash(sids.size(), kFnvInit);
  std::vector<std::size_t> rx_bytes(sids.size(), 0);
  const auto drain = [&]() {
    for (std::size_t i = 0; i < sids.size(); ++i) {
      for (;;) {
        Bytes v = b.Peek(sids[i]);
        if (v.empty()) break;
        rx_hash[i] = Fnv1aUpdate(rx_hash[i], v);
        rx_bytes[i] += v.size();
        b.Consume(sids[i], v.size());
      }
    }
  };

  while (now < kLimitUs) {
    a.Tick(now);
    b.Tick(now);
    while (link.Poll(1, &pkt, now)) a.OnPacket(Bytes(pkt), now);
    while (link.Poll(2, &pkt, now)) b.OnPacket(Bytes(pkt), now);
    drain();

    if (a.SendComplete()) break;
    now += kStepUs;

    if (now >= next_report) {
      const double pct = 100.0 * double(b.DeliveredBytes()) / double(total_bytes + 1);
      std::printf("\r [ %6.1f ms] 已交付 %5.1f%%   cwnd %6.1f KB   重传 %llu   ",
                  double(now) / 1000.0, pct, a.stats().cwnd_kb,
                  (unsigned long long)a.stats().retransmits);
      std::fflush(stdout);
      next_report += 500'000;
    }
  }
  drain();
  std::printf("\r%62s\r", "");

  const auto wall_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall_start).count();

  // 逐流校验：长度 + 滚动哈希。哈希逐字节累加，等价于全量 memcmp 的强度。
  bool ok = true;
  std::size_t verified = 0;
  for (std::size_t i = 0; i < sids.size(); ++i) {
    if (rx_bytes[i] != payloads[i].size() ||
        rx_hash[i] != Fnv1a(Bytes(payloads[i]))) {
      ok = false;
      break;
    }
    verified += rx_bytes[i];
  }

  const double virtual_s = double(now) / 1e6;
  const double goodput_mbps = double(verified) * 8.0 / virtual_s / 1e6;
  const auto& sa = a.stats();
  const auto& sb = b.stats();
  const double retrans_pct = sa.pkts_sent ? 100.0 * double(sa.retransmits) / double(sa.pkts_sent) : 0.0;

  Rule();
  std::printf(" 虚拟耗时      %.3f s\n", virtual_s);
  std::printf(" 有效吞吐      %.1f Mbps  (%.2f MB/s)\n", goodput_mbps, goodput_mbps / 8.0);
  std::printf(" 发送          %llu 包  (重传 %llu · %.2f%%)\n",
              (unsigned long long)sa.pkts_sent, (unsigned long long)sa.retransmits, retrans_pct);
  std::printf(" 链路丢弃      %llu 包  (%.2f%%)\n", (unsigned long long)link.dropped(),
              sa.pkts_sent ? 100.0 * double(link.dropped()) /
                                 double(sa.pkts_sent + link.dropped())
                           : 0.0);
  std::printf(" 拥塞窗口      %.1f KB  (ssthresh %.1f KB)\n", sa.cwnd_kb, sa.ssthresh_kb);
  std::printf(" 拥塞控制      %s · %s · 事件 %llu 次 · pacing %.0f Mbps\n",
              CcAlgoName(a.cc().algo()), CcPhaseName(a.cc().phase()),
              (unsigned long long)a.cc().CongestionEvents(), sa.pacing_mbps);
  std::printf(" 流控          MAX_DATA 上限 %.1f KB · 已发 %.1f KB · 阻塞 %llu 次\n",
              double(a.FlowLimit()) / 1024.0, double(a.SentHighWater()) / 1024.0,
              (unsigned long long)sa.flow_blocked);
  std::printf(" SRTT          %.2f ms   (RTO %.1f ms)\n", sa.srtt_ms, sa.rto_ms);
  std::printf(" 仿真耗时      %.0f ms  (相对虚拟时间加速 %.0f×)\n", wall_ms,
              wall_ms > 0 ? double(now) / 1000.0 / wall_ms : 0.0);
  std::printf(" 校验          %s  %zu 字节\n", ok ? "[OK] 逐字节一致" : "[FAIL] 数据不一致",
              verified);
  Rule('=');
  std::printf("\n");

  // 附加自检：交付字节数必须等于提交字节数（防重复计数）
  if (sb.bytes_recv != verified) {
    std::printf(" 警告：接收端统计 %llu 与实际 %zu 不一致\n",
                (unsigned long long)sb.bytes_recv, verified);
    ok = false;
  }
  return ok ? 0 : 1;
}
