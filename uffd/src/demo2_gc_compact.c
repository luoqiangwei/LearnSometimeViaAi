/*
 * demo2_gc_compact.c —— 模拟 ART（Android Runtime）CMC GC 并发压缩的核心机制
 *
 * 背景问题：
 *   压缩式 GC 要把对象从旧位置搬到新位置（消除内存碎片）。
 *   搬迁期间应用线程（mutator）如果访问到"正在搬迁的页"，就会读到无效数据。
 *   传统方案一：stop-the-world —— 搬迁期间停掉所有 mutator，卡顿明显。
 *   传统方案二：软件读屏障（每次访问对象都检查"它搬走了吗"）—— 每条
 *               内存访问都付出指令开销，即使 GC 根本没在运行。
 *               （Android 8 引入的 CC GC 在 ARM64 上用的就是 Baker 读屏障）
 *
 * uffd 方案（ART 的 Concurrent Mark-Compact GC 就是这么做的，
 * Android 13 起默认启用）：
 *   把堆注册给 userfaultfd。GC 要搬迁哪一页，就把那一页 MADV_DONTNEED
 *   （或等效手段）解除映射。mutator 一旦踩到被解除映射的页，硬件缺页异常
 *   被 uffd 拦截并转给用户态处理线程：若该页还没搬完就等 GC 搬完，然后
 *   把新内容提供给这一页、唤醒 mutator。
 *   —— 没有踩到搬迁页的访问【零开销】，不用停世界，也不用读屏障指令。
 *
 * 本 demo 对比两个阶段：
 *   阶段 A（STW）：每轮 GC 压缩时全局停表，所有 mutator 空转等待；
 *   阶段 B（uffd）：GC 并发搬迁，只有踩到"正在搬迁页"的访问被短暂阻塞。
 *
 * 简化说明：真实 ART 还要处理对象引用更新和并发写（Brooks 转发指针），
 * 本 demo 聚焦"缺页拦截"这一核心机制本身。
 */
#include "uffd_common.h"

#define HEAP_PAGES        512    /* 模拟堆大小：512 页 = 2MB */
#define N_MUTATORS        4      /* 应用线程数 */
#define PHASE_SECONDS     5      /* 每个阶段运行时长 */
#define GC_INTERVAL_MS    30     /* GC 周期 */
#define VICTIMS_PER_CYCLE 8      /* 每轮搬迁页数 */
#define COMPACT_MS        2      /* 模拟单页搬迁耗时 */

#define MAGIC(idx) (0xC0FFEE0000000000ULL | (uint64_t)(idx))

struct heap_page_meta {
    pthread_mutex_t lock;          /* 串行化 GC 搬迁与 fault 处理对本页的操作 */
    int evacuating;                /* 本页是否正在被 GC 搬迁 */
    int resident;                  /* 页帧当前是否在位（未被 DONTNEED） */
};

static char *g_heap;                       /* uffd 托管的模拟堆 */
static char *g_backup;                     /* 每页的"新位置"内容备份 */
static struct heap_page_meta g_meta[HEAP_PAGES];
static long g_pagesize;

static volatile int g_stop;                /* 停止 mutator / GC */
static volatile int g_handler_stop;        /* 停止 fault 处理线程 */
static volatile unsigned long g_ops;       /* mutator 完成的访问次数 */
static volatile unsigned long g_faults;    /* uffd 处理的缺页次数 */
static volatile unsigned long g_stall_ms;  /* 因缺页被阻塞的累计时长（约值） */

/* ---------- mutator：不停地随机读写堆页面 ---------- */

static void mutate_page(long idx)
{
    uint64_t *page = (uint64_t *)(g_heap + idx * g_pagesize);
    /* 读校验 magic（常数），再原子地把计数器 +1 */
    if (page[0] != MAGIC(idx)) {
        fprintf(stderr, "页 %ld 数据损坏: magic=%lx\n", idx,
                (unsigned long)page[0]);
        exit(EXIT_FAILURE);
    }
    __sync_fetch_and_add(&page[1], 1);
}

static void *mutator_thread(void *arg)
{
    unsigned int seed = (unsigned long)arg + 1;
    while (!g_stop) {
        mutate_page(rand_r(&seed) % HEAP_PAGES);
        __sync_fetch_and_add(&g_ops, 1);
    }
    return NULL;
}

/* ---------- GC 线程：周期性地"搬迁"一些页 ---------- */

