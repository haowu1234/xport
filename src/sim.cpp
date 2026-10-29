#include "xport/sim.h"

namespace xport {

Rng::Rng(u64 seed) : s_(seed ? seed : 0x9E3779B97F4A7C15ull) {}

u64 Rng::Next() {
  s_ += 0x9E3779B97F4A7C15ull;
  u64 z = s_;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

double Rng::NextUnit() {
  // 取高 53 位映射到 [0,1)，避免浮点除法精度问题
  return double(Next() >> 11) * (1.0 / 9007199254740992.0);
}

u64 Rng::Below(u64 n) { return n == 0 ? 0 : Next() % n; }

SimLink::SimLink(const LinkParams& p) : p_(p), rng_(p.seed) {}

void SimLink::Send(u32 /*from*/, u32 to, Bytes pkt, Time now) {
  // 确定性丢包窗口优先于随机丢包：用于制造孤立的一次拥塞事件
  if (now >= p_.loss_from_us && now < p_.loss_to_us) {
    ++window_drops_;
    ++dropped_;
    return;
  }

  if (p_.loss_pct > 0.0 && rng_.NextUnit() * 100.0 < p_.loss_pct) {
    ++dropped_;
    return;
  }

  // 出口带宽串行化：1 Mbps == 1 bit/us
  Time depart = now;
  if (p_.bandwidth_mbps > 0.0) {
    Time& busy = busy_until_[to];
    if (busy < now) busy = now;

    const Time tx = Time(double(pkt.size()) * 8.0 / p_.bandwidth_mbps + 0.5);
    // 出口队列按"串行化积压时间"计量——只算还没发完的部分。
    // 早期版本用 queues_[to].size() 当队列长度，把已经在传播途中的包也算进缓冲区，
    // 于是标称 100ms 的缓冲区实际只剩几十毫秒，排队延迟根本涨不起来，
    // HyStart 这类基于 RTT 增长的算法在仿真里全部失效。
    if (p_.queue_us > 0 && (busy - now) + tx > Time(p_.queue_us)) {
      ++queue_drops_;
      ++dropped_;
      return;
    }
    depart = busy;
    busy = depart + tx;
  }

  const auto enqueue = [&](Time at) {
    Item it{at + p_.delay_us, seq_++, std::vector<u8>(pkt.begin(), pkt.end())};
    queues_[to].push(std::move(it));
  };

  Time at = depart;
  if (p_.jitter_us > 0) at += rng_.Below(u64(p_.jitter_us) + 1);
  enqueue(at);

  if (p_.dup_pct > 0.0 && rng_.NextUnit() * 100.0 < p_.dup_pct) {
    enqueue(at + 1);  // 重复包稍晚到达
  }
}

bool SimLink::Poll(u32 to, std::vector<u8>* out, Time now) {
  auto it = queues_.find(to);
  if (it == queues_.end() || it->second.empty()) return false;
  if (it->second.top().at > now) return false;

  *out = it->second.top().data;
  it->second.pop();
  ++delivered_;
  return true;
}

std::size_t SimLink::in_flight() const {
  std::size_t n = 0;
  for (const auto& [addr, q] : queues_) n += q.size();
  return n;
}

std::size_t SimLink::queue_depth(u32 to, Time now) const {
  auto it = busy_until_.find(to);
  if (it == busy_until_.end() || it->second <= now) return 0;
  const Time backlog_us = it->second - now;
  const double bytes = p_.bandwidth_mbps > 0 ? p_.bandwidth_mbps * double(backlog_us) / 8.0 : 0.0;
  return std::size_t(bytes / double(kMtu));
}

}  // namespace xport
