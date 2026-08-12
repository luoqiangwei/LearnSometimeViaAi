/*
 * leak_demo.c — 故意制造内存泄漏的演示程序
 *
 * 模拟一个多线程服务，包含三条代码路径：
 *   路径 A：handle_http_request -> parse_request_body -> cache_store_entry
 *           快速泄漏（约 8MB/s），malloc 后不释放
 *   路径 B：handle_ws_connection -> session_init -> session_table_insert
 *           慢速泄漏（约 0.3MB/s）
 *   路径 C：handle_health_check -> check_backend
 *           正常分配/释放，不泄漏（对照组）
 *
 * 预期效果：tracker 在 RSS 超阈值 1 时开始抓用户态堆栈，
 * Top10 报告中应能看到 cache_store_entry / session_table_insert
 * 排在前两位，且计数比例与泄漏速率一致。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

/* 模拟业务结构体：被“缓存”的对象 */
struct cache_entry {
    char payload[256 * 1024];   /* 256KB */
};

struct session {
    char state[64 * 1024];      /* 64KB */
};

static long alloc_count_a;
static long alloc_count_b;

static void msleep(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000L * 1000L };
    nanosleep(&ts, NULL);
}

/* ---------- 路径 A：快速泄漏 ---------- */

/* “把请求体放进缓存”，但永远不做淘汰——泄漏点 */
static struct cache_entry *cache_store_entry(const char *body, size_t len)
{
    struct cache_entry *e = malloc(sizeof(*e));
    if (!e)
        return NULL;
    (void)body;
    /* 必须真正写内存，否则 RSS 不会增长（页错误发生在这里） */
    memset(e->payload, 'A', len > sizeof(e->payload) ? sizeof(e->payload) : len);
    alloc_count_a++;
    return e;   /* 故意不 free，也不挂到任何表里 */
}

static void parse_request_body(const char *body, size_t len)
{
    /* 解析后“缓存结果” */
    cache_store_entry(body, len);
}

static void handle_http_request(void)
{
    parse_request_body("POST /api/orders ...", 256 * 1024);
}

static void *http_worker(void *arg)
{
    (void)arg;
    for (;;) {
        handle_http_request();
        msleep(30);     /* ~8.5MB/s */
    }
    return NULL;
}

/* ---------- 路径 B：慢速泄漏 ---------- */

/* “新会话插入会话表”，插入失败路径上漏掉了 free——泄漏点 */
static int session_table_insert(struct session *s)
{
    (void)s;
    /* 模拟表满：返回失败，调用方随后丢弃指针却不 free —— 泄漏。
       正确做法应由调用方在失败时 free(s)。 */
    return -1;
}

static void session_init(void)
{
    struct session *s = malloc(sizeof(*s));
    if (!s)
        return;
    memset(s->state, 'B', sizeof(s->state));
    alloc_count_b++;
    if (session_table_insert(s) != 0) {
        /* 正确的做法应该 free(s)，这里故意不写 */
    }
}

static void handle_ws_connection(void)
{
    session_init();
}

static void *ws_worker(void *arg)
{
    (void)arg;
    for (;;) {
        handle_ws_connection();
        msleep(200);    /* ~0.3MB/s */
    }
    return NULL;
}

/* ---------- 路径 C：正常路径（对照组） ---------- */

static void check_backend(void)
{
    void *tmp = malloc(64 * 1024);
    if (tmp) {
        memset(tmp, 0, 64 * 1024);
        free(tmp);      /* 正常释放 */
    }
}

static void handle_health_check(void)
{
    check_backend();
}

static void *health_worker(void *arg)
{
    (void)arg;
    for (;;) {
        handle_health_check();
        msleep(50);
    }
    return NULL;
}

int main(void)
{
    pthread_t t1, t2, t3;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("leak_demo pid=%d, 路径A ~8MB/s 泄漏, 路径B ~0.3MB/s 泄漏, 路径C 正常\n",
           getpid());

    pthread_create(&t1, NULL, http_worker, NULL);
    pthread_create(&t2, NULL, ws_worker, NULL);
    pthread_create(&t3, NULL, health_worker, NULL);

    for (;;) {
        sleep(5);
        printf("[leak_demo] 缓存对象=%ld 会话对象=%ld\n",
               alloc_count_a, alloc_count_b);
    }
    return 0;
}
