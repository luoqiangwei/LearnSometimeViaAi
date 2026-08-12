/*
 * bench_futex.c — futex 系统调用吞吐基准（两线程 ping-pong）
 * 每轮交换包含 FUTEX_WAIT + FUTEX_WAKE 各一次。
 * 衡量 sys_enter_futex / sys_exit_futex tracepoint 的开销。
 * 用法: bench_futex <往返轮数>
 * 输出: RESULT futex_ops_per_sec <值>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <time.h>

static _Atomic int ftx;
static long rounds;

static int futex_op(int op, int val)
{
    return syscall(SYS_futex, &ftx, op | FUTEX_PRIVATE_FLAG, val, NULL, NULL, 0);
}

/* 等 ftx 变成 expect 之外的值 */
static void fwait(int val)
{
    while (atomic_load_explicit(&ftx, memory_order_acquire) == val)
        futex_op(FUTEX_WAIT, val);
}

static void fwake_set(int val)
{
    atomic_store_explicit(&ftx, val, memory_order_release);
    futex_op(FUTEX_WAKE, 1);
}

static void *pinger(void *arg)
{
    (void)arg;
    for (long i = 0; i < rounds; i++) {
        fwait(0);       /* 等 ponger 置 1 */
        fwake_set(0);   /* 置回 0 唤醒 ponger */
    }
    /* 结束时确保 ponger 不卡在 wait */
    fwake_set(-1);
    return NULL;
}

static void *ponger(void *arg)
{
    (void)arg;
    for (long i = 0; i < rounds; i++) {
        fwait(1);       /* 等 pinger 置 0 */
        fwake_set(1);   /* 置回 1 唤醒 pinger */
    }
    return NULL;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    rounds = argc > 1 ? atol(argv[1]) : 20000;
    atomic_store(&ftx, 1);          /* 让 pinger 先等 */
    pthread_t t1, t2;
    pthread_create(&t1, NULL, pinger, NULL);
    double t0 = now_sec();
    pthread_create(&t2, NULL, ponger, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    double dt = now_sec() - t0;
    printf("RESULT futex_ops_per_sec %.1f\n", 4.0 * rounds / dt);
    return 0;
}
