/*
 * lock_demo.c — 持锁过长 / 等锁过长演示程序
 *
 * holder 线程每次持锁 1500ms，多个 contender 线程疯狂抢锁。
 * 预期效果：
 *   - tracker 的 futex 跟踪会报 contender 线程等锁超过阈值（约 1500ms）
 *   - 若开启 --track-locks（uprobe），会报 holder 持锁超过阈值
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#define N_CONTENDERS 4

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000L * 1000L };
    nanosleep(&ts, NULL);
}

/* 持锁线程：模拟“临界区里做了慢操作”（如锁内 I/O、锁内分配大内存） */
static void *holder(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        /* ---- 临界区开始：持锁 1500ms ---- */
        msleep(1500);
        /* ---- 临界区结束 ---- */
        pthread_mutex_unlock(&g_lock);
        msleep(300);
    }
    return NULL;
}

/* 竞争线程：频繁抢锁，每次会被堵在 futex 上很久 */
static void *contender(void *arg)
{
    long id = (long)arg;
    long rounds = 0;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        /* 临界区极短：正常用法 */
        rounds++;
        if (rounds % 100 == 0)
            printf("[contender %ld] 完成 %ld 轮抢锁\n", id, rounds);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

int main(void)
{
    pthread_t th;
    long i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("lock_demo pid=%d, holder 持锁 1500ms/轮, %d 个 contender 抢锁\n",
           getpid(), N_CONTENDERS);

    pthread_create(&th, NULL, holder, NULL);
    for (i = 0; i < N_CONTENDERS; i++)
        pthread_create(&th, NULL, contender, (void *)i);

    for (;;)
        sleep(60);
    return 0;
}
