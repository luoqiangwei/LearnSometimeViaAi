# 04 性能开销测量：方法、数据与结论

> 核心问题：启用 eBPF 监控后，对整个系统的运行有多大影响？
> 本文给出可复现的测量方法（`bench/bench_overhead.py`）、实测数据、
> 以及如何把本机结论外推到生产环境。

## 1. 测量方法学

### 1.1 开销的三个层次

谈"eBPF 开销"必须分层，否则数字没有意义：

| 层次 | 是什么 | 测量手段 |
|---|---|---|
| **L1 BPF 程序执行** | 每次事件触发时 BPF 程序在内核路径上消耗的时间 | `kernel.bpf_stats_enabled=1` + `bpftool prog show` 的 `run_time_ns/run_cnt` |
| **L2 系统吞吐影响** | 挂载全部 hook 后，典型负载的吞吐下降 | 微基准 A/B 对比（baseline vs loaded） |
| **L3 用户态 agent 消耗** | 读 perf buffer、符号化、轮询等用户态 CPU/内存 | 观测 agent 进程自身；与核数强相关 |

### 1.2 微基准与对应 hook

每个基准针对一个 hook 点，外加一个无 hook 的对照组估计噪声：

| 基准 | 负载 | 对应 hook |
|---|---|---|
| bench_fork | 5000 次 fork+wait | sched_process_fork/exit |
| bench_ctxsw | 管道 ping-pong 60000 往返 | sched_switch（每次切换都触发） |
| bench_futex | raw futex ping-pong 60000 轮 | sys_enter/exit_futex |
| bench_fault | mmap 16MB 逐页写 × 300 轮 | page_fault_user（+ armed 抓栈） |
| bench_syscall | getpid × 4M（对照组，无 hook） | — |

### 1.3 三种测量状态

- **baseline**：系统无 tracker；
- **loaded**：tracker 挂载全部 BPF 程序但不 armed 任何进程——这是
  "常态监控"的开销；
- **armed**：tracker 对 bench_fault 打开抓栈（阈值 1 触发后的状态）——
  这是"泄漏处置中"的开销，分采样率 1/1 与 1/16 两档。

### 1.4 消噪手段（都是实测踩出来的）

1. **交错采集**：baseline 块与 loaded 块逐轮交替，消除"系统随时间变热"
   带来的系统性偏差（未交错时 fork 基准出现过 loaded 比 baseline 快 20%
   的假象）；
2. **warmup 轮丢弃**：每种状态首轮丢弃，消除二进制冷加载；
3. **中位数**：每配置 N 轮取中位数；
4. **对照组**：getpid 无 hook，其"开销"即环境噪声水平；
5. **bench_fault 必须用 raw mmap/munmap**：初版用 malloc/free，glibc 的
   动态 mmap 阈值会让后续分配复用 arena，名义页错误率虚高 10 倍以上。

## 2. 实测数据

环境：KVM 虚机，2 vCPU / 1.5GB RAM，kernel 7.0.0-29，BCC 0.35。

### 2.1 L1：BPF 程序单次执行耗时（bpftool 统计）

| BPF 程序 | 平均耗时（多次测量区间） | 说明 |
|---|---|---|
| sched_switch | ~150-240 ns/次 | 每次上下文切换都执行 |
| sys_enter_futex | ~135-140 ns/次 | |
| sys_exit_futex | ~175-180 ns/次 | |
| page_fault_user（未 armed） | ~90-110 ns/次 | armed 检查后直接返回 |
| sched_process_fork | ~4.0-4.3 µs/次 | 含 perf_submit，但 fork 是低频事件 |
| sched_process_exit | ~1.5-1.8 µs/次 | 同上 |

**换算示例**：一台 32 核机器每秒 100 万次上下文切换（已属高负载），
sched_switch hook 总耗时 = 1M × 150ns = 150ms/秒，摊到 32 核 ≈ **0.5%
单核**。这就是"tracepoint 级开销"的直观量级。

