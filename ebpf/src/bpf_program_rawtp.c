/*
 * bpf_program_rawtp.c — eBPF 进程监控内核侧程序（raw tracepoint 变体，BCC 编译）
 *
 * 与 bpf_program.c（经典 tracepoint 版）功能完全对齐，但挂点全部改用
 * raw tracepoint：绕过 perf event 框架（docs/04-performance.md 2.5 节
 * 实测该层占基础设施开销 ~80%），由 tracepoint 直接分发到 BPF 程序。
 *
 * 挂点映射：
 *   tracepoint:sched:sched_process_fork   -> rawtracepoint:sched_process_fork
 *   tracepoint:sched:sched_process_exec   -> rawtracepoint:sched_process_exec
 *   tracepoint:sched:sched_process_exit   -> rawtracepoint:sched_process_exit
 *   tracepoint:sched:sched_switch         -> rawtracepoint:sched_switch
 *   tracepoint:syscalls:sys_enter_futex   -> rawtracepoint:sys_enter（按 id 过滤）
 *   tracepoint:syscalls:sys_exit_futex    -> rawtracepoint:sys_exit
 *   tracepoint:exceptions:page_fault_user -> rawtracepoint:page_fault_user
 *
 * raw 形态的三个结构性收益（经典版做不到的）：
 *  1. 参数是 tracepoint 的原始实参而非编组后的字符串记录：
 *     sched_switch 直接给 task_struct* prev/next 和 prev_state，
 *     sched_process_fork 直接给 parent/child 的 task_struct*
 *     （经典版的 child_comm __data_loc 字段在 BCC 不可见）；
 *  2. sched_switch 仍在 __schedule 切换前触发（current==prev），
 *     D 态阻塞内核栈抓栈能力与经典版一致（kprobe 变体做不到）；
 *  3. 省去每次触发的参数编组/perf 记录构造。
 *
 * 已知代价与坑（见 docs/04-performance.md 2.8 节实测）：
 *  - futex 没有专属 raw tracepoint，只能挂全系统调用的 sys_enter/sys_exit
 *    再按 id 过滤——每次系统调用都要付 raw 挂接费（~20ns 级），
 *    syscall 率远高于 futex 率时不划算，生产可保留经典 sys_enter_futex。
 *  - raw 参数原型随内核版本变化（下表为 kernel 7.0 实测；可用
 *    bpftrace -lv rawtracepoint:vmlinux:<tp> 查询）；内核在 attach 时
 *    校验 ctx 访问范围不得超出该 tracepoint 的实参个数，越界 EINVAL。
 *  - 参数不如经典版 format 文件现成，要自己跟内核版本对齐。
 *
 * 本文件使用的 raw 参数原型（kernel 7.0.0-29 实测）：
 *   sched_process_fork(parent*, child*)          sched_process_exec(p*, old_pid, bprm*)
 *   sched_process_exit(p*, group_dead)           sched_switch(preempt, prev*, next*, prev_state)
 *   sys_enter(regs*, id)  sys_exit(regs*, ret)   page_fault_user(address, regs*, error_code)
 */
#include <uapi/linux/ptrace.h>
#include <uapi/linux/bpf.h>
#include <linux/sched.h>

#ifndef TASK_UNINTERRUPTIBLE
#define TASK_UNINTERRUPTIBLE 2
#endif
#ifndef __NR_futex
#define __NR_futex 202      /* x86_64 */
#endif

/* ---------- 事件定义（与经典版完全一致，用户态逻辑复用） ---------- */
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
    s32 aux;            /* FORK: parent_pid; EXIT: 0 */
    s32 stack_id;       /* D 态进入时的内核栈 id */
    char comm[TASK_COMM_LEN];
    char comm2[TASK_COMM_LEN];  /* FORK: child_comm（raw 版终于能填了） */
};

/* ---------- 配置（用户态通过数组写入） ---------- */
enum cfg_idx {
    CFG_D_THRESH_NS = 0,
    CFG_FUTEX_THRESH_NS,
    CFG_STACK_SAMPLE,
    CFG_LOCK_THRESH_NS,
    CFG_NR,
};
BPF_ARRAY(config, u64, CFG_NR);

/* ---------- 输出与状态 ---------- */
BPF_PERF_OUTPUT(events);

struct d_entry {
    u64 ts;
    s32 stack_id;
};
BPF_TABLE("lru_hash", u32, struct d_entry, d_start, 10240);

BPF_TABLE("lru_hash", u32, u64, futex_start, 10240);

BPF_HASH(armed, u32, u8, 1024);
BPF_STACK_TRACE(stack_traces, 16384);
struct sc_key { u32 tgid; u32 stack_id; };
BPF_HASH(stack_counts, struct sc_key, u64, 16384);
BPF_PERCPU_ARRAY(pf_counter, u64, 1);

/* 持锁检测（uprobe，与经典版共用同一组处理函数） */
BPF_TABLE("lru_hash", u32, u64, lock_pending, 1024);
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

