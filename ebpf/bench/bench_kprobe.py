#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_kprobe.py — KProbe 与 Tracepoint 两种挂点方式的开销对照

tracker --kprobe 用 7 个 kprobe/kretprobe 复现 tracepoint 版的全部功能
（src/bpf_program_kprobe.c），本脚本定量两种方式的开销差异：

  方法 A：bpf_stats 直接统计各 BPF 程序单次耗时（两种 tracker 分别加载，
          跑同一组微基准，读 run_time_ns/run_cnt 差值）——最确定的定量
  方法 B：四状态分解（taskset 绑核，端到端吞吐）：
          base    无探针
          tp-empty / kp-empty   bpftrace 挂对应探针但程序体为空 → 基础设施
          tp-full  / kp-full    完整 tracker                    → 基础设施+逻辑

用法: sudo python3 bench/bench_kprobe.py
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

EMPTY_KP = (
    'kprobe:wake_up_new_task { 1; } '
    'kprobe:begin_new_exec { 1; } '
    'kprobe:do_exit { 1; } '
    'kprobe:finish_task_switch.isra.0 { 1; } '
    'kprobe:__x64_sys_futex { 1; } '
    'kretprobe:__x64_sys_futex { 1; } '
    'kprobe:handle_mm_fault { 1; }'
)

# 方法 A 负载：(负载名, bench 命令, tp 版 prog 名, kp 版 prog 名)
LOADS_A = [
    ("sched_switch", ["bench/bench_ctxsw", "350000"],
     ["tracepoint__sched__sched_switch"], ["kprobe_sched_switch"]),
    ("futex", ["bench/bench_futex", "350000"],
     ["tracepoint__syscalls__sys_enter_futex",
      "tracepoint__syscalls__sys_exit_futex"],
     ["kprobe_futex_enter", "kprobe_futex_exit"]),
    ("page_fault", ["bench/bench_fault", "700", "16"],
     ["tracepoint__exceptions__page_fault_user"], ["kprobe_page_fault"]),
    ("fork/exit", ["bench/bench_fork", "40000"],
     ["tracepoint__sched__sched_process_fork",
      "tracepoint__sched__sched_process_exit"],
     ["kprobe_fork", "kprobe_exit"]),
]

