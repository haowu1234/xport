// 最小可运行示例：在确定性仿真链路上传 1 MiB，逐字节校验。
//
// 编译运行（仓库根目录）：
//   c++ -std=c++20 -O2 -Iinclude src/*.cpp examples/hello_xport.cpp -o build/hello
//   ./build/hello
//
// 换成真实 UDP socket 只需把 Transport 的构造参数从 SimLink 换成 UdpSocket，
// 其余代码一行不改——这是 IoEndpoint 抽象的全部意义。
#include <cstdio>
#include <vector>

#include "xport/sim.h"
#include "xport/transport.h"

int main() {
  // 1) 一条可配置的仿真链路：100 Mbps、单程 5 ms、1% 丢包、固定种子。
  xport::LinkParams link_cfg;
  link_cfg.bandwidth_mbps = 100;
  link_cfg.delay_us = 5000;
  link_cfg.loss_pct = 1.0;
  link_cfg.seed = 42;  // 同种子 → 逐位可复现
  xport::SimLink link(link_cfg);

  // 2) 两个端点。节点号 1 和 2 互为对端。
  xport::Transport sender(link, /*self=*/1, /*peer=*/2);
  xport::Transport receiver(link, /*self=*/2, /*peer=*/1);

  // 3) 开一条流，提交 1 MiB 数据。Write 只是入队，不会立刻发包。
  std::vector<xport::u8> payload(1 << 20);
  for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = xport::u8(i * 31 + 7);

  const xport::u32 sid = sender.OpenStream();
  sender.Write(sid, xport::Bytes(payload));

  // 4) 驱动循环。时间由调用方给，协议自己不读时钟。
  std::vector<xport::u8> got;
  std::vector<xport::u8> pkt;
  xport::Time now = 0;
  for (; now < 60'000'000; now += 100) {  // 虚拟时间上限 60 s
    sender.Tick(now);
    receiver.Tick(now);

    // 把链路上已到达的包交给对应端点
    while (link.Poll(1, &pkt, now)) sender.OnPacket(xport::Bytes(pkt), now);
    while (link.Poll(2, &pkt, now)) receiver.OnPacket(xport::Bytes(pkt), now);

    // 消费式接收：必须 Peek + Consume，否则流控窗口不前进，传输会停住
    for (;;) {
      const xport::Bytes view = receiver.Peek(sid);
      if (view.empty()) break;
      got.insert(got.end(), view.begin(), view.end());
      receiver.Consume(sid, view.size());
    }

    if (sender.SendComplete() && got.size() == payload.size()) break;
  }

  const xport::Stats& s = sender.stats();
  std::printf("耗时 %.1f ms · 吞吐 %.1f Mbps · 发送 %llu 包（重传 %llu）· PTO 超时 %llu 次\n",
              double(now) / 1000.0, double(payload.size()) * 8.0 / double(now),
              (unsigned long long)s.pkts_sent, (unsigned long long)s.retransmits,
              (unsigned long long)s.pto_fires);
  std::printf("校验 %s（%zu 字节）\n", got == payload ? "[OK] 逐字节一致" : "[FAIL] 数据不一致",
              got.size());
  return got == payload ? 0 : 1;
}