/* uffd 版本：解除映射旧页 → 压缩期间访问它的 mutator 被 uffd 拦截 */
static void *gc_thread_uffd(void *arg)
{
    int uffd = *(int *)arg;
    (void)uffd;
    while (!g_stop) {
        usleep(GC_INTERVAL_MS * 1000);
        for (int v = 0; v < VICTIMS_PER_CYCLE; v++) {
            long idx = rand() % HEAP_PAGES;
            struct heap_page_meta *m = &g_meta[idx];
            char *page = g_heap + idx * g_pagesize;

            pthread_mutex_lock(&m->lock);
            if (m->resident) {
                /* 1. 把页内容搬到"新位置"（备份区） */
                memcpy(g_backup + idx * g_pagesize, page, g_pagesize);
                /* 2. 解除映射旧页帧：此后访问本页 → uffd 缺页事件 */
                if (madvise(page, g_pagesize, MADV_DONTNEED))
                    die("madvise DONTNEED");
                m->resident = 0;
            }
            /* 3. 模拟压缩耗时（真实 GC 在这里拷贝对象、修转发指针） */
            m->evacuating = 1;
            usleep(COMPACT_MS * 1000);
            m->evacuating = 0;
            pthread_mutex_unlock(&m->lock);
            /* 注意：GC 不主动恢复页面。页面保持缺页状态，
             * 直到某个 mutator 真正访问它 → fault 处理线程再从
             * "新位置"把内容提供回来（惰性恢复）。 */
        }
    }
    return NULL;
}

/* uffd fault 处理线程：对应 ART 里的 fault 处理逻辑 */
static void *fault_handler_thread(void *arg)
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

        uint64_t t0 = now_ms();
        uint64_t addr = msg.arg.pagefault.address;
        long idx = (addr - (uint64_t)g_heap) / g_pagesize;
        struct heap_page_meta *m = &g_meta[idx];

        pthread_mutex_lock(&m->lock);
        /* 若 GC 还在搬迁这一页，等它搬完（此时 faulting mutator
         * 正阻塞在内核里——这就是"只有踩中搬迁页的线程才等待"） */
        while (m->evacuating) {
            pthread_mutex_unlock(&m->lock);
            usleep(100);
            pthread_mutex_lock(&m->lock);
        }
        uint64_t page_addr = (uint64_t)g_heap + idx * g_pagesize;
        if (!m->resident) {
            /* 从"新位置"把页内容提供回来，并唤醒 mutator
             *（UFFDIO_COPY 缺省行为即拷贝后唤醒等待线程） */
            struct uffdio_copy copy = {
                .dst = page_addr,
                .src = (uint64_t)(g_backup + idx * g_pagesize),
                .len = g_pagesize,
                .mode = 0,
            };
            if (ioctl(uffd, UFFDIO_COPY, &copy) < 0) {
                /* 两个线程同时踩同一页会产生两条事件，第二条到达时
                 * 页面可能已被恢复（EEXIST），属正常竞争，忽略即可 */
                if (errno != EEXIST)
                    die("UFFDIO_COPY");
            } else {
                m->resident = 1;
            }
        }
        pthread_mutex_unlock(&m->lock);

        __sync_fetch_and_add(&g_faults, 1);
        __sync_fetch_and_add(&g_stall_ms, now_ms() - t0);
    }
    return NULL;
}

/* ---------- 对照组：STW GC ---------- */

static volatile int g_world_stopped;
static char *g_stw_heap;

static void *mutator_thread_stw(void *arg)
{
    unsigned int seed = (unsigned long)arg + 1;
    while (!g_stop) {
        while (g_world_stopped)     /* 全局停表：所有 mutator 空转 */
            ;
        long idx = rand_r(&seed) % HEAP_PAGES;
        uint64_t *page = (uint64_t *)(g_stw_heap + idx * g_pagesize);
        if (page[0] != MAGIC(idx)) {
            fprintf(stderr, "页 %ld 数据损坏\n", idx);
            exit(EXIT_FAILURE);
        }
        __sync_fetch_and_add(&page[1], 1);
        __sync_fetch_and_add(&g_ops, 1);
    }
    return NULL;
}

static void *gc_thread_stw(void *arg)
{
    (void)arg;
    while (!g_stop) {
        usleep(GC_INTERVAL_MS * 1000);
        g_world_stopped = 1;        /* 停掉全世界 */
        for (int v = 0; v < VICTIMS_PER_CYCLE; v++) {
            long idx = rand() % HEAP_PAGES;
            /* 模拟搬迁：拷贝内容 + 压缩耗时 */
            memcpy(g_backup + idx * g_pagesize,
                   g_stw_heap + idx * g_pagesize, g_pagesize);
            usleep(COMPACT_MS * 1000);
        }
        g_world_stopped = 0;
    }
    return NULL;
}

/* ---------- 阶段驱动 ---------- */

static void init_backup(void)
{
    for (long i = 0; i < HEAP_PAGES; i++) {
        uint64_t *b = (uint64_t *)(g_backup + i * g_pagesize);
        b[0] = MAGIC(i);
        b[1] = 0;                  /* 计数器初值 */
        memset(g_backup + i * g_pagesize + 16, 0xAB, g_pagesize - 16);
    }
}

