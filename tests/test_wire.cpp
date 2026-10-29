#include <cstring>

#include "test_framework.h"
#include "xport/bytes.h"
#include "xport/crc32c.h"
#include "xport/packet.h"

using namespace xport;

TEST(VarIntBoundaries) {
  const u64 vals[] = {0,        1,          63,          64,          16383, 16384,
                      (1ull << 30) - 1, (1ull << 30), (1ull << 40), (1ull << 62) - 1};
  for (u64 v : vals) {
    u8 buf[8] = {};
    Writer w(MutBytes(buf, sizeof buf));
    CHECK(w.PutVarInt(v));
    CHECK_EQ(w.size(), VarIntLen(v));

    Reader r(Bytes(buf, w.size()));
    u64 out = 0;
    CHECK(r.GetVarInt(&out));
    CHECK_EQ(out, v);
    CHECK(r.empty());
  }
}

TEST(VarIntRejectsTruncated) {
  u8 buf[8] = {};
  Writer w(MutBytes(buf, sizeof buf));
  CHECK(w.PutVarInt(1ull << 40));  // 8 字节编码

  Reader r(Bytes(buf, 3));
  u64 out = 0;
  CHECK(!r.GetVarInt(&out));  // 声明 8 字节但只剩 3 字节
}

TEST(VarIntRejectsOutOfRange) {
  u8 buf[8] = {};
  Writer w(MutBytes(buf, sizeof buf));
  CHECK(!w.PutVarInt(1ull << 62));  // 62 位是编码上界，超出必须拒绝而不是静默截断
}

TEST(Crc32cKnownVector) {
  const char* s = "123456789";
  CHECK_EQ(Crc32c(Bytes(reinterpret_cast<const u8*>(s), 9)), 0xE3069283u);
  CHECK_EQ(Crc32c(Bytes{}), 0u);
}

TEST(HeaderRoundTrip) {
  u8 buf[64] = {};
  Header h;
  h.type = PacketType::kData;
  h.dst_cid = 0x11223344u;
  h.pkt_num = 1234567890123ull;  // 超过 32 位，验证 64 位包号
  h.payload_len = 0;

  const std::size_t n = EncodeHeader(h, MutBytes(buf, sizeof buf));
  CHECK_EQ(n, kHeaderSize);

  Header out;
  CHECK(DecodeHeader(Bytes(buf, kHeaderSize), &out));
  CHECK(out.type == PacketType::kData);
  CHECK_EQ(out.pkt_num, h.pkt_num);
  CHECK_EQ(out.dst_cid, h.dst_cid);
}

TEST(HeaderRejectsCorruptedCrc) {
  u8 buf[64] = {};
  Header h;
  h.pkt_num = 42;
  EncodeHeader(h, MutBytes(buf, sizeof buf));

  buf[8] ^= 0xFF;  // 篡改包号，CRC 应失配
  Header out;
  CHECK(!DecodeHeader(Bytes(buf, kHeaderSize), &out));
}

TEST(HeaderRejectsShortBuffer) {
  u8 buf[16] = {};
  Header out;
  CHECK(!DecodeHeader(Bytes(buf, sizeof buf), &out));
}

TEST(FramesRoundTrip) {
  const u8 payload_data[] = {1, 2, 3, 4, 5, 6, 7};
  u8 buf[kMaxPayload] = {};

  MutBytes dst(buf, sizeof buf);
  AckFrame ak;
  ak.delay_us = 1234;
  ak.count = 2;
  ak.lo[0] = 100;
  ak.hi[0] = 105;
  ak.lo[1] = 90;
  ak.hi[1] = 92;
  const std::size_t a = EncodeAck(dst, ak);
  CHECK(a > 0);

  StreamFrame sf;
  sf.sid = 3;
  sf.offset = 987654321;
  sf.data = Bytes(payload_data, sizeof payload_data);
  sf.fin = true;
  const std::size_t b = EncodeStream(dst.subspan(a), sf);
  CHECK(b > 0);

  Decoded d;
  CHECK(DecodeFrames(Bytes(buf, a + b), &d));
  CHECK(d.has_ack);
  CHECK_EQ(d.ack.count, 2);
  CHECK_EQ(d.ack.delay_us, 1234);
  CHECK_EQ(d.ack.lo[0], 100);
  CHECK_EQ(d.ack.hi[0], 105);
  CHECK_EQ(d.ack.lo[1], 90);
  CHECK(AckCovers(d.ack, 104));
  CHECK(AckCovers(d.ack, 91));
  CHECK(!AckCovers(d.ack, 95));  // 缺口必须不覆盖
  CHECK_EQ(AckLargest(d.ack), 105);

  CHECK_EQ(d.stream_count, 1);
  CHECK_EQ(d.streams[0].sid, 3);
  CHECK_EQ(d.streams[0].offset, 987654321);
  CHECK_EQ(d.streams[0].data.size(), sizeof payload_data);
  CHECK(std::memcmp(d.streams[0].data.data(), payload_data, sizeof payload_data) == 0);
  CHECK(d.streams[0].fin);
}

