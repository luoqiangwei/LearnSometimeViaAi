# 06 eBPF ↔ 用户态通信开销：perfbuf / ringbuf / map 全路径测量

> 04 文档回答的是"挂点花多少"；本文回答第二个问题：**事件从内核送到
> 用户态、以及用户态写配置回内核，各花多少？卡在哪？**
> 测量程序：`bench/bench_comm.py`（内核→用户态）、
> `bench/bench_mapop.c`（用户态→内核 raw bpf()）、
> `bench/flame_comm.sh`（火焰图采集）。

## 1. 测量设计

### 1.1 两个方向、三种机制

```
用户态 ──► eBPF：bpf() 系统调用（map lookup/update，单发 vs 批量）
eBPF ──► 用户态：事件下发
  ├─ perfbuf  BPF_PERF_OUTPUT + perf_submit（tracker 现用，per-cpu 多环）
  ├─ ringbuf  BPF_MAP_TYPE_RINGBUF + ringbuf_output（5.8+，全系统单环）
  └─ map 基线  只在内核聚合，不下发（通信零成本参照）
```

### 1.2 关键设计决定（都有前章教训）

- **触发点选 `syscalls:sys_enter_getpid`**：开中断上下文，perf 软件时钟
  采样无盲区（04 文档 2.5 节证明 sched_switch 回调采样不到），
  火焰图才能看到完整下发路径；bench_syscall 提供 ~220 万次/s 压力。
- **延迟直接测**：`bpf_ktime_get_ns()` 与用户态 `CLOCK_MONOTONIC` 是
  同一时钟域，事件里带提交时间戳，用户态收到即相减，得到逐事件
  端到端延迟（含排队）。
- **丢包双口径**：提交侧 per-cpu 计数 vs 用户态实际收到数。
- **生产=核0 / 消费=核1 绑核**：排除 agent 抢占对生产端的污染，
  生产端吞吐变化只反映内核路径成本。

### 1.3 环境

同前：KVM 2 vCPU / 1.5GB，kernel 7.0.0-29，BCC 0.35。

## 2. 实测数据：内核 → 用户态

### 2.1 生产端成本（把事件写进环的那一下）

| 机制 | 载荷 32B | 128B | 512B | 1024B |
|---|---|---|---|---|
| **perfbuf** 本体 ns/次（bpf_stats） | 779 | 785 | 784 | 863 |
| **ringbuf** 本体 ns/次 | **150** | **172** | **185** | **217** |
| map 基线（只计数不下发） | 47 | — | — | — |

（极端事件率 ~50-70 万次/s、含环满丢弃路径的混合平均）

生产端 getpid 吞吐的端到端影响（绑核对照，基线 219.6 万 ops/s）：

| 机制 | 32B | 128B | 512B | 1024B |
|---|---|---|---|---|
| perfbuf | +240% | +272% | +335% | +350% |
| ringbuf | +217% | +302% | +288% | +330% |

读法：

- **ringbuf 的生产端写入成本只有 perfbuf 的 1/4 ~ 1/5**（~160ns vs
  ~800ns），且随载荷增长平缓（32B→1024B：150→217ns；perfbuf
  始终 ~800ns 高位）。火焰图（§3）解释了差距来源。
- 注意"本体 ns"与"端到端吞吐影响"的差距：高负载下两者都含有大量
  **环满丢弃**路径（丢包 13-40%），环满时写入失败其实更便宜——
  真实可持续速率下的写入成本见 2.2 的降额采样行。

### 2.2 送达、丢包与延迟（瓶颈其实在消费端）

| 配置 | 提交/s | 送达/s | 丢包 | 延迟 p50 | 延迟 p99 | 消费者 CPU |
|---|---|---|---|---|---|---|
| perfbuf 32B | 645k | 531k | 17.6% | 35µs | 2.2ms | 85% |
| ringbuf 32B | 693k | 412k | 40.6% | 44ms | 73ms | 94% |
| perfbuf 1/8 采样 | 53k | 52k | 1.2% | 14µs | 4.1ms | 46% |
| ringbuf 1/8 采样 | 49k | 49k | 0.0% | 15µs | 1.6ms | 36% |

- **可持续速率（~5 万/s）下两种机制都接近零丢包，p50 延迟 ~15µs**——
  这是"干净状态"的真实延迟水位。
