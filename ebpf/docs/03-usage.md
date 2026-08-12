# 03 使用手册：环境、运行与实验复现

## 1. 环境要求

| 项 | 要求 | 检查命令 |
|---|---|---|
| 内核 | ≥ 5.x（本项目实测 7.0.0-29） | `uname -r` |
| 权限 | root | `id -u` |
| BCC | python3-bpfcc ≥ 0.35 | `python3 -c "import bcc"` |
| tracefs | 已挂载 | `ls /sys/kernel/debug/tracing/events` |
| 编译 | gcc, make | 用于 demo 和 bench |
| D 态实验 | dm-delay 模块 | `modinfo dm-delay` |

Ubuntu 参考安装：`apt install bpfcc-tools python3-bpfcc linux-headers-$(uname -r)`

## 2. 编译

```bash
make            # 编译 demos/ 与 bench/ 下所有程序
```

目标程序如需被**符号化**（自己的程序做泄漏分析），编译务必加
`-g -fno-omit-frame-pointer`（demo 已带）。

## 3. tracker 参数

```bash
sudo python3 src/tracker.py [选项]
```

| 选项 | 默认 | 说明 |
|---|---|---|
| `--pid N` | — | 目标进程（可多次）。RSS 阈值管理与事件过滤 |
| `--comm NAME` | — | 按进程名子串匹配（可多次），含新创建的匹配进程 |
| `--rss-t1 MB` | 64 | RSS 阈值 1：超过即 armed，开始抓用户栈 |
| `--rss-t2 MB` | 96 | RSS 阈值 2：超过即生成 Top10 报告并处置 |
| `--rss-interval S` | 1.0 | RSS 轮询间隔（秒） |
| `--d-threshold-ms MS` | 500 | D 态（不可中断睡眠）告警阈值 |
| `--futex-threshold-ms MS` | 200 | futex 等锁告警阈值 |
| `--lock-threshold-ms MS` | 800 | pthread 持锁告警阈值（配合 `--track-locks`） |
| `--stack-sample N` | 1 | 抓栈采样率：每 N 次页错误抓 1 次 |
| `--track-locks` | 关 | uprobe 统计 pthread_mutex 持锁时长（需 `--pid`） |
| `--no-kill` | 关 | 阈值 2 只出报告，不 SIGKILL |
| `--top N` | 10 | 报告中堆栈条数 |
| `--report-dir DIR` | reports | 报告与事件日志目录 |
| `--verbose` | 关 | D 态事件附带内核栈（前 6 帧） |
| `--kprobe` | 关 | 用 kprobe 挂点替代 tracepoint（开销对照实验，见 04 文档 2.7 节；此模式 D 态事件无内核栈） |
| `--raw-tp` | 关 | 用 raw tracepoint 挂点（绕过 perf 框架，功能完全对齐，见 04 文档 2.8 节） |

不指定 `--pid/--comm` 时：只做全局 D 态/等锁事件观测，不做 RSS 阈值管理。

输出：终端实时事件 + `reports/events_YYYYMMDD.log` + 触发时的
`reports/leak_report_*.md`。

## 4. 实验一：内存泄漏双阈值（核心实验）

终端 1：

```bash
sudo python3 src/tracker.py --comm leak_demo --rss-t1 40 --rss-t2 60 --rss-interval 0.5
```

终端 2：

```bash
./demos/leak_demo        # 路径A ~8MB/s 泄漏，路径B ~0.3MB/s，路径C 正常
```

预期时序（约 10 秒内）：

1. `THRESH-1 ... RSS=40MB ... 开始抓取用户态堆栈`
2. `THRESH-2 ... RSS=60MB ... 生成泄漏报告...`
3. `ACTION 已 SIGKILL ... 报告: reports/leak_report_leak_demo_*.md`

报告解读要点：Top1 栈应占 ~95%（`handle_http_request` 路径），Top2 占
~3%（`handle_ws_connection` 路径）——占比与两条路径的泄漏速率比一致，
这就是"按页错误归因"的含义：占比 ≈ 该路径贡献的 RSS 增长占比。

## 5. 实验二：持锁过长 / 等锁过长

```bash
./demos/lock_demo &      # holder 每轮持锁 1500ms，4 个竞争者
sleep 0.5
sudo python3 src/tracker.py --pid $(pgrep -x lock_demo) \
     --track-locks --lock-threshold-ms 800 --futex-threshold-ms 300
```

预期输出（每 ~1.8s 一轮）：

- `LOCK-HOLD  tid=<holder> 时长 1.50s`（uprobe 统计的持锁时长）
- `FUTEX-WAIT tid=<contender> 时长 1.50s`（futex 等锁时长）

> 提示：shell 里 `xxx & echo $!` 在某些包装环境下拿到的是子壳 pid，
> 用 `pgrep -x lock_demo` 取真实 pid 更可靠。

## 6. 实验三：D 态过长

```bash
sudo ./demos/make_slow_disk.sh start     # dm-delay 慢盘，每次 I/O 500ms
sudo python3 src/tracker.py --d-threshold-ms 300 --verbose
# 观察：D-STATE tid=... comm=dd D态持续 50x ms
#       + 内核栈（io_schedule_timeout / wait_for_completion_io_timeout）
sudo ./demos/make_slow_disk.sh stop      # 用完务必清理
```

无慢盘条件时可用 FUSE 挂起方案或在高 I/O 压力下观察自然 D 态；
检测逻辑与负载来源无关。

## 7. 实验四：性能开销测量

```bash
sudo python3 bench/bench_overhead.py --rounds 5     # 约 3 分钟，结果写 bench/results.json
sudo python3 bench/bench_nostack.py                 # 不抓栈开销专测（更精确定量）
sudo python3 bench/bench_probe_path.py              # 分解“进入/退出BPF路径”开销（三分法）
```

三个测量状态：`baseline`（无 tracker）、`loaded`（hook 全挂载）、
`armed`（对 bench_fault 抓栈）。另输出各 BPF 程序单次执行耗时
（`kernel.bpf_stats_enabled`）与 map 常驻内存。

## 8. 常见问题

- **报告里栈只有一帧 / 全是 `[unknown]`**：目标程序没编帧指针，或栈穿过
  glibc（glibc 无 FP，属预期限制，见 02 文档第 6 节）。
- **FORK/EXEC 事件看不到目标**：旧版本按目标集合过滤会漏掉"刚创建还
  未注册"的进程；现版本已按 comm/parent 三级匹配，若仍异常请查
  `--comm` 子串是否真的匹配 `cat /proc/<pid>/comm`。
- **BCC 编译报错 `BPF_LRU_HASH`**：BCC 0.35 移除了该宏，用
  `BPF_TABLE("lru_hash", ...)`（本项目代码已是新写法）。
- **tracker 退出后进程仍在**：用 `timeout` 包运行或直接 Ctrl-C；
  被 `--comm` 匹配上的 demo 进程不受影响，自行 `pkill -x <name>`。
- **map 内存占用**：默认配置约 20MB（STACK_TRACE 占大头），可在
  `bpf_program.c` 把 `stack_traces` 项数从 16384 调小。
