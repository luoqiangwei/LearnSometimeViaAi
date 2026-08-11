/*
 * demo1_lazy_load.c —— userfaultfd 最小可用示例：按需加载（懒加载）
 *
 * 演示的问题：
 *   普通 mmap + memset 会立刻把全部物理页占住。如果数据"可能用不到"，
 *   或者加载很慢（磁盘 / 网络 / 解压 / 远程拉取），就会浪费内存和启动时间。
 *
 * uffd 的解法：
 *   只映射虚拟地址空间，一个物理页都不分配。谁第一次访问某一页，
 *   谁就触发一次"用户态缺页事件"；处理线程收到事件后才去"取数据"并
 *   提供页面内容。内存只随真实访问增长。
 *
 * 这正是以下真实场景的基本原理：
 *   - QEMU 虚拟机 post-copy 热迁移：虚拟机先在目的机上跑起来，
 *     访问到哪一页，再从源机按需拉哪一页；
 *   - CRIU / 容器快照惰性恢复：进程先从快照镜像启动，
 *     内存页按需从镜像文件恢复；
 *   - Firecracker / serverless 函数的快速冷启动。
 *
 * 运行方式：./demo1_lazy_load
 */
#include "uffd_common.h"

#define MAP_SIZE   (256UL * 1024 * 1024)   /* 映射 256MB 虚拟地址空间 */
#define TOUCH_PAGES 1000                   /* 实际只随机访问 1000 页 */

static long   g_pagesize;
static size_t g_total_pages;
static char  *g_map_base;

/* 模拟"从慢速数据源取回一页数据"：这里用确定性图案代替真实 I/O。
 * 第 N 页的内容 = 重复填充的页号 N。主线程稍后会按同样的规则校验。 */
static void load_page_from_source(long page_index, void *buf)
{
    uint64_t *p = buf;
    for (long i = 0; i < g_pagesize / (long)sizeof(uint64_t); i++)
        p[i] = (uint64_t)page_index;
}

static volatile int g_handler_stop;

/* uffd 处理线程：等待缺页事件，逐页提供内容。
 * 注意：不能靠主线程 close(fd) 来唤醒阻塞在 read() 里的本线程
 * （Linux 下 close 不会中断另一个线程里进行中的 read），
 * 所以 fd 用 O_NONBLOCK 打开、read 循环Drain事件后自然退出。 */
static void *fault_handler_thread(void *arg)
{
    int uffd = *(int *)arg;
    void *page_buf = malloc(g_pagesize);
    if (!page_buf)
        die("malloc");

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
        if (msg.event != UFFD_EVENT_PAGEFAULT) {
            fprintf(stderr, "意外事件 %d\n", msg.event);
            continue;
        }

        uint64_t fault_addr = msg.arg.pagefault.address;
        /* 换算成相对映射起点的页号 */
        long page_index = (fault_addr - (uint64_t)g_map_base) / g_pagesize;

        load_page_from_source(page_index, page_buf);
        uffd_serve_page(uffd, fault_addr & ~(uint64_t)(g_pagesize - 1),
                        page_buf, g_pagesize);
    }
    free(page_buf);
    return NULL;
}

int main(void)
{
    g_pagesize = page_size();
    g_total_pages = MAP_SIZE / g_pagesize;

    printf("=== demo1: userfaultfd 懒加载 ===\n");
    printf("映射虚拟内存: %zu MB (%zu 页)\n",
           MAP_SIZE / 1024 / 1024, g_total_pages);

    long rss_before = read_rss_pages() * g_pagesize;

    /* 1. 只保留虚拟地址空间，此时一个物理页都不占 */
    g_map_base = map_anon(MAP_SIZE);

    /* 2. 注册 uffd MISSING 模式：区域内缺页会发事件给用户态 */
    int uffd = uffd_open(NULL);
    uffd_register(uffd, g_map_base, MAP_SIZE, UFFDIO_REGISTER_MODE_MISSING);

    /* 3. 启动缺页处理线程 */
    pthread_t tid;
    if (pthread_create(&tid, NULL, fault_handler_thread, &uffd))
        die("pthread_create");

    /* 4. 随机访问 TOUCH_PAGES 个不同页面，并校验内容 */
    srand(42);
    long checked = 0;
    uint64_t t0 = now_ms();
    for (int i = 0; i < TOUCH_PAGES; i++) {
        long idx = rand() % g_total_pages;
        uint64_t *page = (uint64_t *)(g_map_base + idx * g_pagesize);
        /* 第一次读这一页：触发缺页 → 处理线程填充 → 读到页号图案 */
        if (page[0] != (uint64_t)idx ||
            page[g_pagesize / sizeof(uint64_t) - 1] != (uint64_t)idx) {
            fprintf(stderr, "第 %ld 页内容校验失败: got %lu\n",
                    idx, page[0]);
            return EXIT_FAILURE;
        }
        checked++;
    }
    uint64_t elapsed = now_ms() - t0;

    long rss_after = read_rss_pages() * g_pagesize;

    printf("随机访问并校验了 %ld 页，全部通过 (耗时 %lu ms)\n",
           checked, elapsed);
    printf("常驻物理内存: 访问前 %ld KB -> 访问后 %ld KB (增长约 %ld MB)\n",
           rss_before / 1024, rss_after / 1024,
           (rss_after - rss_before) / 1024 / 1024);
    printf("对比: 若用 memset 一次性填充 256MB，需立即占满 256MB 物理内存；\n");
    printf("      而 uffd 懒加载只为你真正碰过的页付费。\n");

    g_handler_stop = 1;         /* 通知处理线程退出（poll 超时后检查标志） */
    pthread_join(tid, NULL);
    close(uffd);
    return EXIT_SUCCESS;
}
