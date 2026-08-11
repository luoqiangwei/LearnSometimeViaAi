#ifndef URING_MIN_H
#define URING_MIN_H

#include <linux/io_uring.h>

/*
 * 最小 io_uring 封装(裸 syscall, 不依赖 liburing)。
 * 刻意手写以展示 SQ/CQ/mmap 机制; 生产代码可直接用
 * AOSP 内置的 external/liburing(snapuserd 即静态链接它)。
 */
typedef struct {
    int ring_fd;
    unsigned *sq_head, *sq_tail, *sq_ring_mask, *sq_ring_entries,
             *sq_flags, *sq_array;
    unsigned *cq_head, *cq_tail, *cq_ring_mask, *cq_ring_entries;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    int sqpoll;
    long syscalls;   /* io_uring_enter 调用次数 */
} uring_t;

/* 返回 0 或 -errno; sqpoll!=0 时启用 IORING_SETUP_SQPOLL */
int  uring_init(uring_t *r, unsigned entries, int sqpoll);
void uring_exit(uring_t *r);

/* 取一个空闲 SQE(SQ 满返回 NULL), 已按 ring 序更新 tail */
struct io_uring_sqe *uring_get_sqe(uring_t *r);

/* 把已填充的 SQE 交给内核; minc>0 时阻塞等到至少 minc 个完成(含 SQPOLL 唤醒逻辑) */
int  uring_flush(uring_t *r, unsigned to_submit, unsigned minc);

/* 仅等待至少 minc 个完成 */
int  uring_wait(uring_t *r, unsigned minc);

/* 取出一个 CQE: 有完成返回 1 并写入 *out, 无则返回 0 */
unsigned uring_next_cqe(uring_t *r, struct io_uring_cqe *out);

int  uring_register(int fd, unsigned opcode, const void *arg, unsigned nr);

#endif
