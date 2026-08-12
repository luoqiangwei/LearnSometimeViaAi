#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_comm.py — eBPF 与用户态双向通信开销测量

方向一（内核 -> 用户态，事件下发）三种机制对照：
  perfbuf : BPF_PERF_OUTPUT + perf_submit（tracker 现用）
  ringbuf : BPF_MAP_TYPE_RINGBUF + ringbuf_output（5.8+，跨 CPU 单环）
  map     : 只在内核聚合计数，不下发（通信零成本基线）

  触发点 sys_enter_getpid（开中断、可被 perf 采样，火焰图无盲区），
  bench_syscall 提供 ~200 万次/s 的 getpid 压力。
  指标：生产者单次耗时(bpf_stats)、送达吞吐、丢包率、端到端延迟
        (bpf_ktime_get_ns 与用户态 CLOCK_MONOTONIC 同一时钟域，直接相减)、
        消费者 CPU、生产端吞吐影响；载荷 32/128/512/1024B 扫描；
        ringbuf 的 NO_WAKEUP 唤醒策略对照。

方向二（用户态 -> 内核，map 操作）：BCC python API 的 lookup/update
  逐次计时（raw bpf() 系统调用成本见 bench/bench_mapop.c）。

用法: sudo python3 bench/bench_comm.py [--quick]
"""
import ctypes
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CLK_TCK = os.sysconf("SC_CLK_TCK")

BPF_SRC = r"""
#include <uapi/linux/ptrace.h>
#include <uapi/linux/bpf.h>

#ifndef MECH
#define MECH 1          /* 1=perfbuf 2=ringbuf 3=map */
#endif
#ifndef PAYLOAD
#define PAYLOAD 32
#endif
#ifndef RB_FLAGS
#define RB_FLAGS 0      /* ringbuf_output 的唤醒标志: 0 / 1(NO_WAKEUP) */
#endif
#ifndef SAMPLE
#define SAMPLE 1        /* 每 N 次触发提交 1 次（降额用于干净延迟测量） */
#endif

struct event_t {
    u64 ts;             /* 提交时刻 bpf_ktime_get_ns（= CLOCK_MONOTONIC） */
    u32 seq;            /* 全局递增序号（丢包检测） */
    u32 cpu;
    char data[PAYLOAD];
};

/* 大载荷事件超出 BPF 512B 栈限，用 per-cpu scratch */
BPF_PERCPU_ARRAY(scratch, struct event_t, 1);

#if MECH == 1
BPF_PERF_OUTPUT(events);
#elif MECH == 2
BPF_RINGBUF_OUTPUT(events, 256);    /* 256 页 × 4KB = 1MB 单环 */
#else
BPF_PERCPU_ARRAY(counter, u64, 1);
#endif
BPF_PERCPU_ARRAY(seqno, u64, 1);
BPF_PERCPU_ARRAY(subm, u64, 1);     /* 实际提交数（SAMPLE>1 时用于丢包计算） */

