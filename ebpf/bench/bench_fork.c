/*
 * bench_fork.c — 进程创建/退出吞吐基准
 * 衡量 sched_process_fork / sched_process_exit tracepoint 的开销。
 * 用法: bench_fork <迭代次数>
 * 输出: RESULT forks_per_sec <值>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    long n = argc > 1 ? atol(argv[1]) : 2000;
    double t0 = now_sec();
    for (long i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid == 0)
            _exit(0);
        if (pid < 0) {
            perror("fork");
            return 1;
        }
        waitpid(pid, NULL, 0);
    }
    double dt = now_sec() - t0;
    printf("RESULT forks_per_sec %.1f\n", n / dt);
    return 0;
}
