# 02 系统设计：进程监控与内存泄漏双阈值追踪

> 对应代码：`src/bpf_program.c`（内核侧）、`src/tracker.py`（用户态主控）。

## 1. 总体架构

```
┌─────────────────────────────── 内核 ───────────────────────────────┐
│ sched_process_fork/exec/exit ──┐                                   │
│ sched_switch ──────────────────┤                                   │
│ sys_enter/exit_futex ──────────┼──►  event 判定 ──► events(perf) ──┼──► 用户态
│ page_fault_user ──► armed? ────┤        │                          │    tracker.py
│   └─► get_stackid(USER) ──► stack_traces / stack_counts            │
│ pthread_mutex uprobe ──────────┘        │                          │
│                                          ▼                          │
│   状态 map: d_start / futex_start / lock_* / armed / config        │
└─────────────────────────────────────────────────────────────────────┘

用户态 tracker.py：
  RSS 轮询(/proc) ─► 阈值1 ─► 写 armed map ─► BPF 开始抓栈
                └─► 阈值2 ─► 符号化 Top10 报告 ─► SIGKILL
```

设计原则：**内核只做计数与采集，决策全部在用户态**。阈值判断、进程匹配、
报告生成、杀死进程都是用户态逻辑——内核侧只保留"事件是否值得上报"的
粗过滤，保证 BPF 程序短小、可验证、低开销。

## 2. Hook 点清单与理由

| 功能 | Hook | 关键参数 | 说明 |
|---|---|---|---|
| 进程/线程创建 | `sched:sched_process_fork` | parent_pid, child_pid | `child_comm` 是 `__data_loc`，BCC 结构体不可见，只取 pid，名字由用户态补 |
| 改名/exec | `sched:sched_process_exec` | pid | 触发时 current 即新程序，comm 已是新名 |
| 退出 | `sched:sched_process_exit` | pid | 字段只有 comm/pid/prio/group_dead，**没有 tgid**；current 即退出线程 |
| D 态计时 | `sched:sched_switch` | prev_pid, prev_state, next_pid | `prev_state == TASK_UNINTERRUPTIBLE(2)` 判定 |
| 等锁计时 | `syscalls:sys_enter/exit_futex` | op | 只对 WAIT 类 op 计时（0/6/9/11/13） |
| 泄漏抓栈 | `exceptions:page_fault_user` | address, ip, error_code | armed 后按采样率抓用户栈 |
| 持锁计时 | uprobe `pthread_mutex_lock/unlock` | rdi = mutex 地址 | entry+retprobe+unlock 三点配对 |

### D 态检测的细节（sched_switch）

一次"任务进入不可中断睡眠再被唤醒"在调度器视角是两个事件：

```
任务 T 让出 CPU:  sched_switch(prev=T, prev_state=TASK_UNINTERRUPTIBLE)
                  → 记录 d_start[T] = {时刻, T 的内核栈}
任务 T 重新上 CPU: sched_switch(next=T)
                  → 时长 = now - d_start[T].ts，超阈值则上报
```

为什么在进入 D 态时抓栈而不是超阈值时：`sched_switch` 触发时 `current`
仍是让出 CPU 的 `prev`，`get_stackid()` 抓到的正是 **T 阻塞在哪里**
（如 `io_schedule_timeout`/`wait_for_completion_io_timeout`）；等 T 睡醒
再抓，拿到的只是调度器自身栈，没有定位价值。

### KProbe 变体（开销对照实验，`tracker.py --kprobe`）

为定量"kprobe 与 tracepoint 挂点的开销差异"，项目另有一份用 kprobe
复现全部 7 个挂点的内核程序（`src/bpf_program_kprobe.c`），用户态逻辑
完全复用。挂点映射与实测开销对照见 `docs/04-performance.md` 2.7 节，
这里只记功能层面的差异：

- `__schedule`/`deactivate_task`/`dequeue_task` 被内核 `notrace` 屏蔽，
  D 态检测只能挂 `finish_task_switch.isra.0`——触发时 prev 已切出，
  **抓不到它的阻塞内核栈**，kprobe 版 D 态事件 stack_id 恒为 -1；
- `.isra.0` 是编译器部分内联产生的符号名，跨内核/编译选项不稳定
  （本实验在 kernel 7.0.0-29 上验证）；tracepoint 是稳定 ABI，
  生产代码应始终首选 tracepoint；
- futex 参数需从 `__x64_sys_futex` 的 pt_regs 字段二次读取，
  不如 tracepoint 的 format 参数现成；
- 功能验证结论：泄漏双阈值闭环（armed 抓栈→Top10 报告→SIGKILL）、
  D 态计时（dm-delay 500ms 实测 503ms）、futex 计时、生命周期事件
  均与 tracepoint 版行为一致，仅 D 态内核栈缺失。

### raw tracepoint 变体（生产优化方向，`tracker.py --raw-tp`）

raw tracepoint 是"保留 tracepoint 触发语义、剥掉 perf 框架"的挂法：
同一触发点、同样稳定，但不经 `perf_event_open` 分发，实测可省掉
挂点基础设施开销的大头（数据见 `docs/04-performance.md` 2.8 节）。
`src/bpf_program_rawtp.c` 复现全部 7 个挂点，功能与经典版**完全对齐**
（含 D 态阻塞内核栈抓栈——kprobe 变体做不到的那个能力），另有两个
raw 形态独有的红利：

- `sched_process_fork` 的 raw 实参是 `(parent*, child*)` 两个
  task_struct 指针（kernel 6.4+ 改了该 tracepoint 原型），child_comm
  直接可读——经典版的 `__data_loc` 字段在 BCC 里拿不到；
