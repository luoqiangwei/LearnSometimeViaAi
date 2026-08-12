# 01 eBPF 基础：从内核虚拟机到生产观测

> 分享目标：让没有 eBPF 背景的工程师理解它是什么、为什么安全、能挂在哪、
> 以及主流工具链（BCC / libbpf CO-RE / bpftrace）怎么选。

## 1. eBPF 是什么

eBPF（extended Berkeley Packet Filter）是内核内置的一台**事件驱动虚拟机**：

- 你写一个受限制的 C 子集程序，编译成 BPF 字节码；
- 通过 `bpf(2)` 系统调用把字节码加载进内核，并声明它挂到哪个**事件源**
  （tracepoint、kprobe、uprobe、perf event、网卡 XDP 等）；
- 每次该事件发生，内核就在事件上下文中执行你的程序；
- 程序之间、程序与用户态之间通过 **BPF map**（内核态键值存储）交换数据。

与内核模块的本质区别：**安全性由验证器静态保证**。加载前验证器会模拟执行
所有路径，确认程序必然终止（有界循环/无递归）、不越界、不空指针、不泄漏
内核地址；验证不过就拒绝加载。通过后 JIT 编译成本机指令运行，性能接近
原生内核代码。

```
用户态                          内核态
┌──────────────┐   bpf()    ┌────────────────────────────┐
│  控制程序     │ ─────────► │  验证器 → JIT → 挂载到事件源 │
│ (加载/读map/  │            │                            │
│  符号化/决策) │ ◄───────── │  事件发生 → BPF程序 → map/  │
└──────────────┘  map/perf  │            perf buffer     │
                            └────────────────────────────┘
```

## 2. 程序类型与挂载点

本项目涉及的四类挂载点，也是系统观测最常用的四类：

| 挂载点 | 稳定性 | 说明 | 本项目用途 |
|---|---|---|---|
| **tracepoint** | 稳定 ABI | 内核维护者预埋的静态探测点（`TRACE_EVENT`），参数有格式描述（`/sys/kernel/debug/tracing/events/<子系统>/<名>/format`） | `sched_switch`、`sched_process_*`、`sys_enter/exit_futex`、`page_fault_user` |
| **kprobe/kretprobe** | 不稳定 | 动态挂钩几乎任意内核函数入口/返回，函数签名可能随版本变化 | （备选：`handle_mm_fault`） |
| **uprobe/uretprobe** | 视应用而定 | 挂钩用户态函数（按 inode+偏移），需要自行解析符号与 ASLR | `pthread_mutex_lock/unlock` 持锁计时 |
| **perf event** | 稳定 | 采样式（周期/计数）与 PMU 事件 | 本项目未直接用，思想相同 |

选型经验：**有 tracepoint 就不用 kprobe**。tracepoint 参数是稳定 ABI，
跨内核版本不用改；kprobe 依赖具体函数，内核重构后可能失效。查看系统里
有哪些 tracepoint：

```bash
ls /sys/kernel/debug/tracing/events/
cat /sys/kernel/debug/tracing/events/sched/sched_switch/format   # 看参数
```

### 一个容易踩的坑：`__data_loc` 字段

tracepoint 参数里的字符串（如 `sched_process_fork` 的 `child_comm`）在
format 里标为 `__data_loc char[]`——它不是固定偏移字段，而是"偏移+长度"
编码。BCC 自动生成的 tracepoint 结构体里**没有**这个成员，直接访问
`args->child_comm` 会编译报错。本项目的选择：只取 `child_pid`，字符串由
用户态从 `/proc/<pid>/comm` 补齐。

## 3. BPF map：程序与世界交换数据的唯一通道

常见类型与选型（本项目全部用到）：

