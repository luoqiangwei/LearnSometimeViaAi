/*
 * demo5_uffd_iouring.c —— userfaultfd × io_uring：懒加载 handler 的流水线化
 *
 * 背景问题（demo1 的延伸）：
 *   真实的懒加载场景里（Firecracker 快照恢复、CRIU lazy-pages、QEMU
 *   post-copy），uffd handler 收到缺页事件后要去"取数"——读快照文件、
 *   走网络拉取、解压。取数是真 I/O，每页都有延迟。
 *
 *   传统 handler 是串行的：
 *     read(事件) → pread(取数, 阻塞) → UFFDIO_COPY → 处理下一页
 *   N 次缺页的总耗时 ≈ N × 单次取数延迟。
 *
 * uffd + io_uring 的组合：
 *   uffd 是一个可 poll 的 fd，可以挂进 io_uring 事件循环（POLL_ADD）；
 *   取数用 io_uring 的异步 READ，几十次取数并发在飞；
 *   只有最后的 UFFDIO_COPY 仍是普通 ioctl（io_uring 没有通用 ioctl op，
 *   上游也尚未给 uffd 实现 uring_cmd —— 这正是潜在的内核改进点）。
 *   N 次缺页的总耗时 ≈ (N / 并发度) × 单次取数延迟。
 *
 * 本 demo 用同一突发负载对比两种 handler：
 *   - 数据源：真实文件（O_DIRECT 绕过页缓存），内容为确定性图案可校验；
 *   - 额外叠加"模拟远端取数/解压延迟"（默认每页 1ms，环境变量
 *     UFFD_DEMO5_SIM_US 调整，设 0 则只计真实磁盘 I/O）；
 *   - 16 个访问线程并发踩 256 个不同页面，制造突发缺页。
 */
#include "uffd_common.h"
#include <liburing.h>

#define REGION_SIZE   (64UL * 1024 * 1024)  /* 64MB 数据源与映射区域 */
#define N_ACCESSORS   16                    /* 并发访问线程数 */
#define PAGES_PER     16                    /* 每线程访问的页数 */
#define TOTAL_FAULTS  (N_ACCESSORS * PAGES_PER)
#define N_SLOTS       32                    /* io_uring 路径的并发取数槽位 */
#define DATA_FILE     "demo5_data.bin"

static long  g_pagesize;
static char *g_region;                      /* 当前阶段的 uffd 托管区域 */
static int   g_data_fd;                     /* O_DIRECT 打开的数据文件 */
static long  g_sim_us;                      /* 模拟取数延迟（µs） */
static volatile int g_pages_served;         /* handler 已填页计数 */
static volatile int g_pages_checked;        /* 访问线程已校验计数 */

/* ---------- 数据文件：第 N 页填充重复的页号 N（与 demo1 图案一致） ---------- */

static void fill_pattern(void *buf, long page_index)
{
    uint64_t *p = buf;
    for (long i = 0; i < g_pagesize / (long)sizeof(uint64_t); i++)
        p[i] = (uint64_t)page_index;
}

static void create_data_file(const char *path, size_t len)
{
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        die("open data file");
    void *buf;
    if (posix_memalign(&buf, g_pagesize, g_pagesize))
        die("posix_memalign");
    for (size_t off = 0; off < len; off += g_pagesize) {
        fill_pattern(buf, off / g_pagesize);
        if (pwrite(fd, buf, g_pagesize, off) != g_pagesize)
            die("pwrite data file");
    }
    if (fsync(fd))
        die("fsync");
    close(fd);
    free(buf);
}

/* ---------- 访问线程：并发踩一批互不相同的页，触发突发缺页 ---------- */

struct accessor_arg {
    const long *pages;
    int count;
};