- `sched_switch` 的 raw 实参 `(preempt, prev*, next*, prev_state)`
  白送 prev_state，省一次 task_struct probe read。

代价与坑（都有实测/实证）：

- futex 无专属 raw tracepoint，只能挂全系统调用的 `sys_enter/sys_exit`
  再按 `__NR_futex` 过滤——**每次系统调用都付挂接费**，syscall 率高的
  系统上可能倒贴（2.8 节 getpid 对照实测）；生产建议混合用法：
  sched/page_fault 类走 raw，futex 保留经典 sys_enter_futex；
- raw 参数原型随内核版本变化，需按 `bpftrace -lv rawtracepoint:vmlinux:<tp>`
  对齐；内核在 attach 时校验 ctx 访问范围不得超出实参个数（越界 EINVAL）。

## 3. Map 设计

| Map | 类型 | Key → Value | 用途 |
|---|---|---|---|
| `events` | PERF_OUTPUT | — | 所有告警事件推送用户态 |
| `config` | ARRAY | idx → u64 | 阈值/采样率下发（D态、futex、采样、持锁） |
| `d_start` | LRU_HASH | tid → {ts, stack_id} | D 态进入时刻+阻塞栈 |
| `futex_start` | LRU_HASH | tid → ts | futex 阻塞进入时刻 |
| `armed` | HASH | tgid → u8 | 被抓栈的进程集合（阈值1写入） |
| `stack_traces` | STACK_TRACE | stack_id → 地址序列 | 内核栈与用户栈共用 |
| `stack_counts` | HASH | {tgid, stack_id} → count | 按进程分桶的栈采样计数 |
| `pf_counter` | PERCPU_ARRAY | 0 → count | 采样率计数（每 N 次抓 1 次） |
| `lock_pending` | LRU_HASH | tid → mutex 地址 | uprobe lock 入口暂存（配对 retprobe） |
| `lock_hold_start` | LRU_HASH | {tid, mutex} → ts | 锁获取时刻（配对 unlock） |

## 4. 双阈值状态机

```
                RSS ≥ T1                      RSS ≥ T2
  NORMAL ───────────────────► ARMED ──────────────────────► KILLED/REPORTED
  (只观测)            写 armed[tgid]=1          1) 符号化+Top10报告(进程还活着!)
              BPF 开始在页错误路径抓栈     2) 写 reports/*.md
                                           3) SIGKILL（--no-kill 则仅报告）
```

要点：

- **RSS 由用户态读 `/proc/<pid>/stat`**（默认 1s 一次），不走内核 hook：
  轮询成本接近零，且阈值判断是慢速决策，没必要放在事件路径上。
- **抓栈事件源选"用户态页错误"而不是 malloc uprobe**：RSS 的增长必然
  伴随页错误（写新页），页错误栈 = 内存增长现场；uprobe malloc 只能看到
  "申请"，看不到"是否真的驻留"，且 glibc 有 arena 复用，申请≠增长。
- **采样率可调**（`--stack-sample N`）：armed 后每 N 次页错误抓 1 次栈。
  实测采样率是把开销从 +26% 压到 ≈0 的主要手段（见 04 文档）。
- 符号化必须在 kill 前完成（进程死后 maps 消失），这是流程顺序的硬约束。

## 5. 进程匹配规则（一个实测踩出来的坑）

目标进程**刚创建时，用户态还没来得及把它加入目标集合**（rescan 有周期），
此时它的 fork/exec 事件若按"目标集合"过滤会全部漏掉。因此生命周期事件
采用三级匹配，任一命中即算目标：

1. tgid ∈ targets（常规）；
2. 事件 comm 匹配 `--comm` 子串（exec 后的新进程直接命中）；
3. FORK 事件的 parent 属于目标（目标进程的线程创建必然命中）。

## 6. 已知限制（诚实清单）

- **用户栈走帧指针**：目标程序需 `-fno-omit-frame-pointer`；穿过 glibc
  的栈会丢帧（glibc 无 FP），报告中表现为 `libc.so.6+0x偏移`。要完整符号
  需 DWARF 展开（用户态 libunwind），本项目未做。
- **stack_counts 是全armed进程共享的聚合视图**（虽然 key 带 tgid 分桶），
  多进程同时 armed 时报告各自独立但 stack_traces 容量共享。
- **持锁 uprobe 需按 pid 挂载**（`--track-locks` 要求 `--pid`），且只统计
  pthread mutex 路径；自研自旋锁不走 futex/glibc 的无法观测。
- **RSS 阈值粒度是秒级**（轮询间隔），短于间隔的突发泄漏抓不住；这不是
  缺陷而是成本权衡——更细的粒度意味着更高轮询频率或内核侧 RSS hook
  （`kmem:rss_stat` tracepoint 可做，按 mm 计，工程量大）。
- D 态/FUTEX 事件里的 `tgid` 字段对线程事件是近似值，用户态按
  `/proc/<tid>/status` 的 `Tgid:` 归并到进程。

## 7. 数据流复盘：一次泄漏处置的完整时序

```
t0   leak_demo RSS 缓慢增长（页错误持续发生，未 armed，BPF 直接返回）
t1   轮询发现 RSS=64MB ≥ T1
     → armed[pid]=1；此后每次页错误按采样率 get_stackid + stack_counts++
t2   轮询发现 RSS=96MB ≥ T2
     → 读 stack_counts（按 tgid 过滤）
     → /proc/pid/maps 还在 → 逐栈符号化，按计数排序取 Top10
     → 写 reports/leak_report_<comm>_<pid>_<ts>.md
     → kill(pid, SIGKILL) → sched_process_exit 事件收尾
```
