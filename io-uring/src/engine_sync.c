#define _GNU_SOURCE
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

/* ---------------- pread 单线程 ---------------- */
int run_pread(const config_t *cfg, result_t *res)
{
    int fd = open(cfg->file, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return -1; }

    uint64_t blocks = cfg->file_size / cfg->bs;
    uint64_t *offs = gen_offsets(cfg->ops, blocks, cfg->bs,
                                 strcmp(cfg->pattern, "seqread") == 0);
    uint64_t *lat = malloc(sizeof(uint64_t) * cfg->ops);
    char *buf = xaligned(4096, cfg->bs);

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t0 = now_ns();
    for (long i = 0; i < cfg->ops; i++) {
        uint64_t s = now_ns();
        ssize_t n = pread(fd, buf, cfg->bs, offs[i]);
        lat[i] = now_ns() - s;
        if (n != (ssize_t)cfg->bs) { fprintf(stderr, "pread short: %zd\n", n); break; }
    }
    uint64_t wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &ru1);

    res->name = strcmp(cfg->pattern, "seqread") == 0 ? "read" : "pread";
    res->bs = cfg->bs;
    res->depth = 1;
    res->syscalls = cfg->ops;
    result_finish(res, lat, cfg->ops, cfg->bs, wall, &ru0, &ru1);

    free(offs); free(lat); free(buf); close(fd);
    return 0;
}

/* ---------------- pread 多线程(传统"异步"方案: 用线程换并发) ---------------- */
typedef struct {
    int fd;
    const uint64_t *offs;
    uint64_t *lat;
    size_t bs;
    long nops;
    atomic_long *next;
} mt_arg_t;

static void *pread_worker(void *argp)
{
    mt_arg_t *a = argp;
    char *buf = xaligned(4096, a->bs);
    for (;;) {
        long i = atomic_fetch_add_explicit(a->next, 1, memory_order_relaxed);
        if (i >= a->nops)
            break;
        uint64_t s = now_ns();
        ssize_t n = pread(a->fd, buf, a->bs, a->offs[i]);
        a->lat[i] = now_ns() - s;
        if (n != (ssize_t)a->bs)
            fprintf(stderr, "pread short: %zd\n", n);
    }
    free(buf);
    return NULL;
}

int run_pread_mt(const config_t *cfg, result_t *res)
{
    int fd = open(cfg->file, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return -1; }

    uint64_t blocks = cfg->file_size / cfg->bs;
    uint64_t *offs = gen_offsets(cfg->ops, blocks, cfg->bs,
                                 strcmp(cfg->pattern, "seqread") == 0);
    uint64_t *lat = calloc(cfg->ops, sizeof(uint64_t));
    int nthr = cfg->depth;
    pthread_t *tids = malloc(sizeof(pthread_t) * nthr);
    mt_arg_t arg = { .fd = fd, .offs = offs, .lat = lat, .bs = cfg->bs,
                     .nops = cfg->ops, .next = &(atomic_long){0} };

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t0 = now_ns();
    for (int i = 0; i < nthr; i++)
        pthread_create(&tids[i], NULL, pread_worker, &arg);
    for (int i = 0; i < nthr; i++)
        pthread_join(tids[i], NULL);
    uint64_t wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &ru1);

    static char label[64];
    snprintf(label, sizeof(label), "pread_mt(%dthr)", nthr);
    res->name = label;
    res->bs = cfg->bs;
    res->depth = nthr;
    res->syscalls = cfg->ops;
    result_finish(res, lat, cfg->ops, cfg->bs, wall, &ru0, &ru1);

    free(offs); free(lat); free(tids); close(fd);
    return 0;
}

/* ---------------- 日志落盘: write+fdatasync 每条记录 ---------------- */
int run_write_sync(const config_t *cfg, result_t *res)
{
    int fd = open(cfg->file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return -1; }

    uint64_t *lat = malloc(sizeof(uint64_t) * cfg->ops);
    char *buf = xaligned(4096, cfg->bs);
    memset(buf, 'L', cfg->bs);

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t0 = now_ns();
    for (long i = 0; i < cfg->ops; i++) {
        uint64_t s = now_ns();
        if (pwrite(fd, buf, cfg->bs, (uint64_t)i * cfg->bs) != (ssize_t)cfg->bs)
            fprintf(stderr, "pwrite failed\n");
        if (fdatasync(fd) != 0)
            fprintf(stderr, "fdatasync: %s\n", strerror(errno));
        lat[i] = now_ns() - s;
    }
    uint64_t wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &ru1);

    res->name = "write+fdatasync";
    res->bs = cfg->bs;
    res->depth = 1;
    res->syscalls = cfg->ops * 2;
    result_finish(res, lat, cfg->ops, cfg->bs, wall, &ru0, &ru1);

    free(lat); free(buf); close(fd);
    return 0;
}

/* ---------------- 日志落盘: 攒批 write, 每批一次 fdatasync ---------------- */
int run_write_batch(const config_t *cfg, result_t *res)
{
    int fd = open(cfg->file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return -1; }

    int batch = cfg->depth;
    long nbatches = cfg->ops / batch;
    uint64_t *lat = malloc(sizeof(uint64_t) * nbatches);
    char *buf = xaligned(4096, cfg->bs);
    memset(buf, 'L', cfg->bs);

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t0 = now_ns();
    for (long b = 0; b < nbatches; b++) {
        uint64_t s = now_ns();
        for (int i = 0; i < batch; i++) {
            uint64_t off = ((uint64_t)b * batch + i) * cfg->bs;
            if (pwrite(fd, buf, cfg->bs, off) != (ssize_t)cfg->bs)
                fprintf(stderr, "pwrite failed\n");
        }
        if (fdatasync(fd) != 0)
            fprintf(stderr, "fdatasync: %s\n", strerror(errno));
        lat[b] = now_ns() - s;
    }
    uint64_t wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &ru1);

    static char label[64];
    snprintf(label, sizeof(label), "write_batch(%d)", batch);
    res->name = label;
    res->bs = cfg->bs;
    res->depth = batch;
    res->syscalls = cfg->ops + nbatches;
    /* 延迟按批统计, 吞吐与 CPU 按记录数归一 */
    result_finish(res, lat, nbatches, cfg->bs * batch, wall, &ru0, &ru1);
    res->cpu_us_per_kop = res->cpu_us_per_kop * nbatches / cfg->ops;
    res->ops = cfg->ops;
    res->iops = cfg->ops / res->wall_s;      /* records/s */
    res->bw_mibs = (double)cfg->ops * cfg->bs / (1024.0 * 1024.0) / res->wall_s;

    free(lat); free(buf); close(fd);
    return 0;
}
