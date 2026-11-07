#!/usr/bin/env python3
"""BBR v1/v2/v3 与 CUBIC 对比实验的出图脚本：读 build/exp/*.csv，写 docs/figs/bbr-*.svg。

数据由 xport_ccexp 生成（固定种子、逐位可复现）：
    ./build/xport_ccexp --out build/exp
    python3 scripts/plot_bbr.py

只用标准库；图是纯 SVG，离线可看。多种子的配置画中位数，误差线是最小值到最大值。
"""
import csv
import os
import statistics as st

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXP = os.path.join(ROOT, "build", "exp")
FIGS = os.path.join(ROOT, "docs", "figs")

ALGOS = ["cubic", "bbr", "bbr2", "bbr3"]
LABEL = {"cubic": "CUBIC", "bbr": "BBRv1", "bbr2": "BBRv2", "bbr3": "BBRv3"}
COLOR = {"cubic": "#7f8c8d", "bbr": "#e67e22", "bbr2": "#2980b9", "bbr3": "#c0392b"}
INK, MUTED, GRID = "#1f2933", "#6b7785", "#e8edf2"


def load(name):
    with open(os.path.join(EXP, name + ".csv"), newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def tick(v):
    return "%.0f" % v if abs(v) >= 10 else ("%.1f" % v if abs(v) >= 1 else "%.2f" % v)


class Plot:
    """坐标轴 + 网格 + 图例。x 轴可以是数值，也可以是类别（xcats）。"""

    def __init__(self, title, xlab, ylab, ymax, xmin=0.0, xmax=1.0, xcats=None, logx=False,
                 w=820, h=320):
        self.w, self.h = w, h
        self.pl, self.pr, self.pt, self.pb = 62, 120, 38, 46
        self.iw, self.ih = w - self.pl - self.pr, h - self.pt - self.pb
        self.xcats, self.logx = xcats, logx
        self.xmin, self.xmax, self.ymax = xmin, xmax, ymax * 1.08 if ymax > 0 else 1.0
        self.out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" '
                    'font-family="Helvetica, Arial, sans-serif">' % (w, h),
                    '<rect width="%d" height="%d" fill="#fff"/>' % (w, h),
                    '<text x="%d" y="22" font-size="14" font-weight="600" fill="%s">%s</text>'
                    % (self.pl, INK, title)]
        for i in range(6):
            v = self.ymax * i / 5
            y = self.sy(v)
            self.out.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s"/>'
                            % (self.pl, y, self.pl + self.iw, y, GRID))
            self.out.append('<text x="%d" y="%.1f" font-size="11" fill="%s" text-anchor="end">%s'
                            '</text>' % (self.pl - 6, y + 4, MUTED, tick(v)))
        if xcats:
            for i, c in enumerate(xcats):
                self.out.append('<text x="%.1f" y="%d" font-size="11" fill="%s" '
                                'text-anchor="middle">%s</text>'
                                % (self.cx(i), self.pt + self.ih + 16, MUTED, c))
        self.out.append('<text x="%.1f" y="%d" font-size="12" fill="%s" text-anchor="middle">%s'
                        '</text>' % (self.pl + self.iw / 2, h - 8, INK, xlab))
        self.out.append('<text x="14" y="%.1f" font-size="12" fill="%s" text-anchor="middle" '
                        'transform="rotate(-90 14 %.1f)">%s</text>'
                        % (self.pt + self.ih / 2, INK, self.pt + self.ih / 2, ylab))
        self.legend_n = 0

    def sy(self, v):
        return self.pt + self.ih - min(v, self.ymax) / self.ymax * self.ih

    def sx(self, v):
        import math
        a, b, x = self.xmin, self.xmax, v
        if self.logx:
            a, b, x = math.log(a), math.log(b), math.log(v)
        return self.pl + (x - a) / (b - a) * self.iw

    def cx(self, i):
        return self.pl + (i + 0.5) * self.iw / len(self.xcats)

    def xticks(self, vals):
        for v in vals:
            x = self.sx(v)
            self.out.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s"/>'
                            % (x, self.pt, x, self.pt + self.ih, GRID))
            self.out.append('<text x="%.1f" y="%d" font-size="11" fill="%s" text-anchor="middle">'
                            '%s</text>' % (x, self.pt + self.ih + 16, MUTED, tick(v)))

    def legend(self, name, color):
        y = self.pt + 8 + 18 * self.legend_n
        x = self.pl + self.iw + 14
        self.out.append('<rect x="%d" y="%d" width="12" height="12" fill="%s"/>' % (x, y - 9, color))
        self.out.append('<text x="%d" y="%d" font-size="12" fill="%s">%s</text>'
                        % (x + 18, y + 2, INK, name))
        self.legend_n += 1

    def line(self, pts, color, dots=True, width=2.0):
        if not pts:
            return
        d = " ".join("%.1f,%.1f" % (self.sx(x), self.sy(y)) for x, y in pts)
        self.out.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="%.1f"/>'
                        % (d, color, width))
        if dots:
            for x, y in pts:
                self.out.append('<circle cx="%.1f" cy="%.1f" r="3" fill="%s"/>'
                                % (self.sx(x), self.sy(y), color))

    def errbar(self, x_px, lo, hi):
        if hi - lo <= 1e-9:
            return
        self.out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" '
                        'stroke-width="1.2"/>' % (x_px, self.sy(lo), x_px, self.sy(hi), INK))

    def bar(self, cat_i, k, n, v, color, lo=None, hi=None):
        gw = self.iw / len(self.xcats) * 0.78
        bw = gw / n
        x = self.cx(cat_i) - gw / 2 + k * bw
        y = self.sy(v)
        self.out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s"/>'
                        % (x + 1, y, bw - 2, self.pt + self.ih - y, color))
        if lo is not None:
            self.errbar(x + bw / 2, lo, hi)

    def hline(self, v, label):
        y = self.sy(v)
        self.out.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" '
                        'stroke-dasharray="5 4"/>' % (self.pl, y, self.pl + self.iw, y, INK))
        self.out.append('<text x="%d" y="%.1f" font-size="11" fill="%s" text-anchor="end">%s'
                        '</text>' % (self.pl + self.iw - 4, y - 5, INK, label))

    def save(self, name):
        self.out.append("</svg>")
        os.makedirs(FIGS, exist_ok=True)
        with open(os.path.join(FIGS, name), "w", encoding="utf-8") as f:
            f.write("\n".join(self.out))
        print("wrote docs/figs/" + name)


