/*
 * bpf_program.c — eBPF 进程监控内核侧程序（BCC 编译）
 *
 * 功能：
 *  1. 进程/线程生命周期事件：sched_process_fork / exec / exit
 *  2. D 态（不可中断睡眠）过长检测：sched_switch 进出 D 态计时，
 *     进入 D 态时抓取内核栈（定位阻塞点）
 *  3. 等锁（futex 阻塞）过长检测：sys_enter/exit_futex 计时
 *  4. 内存泄漏堆栈抓取：被 armed 的进程，其用户态页错误按采样率
 *     抓取用户栈并聚合计数（每次 fault ≈ 4KB RSS 增长）
 *  5. 持锁过长检测（uprobe）：pthread_mutex_lock/unlock 配对计时
 */
#include <uapi/linux/ptrace.h>
#include <linux/sched.h>

#ifndef TASK_UNINTERRUPTIBLE
#define TASK_UNINTERRUPTIBLE 2
#endif

/* ---------- 事件定义 ---------- */
enum ev_type {
    EV_FORK = 1,
    EV_EXEC,
    EV_EXIT,
    EV_DSTATE,
    EV_FUTEX_WAIT,
    EV_LOCK_HOLD,
};

struct event_t {
    u64 ts;             /* bpf_ktime_get_ns() */
    u32 pid;            /* 线程 id */
    u32 tgid;           /* 进程 id */
    u32 type;           /* enum ev_type */
    u64 dur_ns;         /* D 态/等锁/持锁 时长 */
    s32 aux;            /* FORK: child_pid; EXIT: 0 */
    s32 stack_id;       /* D 态进入时的内核栈 id */
    char comm[TASK_COMM_LEN];
    char comm2[TASK_COMM_LEN];  /* FORK: child_comm */
};

/* ---------- 配置（用户态通过数组写入） ---------- */
enum cfg_idx {
    CFG_D_THRESH_NS = 0,    /* D 态阈值 */
    CFG_FUTEX_THRESH_NS,    /* 等锁阈值 */
    CFG_STACK_SAMPLE,       /* 页错误抓栈采样率（每 N 次抓 1 次） */
    CFG_LOCK_THRESH_NS,     /* 持锁阈值 */
    CFG_NR,
};
BPF_ARRAY(config, u64, CFG_NR);

/* ---------- 输出与状态 ---------- */
BPF_PERF_OUTPUT(events);

/* D 态进入时间 + 当时的内核栈 */
struct d_entry {
    u64 ts;
    s32 stack_id;
};
/* 注：BCC 0.35 已移除 BPF_LRU_HASH 宏，LRU map 用 BPF_TABLE("lru_hash", ...) */
BPF_TABLE("lru_hash", u32, struct d_entry, d_start, 10240);

/* futex 进入阻塞的时间 */
BPF_TABLE("lru_hash", u32, u64, futex_start, 10240);

/* 泄漏抓栈：armed 进程集合 + 栈聚合 */
BPF_HASH(armed, u32, u8, 1024);
BPF_STACK_TRACE(stack_traces, 16384);
struct sc_key { u32 tgid; u32 stack_id; };
BPF_HASH(stack_counts, struct sc_key, u64, 16384);
BPF_PERCPU_ARRAY(pf_counter, u64, 1);

/* 持锁检测（uprobe） */
BPF_TABLE("lru_hash", u32, u64, lock_pending, 1024);   /* pid -> mutex 地址（lock 入口暂存） */
struct lock_key { u32 pid; u64 mutex; };
BPF_TABLE("lru_hash", struct lock_key, u64, lock_hold_start, 10240);

static inline void fill_event(struct event_t *e, u32 type)
{
    u64 id = bpf_get_current_pid_tgid();
    e->ts = bpf_ktime_get_ns();
    e->pid = (u32)id;
    e->tgid = (u32)(id >> 32);
    e->type = type;
    e->dur_ns = 0;
    e->aux = 0;
    e->stack_id = -1;
    bpf_get_current_comm(&e->comm, sizeof(e->comm));
    e->comm2[0] = 0;
}

