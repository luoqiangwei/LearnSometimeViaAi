#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_probe_path.py — 分解"进入/退出 BPF 路径"的开销

bpf_stats 只统计 BPF 程序本体执行时间，不含探针基础设施开销
（tracepoint 触发 → perf 框架回调 → BPF 入口，及退出返回路径）。
本脚本用三分解法定量：

  baseline   : 系统无任何探针
  probe-only : bpftrace 挂载与 tracker 相同的 7 个 tracepoint，但程序体为空
               → 与 baseline 的差 = 探针基础设施（进入+退出路径）开销
  full       : 完整 tracker（loaded，不抓栈）
               → 与 probe-only 的差 = BPF 程序逻辑开销；与 baseline 的差 = 总开销

bench 绑核 0，agent（bpftrace/tracker）绑核 1，消除用户态干扰。
用法: sudo python3 bench/bench_probe_path.py
"""
import os
import re
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACKER = os.path.join(ROOT, "src", "tracker.py")

EMPTY_PROBES = (
    'tracepoint:sched:sched_switch { 1; } '
    'tracepoint:sched:sched_process_fork { 1; } '
    'tracepoint:sched:sched_process_exec { 1; } '
    'tracepoint:sched:sched_process_exit { 1; } '
    'tracepoint:syscalls:sys_enter_futex { 1; } '
    'tracepoint:syscalls:sys_exit_futex { 1; } '
    'tracepoint:exceptions:page_fault_user { 1; }'
)

BENCHES = [
    ("sched_switch", ["bench/bench_ctxsw", "60000"], 5),
    ("futex",        ["bench/bench_futex", "60000"], 5),
    ("page_fault",   ["bench/bench_fault", "300", "16"], 3),
    ("fork/exit",    ["bench/bench_fork", "5000"], 3),
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
    """kind: 'probe' | 'full'"""
    global agent_proc
    stop_agent()
    if kind == "probe":
        agent_proc = subprocess.Popen(
            ["taskset", "-c", "1", "bpftrace", "-e", EMPTY_PROBES],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
        time.sleep(4)      # bpftrace 编译+attach
    else:
        agent_proc = subprocess.Popen(
            ["taskset", "-c", "1", "python3", "-u", TRACKER,
             "--report-dir", "/tmp/ebpf_probe_reports",
             "--futex-threshold-ms", "1000000"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
        time.sleep(9)      # BCC 编译+attach


def run_bench(args, rounds):
    vals = []
    for _ in range(rounds):
        p = sh(["taskset", "-c", "0", os.path.join(ROOT, args[0])] + args[1:])
        m = re.search(r"RESULT \S+ ([\d.]+)", p.stdout)
        if not m:
            raise RuntimeError("无输出: %s %s" % (args[0], p.stderr[:200]))
        vals.append(float(m.group(1)))
    return statistics.median(vals)


def measure(state):
    """state: 'base'|'probe'|'full'，返回 {bench_name: rate}"""
    if state != "base":
        start_agent(state)
    out = {}
    try:
        for name, cmd, rounds in BENCHES:
            out[name] = run_bench(cmd, rounds)
    finally:
        stop_agent()
    return out


def main():
    if os.geteuid() != 0:
        sys.exit("需要 root 运行")
    os.makedirs("/tmp/ebpf_probe_reports", exist_ok=True)

    print("=" * 74)
    print("三分解测量：baseline / 空探针(probe-only) / 完整tracker(full)")
    print("bench=核0, agent=核1, 中位数")
    print("=" * 74)

    # warmup
    for _, cmd, _ in BENCHES:
        run_bench(cmd, 1)

    print("测 baseline ...")
    base = measure("base")
    print("测 probe-only（bpftrace 空探针 x7）...")
    probe = measure("probe")
    print("测 full（tracker loaded）...")
    full = measure("full")

    print()
    print("%-14s | %12s | %12s | %12s | %s" % (
        "基准(ops/s)", "baseline", "probe-only", "full", "分解"))
    print("-" * 90)
    for name, _, _ in BENCHES:
        b, p, f = base[name], probe[name], full[name]
        infra_pct = (b / p - 1) * 100          # 探针基础设施开销
        logic_pct = (p / f - 1) * 100          # BPF 逻辑开销
        total_pct = (b / f - 1) * 100          # 总开销
        # 每次事件的绝对耗时估算（单核下 ops/s → 秒/op）
        infra_ns = (1 / p - 1 / b) * 1e9
        logic_ns = (1 / f - 1 / p) * 1e9
        print("%-14s | %12.0f | %12.0f | %12.0f | 基础设施 %+5.1f%%(~%4.0fns/op) "
              "逻辑 %+5.1f%%(~%4.0fns/op) 总 %+5.1f%%"
              % (name, b, p, f, infra_pct, infra_ns, logic_pct, logic_ns, total_pct))


if __name__ == "__main__":
    main()
