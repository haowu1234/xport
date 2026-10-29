# xport

基于 UDP 的 C++20 可靠传输框架：**可靠、有序、多流复用**。

当前是 **M3 可运行切片**——协议核心（可靠传输 / 重组 / 多流）、拥塞控制（CUBIC + HyStart++）、
pacing、流控与 DRR 调度全部跑通，并已接入**真实 UDP socket**（WinSock / BSD socket 双平台）。
附带确定性链路仿真器、36 个测试用例、可复现 demo，以及两份报告：
[拥塞控制验证](docs/cc-validation.html) 与 [真实 socket 回环基准](docs/udp-benchmark.html)。

## 真实 socket 实测（本机回环）

单流、单线程忙轮询、127.0.0.1，全部数据逐流校验通过：

| 场景 | 吞吐 | 相对裸 UDP 天花板 |
|---|---:|---:|
| **单流峰值**（8 MB） | **923.9 Mbps** | **87%**（天花板 1082 Mbps） |
| 单流 32 MB | 906.7 Mbps | 85% |
| 8 条并发流（16 MB） | 864.6 Mbps | 81% |
| 小消息 1600 条 200 B | **400 个包**（聚合关 1600 个） | 包数降 4 倍 |
| 1% 真实丢包 | 463.7 Mbps | 重传 32 包即恢复 |
| **2% 真实丢包** | **49.8 Mbps** | ⚠️ 19 次 RTO 停顿，见下 |

**最重要的两个结论**：

1. **瓶颈 100% 在系统调用**：每包 2.22 次 syscall、约 10 µs 进内核，协议自身只占 ~250 ns（<3%）。
   cwnd 涨到 8 MB 而实际在途只有 10 KB——窗口开得再大也没用。
2. **2% 丢包掉到 50 Mbps 不是拥塞控制的问题，是缺 PTO**：链路 RTT 只有 0.15 ms，
   而 RTO 下限是 200 ms（差 1300 倍）。窗口尾部的丢包无法触发"三次重复 ACK"，
   只能等满 200 ms。同一个 2% 配置五次运行是 76/76/276/480/878 ms——纯随机。
   这个缺陷**只有真实 socket 能暴露**，仿真里 RTT 是 2–800 ms，200 ms 的下限从来不是瓶颈。

完整报告（8 张图 / 5 张表）见 **[docs/udp-benchmark.html](docs/udp-benchmark.html)**。

## 仿真实测

200 Mbps 链路、RTT 2 ms、8 MB 数据：

```
$ ./build/xport_demo --mb 8 --loss 1
 有效吞吐      62.6 Mbps  (7.83 MB/s)
 发送          6301 包  (重传 77 · 1.22%)
 拥塞窗口      17.2 KB  (ssthresh 7.8 KB)
 拥塞控制      cubic · cong-avoid · 事件 55 次 · pacing 85 Mbps
 流控          MAX_DATA 上限 10240.0 KB · 已发 8192.0 KB · 阻塞 0 次
 SRTT          2.07 ms   (RTO 200.0 ms)
 校验          [OK] 逐字节一致  8388608 字节
```

```
$ ./build/xport_demo --mb 32 --loss 0
 有效吞吐      195.1 Mbps  (24.39 MB/s)     # 线速的 97.6%
 拥塞控制      cubic · cong-avoid · 事件 5 次
 校验          [OK] 逐字节一致  33554432 字节
```

```
$ ./build/xport_demo --mb 16 --streams 4 --loss 1
 有效吞吐      64.2 Mbps
 校验          [OK] 逐字节一致  16777216 字节    # 4 条流全部逐字节一致
```

丢包率 0–20% 扫描、带宽扫描、并发流数、CUBIC vs AIMD、重复性测试，见
**[docs/benchmarks.md](docs/benchmarks.md)**（`./build/xport_bench --csv bench.csv` 可复现）。

**CUBIC 是否实现正确**，见 **[docs/cc-validation.html](docs/cc-validation.html)**：
逐事件的降窗比例、K 值、实测曲线与独立回算的理论曲线偏差，全部列表给出。

## 三个关键数字

