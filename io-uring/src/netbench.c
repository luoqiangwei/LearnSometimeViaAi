#define _GNU_SOURCE
#include "net_engines.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <time.h>

/*
 * netbench: loopback TCP echo 压测
 * 父进程 = 客户端负载发生器(C 个连接各自 ping-pong M 次),
 * 子进程 = 被测服务器引擎(threads / epoll / uring)。
 * 服务器 CPU 通过 wait4 的 rusage 获得, 精确归属被测引擎。
 */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

typedef struct {
    int port;
    size_t size;
    long msgs;
    uint64_t *lat;     /* msgs 个采样 */
    int err;
} client_arg_t;

static int write_full(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t m = write(fd, buf + off, n - off);
        if (m <= 0) return -1;
        off += m;
    }
    return 0;
}

static void *client_thread(void *argp)
{
    client_arg_t *a = argp;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(a->port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    for (int i = 0; i < 100; i++) {
        if (connect(fd, (void *)&addr, sizeof(addr)) == 0)
            break;
        usleep(1000);
        if (i == 99) { a->err = 1; return NULL; }
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    char *buf = malloc(a->size);
    memset(buf, 'N', a->size);
    for (long i = 0; i < a->msgs; i++) {
        uint64_t t0 = now_ns();
        if (write_full(fd, buf, a->size) != 0) { a->err = 1; break; }
        size_t got = 0;
        while (got < a->size) {
            ssize_t m = read(fd, buf, a->size - got);
            if (m <= 0) { a->err = 1; break; }
            got += m;
        }
        if (a->err) break;
        a->lat[i] = now_ns() - t0;
    }
    free(buf);
    close(fd);
    return NULL;
}

static int cmp_u64(const void *x, const void *y)
{
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return (a > b) - (a < b);
}

static double ru_us(const struct rusage *ru)
{
    return (ru->ru_utime.tv_sec + ru->ru_stime.tv_sec) * 1e6 +
           (ru->ru_utime.tv_usec + ru->ru_stime.tv_usec);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s --engine=threads|epoll|uring [选项]\n"
        "  --conns=N   并发连接数(默认 64)\n"
        "  --msgs=N    每连接 ping-pong 次数(默认 2000)\n"
        "  --size=B    消息字节数(默认 64)\n"
        "  --port=N    监听端口(默认 7800)\n", prog);
}

int main(int argc, char **argv)
{
    const char *engine = NULL;
    int conns = 64, port = 7800;
    long msgs = 2000;
    size_t size = 64;

    static struct option opts[] = {
        {"engine", required_argument, 0, 'e'},
        {"conns",  required_argument, 0, 'c'},
        {"msgs",   required_argument, 0, 'm'},
        {"size",   required_argument, 0, 's'},
        {"port",   required_argument, 0, 'p'},
        {"help",   no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "e:c:m:s:p:h", opts, NULL)) != -1) {
        switch (c) {
        case 'e': engine = optarg; break;
        case 'c': conns = atoi(optarg); break;
        case 'm': msgs = atol(optarg); break;
        case 's': size = strtoul(optarg, NULL, 0); break;
        case 'p': port = atoi(optarg); break;
        default: usage(argv[0]); return 2;
        }
    }
    if (!engine) { usage(argv[0]); return 2; }

    int sync_pipe[2], stats_pipe[2];
    if (pipe(sync_pipe) != 0 || pipe(stats_pipe) != 0) {
        fprintf(stderr, "pipe: %s\n", strerror(errno));
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(sync_pipe[0]);
        close(stats_pipe[0]);
        _exit(net_engine_run(engine, port, size, sync_pipe[1], stats_pipe[1]));
    }
    close(sync_pipe[1]);
    close(stats_pipe[1]);

    char ready;
    if (read(sync_pipe[0], &ready, 1) != 1) {
        fprintf(stderr, "服务器引擎启动失败\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 1;
    }

    /* ---- 客户端负载 ---- */
    pthread_t *tids = malloc(sizeof(pthread_t) * conns);
    client_arg_t *args = calloc(conns, sizeof(client_arg_t));
    for (int i = 0; i < conns; i++) {
        args[i].port = port;
        args[i].size = size;
        args[i].msgs = msgs;
        args[i].lat = malloc(sizeof(uint64_t) * msgs);
    }
    uint64_t t0 = now_ns();
    for (int i = 0; i < conns; i++)
        pthread_create(&tids[i], NULL, client_thread, &args[i]);
    for (int i = 0; i < conns; i++)
        pthread_join(tids[i], NULL);
    uint64_t wall = now_ns() - t0;

    for (int i = 0; i < conns; i++)
        if (args[i].err)
            fprintf(stderr, "warn: 连接 %d 中途出错(数据可能不全)\n", i);

    /* ---- 收停服务器, 取 CPU 与 syscall 计数 ---- */
    kill(pid, SIGINT);
    struct rusage cru;
    int status;
    wait4(pid, &status, 0, &cru);
    char sbuf[64] = {0};
    ssize_t sn = read(stats_pipe[0], sbuf, sizeof(sbuf) - 1);
    long srv_syscalls = sn > 0 ? atol(sbuf) : -1;

    /* ---- 汇总 ---- */
    long total = 0;
    for (int i = 0; i < conns; i++) total += msgs;
    uint64_t *all = malloc(sizeof(uint64_t) * total);
    long k = 0;
    for (int i = 0; i < conns; i++)
        for (long j = 0; j < msgs; j++)
            all[k++] = args[i].lat[j];
    qsort(all, total, sizeof(uint64_t), cmp_u64);
    double sum = 0;
    for (long i = 0; i < total; i++) sum += all[i];
    double avg = sum / total / 1000.0;
    double p50 = all[total / 2] / 1000.0;
    double p99 = all[(long)(total * 0.99)] / 1000.0;
    double wall_s = wall / 1e9;
    double rps = total / wall_s;
    double bw = rps * size / (1024.0 * 1024.0);
    double cpu_kmsg = ru_us(&cru) / total * 1000.0;

    printf("%-8s conns=%-4d size=%-6zu msgs=%-6ld | RPS=%-10.0f BW=%-8.1fMiB/s | "
           "rtt avg=%-7.1fus p50=%-7.1fus p99=%-8.1fus | srvCPU=%-7.1fus/kmsg | srvSys=%ld\n",
           engine, conns, size, msgs, rps, bw, avg, p50, p99, cpu_kmsg, srv_syscalls);
    printf("CSV,%s,%d,%zu,%ld,%.0f,%.1f,%.1f,%.1f,%.1f,%.1f,%ld\n",
           engine, conns, size, total, rps, bw, avg, p50, p99, cpu_kmsg, srv_syscalls);

    for (int i = 0; i < conns; i++) free(args[i].lat);
    free(args); free(tids); free(all);
    return 0;
}
