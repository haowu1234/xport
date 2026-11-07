# xport

**基于 UDP 的 C++20 可靠传输框架** — 可靠 · 有序 · 多流复用 · 零第三方依赖

[![CI](https://github.com/haowu1234/xport/actions/workflows/ci.yml/badge.svg)](https://github.com/haowu1234/xport/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)
![tests](https://img.shields.io/badge/tests-48%20passing-3fb950)
![dependencies](https://img.shields.io/badge/dependencies-none-3fb950)

xport 在 UDP 之上实现了一套可靠、有序、支持多流复用的传输协议：协议语义对齐 QUIC / RFC，
线格式完全自研（**不与标准 QUIC 互通**）。同一个 `IoEndpoint` 抽象下有两套后端
——**确定性链路仿真器**与**真实 UDP socket**，上层代码零改动。

它适合：读懂一个现代传输协议是怎么落地的、在可复现的仿真里验证拥塞控制、
拿来做协议实验的底座。它**不适合**直接用于生产：目前没有握手、没有加密（见[能力矩阵](#能力矩阵)）。

- [关键结果](#关键结果) · [架构](#架构) · [快速开始](#快速开始) · [用法](#用法)
- [实测数据](#实测数据) · [能力矩阵](#能力矩阵) · [文档](#文档) · [路线图](#路线图)

---

## 关键结果

| 单流回环吞吐 | 占裸 UDP 天花板 | 协议单包成本 | 热路径堆分配 | 降窗比例偏差 |
|---:|---:|---:|---:|---:|
| **4.4 Gbps** | **81%** | **118 ns** | **0 次/包** | **0.7%** |

<sub>Apple M5 · macOS · Apple clang 21 · `-O2` · 单线程忙轮询（收发共用一核）·
裸 UDP 天花板 5.4 Gbps（批量收发后 0.45 syscall/包）· 13 次拥塞事件的降窗比例全部为 0.700（理论 β = 0.7）·
理论曲线由 Python 按 RFC 独立回算。较慢机器上的绝对值见[实测数据](#实测数据)。</sub>

---

## 架构

![xport 协议架构](docs/figs/architecture-reference.png)

<sub>架构图展示应用、Transport、IoEndpoint 与可替换后端之间的数据路径和反馈闭环。</sub>

**分层原则**：`Transport` 不认识网络。它只依赖 `IoEndpoint` 的
`Send(Bytes, Time)` 与 `Recv(Batch*, Time)`，所以"跑仿真"和"跑真 socket"是同一条代码路径。
这不是为了优雅，是因为只有把时间从端点外部注入，仿真才可能 100% 确定。

**设计亮点**：

- **时间外部注入**。`Send(Bytes, Time)` 的时间由调用方传入，端点不允许自己读时钟。这是确定性仿真的前提——仿真里时间是假的、由驱动方推进，端点缓存一份就必然和协议看到的不一致（早期踩过：所有包时间戳停在 0，RTO 成批触发）。
- **热路径零堆分配**。在途包表、区间表、接收包号全部用定长结构：环形数组 + `pn & mask` 定位、512 段 `RangeSet`、16384 位滑动位图。每包约 118 ns，与在途包数无关。在途包表从 8192 槽起步，包号跨度不够时翻倍（上限 `max_inflight_packets`，默认 131072 包 ≈ 179 MB 在途），扩容是一次性的，不在稳态路径上。
- **包号与字节偏移分离**。重传换新包号、字节位置不变。ACK 只认包号，流控只认偏移——两层各自独立演进，互不阻塞。
- **消费式接收生命周期**。收到的字节写进每流预分配的环形缓冲，`Peek` 零拷贝读、`Consume` 归还。不消费则流控窗口不前进，对端被背压挡住——这是背压的正确实现，不是 bug。
- **每条退化路径都有计数器**。`ack_overflow`、`rx_overflow`、`send_fail`、`stream_limit`…… 凡是"记不下 / 装不下 / 发不出"的地方都必须计数，否则静默降级会变成查不出来的玄学问题。

> 代价：单连接吞吐上限 = 单核。扩展方向是多连接多线程，而不是把单个连接拆成收发两个线程。

### 一个数据包的一生

从应用写入到对端交付，再到 ACK 回流闭环。每一站标注了实际生效的常量。

![一个数据包的一生](docs/figs/packet-lifecycle.svg)

### 关键结构解剖

字节布局与指针语义都按源码绘制：报文头的字段宽度就是真实字节数，
环形缓冲的三个指针对应 `InStream` 的 `consumed` / `contiguous` / `highest_end`。

![关键结构解剖](docs/figs/anatomy.svg)

<sub>两张图分别由 <code>scripts/plot_concept.py</code> 与 <code>scripts/plot_anatomy.py</code> 生成。</sub>

---

## 快速开始

```bash
git clone https://github.com/haowu1234/xport.git && cd xport

# A. 一键编译，无需 CMake（自动挑 g++ / clang++ / ziglang）
./scripts/quickbuild.sh          # 编译 + 跑测试
./scripts/quickbuild.sh --run    # 再跑 demo 与轨迹探针
```

```bash
# B. CMake
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tests/xport_tests        # 48 个用例
./build/apps/xport_demo --mb 16 --streams 4 --loss 2
```

<details>
<summary><b>Windows / MSVC</b></summary>

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\build\tests\Release\xport_tests.exe
.\build\apps\Release\xport_demo.exe --mb 16 --streams 4 --loss 2
```

</details>

<details>
<summary><b>demo 参数</b></summary>

| 参数 | 说明 |
|---|---|
| `--mb <n>` | 数据量（MB，默认 8） |
| `--streams <n>` | 并发流数（默认 1） |
| `--loss <pct>` | 丢包率百分比（默认 1） |
| `--dup <pct>` | 重复率百分比（默认 0） |
| `--bw <mbps>` | 链路带宽（默认 200，0 表示不限速） |
| `--delay-ms <n>` | 单程延迟（默认 1.0） |
| `--jitter <us>` | 抖动微秒（默认 0） |
| `--seed <n>` | 随机种子（默认 42，同种子结果完全可复现） |

</details>

---

## 用法

完整可编译版本见 [`examples/hello_xport.cpp`](examples/hello_xport.cpp)
（`./build/hello_xport`，CMake 和 quickbuild 都会编它）。核心只有四步：

```cpp
#include "xport/sim.h"
#include "xport/transport.h"

// 1) 一条确定性仿真链路：100 Mbps、单程 5 ms、1% 丢包、固定种子
xport::LinkParams cfg;
cfg.bandwidth_mbps = 100;
cfg.delay_us = 5000;
cfg.loss_pct = 1.0;
cfg.seed = 42;                       // 同种子 → 逐位可复现
xport::SimLink link(cfg);

// 2) 两个端点互为对端
xport::Transport sender(link, /*self=*/1, /*peer=*/2);
xport::Transport receiver(link, /*self=*/2, /*peer=*/1);

// 3) 开流、提交数据（Write 只入队，不直接发包）
const xport::u32 sid = sender.OpenStream();
sender.Write(sid, xport::Bytes(payload));

// 4) 驱动循环：时间由调用方注入，协议自己不读时钟
for (xport::Time now = 0; !sender.SendComplete(); now += 100) {
  sender.Tick(now);
  receiver.Tick(now);
  while (link.Poll(1, &pkt, now)) sender.OnPacket(xport::Bytes(pkt), now);
  while (link.Poll(2, &pkt, now)) receiver.OnPacket(xport::Bytes(pkt), now);

  // 消费式接收：Peek 零拷贝读、Consume 归还窗口
  for (;;) {
    const xport::Bytes view = receiver.Peek(sid);
    if (view.empty()) break;
    sink.insert(sink.end(), view.begin(), view.end());
    receiver.Consume(sid, view.size());
  }
}
```

**换成真实 UDP socket**：把 `Transport` 的第一个参数从 `SimLink` 换成 `UdpSocket`
（`Transport(IoEndpoint&, ...)` 那个构造函数），收包改用 `UdpSocket::Recv(Batch*, now)`，
其余逻辑一行不改。

两个容易踩的点，都是有意设计：

- **必须 `Consume`**。不消费则流控窗口不前进，对端被背压挡住，传输会停在中途。
- **`Tick(now)` 的 `now` 必须单调递增**，且由调用方提供。仿真用虚拟时间，真实路径用
  `MonotonicUs()`；端点自己缓存时间会让仿真失去确定性。

---

## 实测数据

### 真实 socket · 本机回环

回环基准回答两件事：**协议在单核上能跑多快**、**丢包恢复行为对不对**。它回答不了拥塞控制
（回环没有队列、没有带宽限制），那部分只能靠确定性仿真。

下面两组数字来自**两台不同的机器**，绝对值不可横比，各自内部可比。

<b>机器 A · Apple M5 / macOS / clang 21</b>（裸 UDP 天花板 5.4 Gbps，回环 RTT 6 µs）

| 场景 | 吞吐 | 备注 |
|---|---:|---|
| 单流 64 MiB | **4358 Mbps** | 天花板的 81%，0.45 syscall/包（批量收发）|
| 单流 64 MiB · 关批量 | 3462 Mbps | 2.22 syscall/包，用 `use_batch=false` 做 A/B |
| 8 条并发流 16 MiB | 4044 Mbps | 关批量 3094 Mbps |
| 1% 丢包 | 1386 Mbps | 0 次 PTO 超时 |
| 2% 丢包 | 1013 Mbps | 修复前 405 Mbps（87–1032，双峰） |
| 5% 丢包 | 541 Mbps | 修复前 5.6 Mbps |
| 10% 丢包 | 279 Mbps | 修复前 1.8 Mbps |

<b>机器 B · 较慢的 x86 机器</b>（裸 UDP 天花板约 1.2 Gbps，回环 RTT 9 µs）——
早期数据，PTO 之前测的，保留用于对照 syscall 分析：

| 场景 | 吞吐 | 备注 |
|---|---:|---|
| 单流 64 MiB | 903.3 Mbps | 长样本 |
| 单流 256 MiB | 881.4 Mbps | 持续满载 2.4 s，略有降频 |
| 小消息 1600 × 200 B | **400 个包** | 帧聚合开启；关闭时需 1600 个包 |
| 2% 丢包 | 49.8 Mbps | 19 次 RTO 停顿 —— 当时缺 PTO，**已修复**（见 ②） |

![丢包率与吞吐](docs/figs/loss-cliff.svg)

> **关于"达成率"要诚实说明**：同一配置连跑 6 次，协议吞吐落在 **828–926 Mbps**，
> 而裸 UDP 基线自己在 **924–1302 Mbps** 之间漂移（41%）。配对后达成率 **64%–95%，中位约 70%**。
> 基线比被测对象抖得更厉害，所以回环单核只能做 ±10% 量级的判断，不足以验证 5% 以内的优化收益。

![六次重复运行](docs/figs/repeat-runs.svg)

**两个结论：**

**① 瓶颈 100% 在系统调用。** 每包 2.22 次 syscall、约 10 µs 进内核；协议自身只有 ~250 ns（<3%）。
回环上 cwnd 曾涨到 8 MB，实际在途却只有 10 KB —— 窗口开得再大也没用。
（这是应用受限时仍增窗的缺陷，已修复：现在窗口用不满就不涨，见能力矩阵「cwnd 验证」。）

**② 2% 丢包掉到 50 Mbps，是缺 PTO。** 回环 RTT 0.15 ms，而 RTO 下限是 200 ms（差 1300 倍）。
窗口尾部的丢包触发不了三次重复 ACK，只能干等 200 ms。五次运行耗时 76/76/276/480/878 ms，
完全由 RTO 次数决定（RTO=0 → 448 Mbps，RTO=16 → 71 Mbps）。
**这个缺陷只有真实 socket 能暴露** —— 仿真里 RTT 是 2–800 ms，200 ms 的下限从来不是瓶颈。

**已修复：RTO 换成 PTO（RFC 9002 §6.2）。** 超时只发两个探测包、不判丢、不降窗，所以去掉了 200 ms 下限；
连续超时指数退避（上限 60 s）；丢包跨度超过 3×PTO 且中间无确认时判持续拥塞，窗口回到最小值。
同一台机器（Apple M5，回环，各 7 次取中位数）前后对比：

| 丢包率 | 修复前 | PTO | 超时次数（中位数） |
|---:|---:|---:|---:|
| 0% / 1% | 3571 / 1379 Mbps | 3573 / 1386 Mbps | 0 → 0 |
| 2% | 405 Mbps（87–1032，双峰） | **1013 Mbps**（955–1027） | 4 → 0 |
| 5% | 5.6 Mbps | **541 Mbps** | 542 → 24 |
| 10% | 1.8 Mbps | **279 Mbps** | 291 → 35 |

（这台机器比上表那台快约 4 倍，绝对值不可横比，看同机前后即可。）

### 确定性仿真

200 Mbps 链路、RTT 2 ms、8 MiB 数据：

```console
$ ./build/xport_demo --mb 8 --loss 1
 有效吞吐      62.6 Mbps  (7.83 MB/s)
 发送          6301 包  (重传 77 · 1.22%)
 拥塞窗口      17.2 KB  (ssthresh 7.8 KB)
 拥塞控制      cubic · cong-avoid · 事件 55 次 · pacing 85 Mbps
 流控          MAX_DATA 上限 10240.0 KB · 已发 8192.0 KB · 阻塞 0 次
 SRTT          2.07 ms   (PTO 5.1 ms · 超时 0 次)
 校验          [OK] 逐字节一致  8388608 字节
```

```console
$ ./build/xport_demo --mb 32 --loss 0
 有效吞吐      195.1 Mbps  (24.39 MB/s)     # 线速的 97.6%
 拥塞控制      cubic · cong-avoid · 事件 5 次
 校验          [OK] 逐字节一致  33554432 字节
```

### 单核成本

| 指标 | 结果 | 说明 |
|---|---|---|
| 单包协议成本 | 收 **72 ns** / 发 **46 ns**（合计约 118 ns） | 单核上限：接收约 160 Gbps、发送约 250 Gbps（不含内核）。`xport_cpubench` 5 次中位数，`f1ad17d` |
| 稳态 ACK 成本 | **0.4–1.0 µs，与在途包数无关** | 优化前 16→4096 包涨 61 倍（0.8→49 µs） |
| 热路径堆分配 | **0 次/包** | 区间表、包号位图、流表全换定长结构 |
| 接收缓冲落地 | 0.49 → **0.03 ns/B** | 每流预分配环形缓冲，8 Gbps 单流仅占单核 29% |
| 回环 RTT | **9.0 µs**（64 B ping-pong） | 旧的 200 ms RTO 下限是 RTT 的 1300 倍（现已换成 PTO） |

### 拥塞控制

CUBIC 的可信度不来自"跑得快"，来自**曲线能被独立算出**。每张图的数据都是固定种子的确定性仿真，
换台机器重跑逐位一致。

![CUBIC 锯齿](docs/figs/cubic-sawtooth.svg)

同样的 110 秒、同一条 5 Mbps / 800 ms 的长肥管道，换算法就能看出差别：

![CUBIC 与 Reno 对比](docs/figs/cubic-vs-reno.svg)

逐事件的降窗比例、K 值与理论曲线偏差，全部列表在 [cc-validation.html](docs/cc-validation.html)。

---

## 文档

| 报告 | 内容 |
|---|---|
| [**udp-benchmark.html**](docs/udp-benchmark.html) | 真实 socket 回环基准 · 11 张图 / 7 张表 |
| [**cc-validation.html**](docs/cc-validation.html) | 拥塞控制正确性验证 · 逐事件降窗比例与理论曲线对照 |
| [**benchmarks.md**](docs/benchmarks.md) | 仿真性能矩阵 · 丢包扫描 / 带宽扫描 / 多流 / CUBIC vs AIMD |
| [**scaling.md**](docs/scaling.md) | 单流带宽上限分析 · 从 1 Gbps 到 400 G 的瓶颈推算 |

**关于验证方法**：CUBIC 的理论曲线由 Python 按 RFC 独立回算，不复用 C++ 实现。
拿自己的实现证明自己正确，是自证循环，不成立。

---

## 能力矩阵

| 能力 | 状态 | 说明 |
|---|:---:|---|
| 20 B 定长头 + CRC16 | ✅ | 解析在一条 cache line 内完成 |
| varint（1/2/4/8 B） | ✅ | 对齐 RFC 9000；超 62 位拒绝而非静默截断 |
| 帧编解码 | ✅ | STREAM / ACK / MAX_DATA / PING，含截断与非法帧防御 |
| 流数上限 | ✅ | `max_streams`（默认 256）；越界 sid 直接丢弃，防止对端一个包撑爆流表 |
| 帧聚合 | ✅ | 一个报文最多 8 段（可跨流，`kMaxSegsPerPacket`），小消息 pps 降 4 倍 |
| ACK | ✅ | **显式区间**（最多 16 段），不是位图窗口 |
| 包级可靠传输 | ✅ | 滑动窗口 + 包阈值 3 + 时间阈值 1.125×RTT |
| 恢复期去重 | ✅ | 降窗前发出的包再丢不算新事件，避免一次拥塞被反复砍 |
| PTO 与重传 | ✅ | RFC 9002 §6.2：SRTT + max(4·RTTVAR, 1 ms) + ack_delay，无下限；超时发 2 个探测包、不降窗；指数退避；重传按 offset 去重 |
| 持续拥塞 | ✅ | RFC 9002 §7.6：丢包跨度 > 3×PTO 且中间无确认 → 窗口回到最小值 |
| 流级重组 | ✅ | 区间合并、乱序缓存、重复包幂等 |
| 接收缓冲生命周期 | ✅ | **消费式**：环形缓冲 + `Peek` 零拷贝 + `Consume` 归还 |
| 发送缓冲生命周期 | ✅ | 64 KB 分块，已确认的连续前缀覆盖整块即释放；长连接内存只与在途量 + 未发量有关 |
| 多流复用 | ✅ | **DRR**，按字节公平 |
| 拥塞控制 | ✅ | **CUBIC**（RFC 9438）+ 快速收敛 + Reno 友好区；可切 Reno 对照 |
| 慢启动退出 | ✅ | **HyStart++**（RFC 9406） |
| cwnd 验证 | ✅ | 应用受限 / 流控受限时不增窗并冻结 CUBIC 时间轴（RFC 9438 §4.8、RFC 7661） |
| pacing | ✅ | 令牌桶，慢启动 2×、拥塞避免 1.25×，突发 8 包 |
| 流控 | ✅ | MAX_DATA 连接级窗口 + 随 ACK 捎带 + 探测防死锁 |
| 真实 socket 后端 | ✅ | `IoEndpoint` + Winsock / BSD socket，非阻塞轮询；CI 覆盖 Linux / macOS / Windows |
| 握手 / CID 协商 / cookie | ❌ | 当前两端静态配对，窗口由配置约定 |
| 加密 | ❌ | v1 明文 |
| 批量收发 | ✅ | Linux `sendmmsg` / macOS `sendmsg_x`（私有 syscall，运行时探测 + 回退）；Windows 待做。单流 +26%，syscall/包 2.22 → 0.45 |
| GSO / GRO | ❌ | 帧聚合已在协议层就绪，缺内核 offload 那部分收益 |

---

<details>
<summary><b>设计约束 — 13 条，每条都对应一个真实踩过的坑</b></summary>

1. **`pkt_num` 与 `stream_offset` 分离**。重传必须换新包号，但字节位置不变。早期用"连续包号"做累积确认，包号空洞永远填不上，`acked_off` 被后续包顶到末尾，中间数据被误判"已确认"而永久丢失。
2. **ACK 帧不能再假设"窗口足够小"**。64 字节缓冲区在 16 段区间时溢出，`EncodeAck` 失败后静默返回，ACK 永久停发，发送端只剩 RTO 空转。
3. **初始 RTO 必须大于链路 RTT**。设成 90 ms 时，RTT=100 ms 的链路上每个包都在 ACK 到达前超时，每个 RTT 降一次窗，三次后塌到最小窗口。
4. **一次拥塞只能降一次窗**。同一窗口多个包同时判丢，每个都算一次事件的话，cwnd 会以 RTT 为周期连续乘 0.7，几秒内指数衰减到最小值。用恢复期去重解决。
5. **仿真器的队列要按"串行化积压"计量**。用"队列里的包数"会把传播途中的包也算进容量，标称 100 ms 的缓冲区实际只剩几十毫秒，HyStart 这类基于 RTT 增长的算法全部失效。
6. **"记不下"必须返回失败**。区间表满了就丢最老记录，而发送端是"先记区间、再释放包槽"——区间没记进去、包已删，那段数据永远不重传。现象极迷惑：200 Mbps 正常，1 Gbps 卡在 3.1 MB 不动。现在 `Add()` 返回 `bool`，失败整段重入重传队列，并计 `ack_overflow`。**能静默降级的地方一定要有计数器。**
7. **测试断言也要能自证**。流控用例原本用 64 KB 窗口断言"一定被挡住过"，但该链路 BDP 只有 25 KB，永远追不上额度 —— 用例一直是假通过。改成 16 KB 才真正触发门控。
8. **接收缓冲必须由应用归还**。每流预分配环形缓冲，`Peek` 取走、`Consume` 归还。收包热路径零缺页（1138 → 421 ns/包），代价是**调用方必须消费**：不消费窗口就不前进，对端被背压挡住。这是有意设计，不是 bug。
9. **流控窗口同时是内存预算**。`initial_max_data` 决定环形缓冲要分配多少。曾有基准把它设成 1 TB 来"关掉流控"，改造后直接 `bad_alloc`；现在硬夹到 1 GB，越界写有 `rx_overflow` 兜底。
10. **仿真器是独立库，不是测试代码**。固定种子 100% 可复现，把"偶发丢包导致的 bug"变成可调试的确定性问题 —— 上面一半缺陷是这样定位的。
11. **IO 端点不能自己缓存时间**。`Send` 没带 `now` 时仿真端点用了缓存的 `now_`（初值 0），所有包时间戳停在 0，RTO 成批触发，36 个测试里 7 个丢包用例全线失败。**时间必须由驱动方给，端点不许自己记。**
12. **RTO 下限在低 RTT 链路上会变成主导项**。200 ms 在 RTT 2 ms 的仿真里是安全网，在 RTT 0.15 ms 的回环上等于让传输反复睡 200 ms。正解是 QUIC 的 PTO（几个 RTT 的探测包，超时不降窗）——已按此实现，5% 丢包回环吞吐从 5.6 Mbps 回到 541 Mbps。
13. **回环验证不了拥塞控制**。回环没有队列、没有带宽限制、没有随机丢包，cwnd 一路涨到上限；每包一次 syscall 让实际在途只有 10 KB，远小于 BDP。回环能验证正确性与单核处理能力，拥塞控制只能靠确定性仿真。

</details>

<details>
<summary><b>目录结构</b></summary>

```
include/xport/
  types.h       基础类型、MTU、Stats
  bytes.h       大端读写 + 边界检查的 Reader/Writer + varint
  crc32c.h      头部校验
  packet.h      报文头与帧编解码（ACK 用显式区间，一包多帧）
  ranges.h      定长区间表 RangeSet（替代 std::map，热路径零分配）
  hash.h        增量 FNV 哈希（流式校验，不保留全量数据）
  cc.h          CUBIC + HyStart++ + pacing 速率（纯数学，可单测）
  io.h          IoEndpoint 抽象
  sim.h         确定性链路仿真 + SimEndpoint
  udp.h         真实 UDP socket 后端（Winsock / BSD）
  transport.h   连接：可靠传输、重组、流控、DRR 调度
src/            对应实现
tests/          test_framework.h（自研，零依赖）+ wire / cc / transport 用例
apps/           xport_demo       演示
                xport_bench      仿真矩阵压测
                xport_ccprobe    轨迹探针
                xport_cpubench   单包 CPU 成本
                xport_udp_bench  真实 socket 回环基准
docs/           四份报告（见上表）
docs/figs/      README 内嵌图表（独立 SVG，由 scripts/plot_*.py 生成）
examples/       hello_xport.cpp  最小可运行示例
scripts/        quickbuild.sh · plot_cc.py · plot_udp.py · plot_readme.py
                plot_arch.py · plot_concept.py · plot_anatomy.py
```

</details>

<details>
<summary><b>复现图表</b></summary>

```bash
# 拥塞控制轨迹 → docs/cc-validation.html
./build/xport_ccprobe --scenario cubic    --out build/traces/cubic.csv
./build/xport_ccprobe --scenario longhaul --algo cubic --out build/traces/longhaul_cubic.csv
./build/xport_ccprobe --scenario longhaul --algo reno  --out build/traces/longhaul_reno.csv
./build/xport_bench --csv build/bench.csv
python scripts/plot_cc.py

# 真实 socket 回环 → docs/udp-benchmark.html
./build/xport_udp_bench --sweep --csv build/udp_bench.csv
python scripts/plot_udp.py

# README 内嵌图表 → docs/figs/*.svg
python scripts/plot_readme.py
```

`xport_udp_bench` 主要参数：`--mb` 数据量 · `--streams` 并发流 ·
`--window` 接收窗口 · `--relay-loss` 进程内转发代理注入真实丢包 ·
`--no-coalesce` 关闭帧聚合做 A/B · `--sweep` 跑完整矩阵

</details>

---

## 路线图

按「收益 ÷ 工作量」排序：

| # | 事项 | 为什么现在是它 |
|---|---|---|
| ✅ | ~~PTO（探测超时）~~ | 已完成：5% 丢包回环吞吐 5.6 → 541 Mbps，见「真实 socket · 本机回环」② |
| ✅ | ~~批量收发（Linux / macOS）~~ | 已完成：单流 3462 → 4358 Mbps，syscall/包 2.22 → 0.45 |
| 1 | **批量收发（Windows）** | `WSASendMsg` + IOCP 预投。语义和同步批量不同（一次调用取回 N 个完成），要单独实现；抽象层已留好接口 |
| 2 | **真实网卡 bench** | 回环测的是 syscall 成本，真实网卡还要加中断、DMA、GRO。到那一步才有"单流 6–7 Gbps"的结论 |
| 3 | **`tc netem` 故障注入 + 24h 长稳 + 与 msquic 对比** | 拿到可信的工程结论，而不是"跑得快" |

---

## 参与贡献

欢迎 issue 和 PR。有一条硬性要求：**性能结论要能复现，正确性结论要有失败用例支撑**
（新用例在修改前的代码上必须失败，否则它守不住任何东西）。
详见 [CONTRIBUTING.md](CONTRIBUTING.md)。

## 参考规范

协议语义对齐下列 RFC，实现与线格式独立：

| RFC | 用到的部分 |
|---|---|
| [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000) | 帧类型语义、varint 编码、流控与多流模型 |
| [RFC 9002](https://www.rfc-editor.org/rfc/rfc9002) | 丢包检测（包阈值 / 时间阈值）、PTO、持续拥塞 |
| [RFC 9438](https://www.rfc-editor.org/rfc/rfc9438) | CUBIC：三次曲线、快速收敛、Reno 友好区 |
| [RFC 9406](https://www.rfc-editor.org/rfc/rfc9406) | HyStart++ 慢启动退出 |
| [RFC 7661](https://www.rfc-editor.org/rfc/rfc7661) | 应用受限时的 cwnd 验证 |

## 许可证

[MIT](LICENSE) © 2026 haowu1234
