#define _GNU_SOURCE
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

/* mmap + memcpy: 传统"零系统调用"读法, 代价是页错误 */
int run_mmap(const config_t *cfg, result_t *res)
{
    int fd = open(cfg->file, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "open: %s\n", strerror(errno)); return -1; }

    int sequential = strcmp(cfg->pattern, "seqread") == 0;
    uint64_t blocks = cfg->file_size / cfg->bs;
    uint64_t *offs = gen_offsets(cfg->ops, blocks, cfg->bs, sequential);
    uint64_t *lat = malloc(sizeof(uint64_t) * cfg->ops);
    char *buf = xaligned(4096, cfg->bs);

    const char *map = mmap(NULL, cfg->file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "mmap: %s\n", strerror(errno));
        free(offs); free(lat); free(buf); close(fd);
        return -1;
    }
    madvise((void *)map, cfg->file_size, sequential ? MADV_SEQUENTIAL : MADV_RANDOM);

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    uint64_t t0 = now_ns();
    for (long i = 0; i < cfg->ops; i++) {
        uint64_t s = now_ns();
        memcpy(buf, map + offs[i], cfg->bs);
        lat[i] = now_ns() - s;
    }
    uint64_t wall = now_ns() - t0;
    getrusage(RUSAGE_SELF, &ru1);

    res->name = "mmap";
    res->bs = cfg->bs;
    res->depth = 1;
    res->syscalls = 2; /* mmap + madvise, 代价转移到了 page fault */
    result_finish(res, lat, cfg->ops, cfg->bs, wall, &ru0, &ru1);

    munmap((void *)map, cfg->file_size);
    free(offs); free(lat); free(buf); close(fd);
    return 0;
}
