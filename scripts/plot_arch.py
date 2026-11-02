#!/usr/bin/env python3
"""生成 README 架构图 docs/figs/architecture.svg。

纯手写 SVG，不依赖任何第三方库；配色与字体沿用 plot_readme.py，
保证和其他图表放在一起时风格一致。图里的常量（包头字段、环形数组容量、
ACK 段数等）都对应源码里的定义，改代码时记得同步这里。

    python scripts/plot_arch.py
"""

import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "docs", "figs")

# ---- 与 plot_readme.py 一致的调色板 ----
INK = "#0f172a"
MUTED = "#64748b"
FAINT = "#94a3b8"
BORDER = "#e2e8f0"
WHITE = "#ffffff"
BLUE = "#2563eb"
TEAL = "#0d9488"
AMBER = "#d97706"
VIOLET = "#7c3aed"
RED = "#dc2626"
SLATE = "#475569"

FONT = ("-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC',"
        "'Microsoft YaHei',sans-serif")
HDR_UNIT = 23.0  # 包头示意图每字节宽度（px）
MONO = "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace"

# 每一层的浅色底 / 描边 / 强调色
TONES = {
    "app": ("#f8fafc", "#cbd5e1", SLATE),
    "transport": ("#eff6ff", "#bfdbfe", BLUE),
    "io": ("#f0fdfa", "#99f6e4", TEAL),
    "backend": ("#fffbeb", "#fde68a", AMBER),
}

W, H = 960, 770


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size=12, color=INK, weight="400", anchor="start", family=FONT, extra=""):
    return ('<text x="%.1f" y="%.1f" font-family="%s" font-size="%s" font-weight="%s" '
            'fill="%s" text-anchor="%s"%s>%s</text>'
            % (x, y, family, size, weight, color, anchor, extra, esc(s)))


def rect(x, y, w, h, fill, stroke=None, rx=10, sw=1, extra=""):
    st = ' stroke="%s" stroke-width="%s"' % (stroke, sw) if stroke else ""
    return ('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%s" fill="%s"%s%s/>'
            % (x, y, w, h, rx, fill, st, extra))


def pill_width(s, size=10.5):
    """按字符估算标签宽度：中文按 1 em，ASCII 按 0.6 em，左右各留 7 px。"""
    return sum(size if ord(ch) > 0x2E80 else size * 0.6 for ch in s) + 14


def pill(x, y, s, color, size=10.5):
    """小标签：圆角底 + 文字。"""
    w = pill_width(s, size)
    return [rect(x, y, w, 18, color, rx=9, extra=' fill-opacity="0.12"'),
            text(x + w / 2, y + 12.6, s, size, color, "600", "middle")], w


def card(x, y, w, h, title, lines, accent, tag=None):
    """一个模块卡片：白底、左侧色条、标题、若干行说明。"""
    o = [rect(x, y, w, h, WHITE, BORDER, rx=8, extra=' filter="url(#shadow)"'),
         rect(x, y + 12, 3, h - 24, accent, rx=1.5),
         text(x + 16, y + 24, title, 13, INK, "600")]
    if tag:
        p, _ = pill(x + w - 12 - pill_width(tag, 9.5), y + 11, tag, accent, 9.5)
        o += p
    for i, (s, mono) in enumerate(lines):
        o.append(text(x + 16, y + 44 + i * 17, s, 11, MUTED, "400", "start", MONO if mono else FONT))
    return o


def layer(x, y, w, h, key, label, sub):
    fill, stroke, accent = TONES[key]
    return [rect(x, y, w, h, fill, stroke, rx=14),
            text(x + 18, y + 26, label, 12, accent, "700", extra=' letter-spacing="0.6"'),
            text(x + 18, y + 44, sub, 10.5, MUTED)]


def arrow(x1, y1, x2, y2, color=FAINT, label=None, dashed=False, lx=None, ly=None):
    dash = ' stroke-dasharray="4 4"' if dashed else ""
    o = ['<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="1.6"%s '
         'marker-end="url(#arrow-%s)"/>' % (x1, y1, x2, y2, color, dash, color.lstrip("#"))]
    if label:
        o.append(text(lx if lx is not None else (x1 + x2) / 2 + 8,
                      ly if ly is not None else (y1 + y2) / 2 + 4, label, 10.5, color, "500"))
    return o


def defs():
    o = ["<defs>",
         '<filter id="shadow" x="-10%" y="-10%" width="120%" height="140%">'
         '<feDropShadow dx="0" dy="1" stdDeviation="1.6" flood-color="#0f172a" flood-opacity="0.07"/>'
         "</filter>"]
    for c in (FAINT, BLUE, TEAL, AMBER, SLATE):
        o.append('<marker id="arrow-%s" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
                 'markerHeight="7" orient="auto-start-reverse"><path d="M0 0 L10 5 L0 10 z" '
                 'fill="%s"/></marker>' % (c.lstrip("#"), c))
    o.append("</defs>")
    return o


