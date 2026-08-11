#ifndef IOBENCH_COMMON_H
#define IOBENCH_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <sys/resource.h>

/* 单个引擎跑出来的结果 */
typedef struct {
    const char *name;        /* 引擎标签 */
    long ops;                /* 完成的 IO 次数 */
    size_t bs;               /* 每次 IO 字节数 */
    int depth;               /* 并发深度 */
    double wall_s;           /* 墙钟时间 */
    double iops;
    double bw_mibs;
    double lat_avg_us;
    double lat_p50_us;
    double lat_p99_us;
    double cpu_us_per_kop;   /* 每 1000 次 IO 消耗的 CPU(us+sys) 微秒数 */
    long   syscalls;         /* 关键系统调用总次数, -1 表示未统计 */
    long   pagefaults;       /* minor+major page fault 增量 */
} result_t;

/* 运行配置, 由 iobench.c 解析命令行得到 */
typedef struct {
    const char *pattern;  /* randread | seqread | logwrite */
    const char *engine;   /* pread | pread_mt | mmap | uring | write_sync | write_batch */
    const char *file;
    uint64_t file_size;   /* 测试文件大小(读场景) */
    size_t bs;
    long ops;
    int depth;
    int sqpoll;           /* uring: 是否启用 SQPOLL */
    int fixed_buf;        /* uring: 是否使用注册缓冲区 */
} config_t;

uint64_t now_ns(void);
uint64_t rand64(uint64_t *state);
int  prepare_file(const char *path, uint64_t size);
void drop_caches(void);
void *xaligned(size_t alignment, size_t size);

/* 用 latency 数组(lat[i]=ns)和 rusage 差值填充 result */
void result_finish(result_t *r, uint64_t *lat, long nops, size_t bs,
                   uint64_t wall_ns, const struct rusage *ru0, const struct rusage *ru1);
void result_print(const result_t *r);

/* 生成 [0, blocks) 均匀随机偏移表(固定种子, 保证各引擎访问序列一致) */
uint64_t *gen_offsets(long nops, uint64_t blocks, size_t bs, int sequential);

/* engine 入口, 返回 0 成功 */
int run_pread(const config_t *cfg, result_t *res);
int run_pread_mt(const config_t *cfg, result_t *res);
int run_mmap(const config_t *cfg, result_t *res);
int run_uring(const config_t *cfg, result_t *res);
int run_write_sync(const config_t *cfg, result_t *res);
int run_write_batch(const config_t *cfg, result_t *res);
int run_uring_logwrite(const config_t *cfg, result_t *res);
int run_libaio(const config_t *cfg, result_t *res, int direct);
int run_paio(const config_t *cfg, result_t *res);
int run_preadv2_nowait(const config_t *cfg, result_t *res);

#endif