### 2.2 L2：系统吞吐影响（交错法，3 轮中位数）

| 基准 | 对应 hook | baseline (ops/s) | loaded (ops/s) | 开销 |
|---|---|---|---|---|
| fork/exit 吞吐 | sched_process_fork/exit | 6112 | 7594 | -19.5%* |
| 上下文切换吞吐 | sched_switch | 119910 | 109994 | +9.0% |
| futex 操作吞吐 | sys_enter/exit_futex | 231951 | 217638 | +6.6% |
| 页错误吞吐 | page_fault_user | 458595 | 423230 | +8.4% |
| getpid 吞吐（对照组，无 hook） | — | 2233376 | 2037176 | +9.6% |

\* fork 基准反复出现 loaded 快于 baseline 的反常值：交错采集下仍稳定
出现，倾向归因于 KVM vCPU 的频率/调度效应（tracker 常驻使 vCPU 保持
活跃，fork 这种纯 CPU 型负载对频率最敏感），而非 eBPF 让 fork 变快。

**读法**：对照组（无任何 hook）都有 +9.6% 的"开销"，说明本机测量噪声
下限约 ±10%；ctxsw / futex / 页错误三项 loaded 开销全部落在噪声带内。
结合 L1 的单次百纳秒级数据，结论是：**常态监控的内核侧开销在本机
可测范围内与噪声不可区分**。futex 数据同时说明测量方法的价值——
未交错时该项曾测出 +65%，交错后收敛到 +6.6%。

### 2.3 L3：armed 抓栈的开销与采样率的杠杆

对 bench_fault（~45 万次页错误/秒，相当于高速泄漏）armed 后的吞吐对比
（多次独立测量取区间）：

| 状态 | 页错误吞吐 | 相对 baseline 开销 |
|---|---|---|
| baseline | ~459,000/s | — |
| armed，采样 1/1（每次页错误都抓栈） | ~316,000-322,000/s | **+26% ~ +45%** |
| armed，采样 1/16（每 16 次抓 1 次） | ~389,000-433,000/s | **≈ 噪声 ~ +18%** |

解读：

- 1/1 全采的开销是真实且可观的：`get_stackid()` 单次约 1-3µs，45 万次/秒
  的抓栈意味着 0.5-1.4 个核的持续占用，2 vCPU 机器上表现为 26-45% 的
  吞吐下降——**这就是"平时不抓、触发才抓、抓也采样"设计的原因**。
- 采样 1/16 后抓栈成本基本消失；残余开销主要来自 armed 模式下用户态
  agent 更高频的 RSS 轮询（本测试为 0.1s 间隔的全 /proc 扫描，生产环境
  用 1s 级间隔即可忽略）。

armed 抓栈是整个系统里**唯一真正昂贵**的动作：`get_stackid()` 要行走
最多 127 帧并查重写入 STACK_TRACE map，单次约 1-3µs，是 tracepoint
判定（~90ns）的几十倍。因此设计上是"平时不抓、阈值 1 触发才抓、
且采样率可调"——数据证明采样率 1/16 即可把开销压回噪声水平，而
Top10 归因所需的样本量（数千次采样）在泄漏速率下秒级就能攒够。

### 2.4 内存开销

`bpftool map show` 实测 tracker 的 BPF map 常驻内核内存合计 **~20MB**，
其中 STACK_TRACE（16384 项 × ~1KB）占 ~17MB。生产部署可按目标进程数
和栈深度调小项数；map 内存在加载时即锁定（memlock），不随使用量增长。

### 2.5 探针路径开销：bpf_stats 的边界与分解测量

