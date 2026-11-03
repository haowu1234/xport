#!/usr/bin/env python3
"""生成 README 概念图 docs/figs/packet-lifecycle.svg —— 一个数据包的一生。

和 plot_arch.py 的分工：那张讲"有哪些模块"（静态分层），这张讲"一个包经历了什么"
（动态流程）。配色、字体、圆角风格三张图保持一致。

图里的常量对应源码定义，改代码时同步这里：
  8 段/包      Transport::kMaxSegsPerPacket
  16 段 ACK    kMaxAckRanges
  16384 位图   Transport::kRecvBits
  4 KB quantum TransportConfig::drr_quantum (4 * 1368)

    python scripts/plot_concept.py
"""

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

FONT = ("-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC',"
        "'Microsoft YaHei',sans-serif")
MONO = "ui-monospace,SFMono-Regular,Menlo,Consolas,monospace"

W, H = 1060, 610


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def t(x, y, s, size=12, color=INK, weight="400", anchor="start", family=None, op=None):
    extra = ' fill-opacity="%s"' % op if op is not None else ""
    return ('<text x="%.1f" y="%.1f" font-family="%s" font-size="%s" font-weight="%s" '
            'fill="%s" text-anchor="%s"%s>%s</text>'
            % (x, y, family or FONT, size, weight, color, anchor, extra, esc(s)))


def rect(x, y, w, h, fill, stroke=None, rx=8, sw=1, op=None, extra=""):
    st = ' stroke="%s" stroke-width="%s"' % (stroke, sw) if stroke else ""
    fo = ' fill-opacity="%s"' % op if op is not None else ""
    return ('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%s" fill="%s"%s%s%s/>'
            % (x, y, w, h, rx, fill, fo, st, extra))


def circle(cx, cy, r, fill, stroke=None, sw=1.4, op=None):
    st = ' stroke="%s" stroke-width="%s"' % (stroke, sw) if stroke else ""
    fo = ' fill-opacity="%s"' % op if op is not None else ""
    return '<circle cx="%.1f" cy="%.1f" r="%.1f" fill="%s"%s%s/>' % (cx, cy, r, fill, fo, st)


def path(d, stroke=None, fill="none", sw=1.6, dash=None, marker=None, cap="round"):
    s = '<path d="%s" fill="%s"' % (d, fill)
    if stroke:
        s += ' stroke="%s" stroke-width="%s" stroke-linecap="%s"' % (stroke, sw, cap)
    if dash:
        s += ' stroke-dasharray="%s"' % dash
    if marker:
        s += ' marker-end="url(#a-%s)"' % marker.lstrip("#")
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
    for c in (BLUE, TEAL, AMBER, VIOLET, GREEN, FAINT, RED):
        o.append('<marker id="a-%s" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="6.5" '
                 'markerHeight="6.5" orient="auto-start-reverse"><path d="M0 1 L9 5 L0 9 z" '
                 'fill="%s"/></marker>' % (c.lstrip("#"), c))
    # 数据包渐变：让"包"这个图形有体积感
    o.append('<linearGradient id="pkt" x1="0" y1="0" x2="1" y2="1">'
             '<stop offset="0" stop-color="#3b82f6"/><stop offset="1" stop-color="#2563eb"/>'
             "</linearGradient>")
    o.append("</defs>")
    return o


# ---------- 图形化小图标（都用纯路径，不依赖字体） ----------

def icon_streams(x, y, c):
    """三条粗细不同的流，表示多流复用"""
    o = []
    for i, (w, op) in enumerate(((16, 1.0), (11, 0.65), (14, 0.85))):
        yy = y + i * 7
        o.append(rect(x, yy, w, 4.5, c, rx=2.2, op=op))
    return o


def icon_pack(x, y, c):
    """一个大框里塞多个小块，表示帧聚合"""
    o = [rect(x, y, 30, 20, c, rx=4, op=0.14), rect(x, y, 30, 20, "none", c, rx=4, sw=1.2)]
    for i in range(3):
        o.append(rect(x + 3.5 + i * 8.3, y + 5.5, 6.4, 9, c, rx=1.6, op=0.75))
    return o


def icon_meter(x, y, c):
    """半圆仪表 + 指针，表示拥塞控制"""
    return [path("M%.1f %.1f a14 14 0 0 1 28 0" % (x, y + 14), c, sw=2.6),
            path("M%.1f %.1f L%.1f %.1f" % (x + 14, y + 14, x + 21, y + 4.5), c, sw=2),
            circle(x + 14, y + 14, 2.4, c)]