- 满载时送达卡在 ~40-53 万/s，与机制无关：**python 消费端每事件
  ~2µs（回调+解析+时间戳）就是 ~50 万/s 的天花板**。消费不过来，
  环就排队（延迟 p50 从 15µs 涨到 44ms）然后丢包。
  **高事件率场景的真瓶颈在用户态消费能力，不是内核机制。**
- ringbuf 满载丢包率高于 perfbuf（40% vs 18%）：单环 1MB 对所有 CPU
  共享，打满后新事件一律丢弃；perfbuf per-cpu 各有私环，核 0 生产
  侧独享一个环更抗单点压力。但这不是机制优劣问题——把环调大/
  消费端提速即可缓解（见结论）。

### 2.3 唤醒策略与消费形态（ringbuf NO_WAKEUP）

ringbuf_output 支持 `BPF_RB_NO_WAKEUP`（本次提交不唤醒消费者）。
单独看这个 flag 不够——**唤醒成本与消费形态是一个系统问题**，
实测矩阵见下一节 2.4，此处先给结论：

- **高频事件：NO_WAKEUP + 用户态节拍主动 consume 更优**——省掉每
  事件的 wake_up（实测 ~134ns/事件，见 2.4），消费者按自己的节拍
  批量读取，生产端与消费端都省；
- **低频/偶发事件（本项目 tracker 阈值控制后的形态）：默认唤醒
  更好**——wake_up 总成本可忽略，换来 µs 级送达延迟（实测 6µs vs
  5ms 节拍下的 2.7ms，差 400 倍）和消费者"无事睡眠"的零空转。
- **坑：BCC 的 `ring_buffer_poll(timeout)` 只在"被唤醒"时消费**；
  用 NO_WAKEUP 必须改调 `ring_buffer_consume()`（无条件消费），
  否则环满后 100% 丢弃（实测验证，见 bench_comm.py 的 Consumer）。

### 2.4 唤醒成本专题：花在哪、什么时候该关（`bench/bench_wakeup.py`）

对照 ringbuf 默认唤醒 vs NO_WAKEUP+5ms 节拍，在满载（1/1 提交）与
降额（1/8，~10-14 万/s）两档下测量，并引入两个定位指标：
**消费端唤醒次数/s**（poll 循环返回次数）与 **cpu0 irq+softirq 占比**
（/proc/stat，验证唤醒是否以中断形式落在生产者 CPU 上）：

| 配置 | 生产 ops/s | 本体 ns | 送达/s | 丢包 | 延迟 p50 | 消费 CPU | 唤醒/s |
|---|---|---|---|---|---|---|---|
| 默认唤醒 1/1 | 1396k | 135 | 473k | 66% | 40ms | 99% | **20** |
| NO_WAKE 1/1 | 1143k | 136 | 454k | 59% | 40ms | 94% | 106 |
| 默认唤醒 1/8 | 802k | **184** | 100k | 0% | **6.1µs** | 79.5% | 61260 |
| NO_WAKE 1/8 | 1117k | **50** | 140k | 0% | 2660µs | 36.9% | 197 |

（两档 cpu0 irq+softirq 均 ~0.2-0.3%，即 **ringbuf 唤醒不以中断形式
落在生产者 CPU 上**——与 perfbuf 的 irq_work+IPI 路径（§3.1 火焰图
41%）机制完全不同。）

机制解读（数据链完全自洽）：

1. **ringbuf 的 wake_up 是 commit 路径里的同步轻量调用，且只在
   "有等待者"时才真正执行**。1/8 降额时消费者读空即睡、等待者常驻，
   每次提交都 wake_up：本体 50ns → 184ns，**唤醒税 ~134ns/事件**；
   满载时消费者一直在忙（唤醒/s 只有 20，等待者几乎不存在），
   wake_up 被跳过，两种配置本体都是 ~135ns。
2. **唤醒税在"中等事件率"档位最重**：低到消费者能睡、又快到
   唤醒次数可观（本档 6.1 万次/s）的区间——生产系统最常见的区间，
   也是 NO_WAKEUP+节拍消费最划算的区间（生产端影响 +172%→+95%）。
3. **但 NO_WAKEUP 的延迟代价是节拍周期**：5ms 节拍下 p50 延迟
   2.7ms，默认唤醒 6.1µs（差 400 倍）。对"偶发但要紧"的事件
   （泄漏越线、D 态告警），这个延迟差就是事件意义本身——
   **低频事件不要关唤醒**。
