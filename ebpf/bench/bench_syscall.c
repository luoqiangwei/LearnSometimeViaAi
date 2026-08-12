/*
 * bench_syscall.c — 普通系统调用吞吐基准（getpid 空转）
 * tracker 没有 hook getpid，此基准作为对照组：开销应接近 0，
 * 用于估计测量噪声与“仅加载 BPF 程序”的间接影响。
 * 用法: bench_syscall <迭代次数>
 * 输出: RESULT syscalls_per_sec <值>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 2000000;
    volatile long sink = 0;
    double t0 = now_sec();
    for (long i = 0; i < n; i++)
        sink += syscall(SYS_getpid);
    double dt = now_sec() - t0;
    (void)sink;
    printf("RESULT syscalls_per_sec %.1f\n", n / dt);
    return 0;
}
