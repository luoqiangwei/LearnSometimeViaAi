/*
 * bench_ctxsw.c — 上下文切换吞吐基准（管道 ping-pong）
 * 父子进程通过两个管道轮流发 1 字节，每次往返产生 2 次上下文切换。
 * 衡量 sched_switch tracepoint（每次切换都执行）的开销。
 * 用法: bench_ctxsw <往返次数>
 * 输出: RESULT switches_per_sec <值>
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
    long n = argc > 1 ? atol(argv[1]) : 20000;
    int p2c[2], c2p[2];         /* parent->child, child->parent */
    char buf = 0;

    if (pipe(p2c) || pipe(c2p)) {
        perror("pipe");
        return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        /* 子进程：读 p2c，写 c2p */
        close(p2c[1]); close(c2p[0]);
        for (long i = 0; i < n; i++) {
            if (read(p2c[0], &buf, 1) != 1)
                _exit(1);
            if (write(c2p[1], &buf, 1) != 1)
                _exit(1);
        }
        _exit(0);
    }
    /* 父进程：写 p2c，读 c2p，计时 */
    close(p2c[0]); close(c2p[1]);
    double t0 = now_sec();
    for (long i = 0; i < n; i++) {
        if (write(p2c[1], &buf, 1) != 1) { perror("write"); return 1; }
        if (read(c2p[0], &buf, 1) != 1)  { perror("read");  return 1; }
    }
    double dt = now_sec() - t0;
    waitpid(pid, NULL, 0);
    printf("RESULT switches_per_sec %.1f\n", 2.0 * n / dt);
    return 0;
}
