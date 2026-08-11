/*
 * demo3_dirty_track.c —— UFFDIO_WRITEPROTECT 脏页跟踪（增量检查点）
 *
 * 背景问题：
 *   做进程/虚拟机的热迁移或周期性快照时，每一轮都要把内存同步到对端。
 *   如果全量拷贝 100GB 内存，而两轮之间只改了 100MB，99.9% 的传输是浪费。
 *   需要一种廉价的"哪些页被写脏了"的跟踪手段。
 *
 * uffd 的解法（UFFDIO_WRITEPROTECT，内核 5.7+）：
 *   1. 把内存区域用 UFFDIO_REGISTER_MODE_WP 注册；
 *   2. 用 UFFDIO_WRITEPROTECT 把整个区域设为"写保护"；
 *   3. 应用写某一页时触发写保护缺页 → 用户态收到事件 → 在脏页位图上
 *      记一笔 → 解除该页写保护让写入继续；
 *   4. 下一轮检查点只传输位图里标记的脏页，传完重新全量写保护。
 *
 * 同样的机制被用于：CRIU 增量迁移、QEMU 热迁移 dirty logging、
 * 数据库/运行时的增量快照等。
 *
 * 对比：传统做法是 mprotect + SIGSEGV 信号处理，信号逐线程同步派发、
 * 还要处理重入，开销和复杂度都高得多；uffd 走独立 fd 的事件队列，
 * 由专门线程异步处理，快一个数量级。
 */
#include "uffd_common.h"

#define TRACK_PAGES  128          /* 被跟踪的内存区：128 页 = 512KB */
#define ROUNDS       3            /* 模拟 3 轮"增量检查点" */
#define WRITES_PER_ROUND 7        /* 每轮应用只写 7 页 */

static char  *g_region;
static long   g_pagesize;
static unsigned char g_dirty[TRACK_PAGES];   /* 脏页位图 */
static volatile int g_handler_stop;

static void set_wp(int uffd, uint64_t addr, size_t len, int protect)
{
    struct uffdio_writeprotect wp = {
        .range = { .start = addr, .len = len },
        .mode = protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0,
    };
    if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) < 0)
        die("UFFDIO_WRITEPROTECT");
}

/* 处理线程：收到写保护缺页 → 记录脏页 → 解除该页保护 → 写入自动重试 */
static void *wp_handler_thread(void *arg)
{
    int uffd = *(int *)arg;

    while (!g_handler_stop) {
        struct uffd_msg msg;
        ssize_t n = read(uffd, &msg, sizeof(msg));
        if (n != sizeof(msg)) {
            /* fd 是 O_NONBLOCK：暂无事件时稍候重试 */
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                usleep(100);
                continue;
            }
            break;
        }
        if (msg.event != UFFD_EVENT_PAGEFAULT)
            continue;
        if (!(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP)) {
            fprintf(stderr, "意外的非 WP 缺页\n");
            continue;
        }
        uint64_t addr = msg.arg.pagefault.address;
        long idx = (addr - (uint64_t)g_region) / g_pagesize;
        g_dirty[idx] = 1;
        /* 解除本页写保护（默认会唤醒被阻塞的写入线程） */
        set_wp(uffd, addr & ~(uint64_t)(g_pagesize - 1), g_pagesize, 0);
    }
    return NULL;
}

int main(void)
{
    g_pagesize = page_size();
    size_t len = (size_t)TRACK_PAGES * g_pagesize;

    printf("=== demo3: UFFDIO_WRITEPROTECT 脏页跟踪 ===\n");
    printf("跟踪内存 %d 页 (%zu KB)\n", TRACK_PAGES, len / 1024);

    /* 1. 准备内存并全部填充（模拟一个正在运行的进程的工作集） */
    g_region = map_anon(len);
    memset(g_region, 1, len);

    /* 2. 注册 WP 模式 */
    uint64_t features = 0;
    int uffd = uffd_open(&features);
    uffd_register(uffd, g_region, len, UFFDIO_REGISTER_MODE_WP);
    if (!(features & UFFD_FEATURE_PAGEFAULT_FLAG_WP)) {
        fprintf(stderr, "内核不支持 uffd 写保护（需要 5.7+）\n");
        return EXIT_FAILURE;
    }

    pthread_t handler;
    if (pthread_create(&handler, NULL, wp_handler_thread, &uffd))
        die("pthread_create");

    /* 3. 模拟多轮"运行 + 增量检查点" */
    srand(7);
    for (int round = 1; round <= ROUNDS; round++) {
        /* 新一轮开始：清位图，全量写保护 */
        memset(g_dirty, 0, sizeof(g_dirty));
        set_wp(uffd, (uint64_t)g_region, len, 1);

        /* 应用继续运行：随机写 WRITES_PER_ROUND 个不同的页。
         * 每次写入先被 uffd 拦截（记脏、解保护），然后才真正落笔。 */
        for (int w = 0; w < WRITES_PER_ROUND; w++) {
            long idx = rand() % TRACK_PAGES;
            memset(g_region + idx * g_pagesize, round, 16);
        }
        /* 写入线程每次都被阻塞到处理完成，所以此刻位图已完整 */

        long dirty_cnt = 0;
        for (int i = 0; i < TRACK_PAGES; i++)
            dirty_cnt += g_dirty[i];
        printf("第 %d 轮: 应用写了 %d 页, 跟踪到脏页 %ld 页 -> "
               "本轮检查点只需同步 %ld/%d 页 (省 %.0f%%)\n",
               round, WRITES_PER_ROUND, dirty_cnt, dirty_cnt, TRACK_PAGES,
               100.0 * (TRACK_PAGES - dirty_cnt) / TRACK_PAGES);
    }

    g_handler_stop = 1;
    pthread_join(handler, NULL);
    close(uffd);
    printf("\n结论: 用 uffd 写保护可以精确、低开销地收集脏页集合，\n");
    printf("热迁移/快照每轮只需传输真正变化的页。\n");
    return EXIT_SUCCESS;
}