| 类型 | 语义 | 本项目实例 |
|---|---|---|
| `HASH` | 普通哈希，满则插入失败 | `armed`（被抓栈的进程）、`stack_counts`（栈→采样数） |
| `LRU_HASH` | 满则自动淘汰最久未用项，防打满 | `d_start`、`futex_start`（跨事件暂存时间戳） |
| `ARRAY` | 定长数组，按下标访问 | `config`（用户态下发阈值） |
| `PERCPU_ARRAY` | 每 CPU 一份，免锁 | `pf_counter`（抓栈采样计数） |
| `STACK_TRACE` | 专门存调用栈，`get_stackid()` 写入 | `stack_traces`（内核栈+用户栈） |
| `PERF_OUTPUT` | 每 CPU 环形缓冲，向用户态推事件 | `events`（所有告警事件） |

两条经验：

1. **跨事件保存状态用 LRU 系**（如"进入 D 态的时刻"），普通 HASH 被
   异常路径打满后会悄悄丢数据；
2. **map 内存是常驻内核内存**。`STACK_TRACE` 一项约 1KB，开 16384 项
   ≈ 17MB（本项目实测总 map 内存 ~20MB）。生产环境要按目标规模算好这笔账。

### BCC 0.35 的一个版本变迁

老教程里常见的 `BPF_LRU_HASH(name, key, leaf, size)` 宏在 BCC 0.35 中
**已被移除**（本项目实测编译报错），LRU 表需写成：

```c
BPF_TABLE("lru_hash", u32, u64, my_map, 10240);
```

## 4. 调用栈抓取与符号化

`get_stackid(ctx, flags)` 把当前栈写入 STACK_TRACE map 并返回 id，
`flags=0` 抓内核栈，`BPF_F_USER_STACK` 抓用户栈。**符号化发生在用户态**：
内核只存原始地址，用户态代理（本项目为 BCC Python）用进程还 alive 时的
`/proc/<pid>/maps` 把地址解析成符号。

由此引出两个工程要点（本项目都踩过）：

- **必须先符号化再杀进程**。进程死后 maps 消失，地址永远无法解析。
  本项目阈值 2 的流程是：生成报告（符号化）→ 写盘 → 才 `SIGKILL`。
- **用户栈行走默认靠帧指针（frame pointer）**。目标程序必须带
  `-fno-omit-frame-pointer` 编译，否则栈只有一帧。而 **glibc 本身不保留
  帧指针**，穿过 libc（如 `memset` 内部）的栈会丢中间帧——报告中顶层
  显示 `libc.so.6+0x偏移` 而不是函数名，就是这条链断在了 libc 里。
  生产级方案是 DWARF 展开（`.eh_frame`），但内核侧做不了，需要把原始栈
  送回用户态用 libunwind 处理，成本显著上升，本项目未采用。

## 5. 工具链对比：BCC / libbpf CO-RE / bpftrace

| | BCC | libbpf + CO-RE | bpftrace |
|---|---|---|---|
| 形态 | C 字符串内嵌，**运行时编译** | 预编译 `.bpf.o`，运行时免编译 | 单行脚本/小程序 |
| 依赖 | 目标机需 clang+内核头文件 | 仅需内核 BTF | 同 BCC |
| 符号化 | 内置（`b.sym()`） | 需自集成（blazesym 等） | 内置 |
| 表达能力 | 完整 C + Python 生态 | 完整 C + 任意语言 | 领域语言，复杂状态机吃力 |
| 适用 | 学习、原型、分析工具 | 生产常驻 agent | 现网临时排查 |

本项目的权衡：**主题是教学分享、需要用户态符号化与复杂状态机**，
选 BCC；若做成生产常驻 agent，同样的设计应平移到 libbpf CO-RE
（BPF 侧的 hook 点与 map 设计可原样保留）。

## 6. 环境速查（本项目实测环境）

- Ubuntu 26.04 / kernel 7.0.0 / x86_64 / KVM 虚机（2C/1.5GB）
- 检查项：`/sys/kernel/btf/vmlinux`（BTF）、`/sys/kernel/debug/tracing/events/`
  （tracepoint）、`python3 -c "import bcc"`（BCC）
- 权限：需要 root（或 `CAP_BPF`+`CAP_PERFMON`+`CAP_SYS_ADMIN` 组合）
