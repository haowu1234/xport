#pragma once
// 确定性链路仿真器：虚拟时钟 + 固定种子。
// 同一个 seed 下，任何一次丢包/乱序都能 100% 复现——这是排障时最值钱的属性。
#include <map>
#include <queue>
#include <vector>

#include "xport/io.h"
#include "xport/types.h"

namespace xport {

// splitmix64：不依赖标准库分布实现，跨编译器结果一致
class Rng {
 public:
  explicit Rng(u64 seed);
  u64 Next();
  double NextUnit();          // [0, 1)
  u64 Below(u64 n);           // [0, n)

 private:
  u64 s_;
};

struct LinkParams {
  double loss_pct = 0.0;      // 丢包率
  double dup_pct = 0.0;       // 重复率
  double bandwidth_mbps = 0;  // 带宽上限；0 表示不限速
  u32 delay_us = 100;         // 单程传播延迟
  u32 jitter_us = 0;          // 抖动（均匀分布）
  u32 queue_us = 5000;        // 出口队列容量，按"带宽 × 该时长"换算；0 表示无限（会 bufferbloat）
  // 确定性丢包窗口 [from, to)：窗口内发往目的端的包 100% 丢弃。
  // 用于制造"孤立的一次拥塞事件"——随机丢包做不到这么可控，
  // 而验证 CUBIC 的降窗与回涨必须建立在单事件上。
  Time loss_from_us = kNever;
  Time loss_to_us = 0;
  u64 seed = 1;
};

class SimLink {
 public:
  explicit SimLink(const LinkParams& p);

  void Send(u32 from, u32 to, Bytes pkt, Time now);
  // 取出一个在 now 时刻已到达 to 的包；没有则返回 false
  bool Poll(u32 to, std::vector<u8>* out, Time now);

  u64 dropped() const { return dropped_; }
  u64 queue_drops() const { return queue_drops_; }
  u64 window_drops() const { return window_drops_; }
  u64 delivered() const { return delivered_; }
  std::size_t in_flight() const;
  // 目的端出口队列当前积压包数（画"排队延迟 vs cwnd"图用）
  std::size_t queue_depth(u32 to, Time now) const;

 private:
  struct Item {
    Time at;
    u64 seq;
    std::vector<u8> data;
  };
  struct Later {
    bool operator()(const Item& a, const Item& b) const {
      return a.at != b.at ? a.at > b.at : a.seq > b.seq;
    }
  };
  using Queue = std::priority_queue<Item, std::vector<Item>, Later>;

  LinkParams p_;
  Rng rng_;
  u64 seq_ = 0;
  u64 dropped_ = 0;
  u64 queue_drops_ = 0;
  u64 window_drops_ = 0;
  u64 delivered_ = 0;
  std::map<u32, Queue> queues_;
  std::map<u32, Time> busy_until_;  // 每个方向的串行化占用（模拟出口带宽）
};

// 把 SimLink 适配成 IoEndpoint：同一份 transport 代码既能跑仿真也能跑真实 socket。
// 只有这一层是"虚假"的——它把 (self, peer) 绑定进端点，因为真实 socket 已经
// 通过 connect() 记住了对端，上层不应该再关心地址。
class SimEndpoint final : public IoEndpoint {
 public:
  SimEndpoint(SimLink& link, u32 self, u32 peer) : link_(link), self_(self), peer_(peer) {}

  bool Send(Bytes pkt, Time now) override {
    link_.Send(self_, peer_, pkt, now);
    now_ = now;
    ++sent_;
    return true;
  }

  std::size_t Recv(Batch* out, Time now) override {
    now_ = now;
    std::size_t n = 0;
    for (;;) {
      if (out->size() < n + 1) out->emplace_back();
      if (!link_.Poll(self_, &(*out)[n].data, now)) break;  // 发给本端的包排在 self_ 队列
      (*out)[n].at = now;
      ++n;
      ++recv_;
    }
    return n;
  }

  // 仿真里"当前时间"由驱动方显式传入，端点自身不持有时钟。
  Time Now() override { return now_; }
  void SetNow(Time t) { now_ = t; }
  bool ok() const override { return true; }

 private:
  SimLink& link_;
  u32 self_;
  u32 peer_;
  Time now_ = 0;
};

}  // namespace xport