/* ---------- 1. 进程/线程生命周期 ---------- */

TRACEPOINT_PROBE(sched, sched_process_fork)
{
    struct event_t e = {};
    fill_event(&e, EV_FORK);
    /* args: parent_pid, child_pid（child_comm 是 __data_loc 字段，
       BCC 生成的结构体中不可直接访问，comm 由用户态从 /proc 补充） */
    e.pid = args->child_pid;
    e.tgid = args->child_pid;
    e.aux = args->parent_pid;
    events.perf_submit(args, &e, sizeof(e));
    return 0;
}

TRACEPOINT_PROBE(sched, sched_process_exec)
{
    struct event_t e = {};
    fill_event(&e, EV_EXEC);    /* comm 已是 exec 后的新名字 */
    events.perf_submit(args, &e, sizeof(e));
    return 0;
}

TRACEPOINT_PROBE(sched, sched_process_exit)
{
    /* 触发时 current 就是退出线程，fill_event 填的 pid/tgid 即所求；
       该 tracepoint 的字段只有 comm/pid/prio/group_dead，没有 tgid */
    struct event_t e = {};
    fill_event(&e, EV_EXIT);
    events.perf_submit(args, &e, sizeof(e));
    return 0;
}

/* ---------- 2. D 态过长 ---------- */

TRACEPOINT_PROBE(sched, sched_switch)
{
    u64 now = bpf_ktime_get_ns();
    u64 *threshp, thresh = 0;

    /* prev 任务离开 CPU：若进入 D 态，记录时刻并抓内核栈。
       此时 current 仍是 prev，get_stackid 抓到的就是它的阻塞栈。 */
    if (args->prev_state == TASK_UNINTERRUPTIBLE) {
        struct d_entry ent = {};
        ent.ts = now;
        ent.stack_id = stack_traces.get_stackid(args, 0);
        u32 pid = (u32)args->prev_pid;
        d_start.update(&pid, &ent);
    }

    /* next 任务上 CPU：若之前记录过 D 态，说明它睡醒了，计算睡了多久 */
    u32 npid = (u32)args->next_pid;
    struct d_entry *ent = d_start.lookup(&npid);
    if (!ent)
        return 0;
    u64 delta = now - ent->ts;
    s32 sid = ent->stack_id;
    d_start.delete(&npid);

    threshp = config.lookup(&(u32){CFG_D_THRESH_NS});
    if (threshp)
        thresh = *threshp;
    if (delta <= thresh)
        return 0;

    struct event_t e = {};
    e.ts = now;
    e.pid = npid;
    e.tgid = npid;      /* sched_switch 没有 next 的 tgid，用户态按 /proc 归并 */
    e.type = EV_DSTATE;
    e.dur_ns = delta;
    e.stack_id = sid;
    bpf_probe_read_str(&e.comm, sizeof(e.comm), args->next_comm);
    events.perf_submit(args, &e, sizeof(e));
    return 0;
}

/* ---------- 3. 等锁过长（futex 阻塞） ---------- */

static inline int futex_is_waitlike(int op)
{
    switch (op & 0x7f) {    /* 去掉 FUTEX_PRIVATE_FLAG 等标志位 */
    case 0:                 /* FUTEX_WAIT */
    case 6:                 /* FUTEX_LOCK_PI */
    case 9:                 /* FUTEX_WAIT_BITSET */
    case 11:                /* FUTEX_WAIT_REQUEUE_PI */
    case 13:                /* FUTEX_LOCK_PI2 */
        return 1;
    default:
        return 0;
    }
}