static void *accessor_thread(void *arg)
{
    struct accessor_arg *a = arg;
    for (int i = 0; i < a->count; i++) {
        long idx = a->pages[i];
        volatile uint64_t *page = (uint64_t *)(g_region + idx * g_pagesize);
        /* 这次读触发缺页并阻塞，直到 handler 取数+填页完成 */
        uint64_t first = page[0];
        uint64_t last = page[g_pagesize / sizeof(uint64_t) - 1];
        if (first != (uint64_t)idx || last != (uint64_t)idx) {
            fprintf(stderr, "页 %ld 校验失败: %lu/%lu\n", idx, first, last);
            exit(EXIT_FAILURE);
        }
        __sync_fetch_and_add(&g_pages_checked, 1);
    }
    return NULL;
}

/* ---------- handler A：传统阻塞式（串行取数） ---------- */

static void *handler_blocking(void *arg)
{
    int uffd = *(int *)arg;
    void *buf;
    if (posix_memalign(&buf, g_pagesize, g_pagesize))
        die("posix_memalign");

    while (g_pages_served < TOTAL_FAULTS) {
        struct uffd_msg msg;
        ssize_t n = read(uffd, &msg, sizeof(msg));
        if (n != sizeof(msg)) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                usleep(100);
                continue;
            }
            die("read uffd");
        }
        if (msg.event != UFFD_EVENT_PAGEFAULT)
            continue;

        uint64_t fault_addr = msg.arg.pagefault.address;
        long idx = (fault_addr - (uint64_t)g_region) / g_pagesize;

        /* 串行取数：真实磁盘读 + 模拟远端/解压延迟，全程阻塞本线程 */
        if (pread(g_data_fd, buf, g_pagesize, idx * g_pagesize) != g_pagesize)
            die("pread");
        if (g_sim_us)
            usleep(g_sim_us);

        uffd_serve_page(uffd, fault_addr & ~(uint64_t)(g_pagesize - 1),
                        buf, g_pagesize);
        __sync_fetch_and_add(&g_pages_served, 1);
    }
    free(buf);
    return NULL;
}

/* ---------- handler B：io_uring 异步流水线 ---------- */

struct fetch_slot {
    void *buf;                  /* O_DIRECT 对齐的取数缓冲 */
    long page_index;
    uint64_t dst;               /* 缺页地址（页对齐） */
    int read_done;              /* 数据已读回 */
    int timer_pending;          /* 模拟延迟定时器尚未到期 */
    struct __kernel_timespec ts;
};

#define TAG_POLL  ((void *)1)               /* uffd 事件 poll 完成 */
#define tag_read(s)  ((void *)((char *)(s) + 0))
#define tag_timer(s) ((void *)((char *)(s) + 2))
/* slot 指针来自 4 字节对齐的数组，用低两位区分完成类型 */

