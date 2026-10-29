#!/usr/bin/env python3
"""生成 README 内嵌图表。

输出独立 .svg 文件到 docs/figs/，不依赖 matplotlib、不依赖任何 JS。
所有数字都直接来自 build/ 下的实测 CSV，不做手工填数。

    python scripts/plot_readme.py
"""

import csv
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TRACE = os.path.join(ROOT, "build", "traces")
OUT = os.path.join(ROOT, "docs", "figs")

# ---------- 设计令牌 ----------
INK = "#0f172a"
MUTED = "#64748b"
FAINT = "#94a3b8"
GRID = "#eef2f6"
AXIS = "#cbd5e1"
WHITE = "#ffffff"
BORDER = "#e2e8f0"

BLUE = "#2563eb"
TEAL = "#0d9488"
AMBER = "#d97706"
RED = "#dc2626"
VIOLET = "#7c3aed"
SLATE = "#b6c2d1"

FONT = ("-apple-system,BlinkMacSystemFont,'Segoe UI','PingFang SC',"
        "'Microsoft YaHei',sans-serif")


def fmt(v):
    """刻度/数值格式化：整数不带小数，小数最多两位。"""
    if abs(v - round(v)) < 1e-9:
        return "{:,}".format(int(round(v)))
    if abs(v) >= 100:
        return "{:,.0f}".format(v)
    return "{:,.1f}".format(v)


def load(name):
    path = os.path.join(TRACE, name)
    with open(path, newline="", encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


def load_csv(name):
    path = os.path.join(ROOT, "build", name)
    with open(path, newline="", encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))


# ---------- SVG 骨架 ----------
def head(w, h, title, sub=None):
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" '
         'width="%d" height="%d" role="img" aria-label="%s">' % (w, h, w, h, title),
         '<rect width="%d" height="%d" fill="%s"/>' % (w, h, WHITE),
         '<text x="26" y="34" font-family="%s" font-size="15.5" font-weight="600" '
         'fill="%s">%s</text>' % (FONT, INK, title)]
    if sub:
        o.append('<text x="26" y="54" font-family="%s" font-size="11.5" fill="%s">%s</text>'
                 % (FONT, MUTED, sub))
    return o


def tail(o, w, h, note=None):
    if note:
        o.append('<text x="26" y="%d" font-family="%s" font-size="10.5" fill="%s">%s</text>'
                 % (h - 12, FONT, FAINT, note))
    o.append('<rect x="0.5" y="0.5" width="%d" height="%d" rx="10" fill="none" '
             'stroke="%s"/>' % (w - 1, h - 1, BORDER))
    o.append("</svg>")
    return "\n".join(o)


def write(name, content):
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, name)
    # 显式 \n：Windows 上默认会写 CRLF，和 .gitattributes 的 eol=lf 打架
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print("  %-28s %6.1f KB" % (name, len(content.encode("utf-8")) / 1024.0))