TRACEPOINT_PROBE(syscalls, sys_enter_getpid)
{
    u32 zero = 0;
    u64 *s = seqno.lookup(&zero);
    if (!s)
        return 0;
    (*s)++;
#if MECH == 3
    u64 *c = counter.lookup(&zero);
    if (c)
        (*c)++;
    return 0;
#else
    if (*s % SAMPLE != 0)
        return 0;
    struct event_t *e = scratch.lookup(&zero);
    if (!e)
        return 0;
    e->ts = bpf_ktime_get_ns();
    e->seq = (u32)*s;
    e->cpu = bpf_get_smp_processor_id();
    u64 *sub = subm.lookup(&zero);
    if (sub)
        (*sub)++;
#if MECH == 1
    events.perf_submit(args, e, sizeof(*e));
#else
    events.ringbuf_output(e, sizeof(*e), RB_FLAGS);
#endif
    return 0;
#endif
}
"""

from bcc import BPF


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


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


def proc_cpu_ticks(pid):
    with open("/proc/%d/stat" % pid) as f:
        data = f.read()
    rest = data[data.rindex(")") + 2:]
    f = rest.split()
    return int(f[11]) + int(f[12])


def run_producer(rounds=4000000, cpu=0):
    """跑 getpid 压力，返回 ops/s"""
    cmd = ["taskset", "-c", str(cpu), os.path.join(ROOT, "bench/bench_syscall"),
           str(rounds)]
    p = sh(cmd)
    m = re.search(r"RESULT \S+ ([\d.]+)", p.stdout)
    if not m:
        raise RuntimeError("bench_syscall 无输出: %s" % p.stderr[:200])
    return float(m.group(1))


class Consumer:
    """事件消费者：统计收到数、延迟分布、自身 CPU"""
    HDR = 16          # event_t 头部（ts+seq+cpu）

    def __init__(self, b, mech, payload, rb_flags=0):
        self.b = b
        self.mech = mech
        self.rb_flags = rb_flags
        self.count = 0
        self.lats = []            # 蓄水池抽样（最多 200k 条）
        self.lat_sum = 0
        self.lat_max = 0
        self.lost_cb = 0
        self.stop = threading.Event()
        fmt = "QI I" if False else None
        self.payload = payload

    def _parse(self, data):
        # BCC 回调的 data 是地址(int)，从该地址读 event_t 头 8 字节（ts）
        return int.from_bytes(ctypes.string_at(data, 8), "little")

    def _on_event(self, cpu, data, size):
        ts = self._parse(data)
        now = time.monotonic_ns()
        lat = now - ts
        self.count += 1
        self.lat_sum += lat
        if lat > self.lat_max:
            self.lat_max = lat
        if len(self.lats) < 200000:
            self.lats.append(lat)

    def _on_lost(self, lost):
        self.lost_cb += lost

    def start(self):
        if self.mech == 1:
            self.b["events"].open_perf_buffer(self._on_event,
                                              lost_cb=self._on_lost)
            poll = lambda t: self.b.perf_buffer_poll(timeout=t)
        else:
            self.b["events"].open_ring_buffer(self._on_event)
            if self.rb_flags == 1:
                # NO_WAKEUP：内核不唤醒，必须自己定时 consume（5ms 节拍）
                def poll(_t):
                    self.b.ring_buffer_consume()
                    time.sleep(0.005)
            else:
                poll = lambda t: self.b.ring_buffer_poll(timeout=t)

        def loop():
            while not self.stop.is_set():
                poll(50)
        self.th = threading.Thread(target=loop, daemon=True)
        self.th.start()

    def finish(self):
        self.stop.set()
        self.th.join(timeout=2)
        lats = sorted(self.lats)
        n = len(lats)
        return {
            "count": self.count,
            "lat_avg": self.lat_sum / max(1, self.count),
            "lat_p50": lats[n // 2] if n else 0,
            "lat_p99": lats[int(n * 0.99)] if n else 0,
            "lat_max": self.lat_max,
            "lost_cb": self.lost_cb,
        }


def map_total(b, name):
    """percpu 计数器合计（values() 每元素是各 CPU 值的数组）"""
    t = 0
    for v in b[name].values():
        for x in v:
            t += x
    return t


def measure_one(mech, payload, rb_flags=0, sample=1, rounds=4000000):
    """测一种配置，返回指标 dict"""
    cflags = ["-DMECH=%d" % mech, "-DPAYLOAD=%d" % payload,
              "-DRB_FLAGS=%d" % rb_flags, "-DSAMPLE=%d" % sample]
    b = BPF(text=BPF_SRC, cflags=cflags)
    consumer = None
    if mech != 3:
        consumer = Consumer(b, mech, payload, rb_flags)
        consumer.start()
    time.sleep(0.5)

    s0 = prog_snapshot()
    p0 = map_total(b, "seqno")
    u0 = map_total(b, "subm") if mech != 3 else 0
    c0 = proc_cpu_ticks(os.getpid())
    t0 = time.monotonic()
    rate = run_producer(rounds)
    dt = time.monotonic() - t0
    time.sleep(1.0)      # 等消费者排空
    s1 = prog_snapshot()
    p1 = map_total(b, "seqno")
    u1 = map_total(b, "subm") if mech != 3 else 0
    c1 = proc_cpu_ticks(os.getpid())

    prog_ns = None
    name = "tracepoint__syscalls__sys_enter_getpid"
    if name in s1 and s1[name][1] > s0.get(name, (0, 0))[1]:
        prog_ns = ((s1[name][0] - s0.get(name, (0, 0))[0]) /
                   (s1[name][1] - s0.get(name, (0, 0))[1]))

    produced = p1 - p0
    submitted = u1 - u0
    out = {
        "mech": mech, "payload": payload, "rb_flags": rb_flags,
        "sample": sample,
        "producer_rate": rate,
        "producer_ns": prog_ns,
        "produced": produced,
        "submitted": submitted,
        "submit_rate": submitted / dt,
        "consumer_cpu": 100.0 * (c1 - c0) / CLK_TCK / dt,
    }
    if consumer:
        fin = consumer.finish()
        out.update(fin)
        out["recv_rate"] = fin["count"] / dt
        out["drop_pct"] = (100.0 * (submitted - fin["count"]) / submitted
                           if submitted else 0)
    del b
    return out


def print_row(r):
    name = {1: "perfbuf", 2: "ringbuf", 3: "map基线"}[r["mech"]]
    if r.get("rb_flags"):
        name += "(NO_WAKE)"
    if r.get("sample", 1) > 1:
        name += "(1/%d采样)" % r["sample"]
    line = ("%-18s 载荷%-5d 生产%7.0f/s 本体%5.0fns" %
            (name, r["payload"], r["producer_rate"],
             r["producer_ns"] or -1))
    if r["mech"] != 3:
        line += (" | 提交%9.0f/s 送达%9.0f/s 丢包%5.1f%% 延迟p50 %6.1fµs "
                 "p99 %7.1fµs max %8.1fµs 消费者CPU %5.1f%%" %
                 (r["submit_rate"], r["recv_rate"], r["drop_pct"],
                  r["lat_p50"] / 1e3, r["lat_p99"] / 1e3,
                  r["lat_max"] / 1e3, r["consumer_cpu"]))
    print(line, flush=True)
    return line


def map_api_cost():
    """方向二：BCC python map API 逐次成本"""
    print("=" * 78)
    print("方向二：用户态 -> 内核 map 操作成本（BCC python API；raw bpf() 见 bench_mapop.c）")
    print("=" * 78)
    b = BPF(text="BPF_HASH(m, u32, u64, 1024);")
    m = b["m"]
    key = ctypes.c_uint32(1)
    val = ctypes.c_uint64(42)
    N = 20000
    for _ in range(2000):           # warmup
        m[key] = val
    t0 = time.monotonic()
    for _ in range(N):
        m[key] = val
    up_ns = (time.monotonic() - t0) / N * 1e9
    t0 = time.monotonic()
    for _ in range(N):
        _ = m[key]
    lk_ns = (time.monotonic() - t0) / N * 1e9
    print("  BCC python update: %6.0f ns/次   lookup: %6.0f ns/次"
          % (up_ns, lk_ns))


def main():
    if os.geteuid() != 0:
        sys.exit("需要 root 运行")
    quick = "--quick" in sys.argv
    os.sched_setaffinity(0, {1})      # 消费者/agent 绑核 1，生产 bench 绑核 0
    sh(["sysctl", "-w", "kernel.bpf_stats_enabled=1"])
    rounds = 2000000 if quick else 4000000

    print("=" * 78)
    print("方向一：内核 -> 用户态事件下发（生产=核0 getpid 压力，消费=核1）")
    print("=" * 78)
    # 生产端基线（无任何 agent）
    base_rate = statistics.median([run_producer(rounds),
                                   run_producer(rounds)])
    print("生产端基线（无 agent）: %.0f ops/s" % base_rate)

    payloads = [32, 128, 512, 1024] if not quick else [32, 512]
    results = []
    try:
        # map 基线（只测一档）
        r = measure_one(3, 32, rounds=rounds)
        r["overhead_pct"] = (base_rate / r["producer_rate"] - 1) * 100
        print_row(r); results.append(r)
        for mech in (1, 2):
            for pl in payloads:
                r = measure_one(mech, pl, rounds=rounds)
                r["overhead_pct"] = (base_rate / r["producer_rate"] - 1) * 100
                print_row(r); results.append(r)
                print("                     └─ 生产端吞吐影响 %+5.1f%%"
                      % r["overhead_pct"], flush=True)
        # 降额采样（可持续速率下的干净延迟/丢包）
        for mech in (1, 2):
            r = measure_one(mech, 32, sample=8, rounds=rounds)
            r["overhead_pct"] = (base_rate / r["producer_rate"] - 1) * 100
            print_row(r); results.append(r)
        # ringbuf 唤醒策略
        if not quick:
            r = measure_one(2, 32, rb_flags=1, rounds=rounds)
            r["overhead_pct"] = (base_rate / r["producer_rate"] - 1) * 100
            print_row(r); results.append(r)
    finally:
        sh(["sysctl", "-w", "kernel.bpf_stats_enabled=0"])

    print()
    map_api_cost()
    with open("/tmp/bench_comm_results.json", "w") as f:
        json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
