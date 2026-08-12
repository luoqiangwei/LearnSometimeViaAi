#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_overhead.py — 量化 eBPF tracker 对系统的性能开销

对每项微基准测三种状态：
  baseline : 系统上没有 tracker
  loaded   : tracker 已加载所有 BPF 程序，但未对任何进程 armed 抓栈
  armed    : tracker 运行且对 bench_fault armed（页错误抓栈，采样率可调）

每个配置跑 N 轮取中位数，吞吐型指标的开销% = (baseline/实测 - 1) * 100。

另外采集：
  * bpftool prog show 的 run_time_ns/run_cnt（需打开 kernel.bpf_stats_enabled）
  * bpftool map show 的 memlock 汇总（BPF map 常驻内存）

用法: sudo python3 bench/bench_overhead.py [--rounds 3] [--quick]
输出: 终端表格 + bench/results.json
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACKER = os.path.join(ROOT, "src", "tracker.py")
RESULTS_JSON = os.path.join(ROOT, "bench", "results.json")

# name, binary args, 指标说明, 对应 hook
BENCHES = [
    ("fork/exit 吞吐",      ["bench/bench_fork", "5000"],     "forks_per_sec",    "sched_process_fork/exit"),
    ("上下文切换吞吐",       ["bench/bench_ctxsw", "60000"],   "switches_per_sec", "sched_switch"),
    ("futex 操作吞吐",      ["bench/bench_futex", "60000"],   "futex_ops_per_sec","sys_enter/exit_futex"),
    ("页错误吞吐",          ["bench/bench_fault", "300", "16"], "faults_per_sec",  "page_fault_user(+抓栈)"),
    ("getpid 吞吐(对照)",   ["bench/bench_syscall", "4000000"],"syscalls_per_sec", "无(对照组)"),
]

# armed 阶段用单次长跑，保证 RSS 轮询(0.1s)发现进程后 armed 能覆盖大部分运行期
ARMED_FAULT_CMD = ["bench/bench_fault", "300", "16"]