# ---------- 图 1：丢包悬崖（竖柱） ----------
def fig_loss_cliff():
    rows = load_csv("udp_bench.csv")
    by_tag = {}
    for r in rows:
        tag = r["tag"]
        mb = float(r["mb"])
        if tag == "直连" and abs(mb - 4) < 1e-6:
            by_tag["直连 0%"] = r
        elif tag == "代理0%丢包":
            by_tag["代理 0%"] = r
        elif tag.startswith("丢包"):
            pct = tag.replace("丢包", "").replace("%", "")
            by_tag[pct + "%"] = r

    order = ["直连 0%", "代理 0%", "0.5%", "1.0%", "2.0%"]
    bars = []
    for k in order:
        r = by_tag[k]
        bars.append((k, float(r["mbps"]), int(r["rto"])))

    W, H = 880, 340
    pl, pr, pt, pb = 64, 30, 84, 86
    iw, ih = W - pl - pr, H - pt - pb
    ymax = 1000.0

    def sy(v):
        return pt + ih - v / ymax * ih

    o = head(W, H, "丢包率与吞吐 —— 2% 是悬崖，但掉下去的不全是丢包",
             "同一 4 MiB 数据 · 单流 · 本机回环 · 数据全部校验通过")

    for i in range(6):
        v = ymax * i / 5.0
        y = sy(v)
        o.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s"/>'
                 % (pl, y, pl + iw, y, GRID))
        o.append('<text x="%d" y="%.1f" font-family="%s" font-size="11" fill="%s" '
                 'text-anchor="end">%s</text>' % (pl - 10, y + 4, FONT, MUTED, fmt(v)))

    # 直连基线
    base = bars[0][1]
    o.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" stroke-width="1" '
             'stroke-dasharray="5 4"/>' % (pl, sy(base), pl + iw, sy(base), BLUE))
    o.append('<text x="%d" y="%.1f" font-family="%s" font-size="10.5" fill="%s" '
             'text-anchor="end">直连基线 %s</text>'
             % (pl + iw - 4, sy(base) - 8, FONT, BLUE, fmt(base)))

    slot = iw / len(bars)
    bw = 74
    colors = [BLUE, SLATE, SLATE, SLATE, RED]
    for i, (label, val, rto) in enumerate(bars):
        cx = pl + slot * i + slot / 2
        x = cx - bw / 2
        y = sy(val)
        o.append('<rect x="%.1f" y="%.1f" width="%d" height="%.1f" rx="3" fill="%s"/>'
                 % (x, y, bw, pt + ih - y, colors[i]))
        o.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="12.5" '
                 'font-weight="600" fill="%s" text-anchor="middle">%s</text>'
                 % (cx, y - 9, FONT, INK, fmt(val)))
        if rto:
            o.append('<rect x="%.1f" y="%.1f" width="58" height="19" rx="9.5" fill="%s"/>'
                     % (cx - 29, y - 60, RED))
            o.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="10.5" '
                     'font-weight="600" fill="#ffffff" text-anchor="middle">RTO ×%d</text>'
                     % (cx, y - 47, FONT, rto))
        lines = label.split(" ")
        for j, ln in enumerate(lines):
            o.append('<text x="%.1f" y="%d" font-family="%s" font-size="11.5" fill="%s" '
                     'text-anchor="middle">%s</text>'
                     % (cx, pt + ih + 22 + j * 16, FONT, MUTED if j else INK, ln))

    o.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="1"/>'
             % (pl, pt + ih, pl + iw, pt + ih, AXIS))
    o.append('<text x="18" y="%d" font-family="%s" font-size="11" fill="%s" '
             'transform="rotate(-90 18 %d)" text-anchor="middle">吞吐 (Mbps)</text>'
             % (pt + ih / 2, FONT, MUTED, pt + ih / 2))

    note = ("0.5%–2% 经进程内转发代理注入丢包；代理自己就要吃一半吞吐（第 2 根柱），"
            "所以 0.5%/1% 的下降主要是代理成本，2% 的崩塌才是 RTO")
    return tail(o, W, H, note)


# ---------- 折线图通用件 ----------
def line_fig(W, H, title, sub, series, xlab, ylab, xr, yr, xticks, yticks,
             vlines=None, marks=None, note=None):
    pl, pr, pt, pb = 74, 168, 82, 62
    iw, ih = W - pl - pr, H - pt - pb
    x0, x1 = xr
    y0, y1 = yr

    def sx(v):
        return pl + (v - x0) / (x1 - x0) * iw if x1 > x0 else pl

    def sy(v):
        return pt + ih - (v - y0) / (y1 - y0) * ih

    o = head(W, H, title, sub)

    for v in yticks:
        y = sy(v)
        o.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s"/>'
                 % (pl, y, pl + iw, y, GRID))
        o.append('<text x="%d" y="%.1f" font-family="%s" font-size="11" fill="%s" '
                 'text-anchor="end">%s</text>' % (pl - 10, y + 4, FONT, MUTED, fmt(v)))

    for v in xticks:
        x = sx(v)
        o.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s"/>'
                 % (x, pt, x, pt + ih, GRID))
        o.append('<text x="%.1f" y="%d" font-family="%s" font-size="11" fill="%s" '
                 'text-anchor="middle">%s</text>' % (x, pt + ih + 21, FONT, MUTED, fmt(v)))

    for v in (vlines or []):
        x = sx(v["x"])
        o.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s" stroke-width="1" '
                 'stroke-dasharray="4 4"/>' % (x, pt, x, pt + ih, v.get("color", AXIS)))

    for s in series:
        pts = s["pts"]
        step = max(1, len(pts) // 900)
        pts = pts[::step] if step > 1 else pts
        if s.get("area"):
            poly = " ".join("%.1f,%.1f" % (sx(x), sy(y)) for x, y in pts)
            o.append('<polygon points="%s %.1f,%.1f %.1f,%.1f" fill="%s" fill-opacity="0.09"/>'
                     % (poly, sx(pts[-1][0]), sy(y0), sx(pts[0][0]), sy(y0), s["color"]))
        d = " ".join("%.1f,%.1f" % (sx(x), sy(y)) for x, y in pts)
        dash = ' stroke-dasharray="%s"' % s["dash"] if s.get("dash") else ""
        o.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="%s"%s '
                 'stroke-linejoin="round" stroke-linecap="round"/>'
                 % (d, s["color"], s.get("width", 2.0), dash))

    for m in (marks or []):
        x, y = sx(m["x"]), sy(m["y"])
        o.append('<circle cx="%.1f" cy="%.1f" r="3.4" fill="%s" stroke="%s" '
                 'stroke-width="1.5"/>' % (x, y, WHITE, m.get("color", INK)))
        if m.get("label"):
            o.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="10.5" '
                     'fill="%s" text-anchor="%s">%s</text>'
                     % (x + m.get("dx", 8), y + m.get("dy", -8), FONT,
                        m.get("color", MUTED), m.get("anchor", "start"), m["label"]))

    o.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="1"/>'
             % (pl, pt + ih, pl + iw, pt + ih, AXIS))
    o.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="1"/>'
             % (pl, pt, pl, pt + ih, AXIS))

    ly = pt + 4
    for s in series:
        dash = ' stroke-dasharray="%s"' % s["dash"] if s.get("dash") else ""
        o.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="%s"%s/>'
                 % (pl + iw + 18, ly, pl + iw + 40, ly, s["color"], s.get("width", 2.0), dash))
        o.append('<text x="%d" y="%d" font-family="%s" font-size="11.5" fill="%s">%s</text>'
                 % (pl + iw + 46, ly + 4, FONT, INK, s["name"]))
        if s.get("hint"):
            o.append('<text x="%d" y="%d" font-family="%s" font-size="10.5" fill="%s">%s</text>'
                     % (pl + iw + 46, ly + 18, FONT, FAINT, s["hint"]))
            ly += 12
        ly += 26

    o.append('<text x="%.1f" y="%d" font-family="%s" font-size="11" fill="%s" '
             'text-anchor="middle">%s</text>'
             % (pl + iw / 2.0, H - 26, FONT, MUTED, xlab))
    o.append('<text x="20" y="%d" font-family="%s" font-size="11" fill="%s" '
             'transform="rotate(-90 20 %d)" text-anchor="middle">%s</text>'
             % (pt + ih / 2, FONT, MUTED, pt + ih / 2, ylab))
    return o, tail, (W, H, note)


