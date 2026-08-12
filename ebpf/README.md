# eBPF 进程监控与内存泄漏追踪

基于 eBPF（BCC）的 Linux 进程监控系统，配套演示程序、性能基准与系列分享文档。

**核心能力**：进程/线程生命周期跟踪、RSS 监控、D 态（不可中断睡眠）过长
检测、futex 等锁过长检测、pthread 持锁过长检测；**内存泄漏双阈值自动
处置**——RSS 越过阈值 1 自动开启用户态堆栈抓取，越过阈值 2 先解析堆栈
生成 Top10 归因报告、再杀死进程，直接回答"是哪条代码路径在泄漏"。

实测环境：Ubuntu 26.04 / kernel 7.0.0-29 / BCC 0.35 / KVM 2C1.5G。

## 快速开始

```bash
make                                              # 编译 demo 与 bench

# 核心实验：泄漏双阈值处置
sudo python3 src/tracker.py --comm leak_demo --rss-t1 40 --rss-t2 60
./demos/leak_demo        # 另一个终端；约 10s 后自动出报告并杀死

# 其它实验
./demos/lock_demo &                                                              # 先启动目标
sudo python3 src/tracker.py --pid $(pgrep -x lock_demo) --track-locks           # 持锁/等锁
sudo ./demos/make_slow_disk.sh start                                            # D 态慢盘
sudo python3 src/tracker.py --verbose --d-threshold-ms 300                      # 观察 D 态+内核栈
sudo ./demos/make_slow_disk.sh stop                                             # 用完清理

# 性能开销测量（约 3 分钟）
sudo python3 bench/bench_overhead.py
```

详细步骤见 [docs/03-usage.md](docs/03-usage.md)。

## 目录

```
src/        tracker.py（用户态主控）+ bpf_program.c（内核侧 BPF 程序）
            + bpf_program_kprobe.c（kprobe 变体，--kprobe）
            + bpf_program_rawtp.c（raw tracepoint 变体，--raw-tp，生产优化方向）
demos/      leak_demo（泄漏）、lock_demo（持锁）、make_slow_disk.sh（D 态慢盘）
bench/      6 个微基准 + bench_overhead.py（开销测量编排）
            + bench_nostack.py / bench_probe_path.py / bench_kprobe.py
            / bench_rawtp.py（挂点专题）+ bench_comm.py / flame_comm.sh（通信专题）
docs/       分享文档（见下）
reports/    运行产物：事件日志 + 泄漏报告
```

## 分享文档（建议阅读顺序）

1. [docs/01-ebpf-basics.md](docs/01-ebpf-basics.md) — eBPF 原理：内核
   虚拟机、验证器、map、hook 类型、BCC/CO-RE/bpftrace 选型
2. [docs/02-design.md](docs/02-design.md) — 本系统设计：hook 点、
   map 表、双阈值状态机、已知限制
3. [docs/03-usage.md](docs/03-usage.md) — 环境、参数与四个动手实验
4. [docs/04-performance.md](docs/04-performance.md) — 性能开销：三层
   测量方法、实测数据、外推结论、kprobe/raw tracepoint 挂点对照
5. [docs/05-case-study.md](docs/05-case-study.md) — 一次泄漏自动处置的
   完整复盘（真实报告解读）
6. [docs/06-communication.md](docs/06-communication.md) — 通信开销：
   perfbuf vs ringbuf vs map 全路径测量、火焰图定位卡点、
   map 批量操作

## 分享提纲（60 分钟建议）

| 时长 | 内容 | 素材 |
|---|---|---|
| 10' | eBPF 是什么、为什么安全（验证器/JIT/map） | docs/01 |
| 10' | hook 选型：tracepoint vs kprobe vs uprobe，现场看 format 文件 | docs/01 |
| 15' | 系统设计：为什么内核只计数、决策在用户态；双阈值状态机 | docs/02 |
| 15' | 现场演示：leak_demo 从越线到 Top10 报告到被杀 | docs/03、05 |
| 10' | 性能开销：三层测量 + 采样率杠杆 + 20MB map 预算 | docs/04 |
| 10' | 通信与挂点选型：perfbuf vs ringbuf 火焰图、raw tracepoint | docs/06、04 |
