#define _GNU_SOURCE
#include "uring_min.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

static int sys_io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return syscall(__NR_io_uring_setup, entries, p);
}

static int sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags)
{
    return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, NULL, 0);
}

int uring_register(int fd, unsigned opcode, const void *arg, unsigned nr)
{
    return syscall(__NR_io_uring_register, fd, opcode, arg, nr);
}

static unsigned pow2ceil(unsigned v)
{
    unsigned p = 1;
    while (p < v) p <<= 1;
    return p;
}

int uring_init(uring_t *r, unsigned entries, int sqpoll)
{
    memset(r, 0, sizeof(*r));
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    if (sqpoll) {
        p.flags = IORING_SETUP_SQPOLL;
        p.sq_thread_idle = 2000;
    }
    r->ring_fd = sys_io_uring_setup(pow2ceil(entries), &p);
    if (r->ring_fd < 0)
        return -errno;
    r->sqpoll = sqpoll;

    size_t sq_ring_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cq_ring_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        if (cq_ring_sz > sq_ring_sz)
            sq_ring_sz = cq_ring_sz;
        cq_ring_sz = sq_ring_sz;
    }

    void *sq = mmap(NULL, sq_ring_sz, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE, r->ring_fd, IORING_OFF_SQ_RING);
    void *cq;
    if (p.features & IORING_FEAT_SINGLE_MMAP) {
        cq = sq;
    } else {
        cq = mmap(NULL, cq_ring_sz, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_POPULATE, r->ring_fd, IORING_OFF_CQ_RING);
    }
    r->sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe),
                   PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->ring_fd, IORING_OFF_SQES);
    if (sq == MAP_FAILED || cq == MAP_FAILED || r->sqes == MAP_FAILED) {
        fprintf(stderr, "uring mmap: %s\n", strerror(errno));
        close(r->ring_fd);
        return -ENOMEM;
    }

    r->sq_head = sq + p.sq_off.head;
    r->sq_tail = sq + p.sq_off.tail;
    r->sq_ring_mask = sq + p.sq_off.ring_mask;
    r->sq_ring_entries = sq + p.sq_off.ring_entries;
    r->sq_flags = sq + p.sq_off.flags;
    r->sq_array = sq + p.sq_off.array;
    r->cq_head = cq + p.cq_off.head;
    r->cq_tail = cq + p.cq_off.tail;
    r->cq_ring_mask = cq + p.cq_off.ring_mask;
    r->cq_ring_entries = cq + p.cq_off.ring_entries;
    r->cqes = cq + p.cq_off.cqes;
    return 0;
}

void uring_exit(uring_t *r)
{
    close(r->ring_fd);
}

struct io_uring_sqe *uring_get_sqe(uring_t *r)
{
    unsigned tail = __atomic_load_n(r->sq_tail, __ATOMIC_RELAXED);
    unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    if (tail - head >= *r->sq_ring_entries)
        return NULL;
    unsigned idx = tail & *r->sq_ring_mask;
    memset(&r->sqes[idx], 0, sizeof(struct io_uring_sqe));
    r->sq_array[idx] = idx;
    __atomic_store_n(r->sq_tail, tail + 1, __ATOMIC_RELEASE);
    return &r->sqes[idx];
}

int uring_flush(uring_t *r, unsigned to_submit, unsigned minc)
{
    unsigned flags = minc ? IORING_ENTER_GETEVENTS : 0;
    if (r->sqpoll) {
        /* SQPOLL: 内核线程轮询 SQ; 仅在其睡眠时唤醒, 仅等待完成时才 enter */
        if (__atomic_load_n(r->sq_flags, __ATOMIC_ACQUIRE) & IORING_SQ_NEED_WAKEUP) {
            flags |= IORING_ENTER_SQ_WAKEUP;
        } else if (minc == 0) {
            return 0;
        }
        to_submit = 0;
    }
    int ret = sys_io_uring_enter(r->ring_fd, to_submit, minc, flags);
    r->syscalls++;
    return ret;
}

int uring_wait(uring_t *r, unsigned minc)
{
    return uring_flush(r, 0, minc);
}

unsigned uring_next_cqe(uring_t *r, struct io_uring_cqe *out)
{
    unsigned head = __atomic_load_n(r->cq_head, __ATOMIC_ACQUIRE);
    unsigned tail = __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE);
    if (head == tail)
        return 0;
    *out = r->cqes[head & *r->cq_ring_mask];
    __atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
    return 1;
}