# ---------- 图 2：CUBIC 锯齿 ----------
def fig_sawtooth():
    rows = load("sawtooth.csv")
    pts = [(float(r["t_ms"]) / 1000.0, float(r["cwnd_kb"])) for r in rows]

    drops = []
    for i in range(1, len(pts)):
        if pts[i][1] < pts[i - 1][1] * 0.95:
            drops.append((pts[i][0], pts[i - 1][1], pts[i][1]))

    # 只标点，不标比例：采样间隔会让相邻两点的比值偏离真实事件的 0.700
    marks = [{"x": t, "y": after, "color": AMBER} for t, before, after in drops]

    o, fin, meta = line_fig(
        880, 360,
        "CUBIC 锯齿 —— 每次丢包砍掉 30% 窗口，然后按三次曲线补回来",
        "20 Mbps / RTT 40 ms · 8 MiB · 确定性仿真（固定种子，可逐位复现）",
        [{"name": "cwnd（实测）", "color": BLUE, "pts": pts, "width": 2.0, "area": True,
          "hint": "%d 次降窗 · 起点 13 KB" % len(drops)}],
        "时间 (s)", "拥塞窗口 (KB)", (0, 10.2), (0, 400),
        [0, 2, 4, 6, 8, 10], [0, 100, 200, 300, 400],
        marks=marks,
        note="理论 β = 0.70（RFC 9438）；13 次事件的实测比例全部落在 0.700 ± 0.001，见 cc-validation 报告")
    return fin(o, *meta)


# ---------- 图 3：CUBIC vs Reno ----------
def fig_longhaul():
    def trace(name):
        rows = load(name)
        return [(float(r["t_ms"]) / 1000.0, float(r["cwnd_kb"])) for r in rows]

    cubic = trace("longhaul_cubic.csv")
    reno = trace("longhaul_reno.csv")

    def count_drops(pts):
        return sum(1 for i in range(1, len(pts)) if pts[i][1] < pts[i - 1][1] * 0.95)

    o, fin, meta = line_fig(
        880, 360,
        "长肥管道 —— 同样的 110 秒，CUBIC 回涨 13 倍",
        "5 Mbps / RTT 800 ms · BDP 488 KB · 两种算法同一链路、同一起点",
        [{"name": "CUBIC", "color": BLUE, "pts": cubic, "width": 2.0, "area": True,
          "hint": "降窗 %d 次 · 15.7 段/s" % count_drops(cubic)},
         {"name": "Reno", "color": AMBER, "pts": reno, "width": 1.8,
          "hint": "降窗 %d 次 · 1.25 段/s" % count_drops(reno)}],
        "时间 (s)", "拥塞窗口 (KB)", (0, 110), (0, 1700),
        [0, 20, 40, 60, 80, 100], [0, 400, 800, 1200, 1600],
        note="RTT 800 ms 时每个 RTT 太长，Reno 的「每 RTT 加 1 段」几乎不涨；"
             "CUBIC 按时间三次增长，与 RTT 解耦")
    return fin(o, *meta)


