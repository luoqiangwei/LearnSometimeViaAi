#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_rawtp.py — raw tracepoint 与经典 tracepoint 的开销对照

tracker --raw-tp 用 raw tracepoint 复现全部挂点（src/bpf_program_rawtp.c，
绕过 perf event 框架）。本脚本定量优化收益与代价：

  方法 A：bpf_stats 直接统计各 BPF 程序单次耗时（两种 tracker 分别加载，
          同负载读 run_time_ns/run_cnt 差值）——最确定的定量。
          附带 getpid 负载：量化 raw 版 sys_enter/sys_exit 挂在全系统调用
          上的"每次 syscall 都付费"代价（经典版 sys_enter_futex 不触发）。
  方法 B：四状态分解（taskset 绑核，端到端吞吐）：
          base    无探针
          tp-empty / raw-empty   bpftrace 挂对应空探针 → 纯基础设施
          tp-full  / raw-full    完整 tracker            → 基础设施+逻辑

用法: sudo python3 bench/bench_rawtp.py
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

EMPTY_TP = (
    'tracepoint:sched:sched_switch { 1; } '
    'tracepoint:sched:sched_process_fork { 1; } '
    'tracepoint:sched:sched_process_exec { 1; } '
    'tracepoint:sched:sched_process_exit { 1; } '
    'tracepoint:syscalls:sys_enter_futex { 1; } '
    'tracepoint:syscalls:sys_exit_futex { 1; } '
    'tracepoint:exceptions:page_fault_user { 1; }'
)

EMPTY_RAW = (
    'rawtracepoint:sched_switch { 1; } '
    'rawtracepoint:sched_process_fork { 1; } '
    'rawtracepoint:sched_process_exec { 1; } '
    'rawtracepoint:sched_process_exit { 1; } '
    'rawtracepoint:sys_enter { 1; } '
    'rawtracepoint:sys_exit { 1; } '
    'rawtracepoint:page_fault_user { 1; }'
)

# 方法 A 负载：(负载名, bench 命令, 经典版 prog 名, raw 版 prog 名)
LOADS_A = [
    ("sched_switch", ["bench/bench_ctxsw", "350000"],
     ["tracepoint__sched__sched_switch"], ["raw_tp_sched_switch"]),
    ("futex", ["bench/bench_futex", "350000"],
     ["tracepoint__syscalls__sys_enter_futex",
      "tracepoint__syscalls__sys_exit_futex"],
     ["raw_tp_sys_enter", "raw_tp_sys_exit"]),
    ("page_fault", ["bench/bench_fault", "700", "16"],
     ["tracepoint__exceptions__page_fault_user"], ["raw_tp_page_fault"]),
    ("fork/exit", ["bench/bench_fork", "40000"],
     ["tracepoint__sched__sched_process_fork",
      "tracepoint__sched__sched_process_exit"],
     ["raw_tp_fork", "raw_tp_exit"]),
    # getpid 负载：raw 版 sys_enter/exit 对每次 syscall 都触发（代价量化），
    # 经典版 sys_enter_futex 对 getpid 不触发（≈0）
    ("getpid(对照)", ["bench/bench_syscall", "4000000"],
     ["tracepoint__syscalls__sys_enter_futex",
      "tracepoint__syscalls__sys_exit_futex"],
     ["raw_tp_sys_enter", "raw_tp_sys_exit"]),
]

# 方法 B 基准
BENCHES_B = [
    ("sched_switch", ["bench/bench_ctxsw", "60000"], 3),
    ("futex", ["bench/bench_futex", "60000"], 3),
    ("page_fault", ["bench/bench_fault", "300", "16"], 3),
    ("getpid(对照)", ["bench/bench_syscall", "4000000"], 3),
]

agent_proc = None


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def stop_agent():
    global agent_proc
    if agent_proc is None:
        return
    agent_proc.terminate()
    try:
        agent_proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        agent_proc.kill()
        agent_proc.wait()
    agent_proc = None