`bpf_stats` 只统计 BPF 程序**本体**的执行时间（`bpf_prog_run` 内部打点），
不含"事件发生 → tracepoint/perf 框架回调 → BPF 入口"及退出返回的包装
路径。这部分用三分解法定量（`bench/bench_probe_path.py`）：同负载下分别
测 baseline（无探针）、probe-only（bpftrace 挂相同 7 个 tracepoint 但程序
体为空）、full（完整 tracker），两两相减：

| 基准 | 基础设施（进入+退出路径） | BPF 程序逻辑 | 总开销 |
|---|---|---|---|
| futex（~100 万 ops/s） | +17.7%（~177 ns/op） | +3.9%（~46 ns/op） | +22.3% |
| page_fault（~47 万 faults/s） | +5.7%（~121 ns/op） | +2.8%（~63 ns/op） | +8.6% |
| sched_switch / fork | 数据在噪声带内（±10%+），不可靠 | 同左 | 同左 |

要点：**探针基础设施开销（~120-180 ns/次）与 BPF 程序本体（46-150 ns/次）
同量级甚至更大**——评估 eBPF 开销时必须把两部分都算上，合计约
250-350 ns/次事件。即便如此，按真实系统事件率（切换 1-5 万/s）折算，
总路径开销仍在 <1% 单核量级。

**基础设施开销花在哪？两个补充实验的定位：**

① raw tracepoint vs 经典 tracepoint（sched_switch 空探针，ctxsw 绑核对比）：

| 探针形式 | 调用路径 | 开销 |
|---|---|---|
| `tracepoint:`（BCC/bpftrace 默认） | tracepoint → **perf event 框架** → BPF | ~104 ns/次 |
| `rawtracepoint:` | tracepoint → 直接 BPF 分发 | ~19 ns/次 |

差值 ~85 ns/次 ≈ **基础设施开销的 80% 消耗在 perf event 框架层**——
BCC 的 `TRACEPOINT_PROBE` 与 bpftrace 的 `tracepoint:` 都走
`perf_event_open(PERF_TYPE_TRACEPOINT)` + `PERF_EVENT_IOC_SET_BPF`，
每次触发都要做 perf_sample_data 准备、per-cpu 缓冲检查、tracepoint
参数编组（拷贝到 perf 记录）；`rawtracepoint:` 绕过这整套，直接遍历
BPF 程序数组调用。生产级 agent 优化手段：改用 raw tracepoint
（BCC `attach_raw_tracepoint` / libbpf `raw_tp`），代价是要自行解析
原始参数结构。

② 火焰图逐层占比（sys_enter_futex 空探针 + futex 压测，16009 个 bench
样本统计；syscall tracepoint 的路径与 sched 类不同，走专用包装）：

```
do_syscall_64                                   69.8%  (所有syscall必经)
└─ trace_syscall_enter                          10.0%  ← 探针路径总占比
   └─ perf_syscall_enter        (perf 框架包装)  8.5%  ← 约占总路径的 84%
      └─ perf_call_bpf_enter    (perf→BPF 桥)    3.5%
         └─ bpf_prog_<hash>     (JIT 程序本体)   0.9%
```

按"本层出现率 − 子层出现率"近似本层自身耗时，空探针下基础设施的构成：
perf 框架包装 ≈ 49%、perf→BPF 桥（含间接调用/retpoline）≈ 26%、
tracepoint 通用入口（static key + funcs 遍历 + RCU）≈ 16%、空 BPF 程序
基线 ≈ 9%。与三分解法的 177 ns/op（enter+exit 两探针）交叉验证一致
（单 enter ≈ 88 ns ≈ 火焰图测得的 10.0% × ~1µs/op）。

**火焰图（perf 采样）能看到完整路径，但有盲区**。实测采样到的一条
调用栈（sched_process_exit 事件）：

```
perf_trace_sched_process_exit        ← tracepoint 的 perf 包装（“进入路径”）
do_perf_trace_sched_process_exit
perf_trace_run_bpf_submit            ← perf 框架分发
trace_call_bpf
bpf_prog_9742..._tracepoint__sched__sched_process_exit   ← JIT 的 BPF 本体
bpf_perf_event_output_tp             ← 程序内 perf_submit helper
perf_event_output / perf_output_begin
```