4. 消费端 CPU：1/8 档默认 79.5%（6.1 万次唤醒/s × python 每唤醒
   ~13µs 固定成本）vs NO_WAKE 36.9%（197 次/s 定时 consume +
   事件处理）。**每唤醒送达事件数**是好的批量化指标：默认 1/8 档
   仅 1.63 事件/唤醒（几乎每事件一唤醒），NO_WAKE 709 事件/唤醒。
5. 满载档两种配置送达/丢包接近（消费者瓶颈主导），本机该档位
   生产端数字 run 间波动大（693k-1396k 都出现过），不做方向性结论；
   中等事件率档的数据是稳定可复现的。

**最佳实践矩阵**：

| 事件形态 | 推荐配置 | 理由（实测依据） |
|---|---|---|
| 高频持续（≳10 万/s，消费者跑不满） | NO_WAKEUP + 用户态节拍 consume | 省 ~134ns/事件唤醒税；批量化 709 事件/唤醒（默认仅 1.63）；两侧 CPU 都省 |
| 中频（消费者能睡，唤醒/s 可观） | 同上，节拍按延迟预算选 | 唤醒税最重的一档（生产端影响 +172%→+95%） |
| 低频偶发（告警/阈值触发，≲千/s） | **默认唤醒** | 唤醒总成本≈0；延迟 6µs vs 节拍 ms 级；消费者睡眠零空转 |
| 极偶发但绝不能丢 | 默认唤醒 + 大环 | 单事件语义优先于效率 |

一句话：**"频繁事件用户态主动消费、稀疏事件内核主动通知"**——
两种形态 ringbuf 都支持，按事件率切换即可（本项目 tracker 阈值
控制后事件接近零，默认唤醒就是正确选择；若改为内核聚合后高频
批量上报，则应切 NO_WAKEUP+节拍）。

## 3. 火焰图：成本到底花在哪

对 perfbuf / ringbuf 各采两份火焰图（`bench/flame_comm.sh`）：
**生产端**用 `perf record -F 999 -g` 包住 bench_syscall（getpid 压力
12s，触发路径在开中断的 syscall 上下文，无采样盲区），**消费端**
`perf record -p` 挂在 agent 上。SVG 原图在 `docs/assets/`：

- `comm_perfbuf_prod.svg` / `comm_ringbuf_prod.svg`（生产端内核路径）
- `comm_perfbuf_cons.svg` / `comm_ringbuf_cons.svg`（消费端）

### 3.1 生产端帧分解（栈帧出现率 ≈ 该路径的时间占比）

**perfbuf**（典型栈：`syscall → do_syscall_64 → trace_syscall_enter →
perf_syscall_enter → perf_call_bpf_enter → trace_call_bpf →
bpf_prog_...(getpid) → bpf_perf_event_output_tp → perf_event_output →
perf_output_end → perf_output_put_handle → irq_work_queue →
arch_irq_work_raise → x2apic_send_IPI_self`）：

| 帧 | 占比 | 读法 |
|---|---|---|
| trace_syscall_enter → bpf_prog（探针基础设施） | 59.7-66.6% | 04 文档已分析，非通信成本 |
| bpf_perf_event_output_tp / perf_event_output | 53.5% / 51.4% | 下发 helper 主体 |
| **perf_output_end + perf_output_put_handle** | **42.8% / 42.7%** | 收尾阶段是大头 |
| **irq_work（queue/raise/x2apic_send_IPI_self）** | **41.3%** | 唤醒消费者走**自陷 IPI** |
| perf_output_begin | 3.5% | 环空间预留很轻 |
| memcpy | **1.5%** | **数据拷贝根本不是成本** |

**ringbuf**（栈到 `bpf_ringbuf_output → __bpf_ringbuf_reserve →
bpf_ringbuf_commit` 即止）：

| 帧 | 占比 | 读法 |
|---|---|---|
| trace_syscall_enter → bpf_prog（探针基础设施） | 23.9-33.8% | 同一包装层（分母不同占比不同） |
| __bpf_ringbuf_reserve | 7.3% | 单环空间预留（per-cpu 免锁） |
| bpf_ringbuf_commit | 5.6% | 提交（含默认唤醒判断） |
| memcpy | 1.0% | 同样可忽略 |
| perf_output_* / irq_work | **0** | **没有重型唤醒框架** |

**结论级发现：perfbuf 的成本大头是"唤醒"而不是"拷贝"**——
perf_output_end → irq_work → 给自己发 IPI 这条收尾路径占了栈帧的
~42%，真正把 48-1040 字节搬进环里的 memcpy 只有 1.5%。这从机制上
解释了 §2 的三组数据：ringbuf 写入便宜 4-5 倍（无此框架）、
唤醒策略是主要优化杠杆（ringbuf 自身唤醒税的条件性分析见 §2.4：
有等待者时 ~134ns/事件，无等待者时自动跳过）、载荷大小对写入
成本影响有限（拷贝占比本来就小）。