# 方法 B 基准（较短轮次，多轮取中位数；getpid 为无探针对照组指示噪声）
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
    """kind: 'tp-empty' | 'kp-empty' | 'tp-full' | 'kp-full'"""
    global agent_proc
    stop_agent()
    if kind.endswith("empty"):
        script = EMPTY_TP if kind.startswith("tp") else EMPTY_KP
        agent_proc = subprocess.Popen(
            ["taskset", "-c", "1", "bpftrace", "-e", script],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
        time.sleep(4)
    else:
        extra = ["--kprobe"] if kind.startswith("kp") else []
        agent_proc = subprocess.Popen(
            ["taskset", "-c", "1", "python3", "-u", TRACKER] + extra + [
                "--report-dir", "/tmp/ebpf_kprobe_reports",
                "--futex-threshold-ms", "1000000",
                "--d-threshold-ms", "1000000"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
        time.sleep(12)


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


def method_a_variant(kprobe):
    """加载一种 tracker，跑全部负载，返回 {prog: (ev/s, ns/次, cpu%)}"""
    start_agent("kp-full" if kprobe else "tp-full")
    idx = 3 if kprobe else 2
    res = {}
    try:
        for load_name, cmd, tp_progs, kp_progs in LOADS_A:
            progs = kp_progs if kprobe else tp_progs
            s0 = prog_snapshot()
            t0 = time.monotonic()
            run_bench(cmd)
            dt = time.monotonic() - t0
            s1 = prog_snapshot()
            for name in progs:
                if name not in s1:
                    print("  [warn] 未找到 prog %s" % name)
                    continue
                t_diff = s1[name][0] - s0.get(name, (0, 0))[0]
                c_diff = s1[name][1] - s0.get(name, (0, 0))[1]
                if c_diff <= 0:
                    continue
                res[name] = (c_diff / dt, t_diff / c_diff,
                             100.0 * t_diff / (dt * 1e9))
    finally:
        stop_agent()
    return res


def method_a():
    print("=" * 78)
    print("方法 A：bpf_stats 单次耗时对照（kernel.bpf_stats_enabled=1，极端事件率）")
    print("=" * 78)
    sh(["sysctl", "-w", "kernel.bpf_stats_enabled=1"])
    try:
        tp = method_a_variant(kprobe=False)
        kp = method_a_variant(kprobe=True)
    finally:
        sh(["sysctl", "-w", "kernel.bpf_stats_enabled=0"])

    print()
    print("%-22s | %-36s | %-36s" % ("负载", "tracepoint 版", "kprobe 版"))
    print("-" * 100)
    pairs = [
        ("sched_switch", ["tracepoint__sched__sched_switch"],
         ["kprobe_sched_switch"]),
        ("futex enter/exit", ["tracepoint__syscalls__sys_enter_futex",
                              "tracepoint__syscalls__sys_exit_futex"],
         ["kprobe_futex_enter", "kprobe_futex_exit"]),
        ("page_fault", ["tracepoint__exceptions__page_fault_user"],
         ["kprobe_page_fault"]),
        ("fork/exit", ["tracepoint__sched__sched_process_fork",
                       "tracepoint__sched__sched_process_exit"],
         ["kprobe_fork", "kprobe_exit"]),
    ]
    rows = {}
    for label, tp_names, kp_names in pairs:
        def agg(res, names):
            ev = sum(res[n][0] for n in names if n in res)
            cpu = sum(res[n][2] for n in names if n in res)
            per = ["%s:%.0fns" % (n.replace("tracepoint__", "")
                                  .replace("kprobe_", "k:"), res[n][1])
                   for n in names if n in res]
            return ev, cpu, per
        ev_t, cpu_t, per_t = agg(tp, tp_names)
        ev_k, cpu_k, per_k = agg(kp, kp_names)
        rows[label] = (per_t, cpu_t, per_k, cpu_k)
        print("%-22s | %9.0f次/s 合计单核%5.2f%% | %9.0f次/s 合计单核%5.2f%%"
              % (label, ev_t, cpu_t, ev_k, cpu_k))
        print("%-22s | %-36s | %-36s" % ("", " ".join(per_t), " ".join(per_k)))
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

    states = ["base", "tp-empty", "kp-empty", "tp-full", "kp-full"]
    out = {}
    for st in states:
        print("测 %-8s ..." % st)
        out[st] = measure_state(st, rounds)

    print()
    print("%-14s | %10s | %10s | %10s | %10s | %10s" % (
        "基准(ops/s)", "base", "tp-empty", "kp-empty", "tp-full", "kp-full"))
    print("-" * 82)
    res = {}
    for name, _, _ in BENCHES_B:
        b = out["base"][name]
        te, ke = out["tp-empty"][name], out["kp-empty"][name]
        tf, kf = out["tp-full"][name], out["kp-full"][name]
        res[name] = (b, te, ke, tf, kf)
        print("%-14s | %10.0f | %10.0f | %10.0f | %10.0f | %10.0f"
              % (name, b, te, ke, tf, kf))
        tp_infra = (1 / te - 1 / b) * 1e9
        kp_infra = (1 / ke - 1 / b) * 1e9
        print("%-14s | 基础设施: tracepoint %+.0fns/op (%+5.1f%%)  "
              "kprobe %+.0fns/op (%+5.1f%%)" % (
                  "", tp_infra, (b / te - 1) * 100,
                  kp_infra, (b / ke - 1) * 100))
    return res


def main():
    if os.geteuid() != 0:
        sys.exit("需要 root 运行")
    os.makedirs("/tmp/ebpf_kprobe_reports", exist_ok=True)
    rows_a = method_a()
    print()
    res_b = method_b()
    print()
    print("小结：")
    print("  - 方法A：同负载同程序逻辑下，kprobe 版与 tracepoint 版单次耗时直接对比")
    print("  - 方法B：empty 与 base 的差 = 该挂点方式的基础设施开销；")
    print("           getpid 对照组的'开销'即本机噪声下限")


if __name__ == "__main__":
    main()