| 指标 | 结果 | 说明 |
|---|---|---|
| 乘性降窗比例 | **0.700**（13 次事件全部一致） | 理论 beta = 0.7，误差 < 0.001 |
| cwnd 与理论曲线偏差 | 平均 **0.7%**，最大 **1.5%** | 理论由 Python 独立回算，不复用 C++ 实现 |
| 5 Gbps 链路利用率 | **69.1%**（AIMD 同条件 27.0%） | CUBIC 把窗口增长从"每 RTT 线性"改成"按时间三次" |
| 长肥管道回涨速率 | **15.7 段/s** vs Reno 1.25 段/s | BDP 488 KB，110 s 内 11 个周期，见报告 ⑤⑥ |
| 单包协议成本 | 收 **0.42 µs** / 发 **1.05 µs** | 单核上限：接收 **27.5 Gbps**、发送 56 Gbps（不含内核） |
| 稳态 ACK 成本 | **0.4–1.0 µs，与在途包数无关** | 优化前 16→4096 包涨 61 倍（0.8→49 µs） |
| 热路径堆分配 | **0 次/包**（原 2 次/包、34 次/ACK） | 区间表、包号位图、流表全部换成定长结构 |
| 接收缓冲 | **每流预分配环形缓冲**，应用消费后回收 | 落地成本 0.49 → 0.03 ns/B，8 Gbps 单流只占单核 29% |
| 小消息 pps | 1600 条 200 B 消息只发 **400 个包** | 帧聚合开启；关闭时 1600 个包 |
| 真实 socket 单流 | **923.9 Mbps**（裸 UDP 1082 Mbps） | 每包 2.22 次 syscall，协议自身开销 <3% |
| 真实 socket 回环 RTT | **9.0 µs**（64 B ping-pong） | 决定了 RTO 下限 200 ms 是 RTT 的 1300 倍 |

## 快速开始

两种方式，任选其一。

**A. 一键编译（无需 CMake，自动挑编译器）**

```bash
./scripts/quickbuild.sh          # 自动使用 g++ / clang++ / ziglang，编完直接跑测试
./scripts/quickbuild.sh --run    # 再跑一遍 demo 与轨迹探针
```

**B. CMake**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tests/xport_tests
./build/apps/xport_demo
```

Windows（MSVC）：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\build\tests\Release\xport_tests.exe
.\build\apps\Release\xport_demo.exe --mb 16 --streams 4 --loss 2
```

**demo 参数**

```
--mb <n>        数据量（MB，默认 8）
--streams <n>   并发流数（默认 1）
--loss <pct>    丢包率百分比（默认 1）
--dup <pct>     重复率百分比（默认 0）
--bw <mbps>     链路带宽（默认 200，0 表示不限速）
--delay-ms <n>  单程延迟（默认 1.0）
--jitter <us>   抖动微秒（默认 0）
--seed <n>      随机种子（默认 42，同种子结果完全可复现）
```

**轨迹探针**（生成验证图表用）

```bash
./build/xport_ccprobe --scenario cubic --out build/traces/cubic.csv
./build/xport_ccprobe --scenario sawtooth --out build/traces/sawtooth.csv
./build/xport_ccprobe --scenario hystart --hystart on  --out build/traces/hystart_on.csv
./build/xport_ccprobe --scenario hystart --hystart off --out build/traces/hystart_off.csv
./build/xport_ccprobe --scenario longhaul --algo cubic --out build/traces/longhaul_cubic.csv
./build/xport_ccprobe --scenario longhaul --algo reno  --out build/traces/longhaul_reno.csv
./build/xport_bench --csv build/bench.csv
python scripts/plot_cc.py         # → docs/cc-validation.html
```

**真实 socket 回环基准**

```bash
./build/xport_udp_bench --sweep --csv build/udp_bench.csv   # 全部矩阵
./build/xport_udp_bench --mb 32 --trace build/traces/udp_direct.csv
./build/xport_udp_bench --mb 4 --relay-loss 1 --trace build/traces/udp_loss1.csv
python scripts/plot_udp.py        # → docs/udp-benchmark.html
```

```
--mb <n>            数据量（MB，默认 8）
--streams <k>       并发流数（默认 1）
--window <MB>       流控/接收窗口（默认 4）
--msg <n> --rounds  小消息模式（RPC 形态，测帧聚合）
--relay-loss <pct>  用进程内转发代理注入真实丢包
--no-coalesce       关闭帧聚合（A/B 对照）
--sweep             跑完整矩阵（数据量 / 多流 / RPC / 丢包）
```

## 目录

