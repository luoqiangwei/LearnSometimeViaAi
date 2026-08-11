#!/usr/bin/env python3
"""读取 results/results_net.csv, 每组(场景,引擎)取 RPS 最优轮次,
输出: results/tables_net.md + results/net_*.png"""
import csv
import sys
from collections import defaultdict

CSV = "results/results_net.csv"
TABLES = "results/tables_net.md"

SCEN_TITLES = {
    "pingpong_64c_64B":  "场景 N1: 64B 消息 ping-pong, 64 并发连接",
    "pingpong_256c_64B": "场景 N2: 64B 消息 ping-pong, 256 并发连接",
    "bulk_64c_16K":      "场景 N3: 16KiB 消息 echo, 64 并发连接",
}
SCEN_TITLES_EN = {
    "pingpong_64c_64B":  "N1: 64B ping-pong, 64 conns",
    "pingpong_256c_64B": "N2: 64B ping-pong, 256 conns",
    "bulk_64c_16K":      "N3: 16KiB echo, 64 conns",
}

rows = list(csv.DictReader(open(CSV)))
if not rows:
    sys.exit("results_net.csv 为空")

best = {}
for r in rows:
    key = (r["scenario"], r["engine"])
    if key not in best or float(r["rps"]) > float(best[key]["rps"]):
        best[key] = r

by_scen = defaultdict(list)
for scen in SCEN_TITLES:
    for e in ("threads", "epoll", "uring"):
        if (scen, e) in best:
            by_scen[scen].append(best[(scen, e)])

def fmt(x, nd=1):
    return f"{float(x):,.{nd}f}"

with open(TABLES, "w") as f:
    for scen, title in SCEN_TITLES.items():
        if scen not in by_scen:
            continue
        f.write(f"\n### {title}\n\n")
        f.write("| 引擎 | RPS(msg/s) | 带宽 MiB/s | RTT 平均 us | RTT p99 us | 服务端 CPU us/千条 | 服务端 syscall 总数 |\n")
        f.write("|---|---|---|---|---|---|---|\n")
        for r in by_scen[scen]:
            f.write(f"| {r['engine']} | {fmt(r['rps'],0)} | {fmt(r['bw_mibs'])} | "
                    f"{fmt(r['rtt_avg_us'])} | {fmt(r['rtt_p99_us'])} | "
                    f"{fmt(r['srv_cpu_us_per_kmsg'])} | {fmt(r['srv_syscalls'],0)} |\n")
print(f"written {TABLES}")

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib 不可用, 跳过图表生成")

COLORS = {"threads": "#d9534f", "epoll": "#f0ad4e", "uring": "#5cb85c"}

def bar(scen, metric, ylabel, fname, log=False):
    data = by_scen.get(scen)
    if not data:
        return
    names = [r["engine"] for r in data]
    vals = [float(r[metric]) for r in data]
    fig, ax = plt.subplots(figsize=(6.5, 4))
    bars = ax.bar(names, vals, color=[COLORS[n] for n in names])
    ax.set_ylabel(ylabel)
    ax.set_title(SCEN_TITLES_EN.get(scen, scen))
    if log:
        ax.set_yscale("log")
    ax.bar_label(bars, fmt=lambda v: f"{v:,.0f}", fontsize=9, padding=2)
    plt.tight_layout()
    plt.savefig(fname, dpi=130)
    plt.close()
    print(f"written {fname}")

for scen in SCEN_TITLES:
    bar(scen, "rps", "msg/s", f"results/net_{scen}_rps.png")
    bar(scen, "srv_syscalls", "server syscalls (total)", f"results/net_{scen}_sys.png", log=True)
