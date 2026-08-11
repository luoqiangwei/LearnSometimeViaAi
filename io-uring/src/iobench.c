#define _GNU_SOURCE
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "用法: %s --pattern=P --engine=E [选项]\n"
        "  --pattern=randread|seqread|logwrite   IO 模式\n"
        "  --engine=pread|pread_mt|preadv2_nowait|mmap|uring|aio_direct|aio_buffered|posix_aio\n"
        "             |write_sync|write_batch|uring_logwrite\n"
        "  --file=PATH        测试文件(必须已存在且大小为 --fsize)\n"
        "  --fsize=BYTES      文件大小(默认 256MiB)\n"
        "  --bs=BYTES         每次 IO 大小(默认 4096)\n"
        "  --ops=N            IO 总次数/logwrite 为记录数(默认 20000)\n"
        "  --depth=N          uring 队列深度 / pread_mt 线程数 / 写批大小(默认 32)\n"
        "  --sqpoll           uring 启用 SQPOLL 内核轮询\n"
        "  --fixed-buf        uring 使用注册缓冲区(READ_FIXED)\n"
        "  --drop-caches      运行前清页缓存(冷读)\n",
        prog);
}

int main(int argc, char **argv)
{
    config_t cfg = {
        .pattern = NULL, .engine = NULL, .file = NULL,
        .file_size = 256ull << 20, .bs = 4096, .ops = 20000,
        .depth = 32, .sqpoll = 0, .fixed_buf = 0,
    };
    int do_drop = 0;

    static struct option opts[] = {
        {"pattern",     required_argument, 0, 'p'},
        {"engine",      required_argument, 0, 'e'},
        {"file",        required_argument, 0, 'f'},
        {"fsize",       required_argument, 0, 's'},
        {"bs",          required_argument, 0, 'b'},
        {"ops",         required_argument, 0, 'n'},
        {"depth",       required_argument, 0, 'd'},
        {"sqpoll",      no_argument,       0, 'q'},
        {"fixed-buf",   no_argument,       0, 'x'},
        {"drop-caches", no_argument,       0, 'c'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "p:e:f:s:b:n:d:qxch", opts, NULL)) != -1) {
        switch (c) {
        case 'p': cfg.pattern = optarg; break;
        case 'e': cfg.engine = optarg; break;
        case 'f': cfg.file = optarg; break;
        case 's': cfg.file_size = strtoull(optarg, NULL, 0); break;
        case 'b': cfg.bs = strtoul(optarg, NULL, 0); break;
        case 'n': cfg.ops = strtol(optarg, NULL, 0); break;
        case 'd': cfg.depth = atoi(optarg); break;
        case 'q': cfg.sqpoll = 1; break;
        case 'x': cfg.fixed_buf = 1; break;
        case 'c': do_drop = 1; break;
        default: usage(argv[0]); return 2;
        }
    }
    if (!cfg.pattern || !cfg.engine || !cfg.file) {
        usage(argv[0]);
        return 2;
    }

    if (do_drop)
        drop_caches();

    result_t res = {0};
    int ret = -1;
    if (strcmp(cfg.engine, "pread") == 0)
        ret = run_pread(&cfg, &res);
    else if (strcmp(cfg.engine, "pread_mt") == 0)
        ret = run_pread_mt(&cfg, &res);
    else if (strcmp(cfg.engine, "mmap") == 0)
        ret = run_mmap(&cfg, &res);
    else if (strcmp(cfg.engine, "uring") == 0)
        ret = run_uring(&cfg, &res);
    else if (strcmp(cfg.engine, "aio_direct") == 0)
        ret = run_libaio(&cfg, &res, 1);
    else if (strcmp(cfg.engine, "aio_buffered") == 0)
        ret = run_libaio(&cfg, &res, 0);
    else if (strcmp(cfg.engine, "posix_aio") == 0)
        ret = run_paio(&cfg, &res);
    else if (strcmp(cfg.engine, "preadv2_nowait") == 0)
        ret = run_preadv2_nowait(&cfg, &res);
    else if (strcmp(cfg.engine, "write_sync") == 0)
        ret = run_write_sync(&cfg, &res);
    else if (strcmp(cfg.engine, "write_batch") == 0)
        ret = run_write_batch(&cfg, &res);
    else if (strcmp(cfg.engine, "uring_logwrite") == 0)
        ret = run_uring_logwrite(&cfg, &res);
    else {
        fprintf(stderr, "未知引擎: %s\n", cfg.engine);
        usage(argv[0]);
        return 2;
    }

    if (ret == 0)
        result_print(&res);
    return ret == 0 ? 0 : 1;
}