def header_strip(x, y):
    """20 字节定长包头的字节布局（与 src/packet.cpp EncodeHeader 一致）。"""
    fields = [("ver", 1, SLATE), ("type", 1, SLATE), ("flags", 2, FAINT), ("dst_cid", 4, TEAL),
              ("pkt_num · 64 bit", 8, BLUE), ("len", 2, VIOLET), ("crc16", 2, AMBER)]
    unit = HDR_UNIT
    o = [text(x, y - 8, "包头 · 20 B 定长", 11, INK, "600"),
         text(x + 20 * unit, y - 8, "CRC32C 低 16 位，覆盖前 18 B", 10, FAINT, "400", "end")]
    cx = x
    for name, n, color in fields:
        w = n * unit
        o.append(rect(cx + 0.5, y, w - 1, 26, color, rx=3, extra=' fill-opacity="0.14"'))
        o.append(text(cx + w / 2, y + 17, name, 9.5 if n > 1 else 8.5, color, "600", "middle", MONO))
        cx += w
    # 字节刻度
    for b in (0, 4, 8, 16, 20):
        o.append(text(x + b * unit, y + 40, str(b), 9, FAINT, "400", "middle", MONO))
    return o


def build():
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" '
         'role="img" aria-label="xport 协议架构：应用层、Transport、IoEndpoint、仿真与真实 socket 后端">'
         % (W, H, W, H)]
    o += defs()
    o.append(rect(0, 0, W, H, WHITE, rx=0))

    # 标题
    o.append(text(32, 42, "xport 协议架构", 18, INK, "700"))
    o.append(text(32, 64, "单线程 · 时间由调用方注入 · 仿真与真实 socket 走同一条代码路径", 12, MUTED))
    p, pw = pill(W - 32 - pill_width("C++20 · 零第三方依赖"), 30, "C++20 · 零第三方依赖", BLUE)
    o += p

    # ---------- 应用层 ----------
    ax, ay, aw, ah = 32, 86, W - 64, 64
    o += layer(ax, ay, aw, ah, "app", "APPLICATION", "应用只看到字节流，不碰包")
    api = [("OpenStream()", "开流"), ("Write(sid, bytes)", "提交"), ("Peek(sid)", "零拷贝读"),
           ("Consume(sid, n)", "归还窗口")]
    bx = ax + 262
    for name, note in api:
        o.append(rect(bx, ay + 15, 150, 34, WHITE, BORDER, rx=7))
        o.append(text(bx + 12, ay + 30, name, 10.5, INK, "600", "start", MONO))
        o.append(text(bx + 12, ay + 43, note, 9.5, FAINT))
        bx += 158

    # ---------- Transport ----------
    tx, ty, tw, th = 32, 176, W - 64, 336
    o += layer(tx, ty, tw, th, "transport", "TRANSPORT", "无锁 · 热路径零堆分配 · 每包约 113 ns")

    gap = 12
    cw, chh = (tw - 36 - 3 * gap) / 4, 112
    cx0 = tx + 18
    r1 = ty + 58
    o += card(cx0, r1, cw, chh, "多流调度", [
        ("DRR，按字节公平", False), ("帧聚合：每包最多 8 段", False), ("可跨流，小消息 pps ↓4×", False),
        ("max_streams = 256", True)], BLUE, "RFC 9000")
    o += card(cx0 + (cw + gap), r1, cw, chh, "可靠传输", [
        ("SACK 显式区间 ≤ 16 段", False), ("包阈值 3 + 时间阈值 9/8", False),
        ("PTO 探测 · 指数退避", False), ("重传换新包号", False)], BLUE, "RFC 9002")
    o += card(cx0 + 2 * (cw + gap), r1, cw, chh, "拥塞控制", [
        ("CUBIC + 快速收敛", False), ("HyStart++ 慢启动退出", False),
        ("令牌桶 pacing", False), ("cwnd 验证 · 持续拥塞", False)], BLUE, "RFC 9438")
    o += card(cx0 + 3 * (cw + gap), r1, cw, chh, "流控", [
        ("MAX_DATA 连接级窗口", False), ("随每个 ACK 捎带", False),
        ("消费式：已消费 + 窗口", False), ("PING 探测防死锁", False)], BLUE)

    # 数据结构条
    r2 = r1 + chh + 18
    o.append(text(cx0, r2 + 4, "数据结构", 11, BLUE, "600"))
    ds = [("sent_ring_[8192]", "在途包，pn & mask 定位"),
          ("RangeSet", "定长区间合并"),
          ("recv_bits_ 16384 bit", "滑动位图记包号"),
          ("ring buffer / 流", "接收窗口，Peek 零拷贝")]
    dx, dw = cx0, (tw - 36 - 3 * 12) / 4
    for name, note in ds:
        o.append(rect(dx, r2 + 14, dw, 46, WHITE, "#bfdbfe", rx=7))
        o.append(text(dx + 12, r2 + 33, name, 11, INK, "600", "start", MONO))
        o.append(text(dx + 12, r2 + 50, note, 10, MUTED))
        dx += dw + 12

    # 包头
    o += header_strip(cx0, r2 + 98)
    # 包头右侧：帧类型
    fx = cx0 + 20 * HDR_UNIT + 40
    o.append(text(fx, r2 + 90, "帧类型", 11, INK, "600"))
    frames = [("STREAM", BLUE), ("ACK", TEAL), ("MAX_DATA", VIOLET), ("PING", AMBER)]
    for name, color in frames:
        pp, pw2 = pill(fx, r2 + 98, name, color, 10)
        o += pp
        fx += pw2 + 8
    o.append(text(cx0 + 20 * HDR_UNIT + 40, r2 + 140, "语义对齐 QUIC，线格式自研，不与标准 QUIC 互通", 10, FAINT))

    # ---------- IoEndpoint ----------
    ix, iy, iw, ih = 32, ty + th + 28, W - 64, 64
    o += layer(ix, iy, iw, ih, "io", "IO ENDPOINT", "唯一的网络边界，接口只有三个")
    sig = [("Send(Bytes, Time now)", "允许失败 = 丢包"), ("Recv(Batch*, Time now)", "非阻塞批量收"),
           ("Now()", "虚拟时钟 / 单调时钟")]
    sx = ix + 262
    for name, note, bw0 in [(a, b, w0) for (a, b), w0 in zip(sig, (210, 222, 170))]:
        o.append(rect(sx, iy + 15, bw0, 34, WHITE, "#99f6e4", rx=7))
        o.append(text(sx + 12, iy + 30, name, 10.5, INK, "600", "start", MONO))
        o.append(text(sx + 12, iy + 43, note, 9.5, FAINT))
        sx += bw0 + 8

    # ---------- 后端 ----------
    by, bh = iy + ih + 30, 94
    bw = (W - 64 - 20) / 2
    for i, (title, lines, tag) in enumerate([
        ("SimLink · 确定性仿真", ["虚拟时钟 · 固定种子 · 逐位可复现",
                                  "带宽 / 延迟 / 抖动 / 队列 / 丢包 / 重复",
                                  "CUBIC 曲线与 Python 独立回算对照"], "测正确性"),
        ("UdpSocket · 真实网络", ["Winsock / BSD socket · 非阻塞轮询",
                                  "回环实测单流约 3.5 Gbps（裸 UDP 的 63%）",
                                  "瓶颈在 syscall：下一步批量收发 / GSO"], "测性能")]):
        bx2 = 32 + i * (bw + 20)
        fill, stroke, accent = TONES["backend"]
        o.append(rect(bx2, by, bw, bh, fill, stroke, rx=14))
        o.append(text(bx2 + 18, by + 26, title, 13, INK, "700"))
        pp, _ = pill(bx2 + bw - 18 - pill_width(tag), by + 12, tag, accent, 10.5)
        o += pp
        for j, s in enumerate(lines):
            o.append(text(bx2 + 18, by + 48 + j * 16, s, 11, MUTED))

    # ---------- 连线 ----------
    mid = W / 2
    o += arrow(mid, ay + ah, mid, ty - 2, SLATE)
    o += arrow(mid, ty + th, mid, iy - 2, BLUE)  # 中线在包头图与帧类型之间的空白处
    o += arrow(32 + bw / 2, iy + ih, 32 + bw / 2, by - 2, TEAL)
    o += arrow(32 + bw + 20 + bw / 2, iy + ih, 32 + bw + 20 + bw / 2, by - 2, TEAL)

    # 时间注入说明（右侧竖排的一条虚线）
    o.append(text(W - 32, H - 14, "Transport 不认识网络：只有把时间从外部注入，仿真才可能 100% 确定",
                  10.5, FAINT, "400", "end"))
    o.append(rect(0.5, 0.5, W - 1, H - 1, "none", BORDER, rx=12))
    o.append("</svg>")
    return "\n".join(o)


def main():
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "architecture.svg")
    content = build()
    # 显式 \n：Windows 上默认会写 CRLF，和 .gitattributes 的 eol=lf 打架
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print("  %-28s %6.1f KB" % ("architecture.svg", len(content.encode("utf-8")) / 1024.0))


if __name__ == "__main__":
    main()