- 可用：开中断上下文的探针（fork/exit/futex/page_fault 等），JIT 符号
  可解析（`bpf_prog_<hash>_<名字>`），包装层逐帧可见；
- **盲区：`sched_switch` 的回调在 `__schedule` 的关中断核心段执行**，
  软件时钟采样（cpu-clock）打不进关中断区——本实测 32006 个样本中
  sched_switch 的 bpf 帧为 0，8 个命中全部来自开中断区的低频事件。
  要覆盖这类路径需 NMI/硬件 PMU 采样（本 KVM guest 无 vPMU，未演示）；
- ftrace function graph 对 `perf_trace_*`/`trace_call_bpf` 等函数被内核
  递归保护屏蔽（`available_filter_functions` 不可见或写入被忽略），
  此路不通。

结论：定量用"bpf_stats（本体）+ 空探针分解（基础设施）"组合即可覆盖
全路径；火焰图适合展示路径结构与定位热点，但对关中断区的探针回调
有观测盲区，三种方法在本项目数据上互相印证。

### 2.6 不抓栈（loaded）开销专测

针对"平时不抓栈，到底多花多少"这个问题，用三种更确定的方法专测
（`bench/bench_nostack.py`）：

**方法 A：`bpf_stats` 直接统计 BPF 程序耗时**（不依赖吞吐对比，最确定。
注意这是**极端微基准事件率**，远超真实系统）：

| 负载（事件率） | BPF 程序 | 单次耗时 | 占单核 |
|---|---|---|---|
| 上下文切换 10.3 万次/s | sched_switch | 142 ns | 1.46% |
| futex 调用 26 万次/s | sys_enter_futex + sys_exit_futex | 99 + 131 ns | 5.97% |
| 页错误 42.5 万次/s（未 armed） | page_fault_user | 58 ns | 2.47% |
| fork+exit 7514 次/s | sched_process_fork + exit | 4662 + 1512 ns | 4.64% |

外推到真实系统：常规服务器上下文切换约 1-5 万次/s，sched_switch hook
约 0.14-0.7% 单核（32 核摊薄后 <0.03%）；fork/exit 真实频率通常
<100 次/s，占比可忽略。

**方法 B：taskset 绑核隔离的端到端吞吐**（bench 独占核 0，tracker 用户态
绑核 1，排除 agent 抢占干扰，5 轮中位数）：

| 基准 | baseline | loaded | 开销 |
|---|---|---|---|
| 上下文切换吞吐 | 403818 | 363862 | +11.0% |
| 页错误吞吐 | 455363 | 434042 | +4.9% |
| getpid（对照，无 hook） | 2236959 | 2027394 | +10.3% |

对照组都有 +10.3%（KVM 环境的噪声下限），即**不抓栈时的端到端开销
在可测范围内接近零**。

**方法 C：用户态 agent 自身 CPU 占用**：

| 场景 | tracker 用户态 CPU |
|---|---|
| 系统空闲（零星后台事件） | 1.6% 单核 |
| fork/exit 事件流 ~1.5 万事件/s | 84.9% 单核（≈57µs/事件） |

这一行解释了为什么必须用阈值控制事件率：Python agent 每核约能消化
1.7 万事件/s，常态阈值下（D 态 >500ms、futex >200ms 才上报）真实系统
事件接近零，agent 开销 ≈ 1.6%；事件风暴场景应调高阈值或为 agent 限核。

### 2.7 KProbe 与 Tracepoint 挂点开销对照

2.5 节测的是"tracepoint 这条路径内部钱花在哪"；本节回答另一个问题：
**换用 kprobe 挂同样的功能，开销是涨是跌？** 方法：用 kprobe 复现
tracker 的全部 7 个挂点（`src/bpf_program_kprobe.c`，`tracker.py --kprobe`），
同负载下对照（`bench/bench_kprobe.py`）。