static void *handler_iouring(void *arg)
{
    int uffd = *(int *)arg;
    struct io_uring ring;
    if (io_uring_queue_init(64, &ring, 0) < 0)
        die("io_uring_queue_init");

    static struct fetch_slot slots[N_SLOTS];
    int free_stack[N_SLOTS], free_top = 0;
    for (int i = 0; i < N_SLOTS; i++) {
        if (posix_memalign(&slots[i].buf, g_pagesize, g_pagesize))
            die("posix_memalign");
        free_stack[free_top++] = i;
    }

    int poll_armed = 0;

    while (g_pages_served < TOTAL_FAULTS) {
        /* 1. 挂 poll 等 uffd 事件（有事件立即可读时会立即完成；
         *    没有空闲槽位时不挂——事件留在内核队列，poll 是 level-triggered，
         *    槽位释放后再挂会立即触发，不会丢事件） */
        if (!poll_armed && free_top > 0) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (!sqe)
                die("get_sqe poll");
            io_uring_prep_poll_add(sqe, uffd, POLLIN);
            io_uring_sqe_set_data(sqe, TAG_POLL);
            poll_armed = 1;
        }
        io_uring_submit(&ring);

        /* 2. 至少等一个完成 */
        struct io_uring_cqe *cqe;
        if (io_uring_wait_cqe(&ring, &cqe) < 0)
            die("io_uring_wait_cqe");

        /* 3. 收割所有已完成的事件 */
        unsigned head;
        unsigned count = 0;
        io_uring_for_each_cqe(&ring, head, cqe) {
            count++;
            void *tag = io_uring_cqe_get_data(cqe);
            if (tag == TAG_POLL) {
                poll_armed = 0;
                /* uffd 有事件：无空闲槽位时先不读，事件留在内核队列里，
                 * 下次 poll 会再次触发（level-triggered） */
                while (free_top > 0) {
                    struct uffd_msg msg;
                    ssize_t n = read(uffd, &msg, sizeof(msg));
                    if (n != sizeof(msg))
                        break;          /* EAGAIN：事件已掏空 */
                    if (msg.event != UFFD_EVENT_PAGEFAULT)
                        continue;
                    int si = free_stack[--free_top];
                    struct fetch_slot *s = &slots[si];
                    s->page_index =
                        (msg.arg.pagefault.address - (uint64_t)g_region)
                        / g_pagesize;
                    s->dst = msg.arg.pagefault.address
                             & ~(uint64_t)(g_pagesize - 1);
                    s->read_done = 0;
                    s->timer_pending = g_sim_us ? 1 : 0;

                    /* 异步取数（真实磁盘 I/O） */
                    struct io_uring_sqe *rsqe = io_uring_get_sqe(&ring);
                    io_uring_prep_read(rsqe, g_data_fd, s->buf, g_pagesize,
                                       s->page_index * g_pagesize);
                    io_uring_sqe_set_data(rsqe, tag_read(s));

                    /* 异步模拟延迟（网络 RTT / 解压），与取数并行计时 */
                    if (g_sim_us) {
                        s->ts.tv_sec = g_sim_us / 1000000;
                        s->ts.tv_nsec = (g_sim_us % 1000000) * 1000;
                        struct io_uring_sqe *tsqe = io_uring_get_sqe(&ring);
                        io_uring_prep_timeout(tsqe, &s->ts, 1, 0);
                        io_uring_sqe_set_data(tsqe, tag_timer(s));
                    }
                }
            } else {
                uintptr_t v = (uintptr_t)tag;
                struct fetch_slot *s =
                    (struct fetch_slot *)(v & ~(uintptr_t)3);
                if ((v & 3) == 0) {     /* 取数完成 */
                    if (cqe->res != (int)g_pagesize) {
                        fprintf(stderr, "io_uring read 失败: res=%d\n",
                                cqe->res);
                        exit(EXIT_FAILURE);
                    }
                    s->read_done = 1;
                } else {                /* 模拟延迟到期 */
                    s->timer_pending = 0;
                }
                /* 数据与延迟都就绪 → 填页（UFFDIO_COPY 仍是普通 ioctl，
                 * io_uring 目前无法代劳） */
                if (s->read_done && !s->timer_pending) {
                    uffd_serve_page(uffd, s->dst, s->buf, g_pagesize);
                    free_stack[free_top++] = s - slots;
                    __sync_fetch_and_add(&g_pages_served, 1);
                }
            }
        }
        io_uring_cq_advance(&ring, count);
    }

    for (int i = 0; i < N_SLOTS; i++)
        free(slots[i].buf);
    io_uring_queue_exit(&ring);
    return NULL;
}

/* ---------- 阶段驱动 ---------- */

static long g_fault_pages[TOTAL_FAULTS];

static void pick_fault_pages(long total_pages)
{
    /* 同一随机序列用于两个阶段，保证负载一致 */
    static unsigned char picked[REGION_SIZE / 4096 / 8];
    srand(2024);
    int n = 0;
    while (n < TOTAL_FAULTS) {
        long idx = rand() % total_pages;
        if (picked[idx / 8] & (1 << (idx % 8)))
            continue;
        picked[idx / 8] |= 1 << (idx % 8);
        g_fault_pages[n++] = idx;
    }
}

