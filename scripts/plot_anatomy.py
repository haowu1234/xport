#!/usr/bin/env python3
"""生成 README 结构解剖图 docs/figs/anatomy.svg。

和另外两张图的分工：
  plot_arch.py      有哪些模块（静态分层）
  plot_concept.py   一个包经历了什么（动态流程）
  本脚本            关键数据结构长什么样（解剖）

图里每个字节位置、每个指针名都对应源码，改代码时同步这里：
  包头字段偏移   src/packet.cpp EncodeHeader（ver1 type1 flags2 cid4 pn8 len2 crc2）
  CRC 覆盖范围   src/packet.cpp kCrcField = 18
  STREAM 帧      src/packet.cpp EncodeStream（type + varint sid/off/len + fin + data）
  环形缓冲指针   include/xport/transport.h InStream{consumed, contiguous, highest_end}
  零拷贝读       src/transport.cpp Transport::Peek

    python scripts/plot_anatomy.py
"""

import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "docs", "figs")

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
GREEN = "#16a34a"
SLATE = "#475569"

FONT = ("-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC',"
        "'Microsoft YaHei',sans-serif")
MONO = "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace"

W, H = 1180, 1010


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def t(x, y, s, size=12, color=INK, weight="400", anchor="start", family=None, op=None):
    extra = ' fill-opacity="%s"' % op if op is not None else ""
    return ('<text x="%.1f" y="%.1f" font-family="%s" font-size="%s" font-weight="%s" '
            'fill="%s" text-anchor="%s"%s>%s</text>'
            % (x, y, family or FONT, size, weight, color, anchor, extra, esc(s)))


def rect(x, y, w, h, fill, stroke=None, rx=4, sw=1, op=None, dash=None):
    s = '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%s" fill="%s"' % (
        x, y, w, h, rx, fill)
    if op is not None:
        s += ' fill-opacity="%s"' % op
    if stroke:
        s += ' stroke="%s" stroke-width="%s"' % (stroke, sw)
    if dash:
        s += ' stroke-dasharray="%s"' % dash
    return s + "/>"


def line(x1, y1, x2, y2, c=FAINT, sw=1.4, dash=None, marker=None):
    s = '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%s"' % (
        x1, y1, x2, y2, c, sw)
    if dash:
        s += ' stroke-dasharray="%s"' % dash
    if marker:
        s += ' marker-end="url(#m-%s)"' % c.lstrip("#")
    return s + "/>"


def path(d, stroke=None, fill="none", sw=1.6, dash=None, marker=None, cap="round"):
    s = '<path d="%s" fill="%s"' % (d, fill)
    if stroke:
        s += ' stroke="%s" stroke-width="%s" stroke-linecap="%s"' % (stroke, sw, cap)
    if dash:
        s += ' stroke-dasharray="%s"' % dash
    if marker:
        s += ' marker-end="url(#m-%s)"' % stroke.lstrip("#")
    return s + "/>"


def pill_w(s, size=10.5):
    return sum(size if ord(c) > 0x2E80 else size * 0.6 for c in s) + 16


def pill(x, y, s, color, size=10.5, h=19):
    w = pill_w(s, size)
    return [rect(x, y, w, h, color, rx=h / 2, op=0.13),
            t(x + w / 2, y + h * 0.69, s, size, color, "600", "middle")], w


def defs():
    o = ["<defs>",
         '<filter id="sh" x="-20%" y="-20%" width="140%" height="160%">'
         '<feDropShadow dx="0" dy="1.5" stdDeviation="2" flood-color="#0f172a" '
         'flood-opacity="0.08"/></filter>']
    for c in (BLUE, TEAL, AMBER, VIOLET, GREEN, FAINT, RED, SLATE, INK):
        o.append('<marker id="m-%s" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="6" '
                 'markerHeight="6" orient="auto-start-reverse"><path d="M0 1 L9 5 L0 9 z" '
                 'fill="%s"/></marker>' % (c.lstrip("#"), c))
    o.append('<pattern id="hole" width="7" height="7" patternTransform="rotate(45)" '
             'patternUnits="userSpaceOnUse">'
             '<line x1="0" y1="0" x2="0" y2="7" stroke="#dc2626" stroke-width="1.6" '
             'stroke-opacity="0.35"/></pattern>')
    o.append("</defs>")
    return o


def panel(x, y, w, h, n, title, sub):
    """带编号的面板外框"""
    return [rect(x, y, w, h, WHITE, BORDER, rx=13),
            rect(x + 18, y + 18, 22, 22, INK, rx=7, op=0.06),
            t(x + 29, y + 33.5, str(n), 12, INK, "700", "middle", MONO),
            t(x + 50, y + 34, title, 14, INK, "700"),
            t(x + 50, y + 52, sub, 11, MUTED)]


