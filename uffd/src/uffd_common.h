/*
 * uffd_common.h - userfaultfd 演示程序的公共辅助函数
 *
 * userfaultfd 核心概念：
 *   1. 进程通过 userfaultfd() 系统调用创建一个 uffd 文件描述符；
 *   2. 用 UFFDIO_REGISTER 把一段 mmap 出来的虚拟内存"委托"给这个 fd 管理；
 *   3. 此后该区域内发生的缺页（MISSING）/写保护（WP）等异常，
 *      内核不会立即按常规流程处理，而是把事件通过 read(fd) 发给用户态；
 *   4. 用户态处理线程收到事件后，用 UFFDIO_COPY / ZEROPAGE / MOVE /
 *      WRITEPROTECT 等 ioctl 来决定"这一页到底放什么内容、给不给访问"，
 *      然后唤醒被阻塞的缺页线程。
 *
 * 本质上：把"缺页异常处理程序"从内核挪到了用户态，页面内容由自己说了算。
 */
#ifndef UFFD_COMMON_H
#define UFFD_COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/userfaultfd.h>

#define die(msg) do { perror(msg); exit(EXIT_FAILURE); } while (0)

static inline long page_size(void)
{
    return sysconf(_SC_PAGESIZE);
}

/* 创建 userfaultfd 并完成 API 协商（UFFDIO_API 每个 fd 只能调用一次）。
 * 注意：需要内核 >= 4.3。
 * 若 /proc/sys/vm/unprivileged_userfaultfd 为 0（很多发行版的默认值，
 * 出于安全考虑），非特权进程无法创建，需要 root 或 CAP_SYS_PTRACE。
 * features_out 返回内核支持的特性位（可为 NULL）。 */
static inline int uffd_open(uint64_t *features_out)
{
    /* O_NONBLOCK：配合事件循环，read 无事件时返回 EAGAIN 而不是阻塞，
     * 这样处理线程可以随时响应退出标志（阻塞 read 无法被 close 打断）。 */
    int fd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        die("userfaultfd 创建失败（非 root 且 unprivileged_userfaultfd=0 时会被拒绝）");

    struct uffdio_api api = {
        .api = UFFD_API,
        .features = 0,
    };
    if (ioctl(fd, UFFDIO_API, &api) < 0)
        die("UFFDIO_API");
    if (api.api != UFFD_API) {
        fprintf(stderr, "内核 uffd API 版本不匹配\n");
        exit(EXIT_FAILURE);
    }
    if (features_out)
        *features_out = api.features;
    return fd;
}

/* 把 [addr, addr+len) 区域注册到已协商好的 uffd。
 * mode 取值：UFFDIO_REGISTER_MODE_MISSING / UFFDIO_REGISTER_MODE_WP 等。 */
static inline void uffd_register(int uffd, void *addr, size_t len,
                                 uint64_t mode)
{
    struct uffdio_register reg = {
        .range = { .start = (uint64_t)addr, .len = len },
        .mode = mode,
    };
    if (ioctl(uffd, UFFDIO_REGISTER, &reg) < 0)
        die("UFFDIO_REGISTER");
}

/* 分配一块页对齐的匿名内存（尚未被 uffd 托管，也未填充内容）。 */
static inline void *map_anon(size_t len)
{
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        die("mmap");
    return p;
}

/* 读取 /proc/self/statm 中的 RSS（常驻物理内存页数）。 */
static inline long read_rss_pages(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        die("fopen statm");
    long total, rss;
    if (fscanf(f, "%ld %ld", &total, &rss) != 2) {
        fprintf(stderr, "解析 statm 失败\n");
        exit(EXIT_FAILURE);
    }
    fclose(f);
    return rss;
}

static inline uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 把 src 指向的一页内容提供给缺页地址（UFFDIO_COPY）。
 * mode 传 0：缺省行为即拷贝完成后唤醒等待该页的线程；
 * 若传 UFFDIO_COPY_MODE_DONTWAKE 则需之后自行 UFFDIO_WAKE。 */
static inline void uffd_serve_page(int uffd, uint64_t dst, const void *src,
                                   size_t len)
{
    struct uffdio_copy copy = {
        .dst = dst,
        .src = (uint64_t)src,
        .len = len,
        .mode = 0,
    };
    if (ioctl(uffd, UFFDIO_COPY, &copy) < 0)
        die("UFFDIO_COPY");
    if ((size_t)copy.copy != len) {
        fprintf(stderr, "UFFDIO_COPY 只拷贝了 %lld/%zu 字节\n",
                (long long)copy.copy, len);
        exit(EXIT_FAILURE);
    }
}

#endif /* UFFD_COMMON_H */