static double run_phase(void *(*handler_fn)(void *), const char *name)
{
    /* 每个阶段用全新区域与 uffd，保证缺页事件重新发生 */
    g_region = map_anon(REGION_SIZE);
    int uffd = uffd_open(NULL);
    uffd_register(uffd, g_region, REGION_SIZE, UFFDIO_REGISTER_MODE_MISSING);

    g_pages_served = 0;
    g_pages_checked = 0;

    pthread_t htid;
    if (pthread_create(&htid, NULL, handler_fn, &uffd))
        die("pthread_create handler");

    struct accessor_arg args[N_ACCESSORS];
    pthread_t tids[N_ACCESSORS];
    uint64_t t0 = now_ms();
    for (int i = 0; i < N_ACCESSORS; i++) {
        args[i].pages = &g_fault_pages[i * PAGES_PER];
        args[i].count = PAGES_PER;
        if (pthread_create(&tids[i], NULL, accessor_thread, &args[i]))
            die("pthread_create accessor");
    }
    for (int i = 0; i < N_ACCESSORS; i++)
        pthread_join(tids[i], NULL);
    uint64_t elapsed = now_ms() - t0;

    pthread_join(htid, NULL);       /* handler 服务满 TOTAL_FAULTS 页后自退 */
    close(uffd);
    munmap(g_region, REGION_SIZE);

    if (g_pages_checked != TOTAL_FAULTS) {
        fprintf(stderr, "校验页数不符: %d/%d\n", g_pages_checked, TOTAL_FAULTS);
        exit(EXIT_FAILURE);
    }
    printf("  %-34s 总耗时 %5lu ms, 平均每页 %6.1f µs\n", name,
           elapsed, elapsed * 1000.0 / TOTAL_FAULTS);
    return elapsed;
}

int main(void)
{
    g_pagesize = page_size();
    const char *sim = getenv("UFFD_DEMO5_SIM_US");
    g_sim_us = sim ? atol(sim) : 1000;

    printf("=== demo5: userfaultfd × io_uring 懒加载流水线对比 ===\n");
    printf("数据源 %zu MB 真实文件(O_DIRECT), 突发 %d 次缺页(%d 线程并发), "
           "模拟取数延迟 %ld µs/页\n",
           REGION_SIZE / 1024 / 1024, TOTAL_FAULTS, N_ACCESSORS, g_sim_us);

    create_data_file(DATA_FILE, REGION_SIZE);
    g_data_fd = open(DATA_FILE, O_RDONLY | O_DIRECT);
    if (g_data_fd < 0) {
        perror("O_DIRECT 打开失败，退回普通读（磁盘延迟将被页缓存掩盖）");
        g_data_fd = open(DATA_FILE, O_RDONLY);
        if (g_data_fd < 0)
            die("open data file");
    }

    /* 实测单次磁盘读延迟，便于解释结果 */
    {
        void *probe;
        if (posix_memalign(&probe, g_pagesize, g_pagesize))
            die("posix_memalign");
        long total = REGION_SIZE / g_pagesize;
        uint64_t t0 = now_ms() * 1000;
        srand(99);
        for (int i = 0; i < 32; i++)
            if (pread(g_data_fd, probe, g_pagesize,
                      (rand() % total) * g_pagesize) != g_pagesize)
                die("probe pread");
        printf("实测单次 O_DIRECT 读延迟约 %lu µs\n",
               (now_ms() * 1000 - t0) / 32);
        free(probe);
    }

    pick_fault_pages(REGION_SIZE / g_pagesize);

    printf("\n[阶段 A] 传统阻塞 handler（read 事件 → pread 取数 → COPY，全串行）\n");
    double t_a = run_phase(handler_blocking, "阻塞式 handler");

    printf("\n[阶段 B] io_uring handler（事件 poll + 取数全异步，%d 路并发）\n",
           N_SLOTS);
    double t_b = run_phase(handler_iouring, "io_uring handler");

    printf("\n对比: io_uring 方案总耗时降低 %.1f%%（%.1fx 加速）。\n",
           100.0 * (t_a - t_b) / t_a, t_a / t_b);
    printf("原因: 阻塞方案每页串行付出一次取数延迟; io_uring 把取数并发化，\n");
    printf("总耗时 ≈ 并发轮数 × 单次延迟。取数越慢（网络/解压），差距越大。\n");

    close(g_data_fd);
    unlink(DATA_FILE);
    return EXIT_SUCCESS;
}