def med(rows, key, field):
    """按 key 分组，返回 {key: (median, min, max)}。"""
    g = {}
    for r in rows:
        g.setdefault(key(r), []).append(float(r[field]))
    return {k: (st.median(v), min(v), max(v)) for k, v in g.items()}


def fig_throughput(single):
    p = Plot("吞吐 vs 随机丢包（单流 · 50 Mbps · RTT 40 ms · 缓冲 2 BDP）", "随机丢包率 %",
             "吞吐 Mbps", 50.0, xmin=0, xmax=2.0)
    p.xticks([0, 0.5, 1.0, 1.5, 2.0])
    for a in ALGOS:
        pts = sorted((float(r["loss_pct"]), float(r["goodput_mbps"])) for r in single
                     if r["algo"] == a and float(r["buf_bdp"]) == 2.0)
        p.line(pts, COLOR[a])
        p.legend(LABEL[a], COLOR[a])
    p.hline(50.0, "链路 50 Mbps")
    p.save("bbr-throughput-loss.svg")


def fig_qdelay(single):
    bufs = ["0.5", "1.0", "2.0", "8.0"]
    rows = [r for r in single if float(r["loss_pct"]) == 0.0]
    vals = {(r["algo"], r["buf_bdp"]): r for r in rows}
    for field, title, fname in (
            ("q_avg_ms", "平均排队时延（单流 · 无丢包）", "bbr-qdelay-avg.svg"),
            ("q_p95_ms", "排队时延 p95（单流 · 无丢包）", "bbr-qdelay-p95.svg")):
        ymax = max(float(vals[(a, b)][field]) for a in ALGOS for b in bufs)
        p = Plot(title, "瓶颈缓冲（× BDP）", "排队时延 ms", ymax, xcats=[b + " BDP" for b in bufs])
        for k, a in enumerate(ALGOS):
            for i, b in enumerate(bufs):
                p.bar(i, k, len(ALGOS), float(vals[(a, b)][field]), COLOR[a])
            p.legend(LABEL[a], COLOR[a])
        p.save(fname)


def fig_retx(single):
    bufs = ["0.5", "1.0", "2.0", "8.0"]
    vals = {(r["algo"], r["buf_bdp"]): float(r["retx_pct"]) for r in single
            if float(r["loss_pct"]) == 0.0}
    p = Plot("重传率（单流 · 无随机丢包，全部来自缓冲溢出）", "瓶颈缓冲（× BDP）", "重传率 %",
             max(vals.values()), xcats=[b + " BDP" for b in bufs])
    for k, a in enumerate(ALGOS):
        for i, b in enumerate(bufs):
            p.bar(i, k, len(ALGOS), vals[(a, b)], COLOR[a])
        p.legend(LABEL[a], COLOR[a])
    p.save("bbr-retx.svg")


