/*
 * bench_mapop.c — 用户态 -> 内核方向：bpf() 系统调用 map 操作成本
 *
 * 不依赖 libbpf/BCC，直接 raw bpf(2)：
 *   单发: BPF_MAP_UPDATE_ELEM / BPF_MAP_LOOKUP_ELEM 逐次耗时
 *   批量: BPF_MAP_UPDATE_BATCH / BPF_MAP_LOOKUP_BATCH（batch=64）摊薄耗时
 * 用法: bench_mapop [循环次数(默认 200000)]
 * 输出: RESULT <op>_ns_per_op <值>
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/bpf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef BPF_MAP_UPDATE_BATCH
#define BPF_MAP_UPDATE_BATCH 26
#endif
#ifndef BPF_MAP_LOOKUP_BATCH
#define BPF_MAP_LOOKUP_BATCH 24
#endif
#ifndef BPF_MAP_DELETE_BATCH
#define BPF_MAP_DELETE_BATCH 25
#endif

static int map_fd;

static long bpf(int cmd, union bpf_attr *attr)
{
    return syscall(SYS_bpf, cmd, attr, sizeof(*attr));
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int map_create(void)
{
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_type = BPF_MAP_TYPE_HASH;
    attr.key_size = sizeof(uint32_t);
    attr.value_size = sizeof(uint64_t);
    attr.max_entries = 4096;
    return (int)bpf(BPF_MAP_CREATE, &attr);
}

static int map_update(uint32_t *key, uint64_t *val)
{
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_fd = map_fd;
    attr.key = (uint64_t)key;
    attr.value = (uint64_t)val;
    attr.flags = BPF_ANY;
    return (int)bpf(BPF_MAP_UPDATE_ELEM, &attr);
}

static int map_lookup(uint32_t *key, uint64_t *val)
{
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.map_fd = map_fd;
    attr.key = (uint64_t)key;
    attr.value = (uint64_t)val;
    return (int)bpf(BPF_MAP_LOOKUP_ELEM, &attr);
}

/* 批量接口：key 和 value 都是连续数组指针 */
static int map_update_batch(uint32_t *keys, uint64_t *vals, uint32_t *count)
{
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.batch.map_fd = map_fd;
    attr.batch.keys = (uint64_t)keys;
    attr.batch.values = (uint64_t)vals;
    attr.batch.count = *count;
    attr.batch.elem_flags = BPF_ANY;
    int ret = (int)bpf(BPF_MAP_UPDATE_BATCH, &attr);
    *count = attr.batch.count;
    return ret;
}

static int map_lookup_batch(uint32_t *keys, uint64_t *vals, uint32_t *count)
{
    union bpf_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.batch.map_fd = map_fd;
    attr.batch.keys = (uint64_t)keys;
    attr.batch.values = (uint64_t)vals;
    attr.batch.count = *count;
    int ret = (int)bpf(BPF_MAP_LOOKUP_BATCH, &attr);
    *count = attr.batch.count;
    return ret;
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 200000;
    const int BATCH = 64;

    map_fd = map_create();
    if (map_fd < 0) {
        perror("BPF_MAP_CREATE");
        return 1;
    }

    uint32_t k = 7;
    uint64_t v = 42, out = 0;

    /* warmup + 填一个 key 保证 lookup 命中 */
    for (int i = 0; i < 1000; i++)
        map_update(&k, &v);

    double t0 = now_sec();
    for (long i = 0; i < n; i++)
        map_update(&k, &v);
    double dt = now_sec() - t0;
    printf("RESULT update_ns_per_op %.1f\n", dt / n * 1e9);

    t0 = now_sec();
    for (long i = 0; i < n; i++)
        map_lookup(&k, &out);
    dt = now_sec() - t0;
    printf("RESULT lookup_ns_per_op %.1f\n", dt / n * 1e9);

    /* 批量：batch=64，循环 n/64 轮 */
    uint32_t keys[BATCH];
    uint64_t vals[BATCH];
    for (int i = 0; i < BATCH; i++) {
        keys[i] = 100 + i;
        vals[i] = i;
    }
    long iters = n / BATCH;

    t0 = now_sec();
    for (long i = 0; i < iters; i++) {
        uint32_t cnt = BATCH;
        map_update_batch(keys, vals, &cnt);
    }
    dt = now_sec() - t0;
    printf("RESULT update_batch64_ns_per_op %.1f\n", dt / (iters * BATCH) * 1e9);

    t0 = now_sec();
    for (long i = 0; i < iters; i++) {
        uint32_t cnt = BATCH;
        map_lookup_batch(keys, vals, &cnt);
    }
    dt = now_sec() - t0;
    printf("RESULT lookup_batch64_ns_per_op %.1f\n", dt / (iters * BATCH) * 1e9);

    close(map_fd);
    return 0;
}