```
include/xport/
  types.h       基础类型、MTU、Stats
  bytes.h       大端读写 + 带边界检查的 Reader/Writer + varint
  crc32c.h      头部校验
  packet.h      报文头与帧编解码（ACK 用显式区间，一包多帧）
  ranges.h      定长区间表 RangeSet（替代 std::map，热路径零分配）
  hash.h        增量 FNV 哈希（流式校验用，不保留全量数据）
  cc.h          CUBIC + HyStart++ + pacing 速率（纯数学，可单测）
  io.h          IoEndpoint 抽象（按"完成批量"定义，不按 recvmmsg 语义）
  sim.h         确定性链路仿真 + SimEndpoint（把仿真接成同一个 IO 接口）
  udp.h         真实 UDP socket 后端（Winsock / BSD socket）
  transport.h   连接：可靠传输、重组、流控、DRR 调度
src/            对应实现
tests/          test_framework.h（自研，零依赖）+ wire / cc / transport 用例
apps/           xport_demo（演示）、xport_bench（仿真矩阵压测）、xport_ccprobe（轨迹探针）、
                xport_cpubench（单包 CPU 成本）、xport_udp_bench（真实 socket 回环基准）
docs/           benchmarks.md（仿真性能报告）、cc-validation.html（拥塞控制验证）、
                udp-benchmark.html（真实 socket 回环基准）、
                scaling.md（单流带宽上限分析）
scripts/        quickbuild.sh、plot_cc.py、plot_udp.py
```

## 已实现 / 未实现

| 能力 | 状态 | 说明 |
|---|---|---|
| 20B 定长头 + CRC16 | ✅ | 解析在一条 cache line 内完成 |
| varint（1/2/4/8B） | ✅ | 对齐 RFC 9000 编码；超 62 位拒绝而非静默截断 |
| 帧编解码 | ✅ | STREAM / ACK / MAX_DATA / PING，含截断与非法帧防御 |
| 帧聚合 | ✅ | 一个报文最多 16 个 STREAM 帧（可跨流），小消息 pps 降 4 倍 |
| ACK | ✅ | **显式区间**（最多 16 段），不是位图窗口 |
| 包级可靠传输 | ✅ | 滑动窗口 + 包阈值 3 + 时间阈值 1.125×RTT |
| 恢复期（recovery period） | ✅ | 降窗前发出的包再丢不算新事件，避免一次拥塞被反复砍 |
| RTO 与重传 | ✅ | RFC 6298：初始 1 s、下限 200 ms；重传按 offset 去重 |
| 流级重组 | ✅ | 区间合并、乱序缓存、重复包幂等 |
| 接收缓冲生命周期 | ✅ | **消费式**：每流预分配环形缓冲，`Peek` 零拷贝取走、`Consume` 归还 |
| 多流复用 | ✅ | **DRR**（deficit round robin），按字节公平 |
| 拥塞控制 | ✅ | **CUBIC**（RFC 9438）+ 快速收敛 + Reno 友好区；可切 Reno 做对照 |
| 慢启动退出 | ✅ | **HyStart++**（RFC 9406）：RTT 抬升 → 保守慢启动 → 拥塞避免 |
| pacing | ✅ | 令牌桶，慢启动 2×、拥塞避免 1.25×，突发 8 包 |
| 流控 | ✅ | MAX_DATA 连接级窗口 + 随每个 ACK 捎带 + 流控探测防死锁 |
| 链路模型 | ✅ | 带宽串行化 + 排队延迟（按串行化积压计）+ 尾丢弃 |
| 握手 / CID 协商 / cookie | ❌ | 当前两端静态配对，流控窗口由配置约定 |
| 加密 | ❌ | v1 明文（已确认） |
| GSO / GRO | ❌ | 帧聚合已在协议层就绪，但要靠内核 offload 才能拿到 syscall 那份收益 |
| 真实 socket 后端 | ✅ | `IoEndpoint` 抽象 + Winsock / BSD socket 后端，非阻塞轮询 |
| 同一份代码双后端 | ✅ | 仿真与真实 socket 走同一个 `IoEndpoint`，上层零改动 |
| 批量收发（sendmmsg / IOCP） | ❌ | 当前每包一次 `send`/`recv`，是真实链路上的第一瓶颈 |
| PTO（探测超时） | ❌ | 只有 RTO；低 RTT 链路上 200 ms 下限会主导耗时（见回环报告） |

## 设计约束（每一条都对应一个真实踩过的坑）