def fig_single_ts(ts):
    rows = {a: [r for r in ts if r["algo"] == a] for a in ALGOS}
    ymax = max(float(r["qdelay_ms"]) for r in ts)
    p = Plot("排队时延随时间（单流 · 深缓冲 8 BDP = 320 ms · 无丢包）", "时间 s", "排队时延 ms",
             ymax, xmin=0, xmax=20)
    p.xticks([0, 5, 10, 15, 20])
    for a in ALGOS:
        p.line([(float(r["t_s"]), float(r["qdelay_ms"])) for r in rows[a]], COLOR[a], dots=False,
               width=1.4)
        p.legend(LABEL[a], COLOR[a])
    p.save("bbr-qdelay-timeseries.svg")


def fig_jain(intra):
    bufs = ["0.5", "2.0", "8.0"]
    m = med(intra, lambda r: (r["algo"], r["buf_bdp"]), "jain")
    p = Plot("同算法 4 流的 Jain 公平指数（错峰 2 s 启动 · 3 个种子中位数，误差线 = 范围）",
             "瓶颈缓冲（× BDP）", "Jain 指数", 1.0, xcats=[b + " BDP" for b in bufs])
    for k, a in enumerate(ALGOS):
        for i, b in enumerate(bufs):
            v, lo, hi = m[(a, b)]
            p.bar(i, k, len(ALGOS), v, COLOR[a], lo, hi)
        p.legend(LABEL[a], COLOR[a])
    p.save("bbr-fairness-jain.svg")


def fig_intra_ts(ts, algo, buf):
    rows = [r for r in ts if r["algo"] == algo and r["buf_bdp"] == buf]
    p = Plot("%s · 4 流各自吞吐（缓冲 %s BDP · 每 2 s 启动一条）" % (LABEL[algo], buf), "时间 s",
             "吞吐 Mbps", 50.0, xmin=0, xmax=30)
    p.xticks([0, 5, 10, 15, 20, 25, 30])
    shades = ["#1b4f72", "#2e86c1", "#85c1e9", "#a04000"]
    for i in range(4):
        p.line([(float(r["t_s"]), float(r["f%d" % i])) for r in rows], shades[i], dots=False,
               width=1.3)
        p.legend("流 %d" % i, shades[i])
    p.hline(12.5, "公平份额 12.5")
    p.save("bbr-intra-%s-%s.svg" % (algo, buf.replace(".", "_")))


def fig_inter(inter):
    m = med(inter, lambda r: (r["algo"], float(r["buf_bdp"])), "algo_share")
    p = Plot("与 1 条 CUBIC 共享瓶颈时的份额（3 个种子中位数）", "瓶颈缓冲（× BDP，对数轴）",
             "X 的吞吐份额", 1.0, xmin=0.5, xmax=16, logx=True)
    p.xticks([0.5, 1, 2, 4, 8, 16])
    for a in ALGOS:
        pts = sorted((b, v[0]) for (al, b), v in m.items() if al == a)
        p.line(pts, COLOR[a])
        for b, _ in pts:
            _, lo, hi = m[(a, b)]
            p.errbar(p.sx(b), lo, hi)
        p.legend(LABEL[a] + (" ×CUBIC" if a == "cubic" else ""), COLOR[a])
    p.hline(0.5, "公平 0.5")
    p.save("bbr-vs-cubic-share.svg")


def fig_rtt(rtt):
    m = med(rtt, lambda r: r["algo"], "short_share")
    p = Plot("RTT 公平性：40 ms 流对 160 ms 流（缓冲 2 BDP · 3 个种子）", "算法",
             "短 RTT 流的份额", 1.0, xcats=[LABEL[a] for a in ALGOS])
    for i, a in enumerate(ALGOS):
        v, lo, hi = m[a]
        p.bar(i, 0, 1, v, COLOR[a], lo, hi)
    p.hline(0.5, "公平 0.5")
    p.save("bbr-rtt-fairness.svg")


def main():
    single = load("single")
    fig_throughput(single)
    fig_qdelay(single)
    fig_retx(single)
    fig_single_ts(load("single_ts"))
    intra = load("intra")
    fig_jain(intra)
    its = load("intra_ts")
    for a in ("bbr", "bbr3", "cubic"):
        fig_intra_ts(its, a, "2.0")
    fig_inter(load("inter"))
    fig_rtt(load("rtt"))


if __name__ == "__main__":
    main()
