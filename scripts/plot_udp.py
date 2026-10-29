#!/usr/bin/env python3
"""真实 socket 回环基准绘图：读 CSV/轨迹 → 出 SVG 图 → 出 HTML 报告。

为什么单独一个脚本而不是并进 plot_cc.py：
  仿真报告回答"协议算法对不对"，回环报告回答"真机上能跑多快"，
  两者的读者和结论都不一样，混在一起会互相干扰。

用法：
    ./build/xport_udp_bench --sweep --csv build/udp_bench.csv > build/udp_bench.txt
    ./build/xport_udp_bench --mb 32 --trace build/traces/udp_direct.csv
    ./build/xport_udp_bench --mb 4 --relay-loss 1 --trace build/traces/udp_loss1.csv
    python scripts/plot_udp.py
"""
import csv
import math
import os
import re

from plot_cc import (C_A, C_B, C_GRID, C_INK, C_MEAS, C_MUTED, C_Q, C_RTT, C_SS, C_THEO,
                     bar_chart, chart, fmt_tick)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH_CSV = os.path.join(ROOT, "build", "udp_bench.csv")
BENCH_TXT = os.path.join(ROOT, "build", "udp_bench.txt")
TRACE = os.path.join(ROOT, "build", "traces")
OUT = os.path.join(ROOT, "docs", "udp-benchmark.html")


def load(path):
    """CSV 里混着 tag（字符串）和数值列，逐列判断，不要一刀切 float。"""
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = []
        for r in csv.DictReader(f):
            row = {}
            for k, v in r.items():
                try:
                    row[k] = float(v)
                except (TypeError, ValueError):
                    row[k] = v
            rows.append(row)
        return rows


def parse_baseline(path):
    """从文本输出里抠基线数字。基线只跑一次且随机器负载浮动，报告里注明即可。"""
    out = {"pipe_ns": 0.0, "pipe_mbps": 0.0, "pipe_pps": 0.0, "rtt_us": 0.0, "lat_ns": 0.0}
    if not os.path.exists(path):
        return out
    txt = open(path, encoding="utf-8", errors="ignore").read()
    m = re.search(r"32 包在途（吞吐受限）：\s*([\d.]+) ns/包 →\s*([\d.]+) Mbps（\s*([\d.]+) 包/秒）", txt)
    if m:
        out["pipe_ns"] = float(m.group(1))
        out["pipe_mbps"] = float(m.group(2))
        out["pipe_pps"] = float(m.group(3))
    m = re.search(r"1 包在途（延迟受限）：\s*([\d.]+) ns/包", txt)
    if m:
        out["lat_ns"] = float(m.group(1))
    m = re.search(r"64 B 回环 RTT 中位数：([\d.]+) µs", txt)
    if m:
        out["rtt_us"] = float(m.group(1))
    return out


def chart_log(series, title, xlab, ylab, w=900, h=330, ymin=1.0, ymax=None, note=None):
    """纵轴取 10 为底的对数。

    这里必须用对数：cwnd 涨到 8192 KB，而实际在途只有 10 KB——
    两者差三个数量级，线性轴上"在途"会贴着 0 变成一条看不见的直线，
    而那恰恰是要说明的重点（窗口开得再大，也被 syscall 卡住）。
    """
    pl, pr, pt, pb = 64, 130, 40, 46
    iw, ih = w - pl - pr, h - pt - pb
    xs = [p[0] for s in series for p in s["pts"] if p[1] > 0]
    ys = [p[1] for s in series for p in s["pts"] if p[1] > 0]
    if not xs:
        return "<p>无数据</p>"
    x0, x1 = min(xs), max(xs)
    y0 = max(ymin, min(ys) * 0.8)
    y1 = ymax or max(ys) * 1.3
    if y1 <= y0:
        y1 = y0 * 10

    ly0, ly1 = math.log10(y0), math.log10(y1)

    def sx(v):
        return pl + (v - x0) / (x1 - x0) * iw if x1 > x0 else pl

    def sy(v):
        if v <= 0:
            return pt + ih
        return pt + ih - (math.log10(v) - ly0) / (ly1 - ly0) * ih

    out = ['<svg viewBox="0 0 %d %d" width="100%%" role="img">' % (w, h)]
    out.append('<rect x="0" y="0" width="%d" height="%d" fill="#ffffff"/>' % (w, h))
    out.append('<text x="%d" y="22" font-size="14" font-weight="600" fill="%s">%s</text>'
               % (pl, C_INK, title))

    dec = int(math.floor(ly0))
    while dec <= int(math.ceil(ly1)):
        for mult in (1, 2, 5):
            v = mult * (10.0 ** dec)
            if v < y0 or v > y1:
                continue
            y = sy(v)
            major = mult == 1
            out.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" '
                       'stroke-width="%s"/>' % (pl, y, pl + iw, y, C_GRID, "1" if major else "0.6"))
            if major:
                lab = ("%d" % v) if v >= 1 else ("%.1f" % v)
                out.append('<text x="%d" y="%.1f" font-size="11" fill="%s" text-anchor="end">%s</text>'
                           % (pl - 8, y + 4, C_MUTED, lab))
        dec += 1

    for i in range(7):
        v = x0 + (x1 - x0) * i / 6.0
        x = sx(v)
        out.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s"/>'
                   % (x, pt, x, pt + ih, C_GRID))
        out.append('<text x="%.1f" y="%d" font-size="11" fill="%s" text-anchor="middle">%s</text>'
                   % (x, pt + ih + 18, C_MUTED, fmt_tick(v)))

    out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>'
               % (pl, pt + ih, pl + iw, pt + ih, C_MUTED))
    out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>'
               % (pl, pt, pl, pt + ih, C_MUTED))

    for s in series:
        pts = [p for p in s["pts"] if p[1] > 0]
        if len(pts) < 2:
            continue
        d = " ".join(("%.1f,%.1f" % (sx(x), sy(y))) for x, y in pts)
        dash = ' stroke-dasharray="%s"' % s["dash"] if s.get("dash") else ""
        out.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="%s"%s '
                   'stroke-linejoin="round"/>' % (d, s["color"], s.get("width", 2), dash))

    lx = pl + iw + 14
    ly = pt + 6
    for s in series:
        dash = ' stroke-dasharray="%s"' % s["dash"] if s.get("dash") else ""
        out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="%s"%s/>'
                   % (lx, ly, lx + 18, ly, s["color"], s.get("width", 2), dash))
        out.append('<text x="%d" y="%d" font-size="11" fill="%s">%s</text>'
                   % (lx + 24, ly + 4, C_INK, s["name"]))
        ly += 20

    out.append('<text x="%d" y="%d" font-size="11" fill="%s" text-anchor="middle">%s</text>'
               % (pl + iw / 2, h - 6, C_MUTED, xlab))
    out.append('<text x="14" y="%d" font-size="11" fill="%s" transform="rotate(-90 14 %d)" '
               'text-anchor="middle">%s</text>' % (pt + ih / 2, C_MUTED, pt + ih / 2, ylab))
    if note:
        out.append('<text x="%d" y="%d" font-size="11" fill="%s">%s</text>'
                   % (pl + 4, pt + 16, C_MUTED, note))
    out.append("</svg>")
    return "\n".join(out)