static void run_threads(pthread_t *mut, void *(*mut_fn)(void *),
                        void *(*gc_fn)(void *), void *gc_arg)
{
    g_stop = 0;
    g_ops = 0;
    for (long i = 0; i < N_MUTATORS; i++)
        if (pthread_create(&mut[i], NULL, mut_fn, (void *)i))
            die("pthread_create");
    pthread_t gc;
    if (pthread_create(&gc, NULL, gc_fn, gc_arg))
        die("pthread_create gc");

    sleep(PHASE_SECONDS);
    g_stop = 1;
    for (int i = 0; i < N_MUTATORS; i++)
        pthread_join(mut[i], NULL);
    pthread_join(gc, NULL);
}

static void verify_heap(char *heap, const char *tag)
{
    long bad = 0;
    for (long i = 0; i < HEAP_PAGES; i++) {
        uint64_t *p = (uint64_t *)(heap + i * g_pagesize);
        if (p[0] != MAGIC(i))
            bad++;
    }
    printf("  数据完整性校验(%s): %s\n", tag, bad ? "失败!" : "全部通过");
    if (bad)
        exit(EXIT_FAILURE);
}

int main(void)
{
    g_pagesize = page_size();
    size_t heap_len = (size_t)HEAP_PAGES * g_pagesize;
    pthread_t mut[N_MUTATORS];

    printf("=== demo2: 并发 GC 压缩 —— STW vs userfaultfd ===\n");
    printf("模拟堆 %zu KB (%d 页), %d 个 mutator 线程, 每阶段 %d 秒\n",
           heap_len / 1024, HEAP_PAGES, N_MUTATORS, PHASE_SECONDS);
    printf("GC: 每 %d ms 一轮, 每轮搬迁 %d 页, 单页搬迁 %d ms\n\n",
           GC_INTERVAL_MS, VICTIMS_PER_CYCLE, COMPACT_MS);

    g_backup = malloc(heap_len);
    if (!g_backup)
        die("malloc backup");
    init_backup();

    /* ---------- 阶段 A：传统 stop-the-world ---------- */
    printf("[阶段 A] stop-the-world GC ...\n");
    g_stw_heap = map_anon(heap_len);
    memcpy(g_stw_heap, g_backup, heap_len);   /* STW 堆直接一次性填充 */

    uint64_t t0 = now_ms();
    run_threads(mut, mutator_thread_stw, gc_thread_stw, NULL);
    double secs = (now_ms() - t0) / 1000.0;
    printf("  mutator 总访问: %lu 次, 吞吐 %.0f 次/秒\n",
           g_ops, g_ops / secs);
    printf("  （每轮 GC 全应用暂停约 %d ms）\n",
           VICTIMS_PER_CYCLE * COMPACT_MS);
    verify_heap(g_stw_heap, "STW");

    /* ---------- 阶段 B：userfaultfd 并发压缩 ---------- */
    printf("\n[阶段 B] userfaultfd 并发压缩 GC（ART CMC GC 的方式）...\n");
    g_heap = map_anon(heap_len);
    for (long i = 0; i < HEAP_PAGES; i++) {
        pthread_mutex_init(&g_meta[i].lock, NULL);
        g_meta[i].evacuating = 0;
        g_meta[i].resident = 0;    /* 初始全部缺页，首次访问才从备份区恢复 */
    }

    int uffd = uffd_open(NULL);
    uffd_register(uffd, g_heap, heap_len, UFFDIO_REGISTER_MODE_MISSING);

    g_faults = 0;
    g_stall_ms = 0;
    g_handler_stop = 0;
    pthread_t handler;
    if (pthread_create(&handler, NULL, fault_handler_thread, &uffd))
        die("pthread_create handler");

    t0 = now_ms();
    run_threads(mut, mutator_thread, gc_thread_uffd, &uffd);
    secs = (now_ms() - t0) / 1000.0;
    printf("  mutator 总访问: %lu 次, 吞吐 %.0f 次/秒\n",
           g_ops, g_ops / secs);
    printf("  uffd 拦截缺页: %lu 次, 被搬迁页阻塞累计约 %lu ms "
           "(占比 %.2f%%)\n",
           g_faults, g_stall_ms,
           100.0 * g_stall_ms / (secs * 1000 * N_MUTATORS));

    /* 校验会触碰所有页面（包括仍处缺页状态的），需在处理线程退出前做 */
    verify_heap(g_heap, "uffd");

    g_handler_stop = 1;
    pthread_join(handler, NULL);
    close(uffd);

    printf("\n结论: STW 阶段 GC 工作时所有线程一起停；uffd 阶段只有真正\n");
    printf("踩到'正在搬迁页'的访问短暂等待, 其余访问全速进行 —— 这正是\n");
    printf("ART CMC GC 用 uffd 实现并发堆压缩、降低 GC 卡顿的原理。\n");
    return EXIT_SUCCESS;
}