1. **`pkt_num` 与 `stream_offset` 分离**。重传必须换新包号，但字节位置不变。早期版本用"连续包号"做累积确认，结果包号空洞永远填不上，`acked_off` 被后续包顶到末尾，中间数据被误判为"已确认"而永久丢失。现在确认状态按 **offset 区间**记录，和接收端的重组结构对称。
2. **ACK 帧不能再假设"窗口足够小"**。64 字节的 ACK 缓冲区在 16 段区间时会溢出，`EncodeAck` 失败后如果静默返回，ACK 就永久停发、发送端只剩 RTO 空转。现在缓冲区按上界分配，编码失败也会清掉 pending 位而不是原地空转。
3. **初始 RTO 必须大于链路 RTT**。M3 一开始把初始 RTO 设成 90 ms，在 RTT=100 ms 的链路上每个包都会在 ACK 到达前被判超时，于是每个 RTT 降一次窗，三次之后就塌到最小窗口，链路完全跑不动。现在按 RFC 6298 用 1 s 初始值、200 ms 下限。
4. **一次拥塞只能降一次窗**。丢包检测会把同一窗口里的多个包同时判丢，如果每个都算一次拥塞事件，cwnd 会以 RTT 为周期被连续乘以 0.7，几秒内指数衰减到最小值。现在用恢复期去重（包发送时刻早于上次降窗时刻则忽略）。
5. **仿真器的队列要按"串行化积压"计量**。早期版本用"队列里的包数"当缓冲区长度，把已经在传播途中的包也算进容量，于是标称 100 ms 的缓冲区实际只剩几十毫秒，排队延迟涨不起来，HyStart 这类基于 RTT 增长的算法在仿真里全部失效。
6. **"记不下"必须返回失败，不能静默丢弃**。区间表最初设成满了就丢最老的记录，而发送端是"先记区间、再释放包槽"——区间没记进去、包却已经删了，那段数据永远不会被重传。表现极具迷惑性：200 Mbps 档完全正常，1 Gbps 档卡在 3.1 MB 后再也不动。现在 `Add()` 返回 `bool`，失败就把整包段塞回重传队列，并计入 `ack_overflow`。**能静默降级的地方一定要有计数器**。
7. **测试断言也要能自证**。流控用例原本用 64 KB 窗口断言"一定被挡住过"，但该链路 BDP 只有 25 KB，发送端永远追不上额度——用例一直是假通过。改成 16 KB（低于 BDP）才真正触发门控。
8. **接收缓冲必须由应用归还**。协议不再替应用保存全部数据：每流一个预分配环形缓冲，`Peek` 取走、`Consume` 归还。好处是收包热路径零缺页（接收成本 1 138 → 421 ns/包），代价是**调用方必须消费**——不消费，流控窗口就不前进，对端会被背压挡住。这是有意的设计，不是 bug。
9. **流控窗口同时是内存预算**。`initial_max_data` 现在决定每流环形缓冲要分配多少。曾经有个基准把它设成 1 TB 来"关掉流控"，改造后直接 `bad_alloc`；现在窗口上限硬夹到 1 GB，越界写有 `rx_overflow` 计数兜底。
10. **仿真器是独立库，不是测试代码**。固定种子可 100% 可复现，让"偶发丢包导致的 bug"变成可调试的确定性问题——上面几个缺陷都是这么定位的。
11. **IO 端点不能自己缓存时间**。把 `SimLink` 抽成 `IoEndpoint` 时，`Send` 一开始没带 `now` 参数，仿真端点用自己缓存的 `now_`（初始 0）——所有包的时间戳都停在 0，RTO 立刻成批触发。表现是 36 个测试里 7 个丢包用例全线失败、每个只传到 60 KB。现在 `Send(Bytes, Time)` 显式传时间：**时间必须由驱动方给，端点不许自己记**。
12. **RTO 下限在低 RTT 链路上会变成主导项**。`min_rto_us = 200 ms` 在 RTT 2 ms 的仿真里是安全网，在 RTT 0.15 ms 的回环上等于让传输反复睡 200 ms。2% 丢包时五次运行耗时 76/76/276/480/878 ms，全部由 RTO 次数决定（RTO=0 → 448 Mbps，RTO=16 → 71 Mbps）。正解是 QUIC 的 PTO（几个 RTT 的探测包，超时不降窗），这是 M4 之后最该补的一块。
13. **回环验证不了拥塞控制**。回环没有队列、没有带宽限制、没有随机丢包，cwnd 会一路涨到窗口上限；同时每包一次 syscall 让实际在途只有 10 KB，远小于 BDP。回环能验证"正确性"和"单核处理能力"，拥塞控制仍然只能靠确定性仿真。

## 下一步

按"收益 ÷ 工作量"排序：

1. **PTO（探测超时）**：把窗口尾部的丢包恢复从 200 ms 压到 ~2×RTT。回环数据已经证明这是低 RTT 链路上最大的单点，改动局限在丢包检测，不碰拥塞控制。
2. **批量收发**：Windows 用 `WSASendMsg` + IOCP 预投，Linux 用 `sendmmsg`/`recvmmsg`。当前每包 2.22 次 syscall、10 µs 进内核，批量 32 能把这份开销摊掉一个数量级——这是突破 1 Gbps 单流的唯一杠杆。
3. **真实网卡 bench**：回环测出来的是 syscall 成本，真实网卡还要加中断、DMA、GRO。到那一步才有"单流 6–7 Gbps"的结论。
4. **`tc netem` 故障注入 + 24h 长稳 + 与 msquic 对比**。

完整方案见 `../xport-技术方案.md`。