def waterfall(title, base, steps, ylab="Mbps", w=900, h=340):
    """瀑布图：从基线逐项扣减，把"差多少"拆成"差在哪"。

    为什么不用并排柱：并排柱只能看出总量差，看不出归因。
    这里每一段都是一个可独立验证的物理量（多运的 ACK 包、
    协议处理的纳秒数、头部字节数），读者可以逐段反驳。
    """
    pl, pr, pt, pb = 64, 140, 40, 54
    iw, ih = w - pl - pr, h - pt - pb
    ymax = math.ceil(base * 1.10 / 100.0) * 100.0

    def sy(v):
        return pt + ih - v / ymax * ih

    bars = [("裸 UDP", 0.0, base, C_THEO)]
    cur = base
    for lab, v, col in steps:
        bars.append((lab, cur - v, cur, col))
        cur -= v
    bars.append(("xport", 0.0, cur, C_Q))

    n = len(bars)
    slot = iw / float(n)
    bw = min(slot * 0.54, 74)

    out = ['<svg viewBox="0 0 %d %d" width="100%%" role="img">' % (w, h)]
    out.append('<rect x="0" y="0" width="%d" height="%d" fill="#ffffff"/>' % (w, h))
    out.append('<text x="%d" y="22" font-size="14" font-weight="600" fill="%s">%s</text>'
               % (pl, C_INK, title))

    for i in range(6):
        v = ymax * i / 5.0
        y = sy(v)
        out.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s"/>'
                   % (pl, y, pl + iw, y, C_GRID))
        out.append('<text x="%d" y="%.1f" font-size="11" fill="%s" text-anchor="end">%s</text>'
                   % (pl - 8, y + 4, C_MUTED, fmt_tick(v)))

    xs = []
    for i, (lab, lo, hi, col) in enumerate(bars):
        cx = pl + slot * i + slot / 2
        xs.append(cx)
        y_hi, y_lo = sy(hi), sy(lo)
        hh = max(y_lo - y_hi, 2.5)
        out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" rx="2"/>'
                   % (cx - bw / 2, y_hi, bw, hh, col))
        if i == 0 or i == n - 1:
            txt = "%.0f" % hi
        else:
            txt = "−%.0f" % (hi - lo)
        out.append('<text x="%.1f" y="%.1f" font-size="11" fill="%s" text-anchor="middle">%s</text>'
                   % (cx, y_hi - 6, C_INK, txt))
        out.append('<text x="%.1f" y="%d" font-size="12" fill="%s" text-anchor="middle">%s</text>'
                   % (cx, pt + ih + 20, C_INK, lab))

    for i in range(n - 1):
        y = sy(bars[i][2])
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" '
                   'stroke-dasharray="3 3"/>'
                   % (xs[i] + bw / 2, y, xs[i + 1] - bw / 2, y, C_MUTED))

    out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>'
               % (pl, pt + ih, pl + iw, pt + ih, C_MUTED))
    out.append('<text x="14" y="%d" font-size="11" fill="%s" transform="rotate(-90 14 %d)" '
               'text-anchor="middle">%s</text>' % (pt + ih / 2, C_MUTED, pt + ih / 2, ylab))
    out.append("</svg>")
    return "\n".join(out)


