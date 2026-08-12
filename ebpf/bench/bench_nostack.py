#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_nostack.py — 专测"不抓堆栈"（loaded 状态）时 eBPF tracker 的开销

三种互补方法：
  A. bpf_stats 直接统计各 BPF 程序运行耗时，按事件率换算 CPU 占比
     （不依赖吞吐对比，最确定的定量方法）
  B. taskset 绑核隔离：bench 绑核 0、tracker 用户态绑核 1，
     端到端吞吐对比（排除用户态 agent 抢占，只留内核路径开销）
  C. 事件流下 tracker 用户态进程自身的 CPU 占用（/proc/<pid>/stat 差值法）

用法: sudo python3 bench/bench_nostack.py
"""
import json
import os
import re
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACKER = os.path.join(ROOT, "src", "tracker.py")
CLK_TCK = os.sysconf("SC_CLK_TCK")

# 方法 A 的负载：每项约 6-8 秒，让 prog 计数器充分累积
LOADS_A = [
    ("sched_switch 压力", ["bench/bench_ctxsw", "350000"],
     ["tracepoint__sched__sched_switch"]),
    ("futex 压力",        ["bench/bench_futex", "350000"],
     ["tracepoint__syscalls__sys_enter_futex",
      "tracepoint__syscalls__sys_exit_futex"]),
    ("page_fault 压力",   ["bench/bench_fault", "700", "16"],
     ["tracepoint__exceptions__page_fault_user"]),
    ("fork/exit 压力",    ["bench/bench_fork", "40000"],
     ["tracepoint__sched__sched_process_fork",
      "tracepoint__sched__sched_process_exit"]),
]

# 方法 B 的基准（较短轮次，多轮取中位数）
LOADS_B = [
    ("上下文切换吞吐", ["bench/bench_ctxsw", "60000"],  "switches_per_sec"),
    ("页错误吞吐",     ["bench/bench_fault", "300", "16"], "faults_per_sec"),
    ("getpid(对照)",   ["bench/bench_syscall", "4000000"], "syscalls_per_sec"),
]

tracker_proc = None


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def start_tracker(extra, cpu=None):
    global tracker_proc
    stop_tracker()
    cmd = ["python3", "-u", TRACKER, "--report-dir", "/tmp/ebpf_nostack_reports"] + extra
    if cpu is not None:
        cmd = ["taskset", "-c", str(cpu)] + cmd
    tracker_proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                    stderr=subprocess.DEVNULL, cwd=ROOT)
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


def run_bench(args, cpu=None):
    cmd = [os.path.join(ROOT, args[0])] + args[1:]
    if cpu is not None:
        cmd = ["taskset", "-c", str(cpu)] + cmd
    p = sh(cmd)
    m = re.search(r"RESULT \S+ ([\d.]+)", p.stdout)
    if not m:
        raise RuntimeError("无输出: %s %s" % (args[0], p.stderr[:200]))
    return float(m.group(1))


# ---------- 方法 A ----------

def prog_snapshot():
    out = sh(["bpftool", "-j", "prog", "show"]).stdout
    d = {}
    try:
        for p in json.loads(out):
            name = p.get("name")
            if name and p.get("run_time_ns") is not None:
                d[name] = (p.get("run_time_ns", 0), p.get("run_cnt", 0))
    except json.JSONDecodeError:
        pass
    return d


def method_a():
    print("=" * 74)
    print("方法 A：bpf_stats 直接统计 BPF 程序耗时（kernel.bpf_stats_enabled=1）")
    print("=" * 74)
    sh(["sysctl", "-w", "kernel.bpf_stats_enabled=1"])
    start_tracker(["--futex-threshold-ms", "1000000", "--d-threshold-ms", "1000000"])
    rows = []
    try:
        for load_name, cmd, progs in LOADS_A:
            s0 = prog_snapshot()
            t0 = time.monotonic()
            run_bench(cmd)
            dt = time.monotonic() - t0
            s1 = prog_snapshot()
            for name in progs:
                if name not in s1:
                    continue
                t_diff = s1[name][0] - s0.get(name, (0, 0))[0]
                c_diff = s1[name][1] - s0.get(name, (0, 0))[1]
                if c_diff <= 0:
                    continue
                ns_per = t_diff / c_diff
                ev_per_s = c_diff / dt
                cpu_pct = 100.0 * t_diff / (dt * 1e9)   # 单核占比%
                rows.append((load_name, name, ev_per_s, ns_per, cpu_pct))
                print("  %-16s %-42s %9.0f 次/s  %7.0f ns/次  占单核 %5.2f%%"
                      % (load_name, name.replace("tracepoint__", ""), ev_per_s,
                         ns_per, cpu_pct))
    finally:
        stop_tracker()
        sh(["sysctl", "-w", "kernel.bpf_stats_enabled=0"])
    return rows


# ---------- 方法 B ----------

def method_b(rounds=5):
    print("=" * 74)
    print("方法 B：taskset 绑核（bench=核0，tracker=核1），端到端吞吐对比")
    print("=" * 74)
    # warmup
    for _, cmd, _ in LOADS_B:
        run_bench(cmd, cpu=0)
    base, loaded = {}, {}
    vals_b = {m: [] for _, _, m in LOADS_B}
    vals_l = {m: [] for _, _, m in LOADS_B}
    for _ in range(rounds):
        for _, cmd, m in LOADS_B:
            vals_b[m].append(run_bench(cmd, cpu=0))
    start_tracker(["--futex-threshold-ms", "1000000"], cpu=1)
    try:
        for _ in range(rounds):
            for _, cmd, m in LOADS_B:
                vals_l[m].append(run_bench(cmd, cpu=0))
    finally:
        stop_tracker()
    out = []
    for name, _, m in LOADS_B:
        base[m] = statistics.median(vals_b[m])
        loaded[m] = statistics.median(vals_l[m])
        overhead = (base[m] / loaded[m] - 1.0) * 100.0
        out.append((name, base[m], loaded[m], overhead))
        print("  %-14s baseline(核0) %12.0f  loaded(核0) %12.0f  开销 %+5.1f%%"
              % (name, base[m], loaded[m], overhead))
    return out


# ---------- 方法 C ----------

def proc_cpu_ticks(pid):
    with open("/proc/%d/stat" % pid) as f:
        data = f.read()
    rest = data[data.rindex(")") + 2:]
    f = rest.split()
    return int(f[11]) + int(f[12])   # utime + stime


def method_c():
    print("=" * 74)
    print("方法 C：事件流下 tracker 用户态进程的 CPU 占用")
    print("=" * 74)
    # 用 fork 风暴制造事件流（每次 fork+exit 产生 2 个事件）
    start_tracker(["--comm", "bench_fork"])   # 正常阈值，事件全量上报
    try:
        t0 = time.monotonic()
        c0 = proc_cpu_ticks(tracker_proc.pid)
        run_bench(["bench/bench_fork", "15000"])    # ~3万事件
        dt = time.monotonic() - t0
        c1 = proc_cpu_ticks(tracker_proc.pid)
        cpu_pct = 100.0 * (c1 - c0) / CLK_TCK / dt
        ev_rate = 15000 * 2 / dt
        print("  fork/exit 事件流 %.0f 事件/s：tracker 用户态 CPU = %.1f%% 单核"
              % (ev_rate, cpu_pct))
        # 空闲场景（几乎无事件）
        time.sleep(3)
        t0 = time.monotonic()
        c0 = proc_cpu_ticks(tracker_proc.pid)
        time.sleep(5)
        dt = time.monotonic() - t0
        c1 = proc_cpu_ticks(tracker_proc.pid)
        cpu_pct = 100.0 * (c1 - c0) / CLK_TCK / dt
        print("  系统空闲（仅后台零星事件）：tracker 用户态 CPU = %.2f%% 单核"
              % cpu_pct)
    finally:
        stop_tracker()


def main():
    if os.geteuid() != 0:
        sys.exit("需要 root 运行")
    os.makedirs("/tmp/ebpf_nostack_reports", exist_ok=True)
    rows_a = method_a()
    print()
    rows_b = method_b()
    print()
    method_c()
    print()
    print("小结（不抓堆栈时的开销）：")
    total_cpu = sum(r[4] for r in rows_a)
    print("  - 方法A：极端微基准事件率下，全部 BPF 程序合计占单核 %.1f%%" % total_cpu)
    print("  - 方法B：绑核隔离后端到端吞吐影响见上表（对照组指示噪声下限）")
    print("  - 方法C：用户态 agent 平时近 0，事件流时与事件率成正比")


if __name__ == "__main__":
    main()