// 帧聚合：一个报文里多个 STREAM 帧必须按顺序全部解析出来，
// 且各自的 data 都指向入参缓冲区（不拷贝）。
TEST(FramesDecodeMultipleStreams) {
  u8 buf[kMaxPayload] = {};
  const u8 d0[10] = {1, 2, 3};
  const u8 d1[5] = {9};
  MutBytes dst(buf, sizeof buf);

  std::size_t off = 0;
  for (int i = 0; i < 5; ++i) {
    StreamFrame sf;
    sf.sid = u32(i);
    sf.offset = u64(i) * 1000;
    sf.data = (i % 2) ? Bytes(d1, sizeof d1) : Bytes(d0, sizeof d0);
    const std::size_t n = EncodeStream(dst.subspan(off), sf);
    CHECK(n > 0);
    off += n;
  }

  Decoded d;
  CHECK(DecodeFrames(Bytes(buf, off), &d));
  CHECK_EQ(d.stream_count, 5);
  for (int i = 0; i < 5; ++i) {
    CHECK_EQ(d.streams[i].sid, u32(i));
    CHECK_EQ(d.streams[i].offset, u64(i) * 1000);
    CHECK_EQ(d.streams[i].data.size(), (i % 2) ? sizeof d1 : sizeof d0);
  }
  // data 是零拷贝视图：必须落在入参缓冲区内部
  CHECK(d.streams[0].data.data() >= buf && d.streams[0].data.data() < buf + sizeof buf);
}

// 超过单包帧数上限要判非法，避免病态输入撑爆栈上的 Decoded
TEST(FramesRejectTooManyStreams) {
  u8 buf[kMaxPayload] = {};
  const u8 one = 0;
  MutBytes dst(buf, sizeof buf);
  std::size_t off = 0;
  for (std::size_t i = 0; i < kMaxStreamFrames + 1; ++i) {
    StreamFrame sf;
    sf.sid = u32(i);
    sf.data = Bytes(&one, 1);
    const std::size_t n = EncodeStream(dst.subspan(off), sf);
    if (n == 0) return;  // 缓冲区装不下就不测了（包长上限内应装得下）
    off += n;
  }
  Decoded d;
  CHECK(!DecodeFrames(Bytes(buf, off), &d));
}

TEST(FramesRejectTruncatedStream) {
  u8 buf[kMaxPayload] = {};
  StreamFrame sf;
  const u8 data[100] = {};
  sf.data = Bytes(data, sizeof data);
  const std::size_t n = EncodeStream(MutBytes(buf, sizeof buf), sf);

  Decoded d;
  CHECK(!DecodeFrames(Bytes(buf, n - 1), &d));  // 少一个字节
}

TEST(FramesRejectUnknownType) {
  const u8 bad[] = {0x7F};
  Decoded d;
  CHECK(!DecodeFrames(Bytes(bad, sizeof bad), &d));
}

TEST(FramesRejectDuplicateControlFrame) {
  u8 buf[kMaxPayload] = {};
  AckFrame ak;
  ak.count = 1;
  ak.lo[0] = 1;
  ak.hi[0] = 1;
  const std::size_t a = EncodeAck(MutBytes(buf, sizeof buf), ak);
  const std::size_t b = EncodeAck(MutBytes(buf + a, sizeof buf - a), ak);

  Decoded d;
  CHECK(!DecodeFrames(Bytes(buf, a + b), &d));  // 一包内两个 ACK，判非法
}

TEST(MaxDataAndPing) {
  u8 buf[32] = {};
  std::size_t n = EncodeMaxData(MutBytes(buf, sizeof buf), 1ull << 40);
  n += EncodePing(MutBytes(buf + n, sizeof buf - n));

  Decoded d;
  CHECK(DecodeFrames(Bytes(buf, n), &d));
  CHECK(d.has_max_data);
  CHECK_EQ(d.max_data, 1ull << 40);
  CHECK(d.has_ping);
}
