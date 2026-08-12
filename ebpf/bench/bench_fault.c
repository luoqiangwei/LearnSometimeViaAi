/*
 * bench_fault.c — 用户态页错误吞吐基准
 * 循环 mmap 匿名页并逐页写（触发页错误），随即 munmap。
 * 必须用 raw mmap/munmap：malloc/free 会触发 glibc 动态 mmap 阈值调整，
 * 后续分配复用 arena 而不产生新页错误，指标会失真。
 * 衡量 exceptions:page_fault_user tracepoint + armed 抓栈的开销。
 * 用法: bench_fault <轮数> [块大小MB]
 * 输出: RESULT faults_per_sec <值>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 20;
    size_t mb = argc > 2 ? (size_t)atol(argv[2]) : 16;
    size_t sz = mb * 1024 * 1024;
    long faults = sz / 4096;

    double t0 = now_sec();
    for (long r = 0; r < rounds; r++) {
        char *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            perror("mmap");
            return 1;
        }
        for (size_t i = 0; i < sz; i += 4096)
            p[i] = (char)r;     /* 每页一次写页错误 */
        munmap(p, sz);
    }
    double dt = now_sec() - t0;
    printf("RESULT faults_per_sec %.1f\n", rounds * faults / dt);
    return 0;
}