**挂点映射**（kernel 7.0.0-29 实测可用）：

| tracepoint 版 | kprobe 版 | 备注 |
|---|---|---|
| sched:sched_process_fork | wake_up_new_task | arg0=子任务 task_struct* |
| sched:sched_process_exec | begin_new_exec | 实测 comm 已是新名 |
| sched:sched_process_exit | do_exit | current 即退出线程 |
| sched:sched_switch | finish_task_switch.isra.0 | prev 在 rdi；`.isra.0` 是编译器部分内联符号，**名字随内核/编译选项漂移** |
| syscalls:sys_enter_futex | __x64_sys_futex | 参数需从 pt_regs 字段二次读取 |
| syscalls:sys_exit_futex | kretprobe:__x64_sys_futex | return trampoline |
| exceptions:page_fault_user | handle_mm_fault | 略宽：内核访问用户内存的缺页也计入 |

**方法 A：bpf_stats 单次耗时**（同逻辑、同负载直接对比）：

| 负载（事件率） | tracepoint 版 | kprobe 版 | 差值 |
|---|---|---|---|
| sched_switch（~18 万次/s） | 117 ns | 151 ns | +29% |
| futex enter / exit（~32-43 万次/s） | 113 / 146 ns | 149 / 144 ns | enter +32% / **exit 持平** |
| page_fault（~42 万次/s，未 armed） | 60 ns | 59 ns | 持平 |
| fork / exit（~1.5 万次/s） | 4809 / 1524 ns | 5367 / 1602 ns | +12% / +5% |

bpf_stats 只统计 BPF 程序本体，两种方式逻辑相同，差距本来就该小；
enter 侧 +30ns 左右来自取参方式（kprobe 要 probe_read task_struct /
解析 pt_regs，tracepoint 参数由框架现成编组）。注意 **kretprobe 的
futex_exit 与 tracepoint 持平**——现代内核的 return trampoline 代价
已经很小，"kretprobe 很贵"的老经验在该内核上不成立。

**方法 B：空探针基础设施开销**（bpftrace 挂空程序，与 base 对比；
getpid 对照组指示本机噪声下限 ±10%）：

| 基准 | tracepoint 基础设施 | kprobe 基础设施 |
|---|---|---|
| sched_switch | +272 ns/op | +209 ns/op |
| futex（enter+exit 两探针合计） | +279 ns/op | +243 ns/op |
| page_fault | +143 ns/op | +108 ns/op |

**kprobe 的基础设施反而略低于 tracepoint**，机制证据与解释：

1. 所有挂点都被内核优化为 **ftrace 直接调用**：
   `/sys/kernel/debug/kprobes/list` 实测显示每个探针带 `[FTRACE]`
   标记——复用函数入口的 ftrace call site 回调，**没有 int3 断点
   陷入**。这是"kprobe 很慢"印象过时的关键：慢的是 int3 路径
   （函数不可 ftrace 或已被抢占时才退化），而可用列表
   （`available_filter_functions`）内的函数全部走 ftrace 快路径。
2. kprobe 只把 `pt_regs*` 交给 BPF；tracepoint 的 perf 路径要做
   **参数编组**——sched_switch 每次触发要把 prev/next 两个 comm
   字符串拷入 perf 记录。2.5 节测得"perf 框架层占基础设施 ~80%"
   中的相当一部分就是这类编组成本，kprobe 路径天然省掉了它。
3. 端到端 full 状态（含真实 BPF 逻辑）两者差异全部落入噪声带：
   sched_switch 吞吐 kp-full 比 tp-full 高 3%，futex 低 2.8%，
   对照组噪声即 ±10%。

**但 kprobe 赢的这点开销不值得换，功能与工程代价才是结论：**