def icon_drip(x, y, c):
    """空心漏斗 + 两滴落下，表示 pacing 把窗口匀速放行"""
    return [path("M%.1f %.1f L%.1f %.1f L%.1f %.1f L%.1f %.1f Z"
                 % (x, y, x + 24, y, x + 14, y + 12, x + 10, y + 12), c, c, sw=1.3, cap="butt"),
            rect(x + 10.5, y + 12, 3, 5, c, rx=1, op=0.75),
            circle(x + 12, y + 21.5, 2.4, c, op=0.9),
            circle(x + 12, y + 27.5, 1.6, c, op=0.45)]


def icon_shield(x, y, c):
    """盾形，表示可靠传输兜底"""
    return [path("M%.1f %.1f L%.1f %.1f L%.1f %.1f "
                 "C%.1f %.1f %.1f %.1f %.1f %.1f "
                 "C%.1f %.1f %.1f %.1f %.1f %.1f Z"
                 % (x + 12, y, x + 24, y + 5, x + 24, y + 13,
                    x + 24, y + 20, x + 18, y + 25, x + 12, y + 27,
                    x + 6, y + 25, x, y + 20, x, y + 13),
                 c, c, sw=1.2)]


def icon_clock(x, y, c):
    """时钟，表示 PTO 定时器"""
    return [circle(x + 13, y + 13, 12, c, c, sw=1.4, op=0.14),
            circle(x + 13, y + 13, 12, "none", c, sw=1.5),
            path("M%.1f %.1f L%.1f %.1f M%.1f %.1f L%.1f %.1f"
                 % (x + 13, y + 13, x + 13, y + 6.5, x + 13, y + 13, x + 18, y + 15), c, sw=1.8)]


def icon_ring(x, y, c):
    """环形缓冲：圆环 + 缺口 + 填充扇区"""
    return [circle(x + 13, y + 13, 12, "none", c, sw=3.4, op=0.22),
            path("M%.1f %.1f A12 12 0 0 1 %.1f %.1f" % (x + 13, y + 1, x + 25, y + 13),
                 c, sw=3.4),
            circle(x + 13, y + 13, 3.4, c, op=0.5)]


def icon_check(x, y, c):
    """对勾，表示交付"""
    return [circle(x + 13, y + 13, 12, c, op=0.14),
            path("M%.1f %.1f L%.1f %.1f L%.1f %.1f"
                 % (x + 7, y + 13.5, x + 11.5, y + 18, x + 19.5, y + 8), c, sw=2.6)]


def icon_puzzle(x, y, c):
    """带缺口的方块序列，表示乱序重组时的空洞"""
    o = []
    for i, filled in enumerate((True, True, False, True)):
        xx = x + i * 7.4
        if filled:
            o.append(rect(xx, y + 6, 6, 14, c, rx=1.8, op=0.8))
        else:
            o.append(rect(xx, y + 6, 6, 14, "none", c, rx=1.8, sw=1.3))
            o.append(path("M%.1f %.1f L%.1f %.1f" % (xx + 1.5, y + 2.5, xx + 4.5, y + 2.5),
                          c, sw=1.6))
    return o


ICONS = {"streams": icon_streams, "pack": icon_pack, "meter": icon_meter, "drip": icon_drip,
         "shield": icon_shield, "clock": icon_clock, "ring": icon_ring, "check": icon_check,
         "puzzle": icon_puzzle}


def station(x, y, w, h, n, title, icon, lines, color, tag=None):
    """流水线上的一站：序号 + 图标 + 标题 + 说明"""
    o = [rect(x, y, w, h, WHITE, BORDER, rx=11, extra=' filter="url(#sh)"'),
         rect(x, y, w, 3.5, color, rx=1.8, extra=' clip-path="inset(0 0 0 0 round 2px)"')]
    # 左上角序号
    o.append(circle(x + 20, y + 26, 11, color, op=0.14))
    o.append(t(x + 20, y + 30.5, str(n), 12, color, "700", "middle", MONO))
    # 图标放右上
    o += ICONS[icon](x + w - 52, y + 14, color)
    o.append(t(x + 38, y + 31, title, 13, INK, "600"))
    for i, s in enumerate(lines):
        o.append(t(x + 18, y + 53 + i * 15.5, s, 10.5, MUTED))
    if tag:
        p, pw = pill(x + 18, y + h - 27, tag, color, 9.5, 17)
        o += p
    return o