# ============================================================
# 面板 1：报文解剖 —— 真实字节布局
# ============================================================
def panel_packet(px, py, pw):
    o = panel(px, py, pw, 372, 1, "报文解剖",
              "20 B 定长头，字段按真实字节偏移绘制；载荷里可以并排多个 STREAM 帧（帧聚合）")

    # ---- 头部：按字节等比例 ----
    unit = 34.0           # 每字节宽度
    hx = px + 42
    hy = py + 116
    bh = 44

    fields = [("ver", 1, SLATE, "1"), ("type", 1, SLATE, "kData / kAck"),
              ("flags", 2, FAINT, "预留"), ("dst_cid", 4, TEAL, "连接 ID"),
              ("pkt_num", 8, BLUE, "64 位，重传换新号"), ("len", 2, VIOLET, "载荷字节数"),
              ("crc", 2, AMBER, "CRC32C 低 16 位")]

    cx = hx
    for name, n, color, note in fields:
        w = n * unit
        o.append(rect(cx, hy, w, bh, color, color, rx=5, sw=1.3, op=0.13))
        o.append(t(cx + w / 2, hy + 20, name, 11 if n > 1 else 9.5, color, "700", "middle", MONO))
        o.append(t(cx + w / 2, hy + 34, "%dB" % n, 9, color, "500", "middle", MONO, op=0.8))
        cx += w

    # 字节刻度
    o.append(line(hx, hy + bh + 9, hx + 20 * unit, hy + bh + 9, BORDER, 1))
    for b in (0, 1, 2, 4, 8, 16, 18, 20):
        bx = hx + b * unit
        o.append(line(bx, hy + bh + 5, bx, hy + bh + 13, FAINT, 1))
        o.append(t(bx, hy + bh + 26, str(b), 9.5, FAINT, "500", "middle", MONO))
    o.append(t(hx + 10 * unit, hy - 10, "字节偏移", 10, FAINT, "500", "middle"))

    # CRC 覆盖范围标注（0..18）
    cy = hy - 24
    o.append(path("M%.1f %.1f V%.1f H%.1f V%.1f" % (hx, cy + 10, cy, hx + 18 * unit, cy + 10),
                  AMBER, sw=1.3))
    o.append(t(hx + 9 * unit, cy - 5, "CRC 覆盖前 18 B（头部自校验，载荷靠 UDP checksum）",
               10, AMBER, "600", "middle"))

    # ---- 载荷：三个 STREAM 帧并排 ----
    ly = hy + bh + 48
    o.append(t(hx, ly + 14, "载荷", 11, INK, "700"))
    o.append(t(hx + 34, ly + 14, "一个报文最多 8 段，可以来自不同的流", 10, MUTED))

    frames = [("STREAM", "sid=0", 190, BLUE), ("STREAM", "sid=3", 150, VIOLET),
              ("STREAM", "sid=0", 230, BLUE)]
    fy = ly + 24
    fx = hx
    for i, (kind, who, fw, color) in enumerate(frames):
        o.append(rect(fx, fy, fw, 38, color, color, rx=5, sw=1.2, op=0.1))
        o.append(t(fx + 10, fy + 17, kind, 10, color, "700", "start", MONO))
        o.append(t(fx + 10, fy + 30, who, 9.5, color, "500", "start", MONO))
        fx += fw + 7
    o.append(rect(fx, fy, 86, 38, TEAL, TEAL, rx=5, sw=1.2, op=0.1))
    o.append(t(fx + 10, fy + 17, "ACK", 10, TEAL, "700", "start", MONO))
    o.append(t(fx + 10, fy + 30, "16 段区间", 9.5, TEAL, "500", "start", MONO))

    # ---- 展开第一个 STREAM 帧的内部字段 ----
    ey = fy + 62
    o.append(path("M%.1f %.1f L%.1f %.1f" % (hx + 20, fy + 42, hx + 20, ey - 6),
                  FAINT, sw=1.2, dash="3 3", marker=FAINT))
    o.append(t(hx + 32, ey - 10, "展开一个 STREAM 帧", 10, FAINT, "500"))

    sub = [("type", 48, BLUE, "0x08"), ("sid", 56, BLUE, "varint"),
           ("offset", 136, AMBER, "varint · 流内字节位置"), ("len", 56, VIOLET, "varint"),
           ("fin", 44, SLATE, "1B"), ("data …", 330, GREEN, "应用字节")]
    sx2 = hx
    for name, sw_, color, note in sub:
        o.append(rect(sx2, ey, sw_, 36, color, color, rx=4, sw=1.2, op=0.12))
        o.append(t(sx2 + sw_ / 2, ey + 16, name, 9.5, color, "700", "middle", MONO))
        o.append(t(sx2 + sw_ / 2, ey + 29, note, 8.5, color, "500", "middle", MONO, op=0.85))
        sx2 += sw_ + 5

    o.append(t(hx, ey + 60,
               "offset 是流内字节位置，和 pkt_num 完全独立：重传换新包号、offset 不变，"
               "所以 ACK 只认包号、流控只认偏移。",
               10.5, MUTED))
    return o


