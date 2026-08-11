#define _GNU_SOURCE
#include "common.h"
#include "uring_min.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* ---------------- 读场景: randread / seqread ---------------- */
int run_uring(const config_t *cfg, result_t *res)
{
    int depth = cfg->depth;
    int use_fixed = cfg->fixed_buf;
    int use_sqpoll = cfg->sqpoll;

    uring_t r;
    int ret = uring_init(&r, depth, use_sqpoll);
    if (ret == -EPERM || ret == -EACCES) {
        fprintf(stderr, "SQPOLL 无权限, 回退到普通模式\n");
        use_sqpoll = 0;
        ret = uring_init(&r, depth, 0);
    }
    if (ret != 0) {
        fprintf(stderr, "uring_init: %s\n", strerror(-ret));
        return -1;
    }

    int fd = open(cfg->file, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); uring_exit(&r); return -1; }

    /* 每个 in-flight 请求独占一个槽位缓冲 */
    char **buf = malloc(sizeof(char *) * depth);
    struct iovec *iov = malloc(sizeof(struct iovec) * depth);
    for (int i = 0; i < depth; i++) {
        buf[i] = xaligned(4096, cfg->bs);
        iov[i].iov_base = buf[i];
        iov[i].iov_len = cfg->bs;
    }
    if (use_fixed) {
        if (uring_register(r.ring_fd, IORING_REGISTER_BUFFERS, iov, depth) != 0) {
            fprintf(stderr, "REGISTER_BUFFERS: %s, 回退普通读\n", strerror(errno));
            use_fixed = 0;
        }
    }

    int sequential = strcmp(cfg->pattern, "seqread") == 0;
    uint64_t blocks = cfg->file_size / cfg->bs;
    uint64_t *offs = gen_offsets(cfg->ops, blocks, cfg->bs, sequential);
    uint64_t *lat = malloc(sizeof(uint64_t) * cfg->ops);
    uint64_t *slot_t0 = malloc(sizeof(uint64_t) * cfg->ops); /* 按 op 记录提交时间 */

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t0 = now_ns();

    long submitted = 0, done = 0;
    unsigned inflight = 0;
    while (done < cfg->ops) {
        unsigned filled = 0;
        while (submitted < cfg->ops && inflight < (unsigned)depth) {
            struct io_uring_sqe *sqe = uring_get_sqe(&r);
            if (!sqe)
                break;
            long op = submitted;
            unsigned slot = op % depth;
            slot_t0[op] = now_ns();
            if (use_fixed) {
                sqe->opcode = IORING_OP_READ_FIXED;
                sqe->addr = (uint64_t)buf[slot];
                sqe->buf_index = slot;
            } else {
                sqe->opcode = IORING_OP_READ;
                sqe->addr = (uint64_t)buf[slot];
            }
            sqe->fd = fd;
            sqe->len = cfg->bs;
            sqe->off = offs[op];
            sqe->user_data = (uint64_t)op;
            submitted++;
            inflight++;
            filled++;
        }

        /*
         * 等待策略: 管线已满时批量收割(等到 depth/2 个完成),
         * 一次 enter 既提交新请求又批量等完成, 摊薄 syscall;
         * 尾部(没有更多请求可发)时改为逐个等待, 尽快排空。
         */
        int wait;
        if (submitted >= cfg->ops && inflight < (unsigned)depth)
            wait = 1;
        else if (inflight >= (unsigned)depth)
            wait = depth / 2 > 0 ? depth / 2 : 1;
        else
            wait = 0;
        if (wait > (int)inflight)
            wait = 1;
        if (uring_flush(&r, filled, wait) < 0 && !use_sqpoll) {
            fprintf(stderr, "io_uring_enter: %s\n", strerror(errno));
            break;
        }

        struct io_uring_cqe cqe;
        unsigned reaped = 0;
        while (uring_next_cqe(&r, &cqe)) {
            if (cqe.res < 0)
                fprintf(stderr, "uring cqe error: %s\n", strerror(-cqe.res));
            long op = (long)cqe.user_data;
            lat[op] = now_ns() - slot_t0[op];
            reaped++;
        }
        inflight -= reaped;
        done += reaped;
        if (reaped == 0 && filled == 0 && inflight > 0) {
            /* 队列满且无完成: 显式等待 */
            if (uring_wait(&r, 1) < 0) {
                fprintf(stderr, "uring wait: %s\n", strerror(errno));
                break;
            }
            while (uring_next_cqe(&r, &cqe)) {
                if (cqe.res < 0)
                    fprintf(stderr, "uring cqe error: %s\n", strerror(-cqe.res));
                long op = (long)cqe.user_data;
                lat[op] = now_ns() - slot_t0[op];
                inflight--;
                done++;
            }
        }
    }

    uint64_t wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &ru1);

    static char label[64];
    snprintf(label, sizeof(label), "uring%s%s(d%d)",
             use_fixed ? "_fixed" : "", use_sqpoll ? "_sqpoll" : "", depth);
    res->name = label;
    res->bs = cfg->bs;
    res->depth = depth;
    res->syscalls = r.syscalls;
    result_finish(res, lat, cfg->ops, cfg->bs, wall, &ru0, &ru1);

    free(offs); free(lat); free(slot_t0);
    for (int i = 0; i < depth; i++) free(buf[i]);
    free(buf); free(iov);
    close(fd); uring_exit(&r);
    return 0;
}