### 3.2 消费端内核路径

- perfbuf 消费端内核时间的 **56.6% 在 `perf_read`**（read() 协议 +
  mmap 环读取），entry_SYSCALL 合计 ~17%——读侧同样有框架税；
- ringbuf 消费端在同等事件压力下内核侧几乎没有热点（采样窗口内
  entry_SYSCALL ~0.2%）——epoll 等唤 + 直接 mmap 读环的读侧同样轻。

（消费端主体耗时仍在 python 解释器用户态，火焰图用户态帧就是
python 字节码循环，与 §2.2"消费者是瓶颈"互相印证。）

## 4. 实测数据：用户态 → 内核（map 操作）

`bench/bench_mapop.c`（raw bpf() 系统调用，C 编写排除语言开销）：

| 操作 | 单次耗时 | 批量 64 摊薄 |
|---|---|---|
| BPF_MAP_UPDATE_ELEM | ~872 ns | **~214 ns/次** |
| BPF_MAP_LOOKUP_ELEM | ~855 ns | **~293 ns/次** |

同一操作的 BCC python API（tracker 现用的方式）：update ~2.4-4.1µs、
lookup ~2.3-3.7µs——**python/ctypes 封装比 raw 系统调用贵 3-5 倍**。

读法：

- map 操作本质是 ~870ns 的 bpf() 系统调用，与一次普通 syscall
  成本相当；**批量接口摊薄 3-4 倍**（一次 syscall 转移 64 个元素）。
- 对 tracker 这种"armed 写一次、阈值读一圈"的用法，map 成本无关
  痛痒；但对"高频批量下发配置/规则"的生产 agent，应该：
  ① 用 batch 接口（BCC `items_*_batch` 或 libbpf 同名 API）；
  ② 热路径语言别用 python 逐 key 调（wrapper 开销是大头）。

## 5. 结论

1. **ringbuf 在生产端全面优于 perfbuf**：写入成本 1/4~1/5
   （~160 vs ~800ns/次），载荷越大优势越稳。perfbuf 唯一的剩余优势
   是 per-cpu 私环在单点高压下的抗丢包表现，可用"大环+快消费"补齐。
   **tracker 的事件通道迁 ringbuf 是明确的下一步**（BCC
   `BPF_RINGBUF_OUTPUT` + `open_ring_buffer` 即可，API 对仗）。
2. **唤醒策略按事件率选形态**：ringbuf 默认唤醒在"有等待者"时税
   ~134ns/事件（无等待者自动跳过），中高频（≳10 万/s）切
   NO_WAKEUP+用户态节拍 consume（生产端影响 +172%→+95%，批量化
   709 事件/唤醒）；低频偶发事件保持默认唤醒——延迟 6µs vs 节拍
   ms 级、消费者睡眠零空转（详见 §2.4 最佳实践矩阵）。
3. **高事件率下瓶颈在用户态消费端而非内核机制**：~50 万事件/s 就把
   python 消费端打满（~2µs/事件），之后全是排队与丢包。生产系统的
   选项：内核侧先聚合（per-cpu 计数 map，周期读汇总——成本见 map
   基线 47ns/次）、消费端换语言（Go/Rust/C）、或加采样/阈值控制
   事件率（本项目 tracker 的设计）。
4. **用户态→内核方向：单发 map 操作 ~870ns，批量接口摊薄 3-4 倍**；
   python wrapper 再贵 3-5 倍，热路径绕开。
5. **方法论复用**：延迟用"提交时间戳 vs CLOCK_MONOTONIC"直接测；
   火焰图采样点必须选在开中断的触发路径上（syscall 类），
   sched_switch 类关中断路径软件时钟采样不可见（04 文档 2.5 节）。

## 6. 复现

```bash
sudo python3 bench/bench_comm.py            # 三机制全量（约 10 分钟）
sudo python3 bench/bench_comm.py --quick    # 快速版
sudo python3 bench/bench_wakeup.py          # 唤醒成本专题（默认 vs NO_WAKEUP x 事件率）
./bench/bench_mapop 400000                  # 用户态->内核 raw bpf() 成本
sudo bench/flame_comm.sh                    # 火焰图（需 perf 与 FlameGraph 脚本）
```