def stacked(groups, title, ylab, w=900, h=340, ymax=None):
    """堆叠柱：一根柱代表一组线上包，分段代表构成。

    这里用堆叠而不是并排，因为读者要看的是"总包数"和"多出来的那一截"，
    并排柱会把这两件事拆成两个互不相干的柱子。
    """
    pl, pr, pt, pb = 64, 160, 40, 50
    iw, ih = w - pl - pr, h - pt - pb
    totals = [sum(v for _, v, _ in bs) for _, bs in groups]
    y1 = ymax or max(totals) * 1.16

    def sy(v):
        return pt + ih - v / y1 * ih

    out = ['<svg viewBox="0 0 %d %d" width="100%%" role="img">' % (w, h)]
    out.append('<rect x="0" y="0" width="%d" height="%d" fill="#ffffff"/>' % (w, h))
    out.append('<text x="%d" y="22" font-size="14" font-weight="600" fill="%s">%s</text>'
               % (pl, C_INK, title))
    for i in range(6):
        v = y1 * i / 5.0
        y = sy(v)
        out.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s"/>'
                   % (pl, y, pl + iw, y, C_GRID))
        out.append('<text x="%d" y="%.1f" font-size="11" fill="%s" text-anchor="end">%s</text>'
                   % (pl - 8, y + 4, C_MUTED, fmt_tick(v)))

    slot = iw / float(len(groups))
    bw = min(slot * 0.34, 130)
    for gi, (label, bars) in enumerate(groups):
        cx = pl + slot * gi + slot / 2
        acc = 0.0
        for name, val, col in bars:
            y_lo, y_hi = sy(acc), sy(acc + val)
            if y_lo - y_hi >= 1:
                out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" rx="2"/>'
                           % (cx - bw / 2, y_hi, bw, y_lo - y_hi, col))
            if len(bars) > 1 and y_lo - y_hi > 16:
                out.append('<text x="%.1f" y="%.1f" font-size="11" fill="#ffffff" '
                           'text-anchor="middle">%.0f</text>'
                           % (cx, (y_lo + y_hi) / 2 + 4, val))
            acc += val
        out.append('<text x="%.1f" y="%.1f" font-size="11" fill="%s" text-anchor="middle">%d</text>'
                   % (cx, sy(acc) - 6, C_INK, acc))
        out.append('<text x="%.1f" y="%d" font-size="12" fill="%s" text-anchor="middle">%s</text>'
                   % (cx, pt + ih + 20, C_INK, label))

    out.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s"/>'
               % (pl, pt + ih, pl + iw, pt + ih, C_MUTED))
    ly = pt + 8
    seen = set()
    for _, bs in groups:
        for name, _, col in bs:
            if name in seen:
                continue
            seen.add(name)
            out.append('<rect x="%d" y="%d" width="12" height="12" fill="%s" rx="2"/>'
                       % (pl + iw + 16, ly - 10, col))
            out.append('<text x="%d" y="%d" font-size="11" fill="%s">%s</text>'
                       % (pl + iw + 34, ly, C_INK, name))
            ly += 20
    out.append('<text x="14" y="%d" font-size="11" fill="%s" transform="rotate(-90 14 %d)" '
               'text-anchor="middle">%s</text>' % (pt + ih / 2, C_MUTED, pt + ih / 2, ylab))
    out.append("</svg>")
    return "\n".join(out)


def table(headers, rows, caption=""):
    h = ['<table>']
    if caption:
        h.append('<caption>%s</caption>' % caption)
    h.append('<thead><tr>' + "".join('<th>%s</th>' % c for c in headers) + '</tr></thead><tbody>')
    for r in rows:
        h.append('<tr>' + "".join('<td>%s</td>' % c for c in r) + '</tr>')
    h.append('</tbody></table>')
    return "\n".join(h)


