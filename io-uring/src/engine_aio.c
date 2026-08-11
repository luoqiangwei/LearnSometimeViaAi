#define _GNU_SOURCE
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <aio.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <linux/aio_abi.h>

/* ================= Linux libaio(内核 AIO, io_submit 家族) =================
 * 裸 syscall 实现, 不依赖 libaio。
 * direct=1: O_DIRECT —— libaio 唯一能真异步的路径;
 * direct=0: buffered —— 在内核里退化为同步执行(io_submit 当场干完活)。
 */

static int sys_io_setup(unsigned nr, aio_context_t *ctx)
{
    return syscall(__NR_io_setup, nr, ctx);
}

static int sys_io_submit(aio_context_t ctx, long nr, struct iocb **iocbpp)
{
    return syscall(__NR_io_submit, ctx, nr, iocbpp);
}

static int sys_io_getevents(aio_context_t ctx, long min, long max,
                            struct io_event *events, struct timespec *timeout)
{
    return syscall(__NR_io_getevents, ctx, min, max, events, timeout);
}

int run_libaio(const config_t *cfg, result_t *res, int direct)
{
    int depth = cfg->depth;
    int fd = open(cfg->file, O_RDONLY | (direct ? O_DIRECT : 0));
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return -1; }

    aio_context_t ctx = 0;
    if (sys_io_setup(depth * 2, &ctx) != 0) {
        fprintf(stderr, "io_setup: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct iocb *cbs = calloc(depth, sizeof(struct iocb));
    struct iocb **ptrs = malloc(sizeof(struct iocb *) * depth);
    struct io_event *evs = malloc(sizeof(struct io_event) * depth);
    char **buf = malloc(sizeof(char *) * depth);
    for (int i = 0; i < depth; i++)
        buf[i] = xaligned(4096, cfg->bs);

    int sequential = strcmp(cfg->pattern, "seqread") == 0;
    uint64_t blocks = cfg->file_size / cfg->bs;
    uint64_t *offs = gen_offsets(cfg->ops, blocks, cfg->bs, sequential);
    uint64_t *lat = malloc(sizeof(uint64_t) * cfg->ops);
    uint64_t *slot_t0 = malloc(sizeof(uint64_t) * cfg->ops);
    long syscalls = 0;

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t0 = now_ns();

    long submitted = 0, done = 0;
    unsigned inflight = 0;
    while (done < cfg->ops) {
        /* 填充并提交一批 */
        unsigned ns = 0;
        while (submitted + (long)ns < cfg->ops && inflight + ns < (unsigned)depth) {
            long op = submitted + ns;
            unsigned slot = op % depth;
            slot_t0[op] = now_ns();
            struct iocb *cb = &cbs[slot];
            memset(cb, 0, sizeof(*cb));
            cb->aio_data = (uint64_t)op;
            cb->aio_lio_opcode = IOCB_CMD_PREAD;
            cb->aio_fildes = fd;
            cb->aio_buf = (uint64_t)buf[slot];
            cb->aio_nbytes = cfg->bs;
            cb->aio_offset = offs[op];
            ptrs[ns] = cb;
            ns++;
        }
        while (ns > 0) {  /* io_submit 可能部分提交, 循环到全部收下 */
            int n = sys_io_submit(ctx, ns, ptrs);
            syscalls++;
            if (n < 0) { fprintf(stderr, "io_submit: %s\n", strerror(-n)); goto out; }
            if (n == 0) { fprintf(stderr, "io_submit 收下 0 个请求\n"); goto out; }
            memmove(ptrs, ptrs + n, (ns - n) * sizeof(*ptrs));  /* 内核从前向后消费 */
            submitted += n;
            inflight += n;
            ns -= n;
        }

        /* 收割: 与 uring 引擎相同的批量等待策略 */
        int wait;
        if (submitted >= cfg->ops && inflight < (unsigned)depth)
            wait = 1;
        else if (inflight >= (unsigned)depth)
            wait = depth / 2 > 0 ? depth / 2 : 1;
        else
            wait = 0;
        if (wait > (int)inflight)
            wait = 1;
        int n = sys_io_getevents(ctx, wait, depth, evs, NULL);
        syscalls++;
        if (n < 0) { fprintf(stderr, "io_getevents: %s\n", strerror(-n)); goto out; }
        for (int i = 0; i < n; i++) {
            long op = (long)evs[i].data;
            if ((long)evs[i].res != (long)cfg->bs)
                fprintf(stderr, "aio res=%ld\n", (long)evs[i].res);
            lat[op] = now_ns() - slot_t0[op];
        }
        inflight -= n;
        done += n;
    }
out:
    ;
    uint64_t wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &ru1);

    static char label[64];
    snprintf(label, sizeof(label), "aio_%s(d%d)", direct ? "direct" : "buffered", depth);
    res->name = label;
    res->bs = cfg->bs;
    res->depth = depth;
    res->syscalls = syscalls;
    result_finish(res, lat, cfg->ops, cfg->bs, wall, &ru0, &ru1);

    free(offs); free(lat); free(slot_t0);
    for (int i = 0; i < depth; i++) free(buf[i]);
    free(buf); free(cbs); free(ptrs); free(evs);
    syscall(__NR_io_destroy, ctx);
    close(fd);
    return 0;
}

/* ================= POSIX AIO(glibc 用户态模拟: 内部就是线程池跑 pread) ================= */

