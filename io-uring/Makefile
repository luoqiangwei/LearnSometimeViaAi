CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -pthread

SRCS := src/iobench.c src/common.c src/engine_sync.c src/engine_mmap.c src/engine_uring.c src/engine_aio.c src/uring_min.c

iobench: $(SRCS) src/common.h src/uring_min.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

netbench: src/netbench.c src/net_engines.c src/uring_min.c src/net_engines.h src/uring_min.h
	$(CC) $(CFLAGS) -o $@ src/netbench.c src/net_engines.c src/uring_min.c

# 能力探针: 探测 io_uring 是否可用(EPERM/ENOSYS 时业务应回退传统路径)
uring_probe: tools/uring_probe.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f iobench netbench uring_probe

.PHONY: clean
