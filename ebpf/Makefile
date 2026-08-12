# eBPF 进程监控与内存泄漏追踪系统
# 编译 demo 程序与性能基准程序

CC      ?= gcc
CFLAGS  ?= -g -O0 -fno-omit-frame-pointer -Wall -Wextra
LDFLAGS  = -lpthread

DEMOS   = demos/leak_demo demos/lock_demo
BENCH   = bench/bench_fork bench/bench_ctxsw bench/bench_futex \
          bench/bench_fault bench/bench_syscall bench/bench_mapop

all: $(DEMOS) $(BENCH)

demos/leak_demo: demos/leak_demo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

demos/lock_demo: demos/lock_demo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bench/bench_%: bench/bench_%.c
	$(CC) $(CFLAGS) -O2 -o $@ $< $(LDFLAGS)

clean:
	rm -f $(DEMOS) $(BENCH)

.PHONY: all clean
