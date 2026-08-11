#!/usr/bin/env python3
"""读取 results/results.csv, 每组(场景,引擎)取最优轮次,
输出: results/tables.md (markdown 表格) + results/*.png (若 matplotlib 可用)"""
import csv
import sys
from collections import defaultdict

CSV = "results/results.csv"
TABLES = "results/tables.md"

SCEN_TITLES = {
    "randread_cold": "场景 A: 4KiB 随机读(冷页缓存)",
    "randread_warm": "场景 B: 4KiB 随机读(热页缓存)",
    "seqread_cold":  "场景 C: 256KiB 顺序读(冷页缓存)",
    "logwrite":      "场景 D: 4KiB 日志记录落盘(含持久化)",
}

# 图表标题用英文(matplotlib 默认字体无 CJK 字形)
SCEN_TITLES_EN = {
    "randread_cold": "A: 4KiB random read (cold page cache)",
    "randread_warm": "B: 4KiB random read (warm page cache)",
    "seqread_cold":  "C: 256KiB sequential read (cold page cache)",
    "logwrite":      "D: 4KiB log records with durability",
}

rows = list(csv.DictReader(open(CSV)))
if not rows:
    sys.exit("results.csv 为空")

# 取每(场景,引擎) IOPS 最高的一轮
best = {}
for r in rows:
    key = (r["scenario"], r["name"])
    if key not in best or float(r["iops"]) > float(best[key]["iops"]):
        best[key] = r

by_scen = defaultdict(list)
for (scen, _), r in best.items():
    by_scen[scen].append(r)

# 保持引擎出现顺序
for scen in by_scen:
    seen, ordered = set(), []
    for r in rows:
        if r["scenario"] == scen and r["name"] not in seen and (scen, r["name"]) in best:
            ordered.append(best[(scen, r["name"])])
            seen.add(r["name"])
    by_scen[scen] = ordered

def fmt(x, nd=1):
    return f"{float(x):,.{nd}f}"

with open(TABLES, "w") as f:
    for scen, title in SCEN_TITLES.items():
        if scen not in by_scen:
            continue
        f.write(f"\n### {title}\n\n")
        f.write("| 引擎 | 深度/线程 | IOPS(rec/s) | 带宽 MiB/s | 平均延迟 us | p99 延迟 us | CPU us/千次IO | 系统调用次数 |\n")
        f.write("|---|---|---|---|---|---|---|---|\n")
        for r in by_scen[scen]:
            f.write(f"| {r['name']} | {r['depth']} | {fmt(r['iops'],0)} | {fmt(r['bw_mibs'])} | "
                    f"{fmt(r['lat_avg_us'])} | {fmt(r['lat_p99_us'])} | {fmt(r['cpu_us_per_kop'])} | "
                    f"{fmt(r['syscalls'],0)} |\n")
print(f"written {TABLES}")

# ---- 可选: 画柱状图 ----
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib 不可用, 跳过图表生成")

def bar(scen, metric, ylabel, fname, log=False, title_extra=""):
    data = by_scen.get(scen)
    if not data:
        return
    names = [r["name"] for r in data]
    vals = [float(r[metric]) for r in data]
    fig, ax = plt.subplots(figsize=(9, 4.5))
    bars = ax.bar(names, vals, color=["#d9534f" if "uring" not in n else "#5cb85c" for n in names])
    ax.set_ylabel(ylabel)
    ax.set_title(f"{SCEN_TITLES_EN.get(scen, scen)} {title_extra}")
    if log:
        ax.set_yscale("log")
    ax.bar_label(bars, fmt=lambda v: f"{v:,.0f}", fontsize=8, padding=2)
    plt.xticks(rotation=20, ha="right", fontsize=9)
    plt.tight_layout()
    plt.savefig(fname, dpi=130)
    plt.close()
    print(f"written {fname}")

for scen in ("randread_cold", "randread_warm"):
    bar(scen, "iops", "IOPS", f"results/{scen}_iops.png")
    bar(scen, "cpu_us_per_kop", "CPU us / 1000 IO", f"results/{scen}_cpu.png", log=True)
bar("seqread_cold", "bw_mibs", "MiB/s", "results/seqread_cold_bw.png")
bar("logwrite", "iops", "records/s", "results/logwrite_iops.png")
bar("logwrite", "syscalls", "syscalls", "results/logwrite_syscalls.png", log=True)
