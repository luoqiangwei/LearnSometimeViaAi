#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
flame_agent.py — 火焰图采集用的通信 agent（被 flame_comm.sh 调用）

加载 bench_comm.py 的同款 producer（sys_enter_getpid 触发），
消费者只丢弃事件。打印 "READY" 后空转，等待 SIGTERM。
用法: python3 bench/flame_agent.py <mech:1|2> <payload>
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from bench.bench_comm import BPF_SRC  # noqa: E402
from bcc import BPF                   # noqa: E402


def main():
    mech = int(sys.argv[1])
    payload = int(sys.argv[2]) if len(sys.argv) > 2 else 128
    b = BPF(text=BPF_SRC, cflags=["-DMECH=%d" % mech,
                                  "-DPAYLOAD=%d" % payload])
    if mech == 1:
        b["events"].open_perf_buffer(lambda cpu, data, size: None)
        poll = lambda: b.perf_buffer_poll(timeout=100)
    else:
        b["events"].open_ring_buffer(lambda cpu, data, size: None)
        poll = lambda: b.ring_buffer_poll(timeout=100)
    print("READY pid=%d" % os.getpid(), flush=True)
    while True:
        poll()


if __name__ == "__main__":
    main()