def build():
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" '
         'role="img" aria-label="xport：一个数据包的一生 —— 从应用写入到对端交付，再到 ACK 回流">'
         % (W, H, W, H)]
    o += defs()
    o.append(rect(0, 0, W, H, WHITE, rx=0))

    o.append(t(36, 46, "一个数据包的一生", 19, INK, "700"))
    o.append(t(36, 69, "从应用写入到对端交付，再到 ACK 回流闭环。每一站标注了实际生效的常量与机制。",
               12, MUTED))
    p, pw = pill(W - 36 - pill_w("单线程 · 无锁 · 热路径零堆分配"), 34,
                 "单线程 · 无锁 · 热路径零堆分配", BLUE)
    o += p

    # ================= 发送侧 =================
    o.append(t(36, 112, "发送侧", 11, BLUE, "700"))
    o.append(path("M78 108 H%d" % (W - 36), BORDER, sw=1))

    sw_, sh, gap = 222, 128, 14
    sx = 36
    sy = 126
    o += station(sx, sy, sw_, sh, 1, "多流调度", "streams", [
        "DRR 轮转，按字节公平", "每轮每流额度 4 KB", "小流不会被大流饿死"], BLUE, "drr_quantum")
    o += station(sx + (sw_ + gap), sy, sw_, sh, 2, "帧聚合", "pack", [
        "一个报文最多 8 段", "可跨流拼在同一个包里", "小消息 pps 降 4 倍"], VIOLET,
        "kMaxSegsPerPacket")
    o += station(sx + 2 * (sw_ + gap), sy, sw_, sh, 3, "拥塞控制", "meter", [
        "CUBIC 三次曲线 + 快速收敛", "HyStart++ 退出慢启动", "窗口用不满就不涨窗"], AMBER,
        "RFC 9438 / 7661")
    o += station(sx + 3 * (sw_ + gap), sy, sw_, sh, 4, "pacing 放行", "drip", [
        "令牌桶按 cwnd/SRTT 匀速", "慢启动 2×，拥塞避免 1.25×", "杜绝一次倒空整个窗口"], TEAL,
        "突发 8 包")

    # ================= 线上 =================
    wy = sy + sh + 26
    o.append(rect(36, wy, W - 72 - 122, 74, "#f8fafc", BORDER, rx=12))
    o.append(t(54, wy + 25, "线上", 11, INK, "700"))
    o.append(t(54, wy + 44, "20 B 定长头 + CRC16", 10.5, MUTED))
    o.append(t(54, wy + 60, "批量收发：一次 syscall 发一批", 10.5, MUTED))

    # 画 6 个包在飞，其中一个标红表示丢失
    pxs = 250
    for i in range(6):
        px = pxs + i * 76
        lost = i == 3
        c = RED if lost else "url(#pkt)"
        o.append(rect(px, wy + 28, 54, 26, c, rx=5, op=0.35 if lost else None))
        o.append(t(px + 27, wy + 45, "pn=%d" % (101 + i), 9.5,
                   RED if lost else WHITE, "600", "middle", MONO))
        if lost:
            o.append(path("M%.1f %.1f L%.1f %.1f M%.1f %.1f L%.1f %.1f"
                          % (px + 16, wy + 34, px + 38, wy + 48,
                             px + 38, wy + 34, px + 16, wy + 48), RED, sw=2))
            o.append(t(px + 27, wy + 68, "丢失", 9.5, RED, "600", "middle"))
    o.append(t(W - 176, wy + 45, "→", 16, FAINT, "400", "end"))

    # ================= 接收侧 =================
    ry = wy + 100
    o.append(t(36, ry + 4, "接收侧", 11, TEAL, "700"))
    o.append(path("M88 %d H%d" % (ry, W - 36), BORDER, sw=1))

    ry += 18
    o += station(sx, ry, sw_, sh, 5, "乱序重组", "puzzle", [
        "16384 位滑动位图记包号", "区间合并，重复包幂等", "空洞等重传填上"], TEAL, "kRecvBits")
    o += station(sx + (sw_ + gap), ry, sw_, sh, 6, "消费式缓冲", "ring", [
        "每流预分配环形缓冲", "Peek 零拷贝读、Consume 归还", "不消费则窗口不前进"], TEAL,
        "背压")
    o += station(sx + 2 * (sw_ + gap), ry, sw_, sh, 7, "SACK 回执", "clock", [
        "显式区间，最多 16 段", "不是 64 位图：窗口大也不误判", "随 ACK 捎带 MAX_DATA"], VIOLET,
        "kMaxAckRanges")
    o += station(sx + 3 * (sw_ + gap), ry, sw_, sh, 8, "交付应用", "check", [
        "按流有序、逐字节一致", "丢包由 PTO 兜底补回", "超时只探测，不降窗"], GREEN, "RFC 9002")

    # ================= 连线 =================
    # 发送侧四站串联
    for i in range(3):
        x1 = sx + i * (sw_ + gap) + sw_
        o.append(path("M%.1f %.1f H%.1f" % (x1 + 2, sy + sh / 2, x1 + gap - 3),
                      FAINT, sw=1.6, marker=FAINT))
    # 接收侧四站串联
    for i in range(3):
        x1 = sx + i * (sw_ + gap) + sw_
        o.append(path("M%.1f %.1f H%.1f" % (x1 + 2, ry + sh / 2, x1 + gap - 3),
                      FAINT, sw=1.6, marker=FAINT))
    # 发送侧 → 线上
    o.append(path("M%.1f %.1f V%.1f" % (sx + 3 * (sw_ + gap) + sw_ / 2, sy + sh + 2, wy - 3),
                  TEAL, sw=1.8, marker=TEAL))
    # 线上 → 接收侧
    o.append(path("M%.1f %.1f V%.1f" % (sx + sw_ / 2, wy + 76, ry - 3), TEAL, sw=1.8, marker=TEAL))

    # ACK 回流：沿最右侧的专用通道上行。数据下行走中间、ACK 上行走右侧，
    # 两个方向在视觉上分开。试过的更差版本：斜穿右侧会压在第 4 站上；
    # 沿底部绕左侧会把整张图框住像边框；走中间缝隙会穿过"线上"区。
    lane_x = W - 128                             # 右侧专用通道（右边再留 128 给标签）
    ack_y = ry + sh * 0.42                       # 第 8 站右侧的出发高度
    cc_y = sy + sh * 0.62                        # 回到第 3 站的高度
    r = 10
    o.append(path(
        "M%.1f %.1f H%.1f "
        "Q%.1f %.1f %.1f %.1f "
        "V%.1f "
        "Q%.1f %.1f %.1f %.1f "
        "H%.1f"
        % (sx + 3 * (sw_ + gap) + sw_ + 4, ack_y, lane_x - r,
           lane_x, ack_y, lane_x, ack_y - r,
           cc_y + r,
           lane_x, cc_y, lane_x - r, cc_y,
           sx + 3 * (sw_ + gap) + sw_ + 6),
        VIOLET, sw=1.7, dash="5 4"))
    o.append(path("M%.1f %.1f H%.1f"
                  % (sx + 3 * (sw_ + gap) + sw_ + 6, cc_y,
                     sx + 3 * (sw_ + gap) + sw_ - 4),
                  VIOLET, sw=1.7, dash="5 4", marker=VIOLET))
    # 标签横排放在通道右侧的空白里：竖排或压在线上都会糊
    pv, pvw = pill(lane_x + 14, wy + 22, "ACK 回流", VIOLET, 10)
    o += pv
    o.append(t(lane_x + 14, wy + 58, "RTT 采样", 9.5, VIOLET, "500"))
    o.append(t(lane_x + 14, wy + 72, "判丢 · 调窗", 9.5, VIOLET, "500"))
    o.append(t(36, H - 22,
               "丢失的 pn=104 由包阈值（3 个后续包被确认）或 PTO 超时发现，重传换新包号、"
               "字节偏移不变——ACK 只认包号，流控只认偏移。",
               10.5, FAINT))
    o.append(rect(0.5, 0.5, W - 1, H - 1, "none", BORDER, rx=14))
    o.append("</svg>")
    return "\n".join(o)


def main():
    os.makedirs(OUT, exist_ok=True)
    p = os.path.join(OUT, "packet-lifecycle.svg")
    c = build()
    with open(p, "w", encoding="utf-8", newline="\n") as f:
        f.write(c)
    print("  %-28s %6.1f KB" % ("packet-lifecycle.svg", len(c.encode("utf-8")) / 1024.0))


if __name__ == "__main__":
    main()
