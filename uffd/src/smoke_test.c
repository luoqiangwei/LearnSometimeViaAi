/* 最小 uffd 冒烟测试：注册一页，制造一次缺页，看事件是否到达并被服务。
 * 每步打印到 stderr（无缓冲），用于定位环境问题。 */
#include "uffd_common.h"

static char *base;
static long ps;

static void *handler(void *arg)
{
    int uffd = *(int *)arg;
    fprintf(stderr, "[handler] 启动，poll 等待事件...\n");
    struct pollfd pfd = { .fd = uffd, .events = POLLIN };
    int pr = poll(&pfd, 1, 5000);
    fprintf(stderr, "[handler] poll 返回 %d (revents=%x)\n", pr, pfd.revents);
    if (pr <= 0)
        return NULL;
    struct uffd_msg msg;
    ssize_t n = read(uffd, &msg, sizeof(msg));
    fprintf(stderr, "[handler] read 返回 %zd, event=%d addr=%llx flags=%llx\n",
            n, msg.event, msg.arg.pagefault.address,
            (unsigned long long)msg.arg.pagefault.flags);
    if (n != sizeof(msg))
        return NULL;

    void *buf = malloc(ps);
    memset(buf, 0x5A, ps);
    struct uffdio_copy c = {
        .dst = msg.arg.pagefault.address & ~(uint64_t)(ps - 1),
        .src = (uint64_t)buf,
        .len = ps,
        .mode = 0,
    };
    int r = ioctl(uffd, UFFDIO_COPY, &c);
    fprintf(stderr, "[handler] UFFDIO_COPY 返回 %d (errno=%d copy=%lld)\n",
            r, errno, (long long)c.copy);
    return NULL;
}

int main(void)
{
    ps = page_size();
    base = map_anon(ps * 4);
    int uffd = uffd_open(NULL);
    fprintf(stderr, "[main] uffd fd=%d, 注册 %p\n", uffd, base);
    uffd_register(uffd, base, ps * 4, UFFDIO_REGISTER_MODE_MISSING);
    fprintf(stderr, "[main] 注册成功，创建处理线程\n");

    pthread_t t;
    pthread_create(&t, NULL, handler, &uffd);
    usleep(200000);   /* 确保 handler 先进入 poll */

    fprintf(stderr, "[main] 触发缺页（读 %p）...\n", base + ps);
    volatile unsigned char v = base[ps];   /* 第一次读第二页 → 应触发 uffd 事件 */
    fprintf(stderr, "[main] 缺页已解决，读到 %#x（期望 0x5a）\n", v);

    pthread_join(t, NULL);
    fprintf(stderr, "[main] 完成 %s\n", v == 0x5a ? "✓" : "✗ 数据不符");
    return v == 0x5a ? 0 : 1;
}
