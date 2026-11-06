#pragma once
// 投递速率采样（draft-cheng-iccrg-delivery-rate-estimation）。BBR 的带宽估计靠它。
//
// 为什么放在传输层而不是 CC 里：
//   速率 = Δdelivered / Δt，而 delivered 要求"每个包发出时记下当时的累计确认量"，
//   这个快照只有发包路径能拿到。CUBIC 不消费它，所以以前没有；换 BBR 时没有它就无从估带宽。
//
// 与 draft 的对应关系（字段名基本照搬，方便对照）：
//   连接级  C.delivered / C.delivered_time / C.first_sent_time / C.app_limited
//   每包    P.delivered / P.delivered_time / P.first_sent_time / P.is_app_limited
//   样本    rs.delivered / rs.interval / rs.prior_delivered / rs.is_app_limited
//
// 两个容易做错的点：
//   * interval 取 max(发送间隔, 确认间隔)。只看确认间隔会被 ACK 压缩（一批 ACK 同时到）
//     放大成虚高的速率；只看发送间隔则在接收端变慢时反应不过来。
//   * app-limited 样本只能用来"往上"更新带宽估计，不能用来降——发送端自己没数据时
//     测到的低速率不代表网络变慢。这个判断留给 CC，这里只如实打标。
#include "xport/types.h"

namespace xport {

// 发包那一刻的连接状态快照，存在在途记录里，ACK 时取回。
struct RateSnapshot {
  u64 delivered = 0;          // 发送时的累计确认字节
  Time delivered_t = 0;       // 发送时最近一次确认的时刻
  Time first_sent_t = 0;      // 发送时所在"发送区间"的起点
  bool is_app_limited = false;
};

// 一个 ACK 产生的样本。valid=false 时其余字段不可用。
struct RateSample {
  u64 delivered = 0;          // 区间内新确认的字节
  Time interval_us = 0;       // max(send_elapsed, ack_elapsed)
  Time send_elapsed_us = 0;
  Time ack_elapsed_us = 0;
  u64 prior_delivered = 0;
  bool is_app_limited = false;
  bool valid = false;

  // 字节/秒
  u64 RateBps() const {
    if (!valid || interval_us == 0) return 0;
    return u64(double(delivered) * 1e6 / double(interval_us));
  }
};

class RateSampler {
 public:
  // 每发出一个登记在途的包调用（重传同样要调：它也占带宽、也会被确认）。
  // inflight_before 是登记本包之前的在途字节。
  RateSnapshot OnPacketSent(Time now, u64 inflight_before) {
    // 在途为空说明之前有过静默期，发送区间和确认时钟都从现在重新起算，
    // 否则静默期会被算进 interval，样本被稀释成低速率。
    if (inflight_before == 0) {
      first_sent_t_ = now;
      delivered_t_ = now;
    }
    return RateSnapshot{delivered_, delivered_t_, first_sent_t_, app_limited_ != 0};
  }

  // ACK 中每个新确认的包调用一次（顺序无所谓），全部调完后调 Finish。
  void OnPacketAcked(const RateSnapshot& p, Time sent_t, u64 bytes, Time now) {
    delivered_ += bytes;
    delivered_t_ = now;
    // 用"最晚发出"的那个包定界：它的快照最新，区间最短，最贴近当前路径状态。
    // delivered 相同（同一批发出、中间没有确认）时按发送时刻区分。
    if (!have_ || p.delivered > prior_delivered_ ||
        (p.delivered == prior_delivered_ && sent_t >= prior_sent_t_)) {
      have_ = true;
      prior_delivered_ = p.delivered;
      prior_sent_t_ = sent_t;
      prior_app_limited_ = p.is_app_limited;
      send_elapsed_ = sent_t > p.first_sent_t ? sent_t - p.first_sent_t : 0;
      ack_elapsed_ = now > p.delivered_t ? now - p.delivered_t : 0;
      // 下一个发送区间从这个包的发送时刻开始
      first_sent_t_ = sent_t;
    }
  }

  // 结束本 ACK 的采样。min_rtt_us 为 0 表示还没有 RTT 样本（不做下限过滤）。
  RateSample Finish(Time min_rtt_us) {
    // app-limited 标记在"那段时间发出的数据全部被确认"之后解除
    if (app_limited_ != 0 && delivered_ > app_limited_) app_limited_ = 0;

    RateSample rs;
    if (!have_) return rs;
    have_ = false;

    rs.delivered = delivered_ - prior_delivered_;
    rs.prior_delivered = prior_delivered_;
    rs.is_app_limited = prior_app_limited_;
    rs.send_elapsed_us = send_elapsed_;
    rs.ack_elapsed_us = ack_elapsed_;
    rs.interval_us = send_elapsed_ > ack_elapsed_ ? send_elapsed_ : ack_elapsed_;
    // 短于 min_rtt 的区间不可信（draft §3.3）：一个包不可能比 RTT 更快被确认，
    // 出现这种情况只能是 ACK 压缩或时钟粒度问题，速率会被严重高估。
    if (rs.interval_us == 0 || rs.interval_us < min_rtt_us) return rs;
    rs.valid = true;
    return rs;
  }

  // 发送端没有数据可发（或被接收端流控挡住）而窗口还有余量时调用。
  // 标记一直持续到当前在途的数据全部被确认。
  void OnAppLimited(u64 inflight) {
    const u64 v = delivered_ + inflight;
    app_limited_ = v != 0 ? v : 1;
  }

  bool app_limited() const { return app_limited_ != 0; }
  u64 delivered() const { return delivered_; }
  Time delivered_t() const { return delivered_t_; }

 private:
  // 连接级
  u64 delivered_ = 0;
  Time delivered_t_ = 0;
  Time first_sent_t_ = 0;
  u64 app_limited_ = 0;  // 0 = 不受限；否则为"delivered 超过它时解除"的阈值

  // 单个 ACK 内的累积
  bool have_ = false;
  u64 prior_delivered_ = 0;
  Time prior_sent_t_ = 0;
  bool prior_app_limited_ = false;
  Time send_elapsed_ = 0;
  Time ack_elapsed_ = 0;
};

}  // namespace xport