- `__schedule`/`deactivate_task`/`dequeue_task` 等调度器核心函数被
  `notrace` 屏蔽（kprobe 黑名单），D 态检测只能退而求其次挂
  `finish_task_switch.isra.0`——触发时 prev 已切出，**抓不到它的
  阻塞内核栈**（tracepoint 版在切换前触发所以可以），kprobe 版 D 态
  事件的 stack_id 恒为 -1，定位能力打折；
- `.isra.0` 这类编译器生成的符号名**跨内核版本/编译选项不稳定**，
  tracepoint 是稳定 ABI，kprobe 挂点本质上是"扒内核实现细节"；
- tracepoint 的参数结构是文档化的 format 文件，kprobe 要自己跟进
  函数签名与 task_struct 布局（本项目就遇到 7.0 内核
  `deactivate_task` 不再走睡眠主路径的变迁）；
- 一旦挂点函数不可 ftrace，退化到 int3 路径开销是一个数量级的跳变。

**挂点选型结论**（综合 2.5 与本节）：

```
优先 tracepoint（稳定 ABI + 现成参数 + 语义精确）
  ├─ 高频点追求极致开销 → raw tracepoint（2.5 节实测 19ns vs 104ns，
  │   代价：自行解析原始参数结构）
  └─ 没有 tracepoint 覆盖 → kprobe（确认在 available_filter_functions
      内走 ftrace 快路径；做好跨内核适配；语义差异写进文档）
```

### 2.8 raw tracepoint 优化实测：perf 框架到底省了多少

2.5 节定位了基础设施开销 ~80% 在 perf event 框架层，raw tracepoint
是针对性的优化：**同一 tracepoint、同一触发点，但不经
`perf_event_open` + `PERF_EVENT_IOC_SET_BPF` 分发，由 tracepoint
直接调用 BPF 程序**。本节用完整功能变体（`src/bpf_program_rawtp.c`，
`tracker.py --raw-tp`，7 个挂点全部迁移）实测收益与代价
（`bench/bench_rawtp.py`）。

**结构证据**：raw 模式 D 态事件 `--verbose` 抓到的调用栈顶部三帧是
`__traceiter_sched_switch → bpf_trace_run4 → bpf_prog_...`——tracepoint
直接进 BPF；对照 2.5 节经典路径 `perf_trace_sched_switch →
do_perf_trace_* → perf_trace_run_bpf_submit → trace_call_bpf →
bpf_prog_...`——五帧、全在 perf 框架里绕。火焰图层面的直观证据。

**功能对齐验证**（与经典版逐项复跑）：泄漏双阈值闭环（报告 Top1 归因
94.9% 一致）、D 态 503ms 计时**含阻塞内核栈**（kprobe 变体做不到的
能力，raw 保留）、futex 计时、FORK/EXEC/EXIT。另有 raw 独有红利：
`sched_process_fork` 的 raw 实参是 `(parent*, child*)` 两个 task_struct
指针，child_comm 直接可读（经典版 `__data_loc` 字段拿不到）；
`sched_switch` 白送 `prev_state` 实参，省一次 probe read。

**方法 A：bpf_stats 单次本体耗时**（同负载直接对比，三次运行区间）：

| 负载（事件率） | 经典 tracepoint | raw tracepoint | 读法 |
|---|---|---|---|
| sched_switch（~17-20 万/s） | 113-123 ns | 132-151 ns | raw +20~30ns：多两次 task_struct 字段 probe read |
| futex enter/exit（~40-61 万/s） | 94-113 / 120-146 ns | 140 / 151 ns | raw prog 每次 syscall 都执行，含 id 过滤+regs 二次读 |
| page_fault（~41-43 万/s，未 armed） | 60-64 ns | 54-58 ns | raw 略省：不碰任何实参 |
| fork/exit（~1-1.5 万/s） | 3.2-4.7µs / 1.5-2.1µs | 4.4-4.6µs / 3.3-4.3µs | exit +2µs 但频率极低，可忽略 |
| **getpid 对照（~300 万 syscall/s）** | ≈0（专用 futex 探针不触发） | **3.0M 次/s，占单核 14.4%** | raw 无 futex 专属挂点的代价 |