/* TP_PROTO(struct task_struct *parent, struct task_struct *child)：
   current 是父进程；child 指针直接可读 pid/comm，连经典版拿不到的
   child_comm 都有了（kernel 6.4+ 该 tracepoint 改传 task_struct） */
int raw_tp_fork(struct bpf_raw_tracepoint_args *ctx)
{
    struct task_struct *child = (struct task_struct *)ctx->args[1];
    struct event_t e = {};
    fill_event(&e, EV_FORK);
    e.pid = (u32)child->pid;
    e.tgid = (u32)child->pid;
    e.aux = (s32)bpf_get_current_pid_tgid();    /* parent pid */
    bpf_probe_read_str(&e.comm2, sizeof(e.comm2), child->comm);
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}

/* TP_PROTO(struct task_struct *p, pid_t old_pid, struct linux_binprm *bprm)
   触发时 current 即 exec 后任务，comm 已是新名 */
int raw_tp_exec(struct bpf_raw_tracepoint_args *ctx)
{
    struct event_t e = {};
    fill_event(&e, EV_EXEC);
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}

/* TP_PROTO(struct task_struct *p, bool group_dead)：current 即退出线程 */
int raw_tp_exit(struct bpf_raw_tracepoint_args *ctx)
{
    struct event_t e = {};
    fill_event(&e, EV_EXIT);
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}

/* ---------- 2. D 态过长 ---------- */

/* TP_PROTO(bool preempt, struct task_struct *prev, struct task_struct *next,
             unsigned int prev_state)
   与经典 tracepoint 同一触发点（__schedule 切换前），current==prev，
   get_stackid 抓到的就是 prev 的阻塞栈；prev_state 白送（省一次 probe read） */
int raw_tp_sched_switch(struct bpf_raw_tracepoint_args *ctx)
{
    struct task_struct *prev = (struct task_struct *)ctx->args[1];
    struct task_struct *next = (struct task_struct *)ctx->args[2];
    u32 prev_state = (u32)ctx->args[3];
    u64 now = bpf_ktime_get_ns();
    u64 *threshp, thresh = 0;

    if (prev_state == TASK_UNINTERRUPTIBLE) {
        struct d_entry ent = {};
        ent.ts = now;
        ent.stack_id = stack_traces.get_stackid(ctx, 0);
        u32 pid = (u32)prev->pid;
        d_start.update(&pid, &ent);
    }

    u32 npid = (u32)next->pid;
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
    e.tgid = npid;
    e.type = EV_DSTATE;
    e.dur_ns = delta;
    e.aux = 0;
    e.stack_id = sid;
    bpf_probe_read_str(&e.comm, sizeof(e.comm), next->comm);
    e.comm2[0] = 0;
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}

/* ---------- 3. 等锁过长（futex 阻塞） ----------
   futex 无专属 raw tracepoint，挂全系统调用入口/出口按 id 过滤 */

static inline int futex_is_waitlike(int op)
{
    switch (op & 0x7f) {
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

/* TP_PROTO(struct pt_regs *regs, long id) */
int raw_tp_sys_enter(struct bpf_raw_tracepoint_args *ctx)
{
    if ((long)ctx->args[1] != __NR_futex)
        return 0;
    struct pt_regs *regs = (struct pt_regs *)ctx->args[0];
    int op;
    bpf_probe_read_kernel(&op, sizeof(op), &regs->si);
    if (!futex_is_waitlike(op))
        return 0;
    u32 pid = (u32)(bpf_get_current_pid_tgid());
    u64 ts = bpf_ktime_get_ns();
    futex_start.update(&pid, &ts);
    return 0;
}

/* TP_PROTO(struct pt_regs *regs, long ret) */
int raw_tp_sys_exit(struct bpf_raw_tracepoint_args *ctx)
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
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}

/* ---------- 4. 泄漏堆栈抓取（armed 进程的用户态页错误） ---------- */

/* TP_PROTO(unsigned long address, struct pt_regs *regs, unsigned int error_code)
   逻辑与经典版一致：armed 检查 -> 采样 -> 抓用户栈聚合 */
int raw_tp_page_fault(struct bpf_raw_tracepoint_args *ctx)
{
    u64 id = bpf_get_current_pid_tgid();
    u32 tgid = (u32)(id >> 32);

    u8 *a = armed.lookup(&tgid);
    if (!a || !*a)
        return 0;

    u32 zero = 0;
    u64 *c = pf_counter.lookup(&zero);
    if (!c)
        return 0;
    (*c)++;
    u64 *sp = config.lookup(&(u32){CFG_STACK_SAMPLE});
    u64 sample = (sp && *sp) ? *sp : 1;
    if (*c % sample != 0)
        return 0;

    int sid = stack_traces.get_stackid(ctx, BPF_F_USER_STACK);
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
    e.aux = (s32)(k.mutex & 0x7fffffff);
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}
