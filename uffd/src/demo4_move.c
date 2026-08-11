/*
 * demo4_move.c —— UFFDIO_MOVE vs UFFDIO_COPY：页面"搬家"不用真拷贝
 *
 * 背景问题：
 *   GC 压缩、内存整理、虚拟机内存热插拔等场景，本质是"把一页数据从
 *   地址 A 挪到地址 B"。UFFDIO_COPY 只能逐字节拷贝：双倍内存带宽 +
 *   双倍页帧占用（源页拷完才能释放）。
 *
 * uffd 的解法（UFFDIO_MOVE，内核 6.5+，由 Google 工程师为 Android
 * ART 的 GC 压缩场景推动合入）：
 *   直接把源页的页表项"改指"到目的地址 —— 物理页帧原地不动，零拷贝。
 *   操作完成后源地址变成未映射状态。
 *
 * 本 demo：对 64MB 数据分别做一次 UFFDIO_COPY 和一次 UFFDIO_MOVE，
 * 对比耗时，并校验数据完整性、确认 MOVE 后源区域页帧已被释放。
 */
#include "uffd_common.h"

#define REGION_SIZE (64UL * 1024 * 1024)   /* 64MB */

static long g_pagesize;

static uint64_t checksum_region(char *p, size_t len)
{
    /* 每页抽样第一个 8 字节求和，足以验证"是不是那批数据" */
    uint64_t sum = 0;
    for (size_t off = 0; off < len; off += g_pagesize)
        sum += *(uint64_t *)(p + off);
    return sum;
}

static void fill_pattern(char *p, size_t len, uint64_t seed)
{
    for (size_t off = 0; off < len; off += g_pagesize)
        *(uint64_t *)(p + off) = seed + off / g_pagesize;
}

int main(void)
{
    g_pagesize = page_size();
    size_t len = REGION_SIZE;
    int uffd = uffd_open(NULL);

    printf("=== demo4: UFFDIO_MOVE vs UFFDIO_COPY (%zu MB) ===\n",
           len / 1024 / 1024);

    /* ---------- 第 1 轮：UFFDIO_COPY ---------- */
    char *src = map_anon(len);
    fill_pattern(src, len, 0x1000);          /* 填充并驻留源页 */
    uint64_t expect = checksum_region(src, len);

    char *dst = map_anon(len);
    uffd_register(uffd, dst, len, UFFDIO_REGISTER_MODE_MISSING);

    uint64_t t0 = now_ms();
    struct uffdio_copy copy = {
        .dst = (uint64_t)dst,
        .src = (uint64_t)src,
        .len = len,
        .mode = 0,                            /* 无等待者，不用 WAKE */
    };
    if (ioctl(uffd, UFFDIO_COPY, &copy) < 0)
        die("UFFDIO_COPY");
    uint64_t copy_ms = now_ms() - t0;

    if (checksum_region(dst, len) != expect) {
        fprintf(stderr, "COPY 后数据校验失败\n");
        return EXIT_FAILURE;
    }
    printf("UFFDIO_COPY: %lu ms, 数据校验通过\n", copy_ms);

    munmap(src, len);
    munmap(dst, len);

    /* ---------- 第 2 轮：UFFDIO_MOVE ---------- */
    src = map_anon(len);
    fill_pattern(src, len, 0x1000);

    dst = map_anon(len);
    uffd_register(uffd, dst, len, UFFDIO_REGISTER_MODE_MISSING);

    long rss_before = read_rss_pages();

    t0 = now_ms();
    struct uffdio_move move = {
        .dst = (uint64_t)dst,
        .src = (uint64_t)src,
        .len = len,
        .mode = 0,
    };
    if (ioctl(uffd, UFFDIO_MOVE, &move) < 0) {
        if (errno == ENOTTY || errno == EINVAL || errno == EOPNOTSUPP) {
            fprintf(stderr, "内核不支持 UFFDIO_MOVE（需要 6.5+），跳过对比\n");
            return EXIT_SUCCESS;
        }
        die("UFFDIO_MOVE");
    }
    uint64_t move_ms = now_ms() - t0;

    if (checksum_region(dst, len) != expect) {
        fprintf(stderr, "MOVE 后数据校验失败\n");
        return EXIT_FAILURE;
    }
    long rss_after = read_rss_pages();
    printf("UFFDIO_MOVE: %lu ms, 数据校验通过\n", move_ms);

    /* MOVE 把源页帧直接"过户"给 dst：RSS 不应增长（COPY 则会翻倍） */
    printf("MOVE 前后 RSS 变化: %+ld 页（≈0 说明源页帧被直接过户，零拷贝）\n",
           rss_after - rss_before);

    if (copy_ms > 0)
        printf("\n对比: MOVE 比 COPY 快约 %.1f 倍（数据量越大差距越明显）\n",
               (double)copy_ms / (move_ms ? move_ms : 1));
    printf("UFFDIO_MOVE 正是为 ART GC 压缩而生的内核特性（6.8+），新版 ART 已采用。\n");
    return EXIT_SUCCESS;
}