**方法 B：空探针基础设施**（与 base 吞吐差，getpid 对照组指示噪声 ±10%）：

| 基准 | 经典 tp 基础设施 | raw-tp 基础设施 |
|---|---|---|
| futex（两探针合计） | +195 ns/op（+19.2%） | **+94 ns/op（+9.2%）** |
| sched_switch | 噪声带内 | +197 ns/op（+6.9%，含全 syscall 两探针的摊入） |
| page_fault | 噪声带内 | 噪声带内 |
| **getpid 对照** | +11 ns/syscall（+2.3%） | **+41 ns/syscall（+8.4%）** |

**总账与生产建议**：

- **基础设施确实省一半**（futex 路径 195→94ns/op），与 2.5 节
  "raw 19ns vs 经典 104ns"的单点测量互相印证；但 BPF 本体因取参
  方式略贵 20-40ns，**单次事件全路径净收益约 50-100ns**——在
  高频事件（sched_switch、page_fault）上值得拿。
- **futex 是个反例**：raw 没有专属 futex tracepoint，只能挂全系统
  调用的 `sys_enter/sys_exit` 再按 id 过滤——实测每个系统调用多付
  ~60ns 全路径（方法 B：raw-empty 空探针 41ns 基础设施，raw-full
  含本体 57-64ns；getpid 对照组 2.2M syscall/s 时占单核 8.4-13%，
  bpf_stats 上界 14.5%），而经典版 ≈0。syscall 率 ≫ futex 率的系统上
  是倒贴的。**生产建议混合挂法：sched 类/page_fault 迁 raw，
  futex 保留经典 `sys_enter/exit_futex`**（专用 tracepoint 对非
  futex 系统调用零开销）。
- **迁移的坑（都踩过）**：① raw 实参原型随内核版本变化——
  kernel 7.0 的 `sched_process_fork` 只给 2 个 task_struct 实参，
  按老文档读 `args[3]` 会在 **attach 时被内核校验拦下（EINVAL，
  ctx 访问不得超出实参个数）**；用 `bpftrace -lv
  rawtracepoint:vmlinux:<tp>` 先查原型再写代码；② 挂点名与经典
  tracepoint 同名但语义是"原始实参"，取 task_struct 字段要自己
  保证内核版本适配。

## 3. 结论

1. **常态监控（loaded，不抓栈）的开销可忽略**：bpf_stats 直接统计显示
   各 hook 单次执行 58-150ns（fork 这类重事件 1.5-4.7µs 但频率极低），
   极端微基准事件率下占单核 1.5-6%，常规服务器事件率下摊薄 <0.1%；
   绑核隔离后的端到端吞吐影响落入本机噪声下限（~10%）之内。
2. **测量方法本身就是结论的一部分**：futex 基准在未交错测量时曾出现
   +65% 的"开销"，交错采集后收敛到 +6.6%（与对照组噪声同带）。小核数
   虚拟机上做 eBPF 开销测量，不做交错/对照组消噪，结论可能差出一个
   数量级——这也是本测量脚本（`bench/bench_overhead.py`）可复用的价值。
3. **armed 抓栈要控制采样率**：1/1 全采在 ~45 万 faults/s 的泄漏速率下
   造成 26-45% 的吞吐下降；1/16 采样即回落至噪声带附近，且不影响
   Top10 归因（泄漏分析是"抓大头"，不需要全量样本）。