tracker_proc = None


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def start_tracker(extra_args):
    global tracker_proc
    stop_tracker()
    tracker_proc = subprocess.Popen(
        ["python3", "-u", TRACKER, "--report-dir", "/tmp/ebpf_bench_reports"] + extra_args,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
    # 等待 BPF 编译 + attach 完成（BCC 编译需要几秒）
    time.sleep(9)


def stop_tracker():
    global tracker_proc
    if tracker_proc is None:
        return
    tracker_proc.terminate()
    try:
        tracker_proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        tracker_proc.kill()
        tracker_proc.wait()
    tracker_proc = None


def run_bench(args):
    """跑一轮基准，返回指标值"""
    p = sh([os.path.join(ROOT, args[0])] + args[1:])
    m = re.search(r"RESULT \S+ ([\d.]+)", p.stdout)
    if not m:
        raise RuntimeError("基准无输出: %s\n%s%s" % (args[0], p.stdout, p.stderr))
    return float(m.group(1))


def median_of(args, rounds):
    run_bench(args)          # warmup，丢弃：消除二进制冷加载/缓存效应
    vals = []
    for _ in range(rounds):
        vals.append(run_bench(args))
    return statistics.median(vals), vals


def bpf_prog_stats():
    """读 bpftool prog show 的运行耗时统计（需 bpf_stats_enabled）"""
    out = sh(["bpftool", "-j", "prog", "show"]).stdout
    rows = []
    try:
        for p in json.loads(out):
            if p.get("run_time_ns") and p.get("run_cnt"):
                rows.append({
                    "name": p.get("name") or p.get("tag"),
                    "type": p.get("type"),
                    "run_cnt": p["run_cnt"],
                    "avg_ns": p["run_time_ns"] / p["run_cnt"],
                })
    except json.JSONDecodeError:
        pass
    return rows


def bpf_map_memory():
    """汇总 tracker 相关 BPF map 的 memlock 字节数"""
    out = sh(["bpftool", "-j", "map", "show"]).stdout
    total = 0
    interesting = {"events", "config", "d_start", "futex_start", "armed",
                   "stack_traces", "stack_counts", "pf_counter",
                   "lock_pending", "lock_hold_start"}
    per_map = {}
    try:
        for m in json.loads(out):
            name = m.get("name", "")
            if name in interesting:
                per_map[name] = m.get("bytes_memlock", 0)
                total += m.get("bytes_memlock", 0)
    except json.JSONDecodeError:
        pass
    return total, per_map


def pct(base, val):
    return (base / val - 1.0) * 100.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--quick", action="store_true", help="只跑 1 轮，用于自检")
    args = ap.parse_args()
    rounds = 1 if args.quick else args.rounds

    if os.geteuid() != 0:
        sys.exit("需要 root 运行")

    results = {"rounds": rounds, "benches": [], "prog_stats": [], "map_mem": {}}
    os.makedirs("/tmp/ebpf_bench_reports", exist_ok=True)

    print("=" * 74)
    print("阶段 1/2: baseline 与 loaded 交错采集（消除时间漂移）")
    print("  每轮：先跑全部基准(无tracker) -> 启动tracker -> 再跑全部基准 -> 停tracker")
    print("=" * 74)
    base_vals = {m: [] for _, _, m, _ in BENCHES}
    loaded_vals = {m: [] for _, _, m, _ in BENCHES}
    # warmup：两种状态各跑一次，丢弃
    for _, cmd, _, _ in BENCHES:
        run_bench(cmd)
    start_tracker(["--futex-threshold-ms", "1000000"])
    for _, cmd, _, _ in BENCHES:
        run_bench(cmd)
    stop_tracker()

    for r in range(rounds):
        for _, cmd, metric, _ in BENCHES:
            base_vals[metric].append(run_bench(cmd))
        start_tracker(["--futex-threshold-ms", "1000000"])
        try:
            for _, cmd, metric, _ in BENCHES:
                loaded_vals[metric].append(run_bench(cmd))
        finally:
            stop_tracker()
        print("  轮次 %d 完成" % (r + 1))

    base = {m: statistics.median(v) for m, v in base_vals.items()}
    loaded = {m: statistics.median(v) for m, v in loaded_vals.items()}
    for name, _, metric, _ in BENCHES:
        print("  %-18s baseline %12.0f  loaded %12.0f  开销 %+.1f%%"
              % (name, base[metric], loaded[metric], pct(base[metric], loaded[metric])))

    print("=" * 74)
    print("阶段 2/3: armed（对 bench_fault 抓栈，对比采样率 1/1 与 1/16）")
    print("=" * 74)
    fault_metric = BENCHES[3][2]
    armed = {}
    for sample in (1, 16):
        start_tracker(["--comm", "bench_fault", "--rss-t1", "1",
                       "--rss-t2", "100000000", "--no-kill",
                       "--rss-interval", "0.1", "--stack-sample", str(sample)])
        try:
            med, vals = median_of(ARMED_FAULT_CMD, rounds)
            armed[sample] = med
            print("  页错误吞吐(采样 1/%-2d) %12.0f  开销 %+.1f%%"
                  % (sample, med, pct(base[fault_metric], med)))
        finally:
            stop_tracker()

    print("=" * 74)
    print("阶段 3/3: BPF 程序自身耗时与 map 内存")
    print("=" * 74)
    sh(["sysctl", "-w", "kernel.bpf_stats_enabled=1"])
    start_tracker(["--futex-threshold-ms", "1000000"])
    try:
        # 制造一段混合负载让计数器动起来
        run_bench(BENCHES[0][1])
        run_bench(BENCHES[1][1])
        run_bench(BENCHES[2][1])
        stats = bpf_prog_stats()
        total_mem, per_map = bpf_map_memory()
        results["prog_stats"] = stats
        results["map_mem"] = {"total": total_mem, "per_map": per_map}
        for s in sorted(stats, key=lambda x: -x["run_cnt"]):
            if s["name"] and ("tracepoint" in (s["type"] or "") or "perf_event" in (s["type"] or "")):
                print("  prog %-28s 调用 %-9d 平均 %8.0f ns/次"
                      % (s["name"], s["run_cnt"], s["avg_ns"]))
        print("  BPF map 常驻内存合计: %.1f KB" % (total_mem / 1024.0))
    finally:
        stop_tracker()
        sh(["sysctl", "-w", "kernel.bpf_stats_enabled=0"])

    # 汇总
    for name, cmd, metric, hook in BENCHES:
        row = {"name": name, "hook": hook, "baseline": base[metric],
               "loaded": loaded[metric], "loaded_overhead_pct": pct(base[metric], loaded[metric])}
        if metric == fault_metric:
            row["armed_1_1"] = armed[1]
            row["armed_1_16"] = armed[16]
            row["armed_overhead_pct_1_1"] = pct(base[metric], armed[1])
            row["armed_overhead_pct_1_16"] = pct(base[metric], armed[16])
        results["benches"].append(row)

    with open(RESULTS_JSON, "w") as f:
        json.dump(results, f, ensure_ascii=False, indent=2)

    print()
    print("汇总表（开销 %% = 吞吐下降百分比）")
    print("| 基准 | 对应 hook | baseline | loaded | loaded 开销 | armed 1/1 | armed 1/16 |")
    print("|---|---|---|---|---|---|---|")
    for r in results["benches"]:
        print("| %s | %s | %.0f | %.0f | %+.1f%% | %s | %s |" % (
            r["name"], r["hook"], r["baseline"], r["loaded"],
            r["loaded_overhead_pct"],
            "%.0f (%+.1f%%)" % (r["armed_1_1"], r["armed_overhead_pct_1_1"]) if "armed_1_1" in r else "-",
            "%.0f (%+.1f%%)" % (r["armed_1_16"], r["armed_overhead_pct_1_16"]) if "armed_1_16" in r else "-"))
    print("\n结果已写入 %s" % RESULTS_JSON)


if __name__ == "__main__":
    main()
