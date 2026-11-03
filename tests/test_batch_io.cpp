#include <cstring>
#include <vector>

#include "test_framework.h"
#include "xport/batch_io.h"
#include "xport/udp.h"

using namespace xport;

namespace {

// 建一对已互相 connect 的回环 socket
struct Pair {
  std::unique_ptr<UdpSocket> a, b;
  bool ok = false;

  explicit Pair(bool use_batch) {
    UdpConfig ca;
    ca.use_batch = use_batch;
    UdpConfig cb = ca;
    std::string err;
    a = UdpSocket::Open(ca, &err);
    b = UdpSocket::Open(cb, &err);
    if (!a || !b) return;
    ok = a->Connect("127.0.0.1", b->bound_port(), &err) &&
         b->Connect("127.0.0.1", a->bound_port(), &err);
  }
};

// 轮询直到收满 want 个，或试探次数用尽（回环上内核偶尔需要几次轮询）
std::size_t DrainUntil(UdpSocket& s, Batch* out, std::size_t want, int tries = 200) {
  std::size_t total = 0;
  Batch tmp;
  for (int t = 0; t < tries && total < want; ++t) {
    const std::size_t n = s.Recv(&tmp, s.Now());
    for (std::size_t i = 0; i < n; ++i) out->push_back(tmp[i]);
    total += n;
  }
  return total;
}

}  // namespace

TEST(BatchProbeIsConsistent) {
  // 探测结果必须稳定：多次调用同一答案，且后端名和能力位一致
  const batch::Caps& c1 = batch::Probe();
  const batch::Caps& c2 = batch::Probe();
  CHECK_EQ(c1.send, c2.send);
  CHECK_EQ(c1.recv, c2.recv);
  CHECK(c1.backend != nullptr);
  CHECK(std::strlen(c1.backend) > 0);
  // 不支持时后端名必须是 "single"，不能留下误导性的名字
  if (!c1.send) CHECK(std::strcmp(c1.backend, "single") == 0);
}

TEST(BatchSendQueuedDeliversAllPackets) {
  Pair p(/*use_batch=*/true);
  if (!p.ok) return;  // CI 沙箱里可能禁网，跳过而不是失败

  // 刻意用非对齐的个数：37 不是 batch(32) 的整数倍，能测出"尾部留在队列里"的 bug
  const int kCount = 37;
  std::vector<std::vector<u8>> sent;
  for (int i = 0; i < kCount; ++i) {
    std::vector<u8> pkt(64 + std::size_t(i), u8(i));
    pkt[0] = u8(i);  // 标记序号，用于校验内容
    sent.push_back(pkt);
    CHECK(p.a->SendQueued(Bytes(pkt), p.a->Now()));
  }
  CHECK(p.a->Flush(p.a->Now()));
  CHECK_EQ(p.a->queued(), 0u);  // Flush 之后队列必须空

  Batch got;
  const std::size_t n = DrainUntil(*p.b, &got, kCount);
  CHECK_EQ(n, std::size_t(kCount));
  // UDP 回环不保证顺序，按首字节标记核对内容
  for (std::size_t i = 0; i < got.size(); ++i) {
    const u8 idx = got[i].data[0];
    CHECK(idx < kCount);
    CHECK_EQ(got[i].data.size(), sent[idx].size());
    CHECK(std::memcmp(got[i].data.data(), sent[idx].data(), sent[idx].size()) == 0);
  }
}

TEST(BatchFlushIsRequiredForTail) {
  Pair p(/*use_batch=*/true);
  if (!p.ok || !p.a->batch_enabled()) return;

  // 只排 3 个（远小于 batch 32）：不 Flush 就不该有任何报文出去
  for (int i = 0; i < 3; ++i) {
    std::vector<u8> pkt(32, u8(i));
    CHECK(p.a->SendQueued(Bytes(pkt), p.a->Now()));
  }
  CHECK_EQ(p.a->queued(), 3u);

  Batch early;
  DrainUntil(*p.b, &early, 1, 5);
  CHECK_EQ(early.size(), 0u);  // 还压在队列里

  CHECK(p.a->Flush(p.a->Now()));
  Batch late;
  CHECK_EQ(DrainUntil(*p.b, &late, 3), 3u);
}

TEST(BatchAndSinglePathAgree) {
  // 关掉批量后行为必须一致：同样的数据、同样的条数都能收到
  for (bool use_batch : {true, false}) {
    Pair p(use_batch);
    if (!p.ok) return;
    const int kCount = 10;
    for (int i = 0; i < kCount; ++i) {
      std::vector<u8> pkt(100, u8(i + 1));
      CHECK(p.a->SendQueued(Bytes(pkt), p.a->Now()));
    }
    CHECK(p.a->Flush(p.a->Now()));
    Batch got;
    CHECK_EQ(DrainUntil(*p.b, &got, kCount), std::size_t(kCount));
    for (const auto& d : got) CHECK_EQ(d.data.size(), 100u);
  }
}

TEST(BatchSendRespectsOversizedPacket) {
  Pair p(/*use_batch=*/true);
  if (!p.ok) return;
  // 超过 MTU 的报文不能进批量池（会越界写），必须走单包路径
  std::vector<u8> big(kMtu + 100, 0x5A);
  // 不断言成功：内核可能因为超过 MTU 直接拒绝。只要不崩、不破坏队列状态即可。
  (void)p.a->SendQueued(Bytes(big), p.a->Now());
  CHECK_EQ(p.a->queued(), 0u);  // 没有被塞进批量队列
}