4. **内存预算约 20MB/实例**，主要来自 STACK_TRACE map，可按需调小。
5. **kprobe 与 tracepoint 开销同量级，但工程上仍应首选 tracepoint**：
   ftrace 优化让 kprobe 摆脱了 int3 时代的高开销名声（基础设施甚至
   略低，因为免了参数编组），但挂点符号易碎、参数要自己挖、关键
   函数被 notrace 屏蔽导致功能妥协（D 态栈抓不到）。
6. **高频挂点降本走 raw tracepoint，但要挑点**：raw 版基础设施省
   一半（futex 路径 195→94ns/op），单次全路径净收益 50-100ns；
   sched 类/page_fault 值得迁，futex 这类"无专属 raw 挂点、被迫
   挂全系统调用"的点保留经典专用 tracepoint（混合挂法，见 2.8）。
7. 综合：**以 tracepoint 为骨干、"平时只计数、异常才抓栈"的设计，
   可以把 eBPF 监控的系统开销压到可忽略区间**；真正需要预算的是
   触发态的采样率与 map 内存。

## 4. 优化方向（未来工作）

按"投入产出比"排序，都是本项目实测数据直接指向的优化：

1. **高频挂点迁移 raw tracepoint**（**已完成，实测数据见 2.8 节**）：
   `src/bpf_program_rawtp.c` + `tracker.py --raw-tp` 复现全部挂点，
   基础设施省一半（futex 路径 195→94ns/op），单次全路径净收益
   50-100ns。落地形态建议**混合挂法**：sched 类/page_fault 走 raw，
   futex 保留经典专用 tracepoint（raw 只能挂全系统调用，syscall
   率高时倒贴，实测 getpid 对照 +41ns/syscall）。
2. **kretprobe → fexit（BPF trampoline）**（新内核方向）：
   futex 配对计时现在用 kretprobe；`fentry/fexit` 走 BPF trampoline
   （直接 ftrace 挂接，无 kprobe 框架），开销再降一档且参数经 BTF
   直读。需要 libbpf/CO-RE 路线，BCC 支持有限——与第 4 点绑定做。
3. **armed 抓栈的进一步压缩**（已验证的杠杆继续推进）：
   采样率 1/16 已把开销压回噪声带；更进一步可在内核侧做
   "每进程每秒最多 N 次"的限速抓栈（token bucket in BPF），
   把最坏情况钉死，避免 armed 遇上 fault 风暴。
4. **BCC → libbpf + CO-RE 重写**（工程化，非性能瓶颈驱动）：
   编译时零依赖（干掉 BCC 的 clang/头文件依赖，启动从 ~10s 到毫秒级）、
   跨内核版本一次编译、可用 fentry/raw_tp 等新挂点形态。
   本项目 kprobe 变体踩过的 `.isra.0` 符号漂移问题，CO-RE + BTF
   是最彻底的解法。
5. **用户态 agent 降耗**（L3 层的已知短板）：
   事件流场景 Python agent ~57µs/事件（方法 C）。生产化时：
   内核侧先做聚合（per-cpu 计数 map，用户态按周期读汇总而非逐事件）、
   perf buffer 批量消费、或用 Rust/Go 重写 agent。
6. **观测盲区补全**（2.5 节遗留）：
   sched_switch 回调在关中断段执行，软件时钟采样看不到；生产环境
   有硬件 PMU 时用 `perf record -e cycles`（NMI 采样）即可覆盖，
   本 KVM guest 无 vPMU 未演示。

## 5. 复现

```bash
sudo python3 bench/bench_overhead.py --rounds 3    # 三层开销全量测量，写 bench/results.json
sudo python3 bench/bench_nostack.py                # 不抓栈开销专测（bpf_stats/绑核/agent CPU）
sudo python3 bench/bench_probe_path.py             # 探针路径三分解（基础设施 vs BPF 逻辑）
sudo python3 bench/bench_kprobe.py                 # kprobe vs tracepoint 挂点对照
sudo python3 bench/bench_rawtp.py                  # raw tracepoint vs 经典 tracepoint 对照
```
