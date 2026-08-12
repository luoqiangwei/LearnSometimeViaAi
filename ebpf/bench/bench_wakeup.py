#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_wakeup.py — ringbuf 唤醒机制成本专题

针对"唤醒到底花多少、什么事件率下该用哪种消费形态"：

  对照组：ringbuf 默认唤醒（每次提交唤醒消费者）
          vs ringbuf NO_WAKEUP（不唤醒，用户态 5ms 节拍主动 consume）
  事件率：满载(1/1) vs 降额(1/8，~5万/s)

  指标：生产端吞吐、BPF 本体 ns（bpf_stats，不含异步唤醒）、
        消费者 CPU、消费端唤醒次数/s（poll 返回次数）、
        每唤醒送达事件数（批量化程度）、延迟 p50、
        以及 /proc/stat 的 cpu0 softirq 占比——唤醒的 irq_work 在
        生产者 CPU 上的真实落账（bpf_stats 看不见的那部分）。

用法: sudo python3 bench/bench_wakeup.py
"""
import json
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from bench.bench_comm import (BPF_SRC, Consumer, map_total, prog_snapshot,  # noqa: E402
                              proc_cpu_ticks, run_producer, sh)
from bcc import BPF  # noqa: E402

CLK_TCK = os.sysconf("SC_CLK_TCK")


def softirq_jiffies(cpu=0):
    """/proc/stat 指定 CPU 的 irq+softirq jiffies（第 7、8 列，1 起）"""
    with open("/proc/stat") as f:
        for line in f:
            if line.startswith("cpu%d " % cpu):
                f = line.split()
                return int(f[6]) + int(f[7])   # irq + softirq
    return 0


class WakeupConsumer(Consumer):
    """带唤醒次数统计的消费者"""
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.polls = 0

    def start(self):
        if self.rb_flags == 1:
            self.b["events"].open_ring_buffer(self._on_event)
            def loop():
                while not self.stop.is_set():
                    self.b.ring_buffer_consume()
                    self.polls += 1
                    time.sleep(0.005)
        else:
            self.b["events"].open_ring_buffer(self._on_event)
            def loop():
                while not self.stop.is_set():
                    self.b.ring_buffer_poll(timeout=50)
                    self.polls += 1
        self.th = threading.Thread(target=loop, daemon=True)
        self.th.start()

    def finish(self):
        out = super().finish()
        out["polls"] = self.polls
        return out


def measure(flags, sample, rounds=4000000):
    cflags = ["-DMECH=2", "-DPAYLOAD=32",
              "-DRB_FLAGS=%d" % flags, "-DSAMPLE=%d" % sample]
    b = BPF(text=BPF_SRC, cflags=cflags)
    consumer = WakeupConsumer(b, 2, 32, flags)
    consumer.start()
    time.sleep(0.5)

    s0 = prog_snapshot()
    u0 = map_total(b, "subm")
    sq0 = softirq_jiffies(0)
    c0 = proc_cpu_ticks(os.getpid())
    t0 = time.monotonic()
    rate = run_producer(rounds)
    dt = time.monotonic() - t0
    time.sleep(1.0)
    s1 = prog_snapshot()
    u1 = map_total(b, "subm")
    sq1 = softirq_jiffies(0)
    c1 = proc_cpu_ticks(os.getpid())

    name = "tracepoint__syscalls__sys_enter_getpid"
    prog_ns = None
    if name in s1 and s1[name][1] > s0.get(name, (0, 0))[1]:
        prog_ns = ((s1[name][0] - s0.get(name, (0, 0))[0]) /
                   (s1[name][1] - s0.get(name, (0, 0))[1]))
    fin = consumer.finish()
    submitted = u1 - u0
    del b
    return {
        "flags": flags, "sample": sample,
        "producer_rate": rate,
        "producer_ns": prog_ns,
        "submit_rate": submitted / dt,
        "recv_rate": fin["count"] / dt,
        "drop_pct": (100.0 * (submitted - fin["count"]) / submitted
                     if submitted else 0),
        "lat_p50_us": fin["lat_p50"] / 1e3,
        "lat_p99_us": fin["lat_p99"] / 1e3,
        "consumer_cpu": 100.0 * (c1 - c0) / CLK_TCK / dt,
        "wakeups_s": fin["polls"] / dt,
        "ev_per_wakeup": fin["count"] / max(1, fin["polls"]),
        "softirq_cpu0": 100.0 * (sq1 - sq0) / CLK_TCK / dt,
    }


def main():
    if os.geteuid() != 0:
        sys.exit("需要 root 运行")
    os.sched_setaffinity(0, {1})
    sh(["sysctl", "-w", "kernel.bpf_stats_enabled=1"])
    base = 0
    try:
        base = run_producer(4000000)
        print("生产端基线（无 agent）: %.0f ops/s" % base)
        print()
        print("%-22s | %10s | %8s | %9s | %9s | %6s | %7s | %8s | %9s" % (
            "配置", "生产ops/s", "本体ns", "提交/s", "送达/s",
            "丢包%", "延迟p50µs", "消费CPU%", "唤醒/s"))
        print("-" * 118)
        for sample in (1, 8):
            for flags, name in ((0, "默认唤醒"), (1, "NO_WAKE+5ms节拍")):
                r = measure(flags, sample)
                r["name"] = name
                print("ringbuf %-14s 1/%-2d | %10.0f | %8.0f | %9.0f | %9.0f |"
                      " %6.1f | %7.1f | %8.1f | %9.0f" % (
                          name, sample, r["producer_rate"],
                          r["producer_ns"] or -1, r["submit_rate"],
                          r["recv_rate"], r["drop_pct"], r["lat_p50_us"],
                          r["consumer_cpu"], r["wakeups_s"]))
                print("%-22s | 每唤醒送达 %6.2f 事件  cpu0 irq+softirq %5.1f%%  "
                      "生产端影响 %+6.1f%%" % (
                          "", r["ev_per_wakeup"], r["softirq_cpu0"],
                          (base / r["producer_rate"] - 1) * 100))
    finally:
        sh(["sysctl", "-w", "kernel.bpf_stats_enabled=0"])


if __name__ == "__main__":
    main()