def start_agent(kind):
    """kind: 'tp-empty' | 'raw-empty' | 'tp-full' | 'raw-full'"""
    global agent_proc
    stop_agent()
    if kind.endswith("empty"):
        script = EMPTY_TP if kind.startswith("tp") else EMPTY_RAW
        agent_proc = subprocess.Popen(
            ["taskset", "-c", "1", "bpftrace", "-e", script],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
        time.sleep(4)
    else:
        extra = ["--raw-tp"] if kind.startswith("raw") else []
        agent_proc = subprocess.Popen(
            ["taskset", "-c", "1", "python3", "-u", TRACKER] + extra + [
                "--report-dir", "/tmp/ebpf_rawtp_reports",
                "--futex-threshold-ms", "1000000",
                "--d-threshold-ms", "1000000"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
        time.sleep(40)      # raw 版 BCC 编译较慢，等它挂载完成


def run_bench(args, cpu=None):
    cmd = [os.path.join(ROOT, args[0])] + args[1:]
    if cpu is not None:
        cmd = ["taskset", "-c", str(cpu)] + cmd
    p = sh(cmd)
    m = re.search(r"RESULT \S+ ([\d.]+)", p.stdout)
    if not m:
        raise RuntimeError("无输出: %s %s" % (args[0], p.stderr[:200]))
    return float(m.group(1))


# ---------- 方法 A：bpf_stats ----------

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


def method_a_variant(raw):
    start_agent("raw-full" if raw else "tp-full")
    res = {}
    try:
        for load_name, cmd, tp_progs, raw_progs in LOADS_A:
            progs = raw_progs if raw else tp_progs
            s0 = prog_snapshot()
            t0 = time.monotonic()
            run_bench(cmd)
            dt = time.monotonic() - t0
            s1 = prog_snapshot()
            per_load = {}
            for name in progs:
                if name not in s1:
                    print("  [warn] 未找到 prog %s" % name)
                    continue
                t_diff = s1[name][0] - s0.get(name, (0, 0))[0]
                c_diff = s1[name][1] - s0.get(name, (0, 0))[1]
                if c_diff <= 0:
                    per_load[name] = (0.0, 0.0, 0.0)
                    continue
                per_load[name] = (c_diff / dt, t_diff / c_diff,
                                  100.0 * t_diff / (dt * 1e9))
            res[load_name] = per_load   # 立即按负载存档，避免后续负载覆写
    finally:
        stop_agent()
    return res


def method_a():
    print("=" * 78)
    print("方法 A：bpf_stats 单次耗时对照（kernel.bpf_stats_enabled=1，极端事件率）")
    print("=" * 78)
    sh(["sysctl", "-w", "kernel.bpf_stats_enabled=1"])
    try:
        tp = method_a_variant(raw=False)
        raw = method_a_variant(raw=True)
    finally:
        sh(["sysctl", "-w", "kernel.bpf_stats_enabled=0"])

    print()
    print("%-16s | %-38s | %-38s" % ("负载", "经典 tracepoint 版", "raw tracepoint 版"))
    print("-" * 98)
    pairs = [
        ("sched_switch", ["tracepoint__sched__sched_switch"],
         ["raw_tp_sched_switch"]),
        ("futex", ["tracepoint__syscalls__sys_enter_futex",
                   "tracepoint__syscalls__sys_exit_futex"],
         ["raw_tp_sys_enter", "raw_tp_sys_exit"]),
        ("page_fault", ["tracepoint__exceptions__page_fault_user"],
         ["raw_tp_page_fault"]),
        ("fork/exit", ["tracepoint__sched__sched_process_fork",
                       "tracepoint__sched__sched_process_exit"],
         ["raw_tp_fork", "raw_tp_exit"]),
        ("getpid(对照)", ["tracepoint__syscalls__sys_enter_futex",
                          "tracepoint__syscalls__sys_exit_futex"],
         ["raw_tp_sys_enter", "raw_tp_sys_exit"]),
    ]
    rows = {}
    for label, tp_names, raw_names in pairs:
        def agg(res, names):
            per_load = res.get(label, {})
            ev = sum(per_load.get(n, (0, 0, 0))[0] for n in names)
            cpu = sum(per_load.get(n, (0, 0, 0))[2] for n in names)
            per = ["%s:%.0fns" % (n.replace("tracepoint__", "")
                                  .replace("raw_tp_", "r:"), per_load[n][1])
                   for n in names
                   if n in per_load and per_load[n][1] > 0]
            return ev, cpu, per
        ev_t, cpu_t, per_t = agg(tp, tp_names)
        ev_r, cpu_r, per_r = agg(raw, raw_names)
        rows[label] = (per_t, cpu_t, per_r, cpu_r)
        disp = "futex enter/exit" if label == "futex" else label
        print("%-16s | %9.0f次/s 合计单核%6.2f%% | %9.0f次/s 合计单核%6.2f%%"
              % (disp, ev_t, cpu_t, ev_r, cpu_r))
        print("%-16s | %-38s | %-38s" % ("", " ".join(per_t) or "-",
                                         " ".join(per_r) or "-"))
    return rows


# ---------- 方法 B：四状态吞吐分解 ----------

def measure_state(state, rounds):
    if state != "base":
        start_agent(state)
    vals = {name: [] for name, _, _ in BENCHES_B}
    try:
        for _ in range(rounds):
            for name, cmd, _ in BENCHES_B:
                vals[name].append(run_bench(cmd, cpu=0))
    finally:
        stop_agent()
    return {name: statistics.median(v) for name, v in vals.items()}


def method_b(rounds=3):
    print("=" * 78)
    print("方法 B：绑核吞吐分解（bench=核0，agent=核1，中位数）")
    print("=" * 78)
    for _, cmd, _ in BENCHES_B:     # warmup
        run_bench(cmd, cpu=0)

    states = ["base", "tp-empty", "raw-empty", "tp-full", "raw-full"]
    out = {}
    for st in states:
        print("测 %-9s ..." % st)
        out[st] = measure_state(st, rounds)

    print()
    print("%-14s | %10s | %10s | %10s | %10s | %10s" % (
        "基准(ops/s)", "base", "tp-empty", "raw-empty", "tp-full", "raw-full"))
    print("-" * 82)
    res = {}
    for name, _, _ in BENCHES_B:
        b = out["base"][name]
        te, re_ = out["tp-empty"][name], out["raw-empty"][name]
        tf, rf = out["tp-full"][name], out["raw-full"][name]
        res[name] = (b, te, re_, tf, rf)
        print("%-14s | %10.0f | %10.0f | %10.0f | %10.0f | %10.0f"
              % (name, b, te, re_, tf, rf))
        tp_infra = (1 / te - 1 / b) * 1e9
        raw_infra = (1 / re_ - 1 / b) * 1e9
        print("%-14s | 基础设施: 经典tp %+.0fns/op (%+5.1f%%)  "
              "raw-tp %+.0fns/op (%+5.1f%%)" % (
                  "", tp_infra, (b / te - 1) * 100,
                  raw_infra, (b / re_ - 1) * 100))
    return res


def main():
    if os.geteuid() != 0:
        sys.exit("需要 root 运行")
    os.makedirs("/tmp/ebpf_rawtp_reports", exist_ok=True)
    only_a = "--method-a" in sys.argv     # 只跑方法 A（方法 B 数据可复用）
    rows_a = method_a()
    if only_a:
        return
    print()
    res_b = method_b()
    print()
    print("小结：")
    print("  - 方法A：同负载同逻辑下两种挂点方式的单次耗时直接对比；")
    print("           getpid 行量化 raw 版 sys_enter/exit 的全 syscall 触发代价")
    print("  - 方法B：empty 与 base 的差 = 该挂点方式的基础设施开销")


if __name__ == "__main__":
    main()