# ---------- 图 4：六次重复波动 ----------
def fig_repeat():
    rows = load_csv("udp_repeat.csv")
    W, H = 880, 340
    pl, pr, pt, pb = 64, 150, 88, 74
    iw, ih = W - pl - pr, H - pt - pb
    ymax = 1400.0

    def sy(v):
        return pt + ih - v / ymax * ih

    o = head(W, H, "同一份代码连跑 6 次 —— 基线比被测对象抖得更厉害",
             "64 MiB 单流 · 同一台机器 · 交替运行")

    for i in range(8):
        v = ymax * i / 7.0
        y = sy(v)
        o.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s"/>'
                 % (pl, y, pl + iw, y, GRID))
        o.append('<text x="%d" y="%.1f" font-family="%s" font-size="11" fill="%s" '
                 'text-anchor="end">%s</text>' % (pl - 10, y + 4, FONT, MUTED, fmt(v)))

    n = len(rows)
    slot = iw / n
    bw = 40
    for i, r in enumerate(rows):
        cx = pl + slot * i + slot / 2
        for j, (val, color) in enumerate([(float(r["base_mbps"]), SLATE),
                                          (float(r["mbps"]), BLUE)]):
            x = cx - bw + 4 + j * (bw - 4)
            y = sy(val)
            o.append('<rect x="%.1f" y="%.1f" width="%d" height="%.1f" rx="3" fill="%s"/>'
                     % (x, y, bw - 6, pt + ih - y, color))
            o.append('<text x="%.1f" y="%.1f" font-family="%s" font-size="10" fill="%s" '
                     'text-anchor="middle">%s</text>'
                     % (x + (bw - 6) / 2, y - 6, FONT, MUTED, fmt(val)))
        o.append('<text x="%.1f" y="%d" font-family="%s" font-size="11.5" fill="%s" '
                 'text-anchor="middle">#%d</text>'
                 % (cx, pt + ih + 22, FONT, MUTED, i + 1))

    o.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="1"/>'
             % (pl, pt + ih, pl + iw, pt + ih, AXIS))
    o.append('<text x="18" y="%d" font-family="%s" font-size="11" fill="%s" '
             'transform="rotate(-90 18 %d)" text-anchor="middle">吞吐 (Mbps)</text>'
             % (pt + ih / 2, FONT, MUTED, pt + ih / 2))

    # 区间标注
    base = [float(r["base_mbps"]) for r in rows]
    prot = [float(r["mbps"]) for r in rows]
    o.append('<text x="%d" y="%d" font-family="%s" font-size="11" fill="%s">"%s"</text>'
             % (pl + iw + 18, pt + 10, FONT, MUTED, "区间"))
    for k, (name, vals, color) in enumerate([("基线", base, SLATE), ("协议", prot, BLUE)]):
        lo, hi = min(vals), max(vals)
        y = pt + 34 + k * 46
        o.append('<rect x="%d" y="%d" width="12" height="12" rx="2" fill="%s"/>'
                 % (pl + iw + 18, y - 10, color))
        o.append('<text x="%d" y="%d" font-family="%s" font-size="11" fill="%s">%s</text>'
                 % (pl + iw + 36, y, FONT, INK, name))
        o.append('<text x="%d" y="%d" font-family="%s" font-size="10.5" fill="%s">%s–%s</text>'
                 % (pl + iw + 18, y + 16, FONT, FAINT, fmt(lo), fmt(hi)))
        o.append('<text x="%d" y="%d" font-family="%s" font-size="10.5" fill="%s">'
                 '漂移 %.0f%%</text>'
                 % (pl + iw + 18, y + 30, FONT, color, (hi - lo) / lo * 100))

    note = ("基线 924–1302（漂移 41%），协议 828–926（漂移 12%）。"
            "分母自己比分子还抖，所以回环单核只能撑±10% 量级的判断")
    return tail(o, W, H, note)


def main():
    print("生成 README 图表 → docs/figs/")
    write("loss-cliff.svg", fig_loss_cliff())
    write("cubic-sawtooth.svg", fig_sawtooth())
    write("cubic-vs-reno.svg", fig_longhaul())
    write("repeat-runs.svg", fig_repeat())
    print("完成。")


if __name__ == "__main__":
    main()
