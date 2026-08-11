#define _GNU_SOURCE
#include "net_engines.h"
#include "uring_min.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#define MAX_CONNS 1024

static volatile sig_atomic_t g_stop;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static int make_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(fd, (void *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind :%d: %s\n", port, strerror(errno));
        return -1;
    }
    if (listen(fd, 1024) != 0) {
        fprintf(stderr, "listen: %s\n", strerror(errno));
        return -1;
    }
    return fd;
}

static void report(int stats_fd, long syscalls)
{
    dprintf(stats_fd, "%ld", syscalls);
}

/* ================= 引擎 1: thread-per-connection(阻塞) ================= */

typedef struct {
    int fd;
    pthread_t tid;
    int started;
} tconn_t;

static tconn_t g_tconns[MAX_CONNS];
static atomic_long g_threads_sc;   /* read/write/accept syscall 计数 */

static void *echo_worker(void *argp)
{
    tconn_t *c = argp;
    char *buf = malloc(65536);
    for (;;) {
        ssize_t n = read(c->fd, buf, 65536);
        atomic_fetch_add(&g_threads_sc, 1);
        if (n <= 0)
            break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t m = write(c->fd, buf + off, n - off);
            atomic_fetch_add(&g_threads_sc, 1);
            if (m <= 0)
                goto out;
            off += m;
        }
    }
out:
    close(c->fd);
    free(buf);
    return NULL;
}

static int run_threads(int port, int sync_fd, int stats_fd)
{
    int lfd = make_listen(port);
    if (lfd < 0) return 1;
    if (write(sync_fd, "R", 1) != 1) return 1;

    while (!g_stop) {
        int fd = accept(lfd, NULL, NULL);
        atomic_fetch_add(&g_threads_sc, 1);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        atomic_fetch_add(&g_threads_sc, 1);
        int slot = -1;
        for (int i = 0; i < MAX_CONNS; i++) {
            if (!g_tconns[i].started) { slot = i; break; }
        }
        if (slot < 0) { close(fd); continue; }
        g_tconns[slot].fd = fd;
        g_tconns[slot].started = 1;
        pthread_create(&g_tconns[slot].tid, NULL, echo_worker, &g_tconns[slot]);
    }
    close(lfd);
    for (int i = 0; i < MAX_CONNS; i++) {
        if (g_tconns[i].started) {
            shutdown(g_tconns[i].fd, SHUT_RDWR);
            pthread_join(g_tconns[i].tid, NULL);
        }
    }
    report(stats_fd, atomic_load(&g_threads_sc));
    return 0;
}

/* ================= 引擎 2: epoll(ET 非阻塞) ================= */

typedef struct {
    char *buf;
    size_t off, len;   /* 已收未发完的字节 */
} econn_t;

static int run_epoll(int port, size_t bufsize, int sync_fd, int stats_fd)
{
    int lfd = make_listen(port);
    if (lfd < 0) return 1;
    fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL) | O_NONBLOCK);

    long sc = 0;
    int epfd = epoll_create1(0); sc++;
    static econn_t *conns[65536];

    struct epoll_event lev = { .events = EPOLLIN | EPOLLET, .data.fd = lfd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &lev); sc++;
    if (write(sync_fd, "R", 1) != 1) return 1;

    struct epoll_event evs[128];
    while (!g_stop) {
        int n = epoll_wait(epfd, evs, 128, -1); sc++;
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            uint32_t ev = evs[i].events;
            if (fd == lfd) {
                for (;;) {
                    int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK); sc++;
                    if (cfd < 0) break;  /* EAGAIN: 排空 */
                    int one = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); sc++;
                    econn_t *c = calloc(1, sizeof(econn_t));
                    c->buf = malloc(bufsize);
                    conns[cfd] = c;
                    struct epoll_event cev = { .events = EPOLLIN | EPOLLET, .data.fd = cfd };
                    epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &cev); sc++;
                }
                continue;
            }
            econn_t *c = conns[fd];
            if (!c) continue;
            int closed = 0;
            if ((ev & EPOLLOUT) && c->len > 0) {
                /* 冲刷之前没发完的数据 */
                while (c->off < c->len) {
                    ssize_t m = send(fd, c->buf + c->off, c->len - c->off, MSG_NOSIGNAL); sc++;
                    if (m < 0) {
                        if (errno == EAGAIN) break;
                        closed = 1; break;
                    }
                    c->off += m;
                }
                if (!closed && c->off == c->len) {
                    c->len = c->off = 0;
                    struct epoll_event cev = { .events = EPOLLIN | EPOLLET, .data.fd = fd };
                    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &cev); sc++;
                }
            }
            if (!closed && (ev & (EPOLLIN | EPOLLRDHUP)) && c->len == 0) {
                for (;;) {
                    ssize_t m = recv(fd, c->buf, bufsize, 0); sc++;
                    if (m > 0) {
                        ssize_t s = send(fd, c->buf, m, MSG_NOSIGNAL); sc++;
                        if (s < 0) {
                            if (errno != EAGAIN) { closed = 1; break; }
                            s = 0;
                        }
                        if (s < m) {
                            /* 没发完, 挂起等 EPOLLOUT */
                            memmove(c->buf, c->buf + s, m - s);
                            c->off = 0;
                            c->len = m - s;
                            struct epoll_event cev = {
                                .events = EPOLLOUT | EPOLLET, .data.fd = fd };
                            epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &cev); sc++;
                            break;
                        }
                        continue;   /* ET: 继续 recv 直到 EAGAIN */
                    }
                    if (m == 0) { closed = 1; break; }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    closed = 1; break;
                }
            }
            if (closed) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL); sc++;
                close(fd);
                free(c->buf); free(c);
                conns[fd] = NULL;
            }
        }
    }
    report(stats_fd, sc);
    return 0;
}

