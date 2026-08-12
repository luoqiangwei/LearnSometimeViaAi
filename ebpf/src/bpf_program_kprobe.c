/*
 * bpf_program_kprobe.c — eBPF 进程监控内核侧程序（KProbe 变体，BCC 编译）
 *
 * 与 bpf_program.c（tracepoint 版）功能对应，用于对照 kprobe 与
 * tracepoint 的开销差异。挂点映射关系：
 *
 *   tracepoint:sched:sched_process_fork   -> kprobe:wake_up_new_task
 *   tracepoint:sched:sched_process_exec   -> kprobe:begin_new_exec
 *   tracepoint:sched:sched_process_exit   -> kprobe:do_exit
 *   tracepoint:sched:sched_switch         -> kprobe:finish_task_switch.isra.0
 *   tracepoint:syscalls:sys_enter_futex   -> kprobe:__x64_sys_futex
 *   tracepoint:syscalls:sys_exit_futex    -> kretprobe:__x64_sys_futex
 *   tracepoint:exceptions:page_fault_user -> kprobe:handle_mm_fault
 *
 * 已知语义差异（见 docs/04-performance.md 2.7 节）：
 *  - D 态事件不抓内核栈：finish_task_switch 触发时 prev 已切出，
 *    get_stackid 抓不到它的阻塞栈（tracepoint 版在切换前触发所以可以）。
 *    本变体 stack_id 恒为 -1。
 *  - finish_task_switch.isra.0 是编译器部分内联产生的符号名，
 *    不同内核/编译选项下名字可能变化（本机 kernel 7.0.0-29）。
 *  - handle_mm_fault 比 page_fault_user 略宽：内核访问用户内存
 *    （copy_from_user 等）引发的缺页也会计入。
 */
#include <uapi/linux/ptrace.h>
#include <linux/sched.h>

#ifndef TASK_UNINTERRUPTIBLE
#define TASK_UNINTERRUPTIBLE 2
#endif

/* ---------- 事件定义（与 tracepoint 版完全一致，用户态逻辑复用） ---------- */
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
    s32 stack_id;       /* D 态内核栈 id（kprobe 版恒为 -1） */
    char comm[TASK_COMM_LEN];
    char comm2[TASK_COMM_LEN];
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

/* 持锁检测（uprobe，与 tracepoint 版共用同一组处理函数） */
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

/* wake_up_new_task(struct task_struct *p)：新任务首次被唤醒，
   current 是父进程，arg0 是子任务（此时 pid/comm 均已就绪） */
int kprobe_fork(struct pt_regs *ctx)
{
    struct task_struct *child = (struct task_struct *)PT_REGS_PARM1(ctx);
    struct event_t e = {};
    fill_event(&e, EV_FORK);
    e.pid = (u32)child->pid;
    e.tgid = (u32)child->pid;
    e.aux = (s32)bpf_get_current_pid_tgid();    /* parent pid */
    bpf_probe_read_str(&e.comm, sizeof(e.comm), child->comm);
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}

/* begin_new_exec(struct linux_binprm *bprm)：exec 路径，
   实测触发时 comm 已是新程序名（de_thread 之后） */
int kprobe_exec(struct pt_regs *ctx)
{
    struct event_t e = {};
    fill_event(&e, EV_EXEC);
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}

/* do_exit(long code)：current 即退出线程 */
int kprobe_exit(struct pt_regs *ctx)
{
    struct event_t e = {};
    fill_event(&e, EV_EXIT);
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}

/* ---------- 2. D 态过长 ----------
   finish_task_switch(prev) 只在 prev != next 真切换时被调用，
   触发时 current 已是 next。arg0 即 prev（无 BTF 参数信息，
   但 x86-64 调用约定保证第一参数在 rdi）。*/
int kprobe_sched_switch(struct pt_regs *ctx)
{
    struct task_struct *prev = (struct task_struct *)PT_REGS_PARM1(ctx);
    u64 now = bpf_ktime_get_ns();
    u64 *threshp, thresh = 0;

    /* prev 离开 CPU：若进入 D 态记录时刻（栈抓不到，见文件头注释） */
    if (prev->__state == TASK_UNINTERRUPTIBLE) {
        struct d_entry ent = {};
        ent.ts = now;
        ent.stack_id = -1;
        u32 pid = (u32)prev->pid;
        d_start.update(&pid, &ent);
    }

    /* current（= next）上 CPU：若记录过 D 态，计算睡了多久 */
    u64 id = bpf_get_current_pid_tgid();
    u32 npid = (u32)id;
    struct d_entry *ent = d_start.lookup(&npid);
    if (!ent)
        return 0;
    u64 delta = now - ent->ts;
    d_start.delete(&npid);

    threshp = config.lookup(&(u32){CFG_D_THRESH_NS});
    if (threshp)
        thresh = *threshp;
    if (delta <= thresh)
        return 0;

    struct event_t e = {};
    fill_event(&e, EV_DSTATE);
    e.dur_ns = delta;
    events.perf_submit(ctx, &e, sizeof(e));
    return 0;
}

/* ---------- 3. 等锁过长（futex 阻塞） ----------
   __x64_sys_futex(const struct pt_regs *regs)：系统调用包装，
   futex 参数需从 pt_regs 字段取（uaddr=di, op=si, val=dx, ...） */

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

int kprobe_futex_enter(struct pt_regs *ctx)
{
    struct pt_regs *regs = (struct pt_regs *)PT_REGS_PARM1(ctx);
    int op;
    bpf_probe_read_kernel(&op, sizeof(op), &regs->si);
    if (!futex_is_waitlike(op))
        return 0;
    u32 pid = (u32)(bpf_get_current_pid_tgid());
    u64 ts = bpf_ktime_get_ns();
    futex_start.update(&pid, &ts);
    return 0;
}

int kprobe_futex_exit(struct pt_regs *ctx)
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

/* handle_mm_fault(vma, address, flags, regs)：用户 vma 缺页主路径 */
int kprobe_page_fault(struct pt_regs *ctx)
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