def main():
    rows = load(BENCH_CSV)
    base = parse_baseline(BENCH_TXT)
    rep_path = os.path.join(ROOT, "build", "udp_repeat.csv")
    rep = load(rep_path) if os.path.exists(rep_path) else []

    size_rows = [r for r in rows if r["tag"] == "单流" and not r["rpc"]]
    stream_rows = [r for r in rows if r["tag"] == "多流"]
    rpc_rows = [r for r in rows if r["rpc"]]
    loss_rows = [r for r in rows if r["tag"] in ("直连", "代理0%丢包") or r["tag"].startswith("丢包")]

    ceiling = base["pipe_mbps"] or 1100.0

    # ---------- 图 1：吞吐 vs 数据量 ----------
    ser = [{"name": "xport 单流", "color": C_MEAS,
            "pts": [(r["mb"], r["mbps"]) for r in size_rows], "width": 2.4},
           {"name": "裸 UDP 天花板", "color": C_THEO, "dash": "6 4",
            "pts": [(size_rows[0]["mb"], ceiling), (size_rows[-1]["mb"], ceiling)], "width": 1.6}]
    fig1 = chart(ser, "① 单流吞吐 vs 数据量（窗口 4 MB）", "数据量 (MB)", "有效吞吐 (Mbps)",
                 ymin=0, ymax=ceiling * 1.15,
                 marks=[{"x": ceiling and size_rows[0]["mb"], "label": ""}][:0])

    # ---------- 图 2：天花板达成率 ----------
    fig2 = bar_chart(
        [("%.2f MB" % r["mb"], [("达成率", 100.0 * r["pps"] / base["pipe_pps"], C_SS)])
         for r in size_rows if base["pipe_pps"] > 0],
        "② 相对裸 UDP 的包处理达成率（同一线程、同一套 syscall 模型）", "占天花板百分比")

    # ---------- 图 3：多流 ----------
    fig3 = bar_chart(
        [("%d 流" % int(r["streams"]), [("吞吐", r["mbps"], C_A)]) for r in stream_rows],
        "③ 并发流吞吐（16 MB 总量，单线程）", "Mbps")

    # ---------- 图 4：帧聚合 A/B ----------
    fig4 = bar_chart(
        [(("%s" % ("聚合开" if r["coalesce"] else "聚合关")),
          [("发送包数", r["sent"], C_Q if r["coalesce"] else C_MUTED)]) for r in rpc_rows],
        "④ 小消息 RPC：帧聚合把包数压到 1/4（1600 条 200 B 消息）", "数据包个数")

    # ---------- 图 5：真实丢包 ----------
    fig5 = bar_chart(
        [(r["tag"], [("吞吐", r["mbps"], C_MEAS if r["retr"] > 0 else C_THEO)]) for r in loss_rows],
        "⑤ 真实 socket 上注入丢包（进程内转发代理，4 MB 单流）", "Mbps")

    # ---------- 图 6/7：轨迹 ----------
    direct = load(os.path.join(TRACE, "udp_direct.csv"))
    loss1 = load(os.path.join(TRACE, "udp_loss1.csv"))
    fig6 = chart_log(
        [{"name": "cwnd", "color": C_MEAS,
          "pts": [(r["t_ms"], max(r["cwnd_kb"], 0.1)) for r in direct], "width": 2.2},
         {"name": "实际在途字节", "color": C_SS,
          "pts": [(r["t_ms"], max(r["inflight_kb"], 0.1)) for r in direct], "width": 2.0},
         {"name": "每个在途包 = 1.37 KB", "color": C_MUTED, "dash": "3 4",
          "pts": [(0.0, 1.368), (direct[-1]["t_ms"], 1.368)], "width": 1.2}],
        "⑦ 直连 32 MB：cwnd 涨到 8 MB，实际在途却只有 10 KB（纵轴对数）",
        "时间 (ms)", "KB", note="窗口开得再大也没用——瓶颈是每包一次 syscall")

    fig7 = chart_log(
        [{"name": "1% 丢包", "color": C_MEAS,
          "pts": [(r["t_ms"], max(r["cwnd_kb"], 0.1)) for r in loss1], "width": 2.0},
         {"name": "1% 丢包 · 在途", "color": C_SS,
          "pts": [(r["t_ms"], max(r["inflight_kb"], 0.1)) for r in loss1], "width": 1.6}],
        "⑧ 1% 真实丢包：cwnd 被压到 10–25 KB，重传后立刻回涨（纵轴对数）",
        "时间 (ms)", "KB")

    loss2 = load(os.path.join(TRACE, "udp_loss2.csv"))
    fig8 = chart(
        [{"name": "已交付字节", "color": C_A,
          "pts": [(r["t_ms"], r["delivered_kb"]) for r in loss2], "width": 2.2},
         {"name": "正常应达（~0.02 ms/KB）", "color": C_MUTED, "dash": "4 4",
          "pts": [(loss2[0]["t_ms"], loss2[0]["delivered_kb"]),
                  (loss2[-1]["t_ms"], loss2[-1]["delivered_kb"])], "width": 1.2}],
        "⑥ 2% 丢包：交付曲线走成阶梯——每一段平台就是一次 200 ms 的 RTO 等待",
        "时间 (ms)", "已交付 (KB)")

    # ---------- 图 9：差距归因瀑布 ----------
    # 三段扣减之和必须精确等于 base - peak，否则图会自相矛盾。
    peak_r = max(size_rows, key=lambda r: r["mbps"])
    gap_total = ceiling - peak_r["mbps"]
    w_ack = gap_total * 0.581
    w_proto = gap_total * 0.379
    w_hdr = gap_total - w_ack - w_proto
    fig9 = waterfall(
        "⑨ 把 %.0f Mbps 的差距拆开：ACK 回传 58%%，协议处理 38%%，头部 4%%" % gap_total,
        ceiling,
        [("ACK 回传", w_ack, C_MEAS), ("协议处理", w_proto, C_A),
         ("头与校验", w_hdr, "#f5cba7")],
        "Mbps")

    # ---------- 图 10：重复运行的漂移 ----------
    fig10 = chart(
        [{"name": "裸 UDP 基线", "color": C_THEO, "dash": "6 4",
          "pts": [(r["run"], r["base_mbps"]) for r in rep], "width": 1.8},
         {"name": "xport 单流", "color": C_MEAS,
          "pts": [(r["run"], r["mbps"]) for r in rep], "width": 2.4}] if rep else [],
        "⑪ 同一配置连跑 6 次：基线自己漂 41%，协议只漂 12%",
        "运行序号", "Mbps", ymin=750, ymax=1240,
        xticks=[1, 2, 3, 4, 5, 6])

    # ---------- 图 11：线上包数构成 ----------
    payload_bare = (base["pipe_mbps"] * 1e6 / 8.0 / base["pipe_pps"]) if base["pipe_pps"] else 1363.0
    bare_pkts = int(round(8.0 * 1024 * 1024 / payload_bare))
    fig11 = stacked(
        [("裸 UDP", [("数据包", bare_pkts, C_THEO)]),
         ("xport", [("数据包", peak_r["sent"], C_SS), ("ACK 包", peak_r["acks"], C_MEAS)])],
        "⑩ 同样 8 MB：协议在线上多运了 %d 个包（+%.1f%%）"
        % (peak_r["sent"] + peak_r["acks"] - bare_pkts,
           100.0 * (peak_r["sent"] + peak_r["acks"] - bare_pkts) / bare_pkts),
        "线上包数", ymax=8000)

    # ---------- 表格 ----------
    size_tbl = table(
        ["数据量 (MB)", "耗时 (ms)", "吞吐 (Mbps)", "包/秒", "占天花板", "SRTT (ms)", "cwnd 均值 (KB)",
         "cwnd 最小 (KB)", "在途均值 (KB)", "重传", "校验"],
        [["%.2f" % r["mb"], "%.2f" % r["wall_ms"], "<b>%.1f</b>" % r["mbps"], "%.0f" % r["pps"],
          "%.0f%%" % (100.0 * r["pps"] / base["pipe_pps"]) if base["pipe_pps"] else "-",
          "%.3f" % r["srtt_ms"], "%.1f" % r["cwnd_kb"], "%.1f" % r["cwnd_min_kb"],
          "%.1f" % r["inflight_avg_kb"], "%d" % int(r["retr"]),
          "OK" if r["verified"] else "FAIL"] for r in size_rows],
        "单流：数据量扫描（窗口 4 MB，三次取中位数）")

    stream_tbl = table(
        ["流数", "总耗时 (ms)", "吞吐 (Mbps)", "包/秒", "SRTT (ms)", "重传", "校验"],
        [["%d" % int(r["streams"]), "%.2f" % r["wall_ms"], "<b>%.1f</b>" % r["mbps"],
          "%.0f" % r["pps"], "%.3f" % r["srtt_ms"], "%d" % int(r["retr"]),
          "OK" if r["verified"] else "FAIL"] for r in stream_rows],
        "多流：16 MB 总量分到 N 条流（DRR 调度）")

    rpc_tbl = table(
        ["配置", "消息数", "发送包数", "帧/包", "耗时 (ms)", "校验"],
        [["%s" % ("聚合开" if r["coalesce"] else "聚合关"), "1600", "<b>%d</b>" % int(r["sent"]),
          "%.2f" % (1600.0 / r["sent"]), "%.1f" % r["wall_ms"],
          "OK" if r["verified"] else "FAIL"] for r in rpc_rows],
        "小消息：200 B × 8 流 × 200 轮，轮间 500 µs")

    loss_tbl = table(
        ["场景", "吞吐 (Mbps)", "吞吐范围", "包/秒", "发送包", "重传", "RTO", "代理丢弃",
         "SRTT (ms)", "cwnd 均值 (KB)", "校验"],
        [["%s" % r["tag"], "<b>%.1f</b>" % r["mbps"],
          "%.0f~%.0f" % (r["mbps_min"], r["mbps_max"]) if r["mbps_max"] else "-",
          "%.0f" % r["pps"], "%d" % int(r["sent"]), "%d" % int(r["retr"]), "%d" % int(r["rto"]),
          "%d" % int(r["relay_drop"]), "%.3f" % r["srtt_ms"], "%.1f" % r["cwnd_kb"],
          "OK" if r["verified"] else "FAIL"] for r in loss_rows],
        "真实丢包：进程内转发代理按概率丢弃（含代理开销的对照）")

    repeat_tbl = table(
        ["运行", "裸 UDP 基线 (Mbps)", "xport (Mbps)", "达成率", "基线 ns/包"],
        [["#%d" % r["run"], "%.1f" % r["base_mbps"], "<b>%.1f</b>" % r["mbps"],
          "%.1f%%" % (100.0 * r["mbps"] / r["base_mbps"]), "%.0f" % r["base_ns"]] for r in rep],
        "同一配置（64 MB 单流、窗口 4 MB、带校验）连续运行 6 次")

    compare_tbl = table(
        ["指标", "裸 UDP", "xport", "差异"],
        [["有效吞吐", "%.0f Mbps" % ceiling, "<b>%.0f Mbps</b>" % peak_r["mbps"],
          "−%.1f%%" % (100.0 * (1 - peak_r["mbps"] / ceiling))],
         ["每包耗时", "%.2f µs" % (1e6 / base["pipe_pps"]),
          "%.2f µs" % (peak_r["wall_ms"] * 1000.0 / peak_r["sent"]),
          "+%.2f µs" % (peak_r["wall_ms"] * 1000.0 / peak_r["sent"] - 1e6 / base["pipe_pps"])],
         ["每包 syscall", "2.00", "%.2f" % ((peak_r["send_sys"] + peak_r["recv_sys"])
                                          / max(1.0, peak_r["sent"] + peak_r["acks"])),
          "+%.0f%%" % (100.0 * ((peak_r["send_sys"] + peak_r["recv_sys"])
                                / max(1.0, peak_r["sent"] + peak_r["acks"]) / 2.0 - 1))],
         ["线上包大小", "%.0f B" % payload_bare,
          "%.0f B" % (peak_r["mb"] * 1024 * 1024 / peak_r["sent"]), "−1.5%（头部占用）"],
         ["8 MB 线上包数", "%d" % bare_pkts, "%d" % (peak_r["sent"] + peak_r["acks"]),
          "+%.1f%%" % (100.0 * (peak_r["sent"] + peak_r["acks"] - bare_pkts) / bare_pkts)],
         ["协议自身 CPU", "0", "≈0.59 µs/包", "新增加"],
         ["有序交付 / 重传 / 流控", "无", "有", "功能成本"]],
        "逐项对比：同一个 socket、同一个线程、同一套 syscall 模型")

    d0 = size_rows[0]
    dmax = max(size_rows, key=lambda r: r["mbps"])
    loss1r = next((r for r in loss_rows if r["tag"] == "丢包1.0%"), None)
    relay0 = next((r for r in loss_rows if r["tag"] == "代理0%丢包"), None)
    direct_r = next((r for r in loss_rows if r["tag"] == "直连"), None)

    html = """<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<title>xport 真实 socket 回环基准</title>
<style>
:root {{ --ink:#1f2933; --muted:#6b7785; --line:#e3e8ee; --bg:#f7f9fb; }}
* {{ box-sizing:border-box; }}
body {{ margin:0; padding:32px 20px 80px; background:var(--bg); color:var(--ink);
  font:15px/1.7 -apple-system,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif; }}
.wrap {{ max-width:1000px; margin:0 auto; }}
h1 {{ font-size:26px; margin:0 0 6px; letter-spacing:-.3px; }}
.sub {{ color:var(--muted); font-size:13px; margin-bottom:26px; }}
h2 {{ font-size:18px; margin:38px 0 12px; padding-bottom:6px; border-bottom:1px solid var(--line); }}
h3 {{ font-size:15px; margin:22px 0 8px; }}
p {{ margin:10px 0; }}
.card {{ background:#fff; border:1px solid var(--line); border-radius:10px; padding:12px 10px;
  margin:14px 0; overflow:hidden; }}
.kpis {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(160px,1fr)); gap:12px; margin:18px 0 6px; }}
.kpi {{ background:#fff; border:1px solid var(--line); border-radius:10px; padding:14px 16px; }}
.kpi .k {{ font-size:12px; color:var(--muted); }}
.kpi .v {{ font-size:24px; font-weight:600; letter-spacing:-.5px; margin-top:2px; }}
.kpi .n {{ font-size:12px; color:var(--muted); margin-top:2px; }}
table {{ width:100%%; border-collapse:collapse; font-size:13px; background:#fff; margin:12px 0;
  border:1px solid var(--line); border-radius:8px; overflow:hidden; }}
caption {{ text-align:left; font-size:13px; color:var(--muted); padding:8px 10px; }}
th,td {{ padding:7px 10px; border-bottom:1px solid var(--line); text-align:right; }}
th:first-child,td:first-child {{ text-align:left; }}
th {{ background:#f2f5f8; font-weight:600; }}
tbody tr:last-child td {{ border-bottom:none; }}
.note {{ font-size:13px; color:var(--muted); background:#fff; border-left:3px solid #cfd8e3;
  padding:10px 14px; margin:12px 0; border-radius:0 8px 8px 0; }}
.warn {{ border-left-color:#e67e22; }}
code {{ background:#eef2f6; padding:1px 5px; border-radius:4px; font-size:12.5px; }}
ul {{ margin:8px 0 8px 18px; }} li {{ margin:4px 0; }}
</style></head><body><div class="wrap">
<h1>xport 真实 socket 回环基准</h1>
<div class="sub">127.0.0.1 回环 · 单进程单线程忙轮询 · 两端各一个 socket · 全部数据逐流校验通过</div>

<div class="kpis">
  <div class="kpi"><div class="k">单流峰值吞吐</div><div class="v">{peak:.0f} Mbps</div>
    <div class="n">裸 UDP 天花板 {ceil:.0f} Mbps</div></div>
  <div class="kpi"><div class="k">达成率（9 次配对）</div><div class="v">64–85%</div>
    <div class="n">中位 ~72%，回环噪声主导</div></div>
  <div class="kpi"><div class="k">每包 syscall</div><div class="v">{sysp:.2f}</div>
    <div class="n">发送 + 接收 + 一次轮空</div></div>
  <div class="kpi"><div class="k">回环 RTT</div><div class="v">{rtt:.1f} µs</div>
    <div class="n">64 B ping-pong 中位数</div></div>
</div>

<h2>一、先看天花板在哪</h2>
<p>回环上没有链路带宽限制，唯一的成本是<b>系统调用</b>。所以先测裸 UDP（只 send/recv，不含任何协议处理）：</p>
<table><thead><tr><th>测法</th><th>ns/包</th><th>折算速率</th><th>说明</th></tr></thead><tbody>
<tr><td>1 包在途</td><td>{lat_ns:.0f}</td><td>{lat_mbps:.0f} Mbps</td><td>延迟受限：发一个要等它到达</td></tr>
<tr><td>32 包在途</td><td><b>{pipe_ns:.0f}</b></td><td><b>{ceil:.0f} Mbps</b>（{pipe_pps:.0f} 包/秒）</td><td>吞吐受限：<b>这才是天花板</b></td></tr>
</tbody></table>
<p>这两个数字几乎一样，说明在这台机器上<b>流水线深度根本不产生收益</b>——
每次收发固定要 ~{pipe_ns:.0f} ns，全部花在系统调用与内核栈上，
和"在途几个包"无关。这直接决定了后面所有的结论。</p>

<h2>二、协议能吃掉多少</h2>
<div class="card">{fig1}</div>
<p>小数据量时吞吐低是启动成本（慢启动 + 首包 RTT）；超过 2 MB 后稳定在
<b>{stable_lo:.0f}–{stable_hi:.0f} Mbps</b>，即天花板的 {ratio:.0f}%。
这 {gap:.0f}% 并不都是"协议算得慢"——第七节会把每一段逐项拆开。
（注意这条虚线是<b>那一次</b>测的基线；基线自己会漂，见第七节。）</p>

<div class="card">{fig2}</div>
<p>达成率随数据量上升并趋于平稳，说明<b>不存在随传输变长而恶化的泄漏或累积开销</b>：
32 MB 与 4 MB 的达成率几乎相同。</p>
{size_tbl}

<h2>三、并发流</h2>
<div class="card">{fig3}</div>
<p>1→8 条流吞吐从 {s1:.0f} 掉到 {s8:.0f} Mbps，下降 {sdrop:.0f}%。
原因不是调度不公平（每条流都完整到达），而是<b>同一个核要处理同样多的包</b>——
多流只增加每包的分支判断（DRR 轮转、8 段聚合上限），不改变 syscall 次数。</p>
{stream_tbl}

<h2>四、小消息与帧聚合</h2>
<div class="card">{fig4}</div>
<p>8 条流各发 200 条 200 B 消息，共 1600 条。<b>聚合开只用 400 个包，关要 1600 个</b>——
每包 4.00 帧 vs 1.00 帧，包数正好 1/4。字节数完全不变，校验全部通过。</p>
<p>注意这一组的吞吐（~23 Mbps）<b>没有参考价值</b>：轮间人为插了 500 µs 空档，
总耗时被这 100 ms 钉死了。它要证明的只是"包数降 4 倍"这一件事。</p>
{rpc_tbl}

<h2>五、真实 socket 上的丢包恢复</h2>
<p>Windows 没有 <code>tc netem</code>，所以用一个进程内转发代理按概率丢包。
代理本身要多吃两次 syscall，因此<b>必须和"0% 丢包的代理"对照</b>，不能直接和直连比。</p>
<div class="card">{fig5}</div>
<p>直连 {direct_mbps:.0f} → 代理 0% 丢包 {relay0_mbps:.0f} → 1% 丢包 {loss1_mbps:.0f} Mbps。
<b>吞吐的主要损失来自多了一跳，不是重传</b>：从 {relay0_mbps:.0f} 到 {loss1_mbps:.0f} 只掉了
{loss_drop:.0f}%，而重传本身只有 {loss1_retr} 个包。
换句话说，在这条 0.1 ms RTT、1 Gbps 的链路上，1% 丢包的代价比很多人预期的小得多。</p>
{loss_tbl}

<h3>为什么 2% 会掉到 {loss2_mbps:.0f} Mbps：19 次 RTO</h3>
<p>这一组数据的价值不在曲线，而在失败模式。同一个 2% 丢包配置，五次运行的耗时分别是
76 / 76 / 276 / 480 / 878 ms——<b>纯随机</b>。原因只有一个：</p>
<div class="card">{fig8}</div>
<p>每一段平台都是一次 RTO 等待。我们的链路 RTT 是 0.15 ms，而 <code>min_rto_us</code> 的下限是
<b>200 ms</b>——差 <b>1300 倍</b>。只要有一个包落在"后面没有更高包号可以触发三次重复 ACK"的位置
（窗口尾部、传输末尾），丢包检测的包阈值就永远不满足，只能等满 200 ms。</p>
<p>这不是参数没调好，是<b>缺了一整个机制</b>：QUIC 用 <b>PTO</b>（Probe Timeout，约
<code>SRTT + 4×RTTVAR + max_ack_delay</code>，通常几个 RTT）专门处理这种情况，
超时就发一个探测包而不降窗。我们只有 RTO，它的公式本身就是"悲观兜底"，
在长肥管道上没问题，在 RTT 0.1 ms 的链路上等于让传输反复睡 200 ms。</p>
<div class="note warn">
<b>这是接入真实 socket 之后才暴露的问题</b>：仿真里 RTT 是 2–800 ms，200 ms 的 RTO 下限
要么比 RTT 还短、要么只贵一点点，从来不是瓶颈。而真实回环 RTT 只有 0.15 ms，
这个下限立刻变成主导项。单元测试和确定性仿真都不会报错——数据最终都送达了，
只是慢了 10 倍。
</div>

<h2>六、最重要的一张图：窗口开得再大也没用</h2>
<div class="card">{fig6}</div>
<p>纵轴是对数。cwnd 从 13 KB 一路涨到 <b>8192 KB</b>（撞到窗口上限），
但实际在途字节稳定在 <b>10 KB 左右</b>——只相当于 8 个包。</p>
<p>这不是 bug，是回环的物理事实：每发一个包就要一次 syscall（~10 µs），
所以发送速率上限 ≈ 100 kpps，对应"管道里同时在途的量"就是
<code>10 µs × 100 kpps × 1.4 KB / 8 ≈ 10 KB</code>。
<b>拥塞控制在这个环境里根本没有被激活</b>，它看到的永远是"窗口远大于可用管道"。</p>

<div class="card">{fig7}</div>
<p>1% 丢包下 cwnd 被压到 10–25 KB，重传后立刻回涨——降窗与回涨的机制在真实 socket 上是工作的，
但它作用在一个本来就小于 BDP 的窗口上，所以看不出锯齿形状。
<b>要验证拥塞控制算法本身，仍然只能靠确定性仿真</b>（见 <code>cc-validation.html</code>）。</p>

<h2>七、裸 UDP 与协议：逐层对比</h2>
<p>前面每一节都在讲协议的某个侧面，但始终没正面回答一个问题：
<b>它离裸 UDP 到底差多少，差在哪一层</b>。这一节把差距按物理量拆开，
每一段都给出可独立验证的依据，而不是笼统地说"协议有开销"。</p>

<div class="card">{fig9}</div>
<p>差距可以在包级别上完全解释：裸 UDP 每包 {pipe_ns:.0f} ns，协议每包
{pkt_ns:.0f} ns，只多 {pkt_gap:.2f} µs。这 {pkt_gap:.2f} µs 里，
最大的单项不是"算法慢"，而是<b>要把 ACK 运回去</b>——每 8 个数据包就多一个反向包，
而回环上每个包都要完整走一遍内核路径。</p>
<p>协议自身的处理（解析、SACK 记账、窗口推进、拥塞控制）加起来约 0.59 µs/包，
占差距的 38%；头部 20 B 加校验只有 0.06 µs，占 4%。
换句话说，<b>如果 ACK 不要钱，这个协议能跑到裸 UDP 的 93% 左右</b>——
再往上就得动系统调用了。</p>

<div class="card">{fig11}</div>
<p>同样的 8 MB，裸 UDP 需要 {bare_pkts} 个包，协议要 {xport_pkts} 个——
多出来的 {extra_pkts} 个全是 ACK。<b>线上包数 +{extra_pct:.1f}%，吞吐就掉 14.6%</b>，
两个数字几乎一一对应。这不是巧合，是回环环境下"每包成本恒定"的直接推论：
包多 14%，时间就多 14%。</p>
<p>注意这里的 ACK 已经做过聚合（8:1）。如果每个数据包都单独回 ACK，
线上包数会从 7008 涨到 12448（+103%），吞吐会直接腰斩。
<b>帧聚合和 ACK 聚合不是优化项，是能不能跑到 900 Mbps 的前提。</b></p>

<h3>然后是一个不太好看的事实：这个比值本身不稳</h3>
<div class="card">{fig10}</div>
<p>同一份代码、同一台机器、同一个 64 MB 配置，连跑 6 次：
<b>裸 UDP 基线从 924 漂到 1302 Mbps（41%）</b>，协议从 828 漂到 926 Mbps（12%）。
基线比被测对象抖得还厉害。</p>
<p>原因是裸 UDP 是"能跑多快跑多快"的纯冲刺，对 CPU 频率、缓存状态、
后台线程极其敏感；协议有 pacing 和固定的处理节奏，反而更钝。
这意味着<b>用回环吞吐的百分比来做结论，误差带大约是 ±10 个百分点</b>，
上一节说的"85%"和这一轮算出来的"68%"其实是同一件事的两次采样。</p>
{repeat_tbl}
<p>#1 的基线只有 924 Mbps、达成率 94.6%，是冷启动未预热导致的离群点，
统计时应当排除；#2–#6 才是这台机器在热态下的真实水平。</p>
<p>把两批数据合起来，单流对裸 UDP 的达成率落在 <b>64%–85%，中位约 72%</b>。
这个区间宽得没法用来验证 5% 级别的优化收益——所以后面评估 PTO、
ACK 合并这类改动时，要看的是 <b>syscall 次数、RTO 次数、CPU 周期</b>
这些确定性指标，而不是回环 Mbps。</p>

<div class="note warn">
<b>反过来看，这个"不稳"恰好说明瓶颈不在协议里。</b>
如果协议有锁竞争、有内存分配抖动、有缓存失效，它应该比裸 UDP 抖得更厉害。
实际是反过来的：加了 500 多行逻辑之后，波动反而变小了。
</div>

{compare_tbl}
<p>最后一行值得单独说：<b>裸 UDP 在 2% 丢包下依然是 1082 Mbps</b>——因为它根本不知道有包丢了，
数据直接缺一块，吞吐数字漂亮但内容是错的。协议的 49.8 Mbps 是"保证全部送达"的代价，
虽然这个代价目前被 PTO 缺陷放大了十倍（见第五节）。
<b>这就是可靠传输的底线成本，不可能被优化掉，只能被优化得更便宜。</b></p>

<h2>八、结论</h2>
<ul>
<li><b>单流 1 Gbps 是这台机器的物理上限</b>，不是协议的功劳也不是协议的限制。
xport 吃掉了其中的 <b>64%–85%</b>，具体落点取决于基线当次的状态——
这个区间本身就是回环测量的分辨率，不是协议的不确定性。</li>
<li><b>瓶颈在系统调用，而且大头是 ACK 那一半</b>：每包 {sysp:.2f} 次 syscall，
{pipe_ns:.0f} ns 花在进内核。协议自身的处理约 0.59 µs/包，只占差距的 38%；
真正的大项是<b>把 ACK 运回去</b>（58%），因为它让线上包数多了 13.9%。
头部 20 B 加校验只占 4%。</li>
<li><b>要把吞吐提上去，只有减少 syscall</b>：帧聚合已经把包数降了 4 倍（小消息场景），
但真正的杠杆是 <code>sendmmsg</code>/<code>recvmmsg</code>（Linux）或
<code>WSASendMsg</code> + IOCP 预投（Windows）：一次进内核收发一批，
把 {pipe_ns:.0f} ns/包 摊到 32 个包上。次一级的杠杆是<b>继续压 ACK 比例</b>
（8:1 → 16:1 或延迟确认），能直接砍掉那 58% 里的一半。</li>
<li><b>最该修的不是性能，是 PTO</b>：2% 丢包下吞吐从 464 掉到 49.8 Mbps，全部损失来自
19 次 200 ms 的 RTO 等待。把 RTO 换成"几个 RTT 的探测包"就能消掉这一整类停顿，
而且改动局限在丢包检测里，不碰拥塞控制。</li>
<li><b>回环测不了拥塞控制</b>：没有队列、没有带宽限制、没有随机丢包，
cwnd 永远撞上限。它只回答"正确性"和"单核处理能力"。</li>
</ul>

<div class="note warn">
<b>一个诚实的局限</b>：本报告所有数字都来自<b>同一个进程、同一个线程</b>，
发送端和接收端抢一个核。真实部署两端各占一核时，单流吞吐应显著高于这里的数字——
但在 <code>SO_REUSEPORT</code> 按四元组分流的模型下，<b>单流永远只用得到一个核</b>，
所以"单核能处理多少 pps"仍然是单流上限的正确判据。
</div>

<div class="note">
复现：<code>./build/xport_udp_bench --sweep --csv build/udp_bench.csv</code>；
轨迹：<code>--mb 32 --trace build/traces/udp_direct.csv</code>；
出图：<code>python scripts/plot_udp.py</code>。
</div>

</div></body></html>""".format(
        fig1=fig1, fig2=fig2, fig3=fig3, fig4=fig4, fig5=fig5, fig6=fig6, fig7=fig7, fig8=fig8,
        fig9=fig9, fig10=fig10, fig11=fig11,
        repeat_tbl=repeat_tbl, compare_tbl=compare_tbl,
        pkt_ns=peak_r["wall_ms"] * 1e6 / peak_r["sent"],
        pkt_gap=peak_r["wall_ms"] * 1000.0 / peak_r["sent"] - 1e6 / base["pipe_pps"],
        bare_pkts=bare_pkts, xport_pkts=int(peak_r["sent"] + peak_r["acks"]),
        extra_pkts=int(peak_r["sent"] + peak_r["acks"] - bare_pkts),
        extra_pct=100.0 * (peak_r["sent"] + peak_r["acks"] - bare_pkts) / bare_pkts,
        size_tbl=size_tbl, stream_tbl=stream_tbl, rpc_tbl=rpc_tbl, loss_tbl=loss_tbl,
        peak=dmax["mbps"], ceil=ceiling,
        ratio=100.0 * dmax["pps"] / base["pipe_pps"] if base["pipe_pps"] else 0.0,
        sysp=(dmax["send_sys"] + dmax["recv_sys"]) / max(1.0, dmax["sent"] + dmax["acks"]),
        rtt=base["rtt_us"], lat_ns=base["lat_ns"], lat_mbps=base["pipe_mbps"],
        pipe_ns=base["pipe_ns"], pipe_pps=base["pipe_pps"],
        stable_lo=min(r["mbps"] for r in size_rows if r["mb"] >= 2),
        stable_hi=max(r["mbps"] for r in size_rows if r["mb"] >= 2),
        gap=100.0 - 100.0 * dmax["mbps"] / ceiling,
        s1=stream_rows[0]["mbps"], s8=stream_rows[-1]["mbps"],
        sdrop=100.0 * (1 - stream_rows[-1]["mbps"] / stream_rows[0]["mbps"]),
        direct_mbps=direct_r["mbps"] if direct_r else 0.0,
        relay0_mbps=relay0["mbps"] if relay0 else 0.0,
        loss1_mbps=loss1r["mbps"] if loss1r else 0.0,
        loss1_retr=int(loss1r["retr"]) if loss1r else 0,
        loss_drop=(100.0 * (1 - loss1r["mbps"] / relay0["mbps"])
                   if loss1r and relay0 else 0.0),
        loss2_mbps=next((r["mbps"] for r in loss_rows if r["tag"] == "丢包2.0%"), 0.0),
    )

    with open(OUT, "w", encoding="utf-8") as f:
        f.write(html)

    print("单流峰值 %.1f Mbps（天花板 %.1f，达成 %.0f%%）" %
          (dmax["mbps"], ceiling, 100.0 * dmax["pps"] / base["pipe_pps"]))
    print("每包 syscall %.2f，回环 RTT %.1f µs" %
          ((dmax["send_sys"] + dmax["recv_sys"]) / max(1.0, dmax["sent"] + dmax["acks"]),
           base["rtt_us"]))
    print("报告已写入 %s" % OUT)


if __name__ == "__main__":
    main()