/* ================= 引擎 3: io_uring ================= */

enum { OP_ACCEPT = 1, OP_RECV = 2, OP_SEND = 3 };

typedef struct {
    int used;
    int fd;
    char *buf;
    size_t len;      /* 本次要发的字节数 */
    size_t sent;     /* 已发 */
} uconn_t;

static int run_uring_net(int port, size_t bufsize, int sync_fd, int stats_fd)
{
    int lfd = make_listen(port);
    if (lfd < 0) return 1;

    uring_t r;
    int ret = uring_init(&r, 2 * MAX_CONNS + 32, 0);
    if (ret != 0) {
        fprintf(stderr, "uring_init: %s\n", strerror(-ret));
        return 1;
    }
    static uconn_t conns[MAX_CONNS];
    long extra_sc = 0;             /* setsockopt 等普通 syscall */
    int accept_multishot = 1;
    unsigned pending = 0;          /* 已填充未提交的 SQE 数 */

    /* user_data 编码: [63:32]=op [31:0]=slot */
    #define UD(op_, slot_) ((((uint64_t)(op_)) << 32) | (uint32_t)(slot_))

    #define SUBMIT_ACCEPT() do { \
        struct io_uring_sqe *sqe = uring_get_sqe(&r); \
        sqe->opcode = IORING_OP_ACCEPT; \
        sqe->fd = lfd; \
        if (accept_multishot) sqe->ioprio |= IORING_ACCEPT_MULTISHOT; \
        sqe->user_data = UD(OP_ACCEPT, 0); \
        pending++; \
    } while (0)

    #define SUBMIT_RECV(s) do { \
        struct io_uring_sqe *sqe = uring_get_sqe(&r); \
        sqe->opcode = IORING_OP_RECV; \
        sqe->fd = conns[s].fd; \
        sqe->addr = (uint64_t)conns[s].buf; \
        sqe->len = bufsize; \
        sqe->user_data = UD(OP_RECV, s); \
        pending++; \
    } while (0)

    #define SUBMIT_SEND(s, off, len_) do { \
        struct io_uring_sqe *sqe = uring_get_sqe(&r); \
        sqe->opcode = IORING_OP_SEND; \
        sqe->fd = conns[s].fd; \
        sqe->addr = (uint64_t)(conns[s].buf + (off)); \
        sqe->len = (len_); \
        sqe->msg_flags = MSG_NOSIGNAL; \
        sqe->user_data = UD(OP_SEND, s); \
        pending++; \
    } while (0)

    SUBMIT_ACCEPT();
    if (write(sync_fd, "R", 1) != 1) return 1;

    while (!g_stop) {
        /* 一次性提交本轮所有新 SQE, 并阻塞等到至少 1 个完成 */
        if (uring_flush(&r, pending, 1) < 0 && errno != EINTR)
            break;
        pending = 0;
        struct io_uring_cqe cqe;
        while (uring_next_cqe(&r, &cqe)) {
            unsigned op = cqe.user_data >> 32;
            unsigned slot = (uint32_t)cqe.user_data;

            if (op == OP_ACCEPT) {
                if (cqe.res >= 0) {
                    int fd = cqe.res;
                    int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); extra_sc++;
                    int s = -1;
                    for (int i = 0; i < MAX_CONNS; i++)
                        if (!conns[i].used) { s = i; break; }
                    if (s < 0) {
                        close(fd);
                    } else {
                        conns[s].used = 1;
                        conns[s].fd = fd;
                        conns[s].buf = malloc(bufsize);
                        conns[s].len = conns[s].sent = 0;
                        SUBMIT_RECV(s);
                    }
                } else if (cqe.res == -EINVAL && accept_multishot) {
                    accept_multishot = 0;   /* 内核不支持 multishot, 回退单发 */
                }
                if (!(cqe.flags & IORING_CQE_F_MORE))
                    SUBMIT_ACCEPT();        /* 单发模式或 multishot 终止, 重新武装 */
            } else if (op == OP_RECV) {
                if (cqe.res > 0) {
                    conns[slot].len = cqe.res;
                    conns[slot].sent = 0;
                    SUBMIT_SEND(slot, 0, conns[slot].len);
                } else {
                    close(conns[slot].fd);
                    free(conns[slot].buf);
                    conns[slot].used = 0;
                }
            } else if (op == OP_SEND) {
                if (cqe.res > 0)
                    conns[slot].sent += cqe.res;
                if (cqe.res <= 0 && cqe.res != -EAGAIN) {
                    close(conns[slot].fd);
                    free(conns[slot].buf);
                    conns[slot].used = 0;
                } else if (conns[slot].sent < conns[slot].len) {
                    SUBMIT_SEND(slot, conns[slot].sent, conns[slot].len - conns[slot].sent);
                } else {
                    SUBMIT_RECV(slot);      /* 发完一轮, 重新挂接收 */
                }
            }
        }
    }

    report(stats_fd, r.syscalls + extra_sc);
    uring_exit(&r);
    return 0;
}

int net_engine_run(const char *engine, int port, size_t bufsize, int sync_fd, int stats_fd)
{
    struct sigaction sa = { .sa_handler = on_sigint };
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (strcmp(engine, "threads") == 0)
        return run_threads(port, sync_fd, stats_fd);
    if (strcmp(engine, "epoll") == 0)
        return run_epoll(port, bufsize, sync_fd, stats_fd);
    if (strcmp(engine, "uring") == 0)
        return run_uring_net(port, bufsize, sync_fd, stats_fd);
    fprintf(stderr, "未知网络引擎: %s\n", engine);
    return 2;
}