# ============================================================
# 面板 2：环形缓冲与零拷贝
# ============================================================
def ring_sector(cx, cy, r0, r1, a0, a1, fill, op=None, stroke=None, sw=1):
    """环形扇区（角度用度，0° 在正右方，顺时针）"""
    def pt(r, a):
        rad = math.radians(a)
        return cx + r * math.cos(rad), cy + r * math.sin(rad)
    large = 1 if (a1 - a0) % 360 > 180 else 0
    x0, y0 = pt(r1, a0)
    x1, y1 = pt(r1, a1)
    x2, y2 = pt(r0, a1)
    x3, y3 = pt(r0, a0)
    d = ("M%.2f %.2f A%.1f %.1f 0 %d 1 %.2f %.2f L%.2f %.2f A%.1f %.1f 0 %d 0 %.2f %.2f Z"
         % (x0, y0, r1, r1, large, x1, y1, x2, y2, r0, r0, large, x3, y3))
    s = '<path d="%s" fill="%s"' % (d, fill)
    if op is not None:
        s += ' fill-opacity="%s"' % op
    if stroke:
        s += ' stroke="%s" stroke-width="%s"' % (stroke, sw)
    return s + "/>"


def ring_tick(cx, cy, r, a, label, color, flip=False):
    """指针：从环外指向环，带标签"""
    rad = math.radians(a)
    x0, y0 = cx + (r + 30) * math.cos(rad), cy + (r + 30) * math.sin(rad)
    x1, y1 = cx + (r + 4) * math.cos(rad), cy + (r + 4) * math.sin(rad)
    lx, ly = cx + (r + 38) * math.cos(rad), cy + (r + 38) * math.sin(rad)
    anchor = "end" if flip else "start"
    return [line(x0, y0, x1, y1, color, 1.6, marker=color),
            t(lx, ly + 4, label, 10, color, "700", anchor, MONO)]