/* ---------------- 日志落盘: 一批写 + 一次 DRAIN fsync, 单次 enter ---------------- */
int run_uring_logwrite(const config_t *cfg, result_t *res)
{
    int batch = cfg->depth;
    long nbatches = cfg->ops / batch;

    uring_t r;
    int ret = uring_init(&r, batch + 1, cfg->sqpoll);
    if (ret != 0) {
        fprintf(stderr, "uring_init: %s\n", strerror(-ret));
        return -1;
    }

    int fd = open(cfg->file, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); uring_exit(&r); return -1; }

    char *buf = xaligned(4096, cfg->bs);
    memset(buf, 'L', cfg->bs);
    uint64_t *lat = malloc(sizeof(uint64_t) * nbatches);

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t0 = now_ns();

    for (long b = 0; b < nbatches; b++) {
        uint64_t s = now_ns();
        for (int i = 0; i < batch; i++) {
            struct io_uring_sqe *sqe = uring_get_sqe(&r);
            if (!sqe) { fprintf(stderr, "SQ 满\n"); goto out; }
            sqe->opcode = IORING_OP_WRITE;
            sqe->fd = fd;
            sqe->addr = (uint64_t)buf;
            sqe->len = cfg->bs;
            sqe->off = ((uint64_t)b * batch + i) * cfg->bs;
            sqe->user_data = i;
        }
        /* DRAIN: 保证 fsync 排在本批所有写之后执行 */
        struct io_uring_sqe *sqe = uring_get_sqe(&r);
        if (!sqe) { fprintf(stderr, "SQ 满(fsync)\n"); goto out; }
        sqe->opcode = IORING_OP_FSYNC;
        sqe->fd = fd;
        sqe->fsync_flags = IORING_FSYNC_DATASYNC;
        sqe->flags = IOSQE_IO_DRAIN;
        sqe->user_data = batch;

        if (uring_flush(&r, batch + 1, batch + 1) < 0) {
            fprintf(stderr, "io_uring_enter: %s\n", strerror(errno));
            goto out;
        }
        unsigned done = 0;
        struct io_uring_cqe cqe;
        while (done < (unsigned)(batch + 1)) {
            if (uring_next_cqe(&r, &cqe)) {
                if (cqe.res < 0)
                    fprintf(stderr, "uring cqe error: %s\n", strerror(-cqe.res));
                done++;
            } else if (uring_wait(&r, 1) < 0) {
                fprintf(stderr, "uring wait: %s\n", strerror(errno));
                goto out;
            }
        }
        lat[b] = now_ns() - s;
    }
out:
    ;
    uint64_t wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &ru1);

    static char label[64];
    snprintf(label, sizeof(label), "uring_drain(%d)", batch);
    res->name = label;
    res->bs = cfg->bs;
    res->depth = batch;
    res->syscalls = r.syscalls;
    result_finish(res, lat, nbatches, cfg->bs * batch, wall, &ru0, &ru1);
    res->cpu_us_per_kop = res->cpu_us_per_kop * nbatches / cfg->ops;
    res->ops = cfg->ops;
    res->iops = cfg->ops / res->wall_s;      /* records/s */
    res->bw_mibs = (double)cfg->ops * cfg->bs / (1024.0 * 1024.0) / res->wall_s;

    free(lat); free(buf);
    close(fd); uring_exit(&r);
    return 0;
}