int run_paio(const config_t *cfg, result_t *res)
{
    int depth = cfg->depth;
    int fd = open(cfg->file, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return -1; }

    struct aiocb *cbs = calloc(depth, sizeof(struct aiocb));
    const struct aiocb **susplist = malloc(sizeof(struct aiocb *) * depth);
    char **buf = malloc(sizeof(char *) * depth);
    for (int i = 0; i < depth; i++)
        buf[i] = xaligned(4096, cfg->bs);

    int sequential = strcmp(cfg->pattern, "seqread") == 0;
    uint64_t blocks = cfg->file_size / cfg->bs;
    uint64_t *offs = gen_offsets(cfg->ops, blocks, cfg->bs, sequential);
    uint64_t *lat = malloc(sizeof(uint64_t) * cfg->ops);
    uint64_t *slot_t0 = malloc(sizeof(uint64_t) * cfg->ops);
    long *slot_op = malloc(sizeof(long) * depth);   /* 槽位 → op 序号 */
    for (int i = 0; i < depth; i++) slot_op[i] = -1;

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t0 = now_ns();

    long submitted = 0, done = 0;
    while (done < cfg->ops) {
        /* 填满空闲槽位 */
        while (submitted < cfg->ops) {
            int slot = -1;
            for (int i = 0; i < depth; i++)
                if (slot_op[i] < 0) { slot = i; break; }
            if (slot < 0)
                break;
            long op = submitted++;
            slot_t0[op] = now_ns();
            slot_op[slot] = op;
            struct aiocb *cb = &cbs[slot];
            memset(cb, 0, sizeof(*cb));
            cb->aio_fildes = fd;
            cb->aio_buf = buf[slot];
            cb->aio_nbytes = cfg->bs;
            cb->aio_offset = offs[op];
            cb->aio_sigevent.sigev_notify = SIGEV_NONE;
            if (aio_read(cb) != 0) {
                fprintf(stderr, "aio_read: %s\n", strerror(errno));
                goto out;
            }
        }
        /* 等待并收割 */
        int nsus = 0;
        for (int i = 0; i < depth; i++)
            if (slot_op[i] >= 0) susplist[nsus++] = &cbs[i];
        if (nsus == 0)
            continue;
        aio_suspend(susplist, nsus, NULL);   /* 至少一个完成即返回 */
        for (int i = 0; i < depth; i++) {
            if (slot_op[i] < 0)
                continue;
            int err = aio_error(&cbs[i]);
            if (err == EINPROGRESS)
                continue;
            ssize_t n = aio_return(&cbs[i]);
            if (err != 0 || n != (ssize_t)cfg->bs)
                fprintf(stderr, "paio err=%d n=%zd\n", err, n);
            lat[slot_op[i]] = now_ns() - slot_t0[slot_op[i]];
            slot_op[i] = -1;
            done++;
        }
    }
out:
    ;
    uint64_t wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &ru1);

    static char label[64];
    snprintf(label, sizeof(label), "posix_aio(d%d)", depth);
    res->name = label;
    res->bs = cfg->bs;
    res->depth = depth;
    res->syscalls = -1;   /* 内部 syscall 在 glibc 线程里, 无法精确计数 */
    result_finish(res, lat, cfg->ops, cfg->bs, wall, &ru0, &ru1);

    free(offs); free(lat); free(slot_t0); free(slot_op);
    for (int i = 0; i < depth; i++) free(buf[i]);
    free(buf); free(cbs); free((void *)susplist);
    close(fd);
    return 0;
}

/* ================= preadv2 + RWF_NOWAIT: "穷人的异步" =================
 * 命中页缓存则直接返回, 否则 EAGAIN —— 由调用方自己回退阻塞路径。
 * 统计 EAGAIN 回退率, 证明它不是通用异步方案。
 */
int run_preadv2_nowait(const config_t *cfg, result_t *res)
{
    int fd = open(cfg->file, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return -1; }

    uint64_t blocks = cfg->file_size / cfg->bs;
    uint64_t *offs = gen_offsets(cfg->ops, blocks, cfg->bs,
                                 strcmp(cfg->pattern, "seqread") == 0);
    uint64_t *lat = malloc(sizeof(uint64_t) * cfg->ops);
    char *buf = xaligned(4096, cfg->bs);
    struct iovec iov = { .iov_base = buf, .iov_len = cfg->bs };
    long eagain = 0;

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t0 = now_ns();
    for (long i = 0; i < cfg->ops; i++) {
        uint64_t s = now_ns();
        ssize_t n = preadv2(fd, &iov, 1, offs[i], RWF_NOWAIT);
        if (n < 0 && errno == EAGAIN) {
            eagain++;
            n = pread(fd, buf, cfg->bs, offs[i]);   /* 回退阻塞路径 */
        }
        lat[i] = now_ns() - s;
        if (n != (ssize_t)cfg->bs) fprintf(stderr, "short read\n");
    }
    uint64_t wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &ru1);

    fprintf(stderr, "RWF_NOWAIT EAGAIN 回退率: %.1f%% (%ld/%ld)\n",
            100.0 * eagain / cfg->ops, eagain, cfg->ops);

    res->name = "preadv2_nowait";
    res->bs = cfg->bs;
    res->depth = 1;
    res->syscalls = cfg->ops + eagain;   /* EAGAIN 是白烧的 syscall */
    result_finish(res, lat, cfg->ops, cfg->bs, wall, &ru0, &ru1);

    free(offs); free(lat); free(buf); close(fd);
    return 0;
}