TRACEPOINT_PROBE(syscalls, sys_enter_futex)
{
    if (!futex_is_waitlike(args->op))
        return 0;
    u32 pid = (u32)(bpf_get_current_pid_tgid());
    u64 ts = bpf_ktime_get_ns();
    futex_start.update(&pid, &ts);
    return 0;
}

TRACEPOINT_PROBE(syscalls, sys_exit_futex)
{
    u32 pid = (u32)(bpf_get_current_pid_tgid());
    u64 *tsp = futex_start.lookup(&pid);
    if (!tsp)
        return 0;
    u64 delta = bpf_ktime_get_ns() - *tsp;
    futex_start.delete(&pid);

    u64 *threshp = config.lookup(&(u32){CFG_FUTEX_THRESH_NS});
    u64 thresh = threshp ? *threshp : 0;
    if (delta <= thresh)
        return 0;

    struct event_t e = {};
    fill_event(&e, EV_FUTEX_WAIT);
    e.dur_ns = delta;
    events.perf_submit(args, &e, sizeof(e));
    return 0;
}

/* ---------- 4. 泄漏堆栈抓取（armed 进程的用户态页错误） ---------- */

TRACEPOINT_PROBE(exceptions, page_fault_user)
{
    u64 id = bpf_get_current_pid_tgid();
    u32 tgid = (u32)(id >> 32);

    u8 *a = armed.lookup(&tgid);
    if (!a || !*a)
        return 0;

    /* 采样：每 N 次 fault 抓一次 */
    u32 zero = 0;
    u64 *c = pf_counter.lookup(&zero);
    if (!c)
        return 0;
    (*c)++;
    u64 *sp = config.lookup(&(u32){CFG_STACK_SAMPLE});
    u64 sample = (sp && *sp) ? *sp : 1;
    if (*c % sample != 0)
        return 0;

    int sid = stack_traces.get_stackid(args, BPF_F_USER_STACK);
    if (sid < 0)
        return 0;

    struct sc_key key = { .tgid = tgid, .stack_id = (u32)sid };
    u64 init = 1;
    u64 *cnt = stack_counts.lookup_or_init(&key, &init);
    if (cnt)
        (*cnt)++;
    return 0;
}

/* ---------- 5. 持锁过长（pthread_mutex uprobe，仅按 pid 挂载） ---------- */

int uprobe_mutex_lock_entry(struct pt_regs *ctx)
{
    u32 pid = (u32)(bpf_get_current_pid_tgid());
    u64 mutex = (u64)PT_REGS_PARM1(ctx);
    lock_pending.update(&pid, &mutex);
    return 0;
}

int uprobe_mutex_lock_ret(struct pt_regs *ctx)
{
    u32 pid = (u32)(bpf_get_current_pid_tgid());
    u64 *mp = lock_pending.lookup(&pid);
    if (!mp)
        return 0;
    struct lock_key k = { .pid = pid, .mutex = *mp };
    lock_pending.delete(&pid);
    u64 ts = bpf_ktime_get_ns();
    lock_hold_start.update(&k, &ts);
    return 0;
}

int uprobe_mutex_unlock_entry(struct pt_regs *ctx)
{
    u32 pid = (u32)(bpf_get_current_pid_tgid());
    struct lock_key k = { .pid = pid, .mutex = (u64)PT_REGS_PARM1(ctx) };
    u64 *tsp = lock_hold_start.lookup(&k);
    if (!tsp)
        return 0;
    u64 delta = bpf_ktime_get_ns() - *tsp;
    lock_hold_start.delete(&k);

    u64 *threshp = config.lookup(&(u32){CFG_LOCK_THRESH_NS});
    u64 thresh = threshp ? *threshp : 0;
    if (delta <= thresh)
        return 0;

    struct event_t e = {};
    fill_event(&e, EV_LOCK_HOLD);
    e.dur_ns = delta;
    e.aux = (s32)(k.mutex & 0x7fffffff);   /* 低 31 位仅作标识 */
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}
