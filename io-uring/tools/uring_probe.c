#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>

int main(void) {
    struct io_uring_params p;
    memset(&p, 0, sizeof(p));
    int fd = syscall(__NR_io_uring_setup, 8, &p);
    if (fd < 0) { printf("io_uring_setup failed: %s\n", strerror(errno)); return 1; }
    printf("io_uring_setup ok, features=0x%x\n", p.features);

    size_t sqring = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    size_t cqring = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    void *sq = mmap(NULL, sqring, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, fd, IORING_OFF_SQ_RING);
    void *cq = mmap(NULL, cqring, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, fd, IORING_OFF_CQ_RING);
    struct io_uring_sqe *sqes = mmap(NULL, p.sq_entries * sizeof(struct io_uring_sqe),
                                     PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, fd, IORING_OFF_SQES);
    if (sq == MAP_FAILED || cq == MAP_FAILED || sqes == MAP_FAILED) { printf("mmap failed: %s\n", strerror(errno)); return 1; }

    unsigned *tail = sq + p.sq_off.tail, *mask = sq + p.sq_off.ring_mask;
    unsigned *array = sq + p.sq_off.array;
    unsigned idx = *tail & *mask;
    memset(&sqes[idx], 0, sizeof(struct io_uring_sqe));
    sqes[idx].opcode = IORING_OP_NOP;
    sqes[idx].user_data = 42;
    array[idx] = idx;
    __atomic_store_n(tail, *tail + 1, __ATOMIC_RELEASE);

    int ret = syscall(__NR_io_uring_enter, fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    if (ret < 0) { printf("io_uring_enter failed: %s\n", strerror(errno)); return 1; }

    unsigned *cq_head = cq + p.cq_off.head, *cq_tail = cq + p.cq_off.tail;
    if (__atomic_load_n(cq_head, __ATOMIC_ACQUIRE) != __atomic_load_n(cq_tail, __ATOMIC_ACQUIRE)) {
        struct io_uring_cqe *cqe = (void*)((char*)cq + p.cq_off.cqes) + (*cq_head & *(unsigned*)((char*)cq + p.cq_off.ring_mask));
        printf("NOP completed: res=%d user_data=%llu\n", cqe->res, (unsigned long long)cqe->user_data);
        __atomic_store_n(cq_head, *cq_head + 1, __ATOMIC_RELEASE);
    }
    close(fd);
    printf("io_uring fully functional\n");
    return 0;
}