def panel_ring(px, py, pw):
    o = panel(px, py, pw, 420, 2, "环形缓冲与零拷贝",
              "每流一个预分配环形缓冲，大小 = 接收窗口（2 的幂）。offset 直接取模定位，不做分段、不做链式分配")

    cx, cy = px + 212, py + 232
    r0, r1 = 62, 104

    # 四个区段（顺时针）：可读 → 空洞 → 已收到但不连续 → 空闲
    # 角度顺序就是环上的真实顺序：可读 → 空洞 → 已收到但不连续 → 空闲。
    # highest_end 必须指"已收到"段的末端，不是起点——否则和图例对不上。
    a_consumed, a_contig, a_hole_end, a_high = 176, 286, 322, 6
    o.append(ring_sector(cx, cy, r0, r1, a_consumed, a_contig, GREEN, 0.9))
    o.append(ring_sector(cx, cy, r0, r1, a_contig, a_hole_end, "url(#hole)"))
    o.append(ring_sector(cx, cy, r0, r1, a_contig, a_hole_end, "none", stroke=RED, sw=1.2))
    o.append(ring_sector(cx, cy, r0, r1, a_hole_end, a_high, AMBER, 0.75))
    o.append(ring_sector(cx, cy, r0, r1, a_high, a_consumed + 360, "#f1f5f9"))
    o.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="none" stroke="%s" stroke-width="1.2"/>'
             % (cx, cy, r1, BORDER))
    o.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="none" stroke="%s" stroke-width="1.2"/>'
             % (cx, cy, r0, BORDER))

    o.append(t(cx, cy - 6, "ring", 13, INK, "700", "middle", MONO))
    o.append(t(cx, cy + 11, "2 的幂", 9.5, FAINT, "500", "middle"))
    o.append(t(cx, cy + 25, "offset & (cap-1)", 9, FAINT, "500", "middle", MONO))

    # 三个指针
    o += ring_tick(cx, cy, r1, a_consumed, "consumed", GREEN, flip=True)
    o += ring_tick(cx, cy, r1, a_contig, "contiguous", RED)
    o += ring_tick(cx, cy, r1, a_high, "highest_end", AMBER)

    # 图例
    ly = py + 96
    lx = px + 392
    legend = [(GREEN, 0.9, "可读", "[consumed, contiguous) —— 连续已收，应用可取"),
              ("url(#hole)", None, "空洞", "等重传填上；填上后 contiguous 才前进"),
              (AMBER, 0.75, "已收到但不连续", "记在 RangeSet 里，暂时不可读"),
              ("#f1f5f9", None, "空闲", "未消费数据永不超过窗口——广告的就是「已消费 + 窗口」")]
    for i, (c, op, name, desc) in enumerate(legend):
        yy = ly + i * 34
        o.append(rect(lx, yy, 16, 16, c, BORDER, rx=3, op=op))
        o.append(t(lx + 26, yy + 8, name, 11, INK, "600"))
        o.append(t(lx + 26, yy + 23, desc, 10, MUTED))

    # ---- 零拷贝路径 ----
    zy = ly + 4 * 34 + 22
    o.append(rect(lx, zy, pw - (lx - px) - 34, 112, "#f8fafc", BORDER, rx=10))
    o.append(t(lx + 16, zy + 24, "零拷贝读取", 12, TEAL, "700"))
    o.append(t(lx + 16, zy + 44, "Peek 返回的是指向环内的指针，不复制：", 10.5, MUTED))
    o.append(rect(lx + 16, zy + 52, pw - (lx - px) - 70, 24, WHITE, BORDER, rx=5))
    o.append(t(lx + 26, zy + 68, "return Bytes(st.ring.data() + pos, n);", 10, INK, "600",
               "start", MONO))
    o.append(t(lx + 16, zy + 94,
               "回绕时只返回到缓冲末尾那一段，调用方循环取；Consume 归还后空间才可被覆盖。",
               10, MUTED))

    # 从可读扇区外缘绕到零拷贝框左侧。早先版本控制点在圆心附近，线直接穿过
    # 圆环中心，把 "ring / 2 的幂" 那几行字划掉了。
    tip = math.radians(220)
    tx0 = cx + (r1 + 8) * math.cos(tip)
    ty0 = cy + (r1 + 8) * math.sin(tip)
    o.append(path("M%.1f %.1f C%.1f %.1f %.1f %.1f %.1f %.1f"
                  % (tx0, ty0, tx0 - 40, ty0 + 90, lx - 150, zy + 120, lx - 8, zy + 56),
                  TEAL, sw=1.6, dash="4 4", marker=TEAL))

    # 应用侧 Peek / Consume
    ax = px + 42
    ay = py + 356
    for i, (name, note, c) in enumerate([("Peek(sid)", "零拷贝取视图", TEAL),
                                         ("Consume(sid, n)", "归还并前推窗口", BLUE)]):
        bx = ax + i * 176
        o.append(rect(bx, ay, 164, 40, WHITE, c, rx=8, sw=1.3))
        o.append(t(bx + 12, ay + 18, name, 11, INK, "700", "start", MONO))
        o.append(t(bx + 12, ay + 32, note, 9.5, c, "500"))
        o.append(path("M%.1f %.1f V%.1f" % (bx + 82, ay - 4, cy + r1 + 14),
                      c, sw=1.4, dash="3 3"))
    return o


def build():
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" '
         'role="img" aria-label="xport 关键结构解剖：报文字节布局、环形缓冲与零拷贝">'
         % (W, H, W, H)]
    o += defs()
    o.append(rect(0, 0, W, H, WHITE, rx=0))

    o.append(t(36, 46, "关键结构解剖", 19, INK, "700"))
    o.append(t(36, 69, "字节布局与指针语义都按源码绘制——不是示意，是实际内存里的样子。", 12, MUTED))
    p, pw0 = pill(W - 36 - pill_w("热路径零堆分配"), 34, "热路径零堆分配", BLUE)
    o += p

    o += panel_packet(36, 92, W - 72)
    o += panel_ring(36, 484, W - 72)

    o.append(t(36, H - 24,
               "图由 scripts/plot_anatomy.py 生成。字段偏移对应 EncodeHeader / EncodeStream，"
               "指针命名对应 InStream 与 Transport::Peek。",
               10, FAINT))
    o.append(rect(0.5, 0.5, W - 1, H - 1, "none", BORDER, rx=14))
    o.append("</svg>")
    return "\n".join(o)


def main():
    os.makedirs(OUT, exist_ok=True)
    p = os.path.join(OUT, "anatomy.svg")
    c = build()
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(c)
    print("  %-28s %6.1f KB" % ("anatomy.svg", len(c.encode("utf-8")) / 1024.0))


if __name__ == "__main__":
    main()
