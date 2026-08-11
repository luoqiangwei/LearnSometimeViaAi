#define _GNU_SOURCE
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* xorshift64*, 快且对本场景足够均匀 */
uint64_t rand64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

void *xaligned(size_t alignment, size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, alignment, size) != 0) {
        fprintf(stderr, "posix_memalign(%zu): %s\n", size, strerror(errno));
        exit(1);
    }
    return p;
}

/* 创建并填充测试文件(重复 pattern, 速度优先) */
int prepare_file(const char *path, uint64_t size)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    const size_t chunk = 1 << 20;
    char *buf = xaligned(4096, chunk);
    for (size_t i = 0; i < chunk; i++)
        buf[i] = (char)(i * 131 + (i >> 13));
    uint64_t left = size;
    while (left > 0) {
        size_t n = left < chunk ? (size_t)left : chunk;
        if (write(fd, buf, n) != (ssize_t)n) {
            fprintf(stderr, "write %s: %s\n", path, strerror(errno));
            free(buf);
            close(fd);
            return -1;
        }
        left -= n;
    }
    fsync(fd);
    free(buf);
    close(fd);
    return 0;
}

void drop_caches(void)
{
    sync();
    int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "warn: 无法 drop_caches(需 root): %s\n", strerror(errno));
        return;
    }
    if (write(fd, "3", 1) != 1)
        fprintf(stderr, "warn: drop_caches 写入失败\n");
    close(fd);
}

uint64_t *gen_offsets(long nops, uint64_t blocks, size_t bs, int sequential)
{
    uint64_t *offs = malloc(sizeof(uint64_t) * nops);
    if (!offs) {
        fprintf(stderr, "malloc offsets failed\n");
        exit(1);
    }
    uint64_t st = 0x9E3779B97F4A7C15ull;
    for (long i = 0; i < nops; i++) {
        uint64_t blk = sequential ? ((uint64_t)i % blocks) : (rand64(&st) % blocks);
        offs[i] = blk * bs;
    }
    return offs;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static double ru_us(const struct rusage *ru)
{
    return (ru->ru_utime.tv_sec + ru->ru_stime.tv_sec) * 1e6 +
           (ru->ru_utime.tv_usec + ru->ru_stime.tv_usec);
}

void result_finish(result_t *r, uint64_t *lat, long nops, size_t bs,
                   uint64_t wall_ns, const struct rusage *ru0, const struct rusage *ru1)
{
    r->ops = nops;
    r->wall_s = wall_ns / 1e9;
    r->iops = nops / r->wall_s;
    r->bw_mibs = (double)nops * bs / (1024.0 * 1024.0) / r->wall_s;
    r->cpu_us_per_kop = (ru_us(ru1) - ru_us(ru0)) / nops * 1000.0;
    r->pagefaults = (ru1->ru_minflt - ru0->ru_minflt) + (ru1->ru_majflt - ru0->ru_majflt);

    if (lat && nops > 0) {
        qsort(lat, nops, sizeof(uint64_t), cmp_u64);
        double sum = 0;
        for (long i = 0; i < nops; i++)
            sum += lat[i];
        r->lat_avg_us = sum / nops / 1000.0;
        r->lat_p50_us = lat[nops / 2] / 1000.0;
        r->lat_p99_us = lat[(long)(nops * 0.99)] / 1000.0;
    }
    if (r->syscalls == 0)
        r->syscalls = -1;
}

void result_print(const result_t *r)
{
    printf("%-18s ops=%-7ld bs=%-7zu depth=%-3d | IOPS=%-9.0f BW=%-8.1fMiB/s | "
           "avg=%-8.1fus p50=%-8.1fus p99=%-9.1fus | CPU=%-7.1fus/kop | sys=%-8ld pf=%ld\n",
           r->name, r->ops, r->bs, r->depth, r->iops, r->bw_mibs,
           r->lat_avg_us, r->lat_p50_us, r->lat_p99_us,
           r->cpu_us_per_kop, r->syscalls, r->pagefaults);
    /* 机器可读行, run_bench.sh 用 grep '^CSV,' 收集 */
    printf("CSV,%s,%ld,%zu,%d,%.0f,%.1f,%.1f,%.1f,%.1f,%.1f,%ld,%ld\n",
           r->name, r->ops, r->bs, r->depth, r->iops, r->bw_mibs,
           r->lat_avg_us, r->lat_p50_us, r->lat_p99_us,
           r->cpu_us_per_kop, r->syscalls, r->pagefaults);
}
