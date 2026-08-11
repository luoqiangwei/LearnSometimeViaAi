# userfaultfd (uffd) 技术研究与演示

一个用于内部分享的 userfaultfd 学习项目：通过 5 个可运行的 demo，直观展示
userfaultfd 解决了什么问题、在哪些场景有价值，以及在 AOSP 中的应用与后续改进方向。

## 构建与运行

```bash
make            # 编译全部 demo 到 bin/
make run        # 依次运行 4 个 demo
make smoke      # 最小冒烟测试（排查环境是否支持 uffd）
```

> **权限要求**：uffd 需要内核 ≥ 4.3；多数发行版默认
> `/proc/sys/vm/unprivileged_userfaultfd = 0`，即只有 root（或带
> `CAP_SYS_PTRACE` 的进程）可以创建 uffd。本项目的 demo 请以 root 运行，
> 或 `sysctl vm.unprivileged_userfaultfd=1`（注意这会放开攻击面，见下文
> "权限与安全"）。

---

## 一、userfaultfd 解决了什么问题

### 1. 核心机制：把缺页异常处理权交给用户态

Linux 普通的缺页处理完全由内核决定：匿名页缺页 → 分配零页；文件页缺页 →
从块设备读入。策略和数据源都是内核写死的。

userfaultfd（2015 年合入内核 4.3）把这个权力开放出来：

```
 应用线程                     内核                        处理线程(用户态)
    │                          │                              │
    │ 访问注册区域内缺失的页      │                              │
    │─────────────────────────▶│  不分配页面，                  │
    │                          │  把缺页事件投进 fd 队列         │
    │  阻塞等待                  │─────────────────────────────▶│ read() 收到事件
    │                          │                              │ 自行决定页面内容：
    │                          │   UFFDIO_COPY / ZEROPAGE      │ 从磁盘/网络/快照/解压…
    │                          │◀──────────────────────────────│
    │◀─ 唤醒，访问继续 ──────────│                              │
```

关键点：**"这一页放什么内容、从哪来、什么时候给"完全由用户态程序决定**。
内核只负责拦截和传递事件，甚至处理线程可以住在另一个进程里（非协作模式，
fd 通过 unix socket 传递）。

### 2. 它解决的三类问题

| 问题 | 传统做法的痛点 | uffd 的解法 |
|---|---|---|
| **按需加载**：大数据集/快照/虚拟机内存，启动时不该全量读入 | mmap 预读策略由内核定，无法自定义数据源（网络、压缩、快照文件） | MISSING 模式：访问到哪页才加载哪页，数据源自定义（demo1） |
| **并发 GC 压缩**：搬迁对象时应用线程可能踩到"正在搬迁的页" | stop-the-world 卡顿；或软件读屏障让每次内存访问都付指令开销 | 搬迁页解除映射，踩中的线程被 uffd 拦截并等待，其余访问零开销（demo2） |
| **脏页跟踪**：热迁移/增量快照只需同步被改过的页 | mprotect+SIGSEGV：信号逐线程派发、拆分 VMA、慢 | WP 模式：写保护事件走 fd 队列，专门线程异步处理（demo3） |

### 3. 与 mprotect + SIGSEGV 的对比

uffd 出现之前，"用户态接管缺页"只能靠 `mprotect(PROT_NONE)` + SIGSEGV
信号处理器模拟。uffd 在内核文档中被定位为该 trick 的"更优的正式实现"：

- **不动 VMA**：mprotect 每次调用都可能拆分 VMA，TB 级地址空间按页追踪会
  产生海量 VMA；uffd 所有操作不触碰 VMA、不以写方式持有 mmap_lock。
- **多线程友好**：信号是往线程派发的，处理函数还要考虑重入；uffd 把事件放进
  一个 fd 队列，由专门线程（甚至另一个进程）统一处理。
- **原子填充**：UFFDIO_COPY/ZEROPAGE 是原子的，读者要么看到完整页要么继续
  等待，不会看到"半个页"。
- **诚实性能数据**（Peter Xu 实测，Linux 5.9）：单线程轻负载下
  mprotect+SIGSEGV 每次缺页约 1.92µs，uffd-wp 跨线程模式约 4.74µs，
  uffd-wp+SIGBUS 单线程模式约 1.85µs —— 即 uffd 的优势在协作能力与
  可扩展性，而不是单次缺页的绝对延迟。

### 4. 内核演进时间线

| 版本 | 特性 | 背景/作者 |
|---|---|---|
| 4.3 (2015) | userfaultfd 合入，MISSING 模式，匿名内存 | Andrea Arcangeli，为 QEMU post-copy 热迁移而做 |
| 4.11 (2017) | MISSING 支持 shmem/hugetlbfs；FORK/REMAP/REMOVE/UNMAP 事件（非协作模式，CRIU 需求驱动） | Mike Rapoport 等 |
| 5.2 (2019) | `vm.unprivileged_userfaultfd` sysctl | Peter Xu，收缩攻击面 |
| 5.7 (2020) | **WP 写保护模式**（UFFDIO_WRITEPROTECT） | Peter Xu |
| 5.11 (2021) | **UFFD_USER_MODE_ONLY**；sysctl 默认收紧为 0 | Lokesh Gidra (Google)，与 Android userland 行为对齐 |
| 5.13/5.14 | **MINOR 模式**（minor fault）：hugetlbfs / shmem | Axel Rasmussen (Google)，为 QEMU 共享内存后端按需填充 |
| 5.19 (2022) | WP 支持 shmem/hugetlbfs | Peter Xu |
| 6.1 (2022) | **/dev/userfaultfd** 字符设备，文件权限做访问控制 | Axel Rasmussen |
| 6.6 (2023) | **UFFDIO_POISON**：模拟内存中毒（VM 迁移后保留中毒页语义） | Axel Rasmussen |
| 6.7 (2024) | **UFFD_FEATURE_WP_ASYNC**：无消息异步脏页追踪 | Muhammad Usama Anjum (Collabora)，Wine write-watch 驱动 |
| 6.8 (2024) | **UFFDIO_MOVE**：页表项过户代替逐字节拷贝 | Andrea Arcangeli 实现、Suren Baghdasaryan (Google) 上游化，为 Android ART GC 压缩驱动 |
| 6.9 (2024) | uffd 操作改用 per-VMA 锁 | Lokesh Gidra，解决 ART GC 压缩时 mmap_lock 竞争卡顿 |

### 5. 权限与安全

uffd 能拦截**内核态**缺页（例如 `copy_from_user()` 访问用户页时），攻击者
可借此把内核执行流任意暂停，扩大条件竞争窗口、辅助堆喷射——多个真实
exploit 用过这招。因此：

- 5.2 引入 `vm.unprivileged_userfaultfd`；5.11 起默认值为 0：非特权进程
  只能创建 `UFFD_USER_MODE_ONLY` 的 uffd（只处理用户态缺页），否则需要
  `CAP_SYS_PTRACE`。
- 6.1 引入 `/dev/userfaultfd`：发行版可用文件权限精确授权，不必在
  "全放开"和"全禁止"之间二选一。

---

## 二、演示项目

五个 demo 全部实测通过（环境：Linux 7.0 x86-64，root；demo5 需要 liburing）。

### demo1：懒加载 —— 按需填页（`src/demo1_lazy_load.c`)

映射 256MB 匿名内存并注册 MISSING 模式，一个物理页都不分配；主线程随机
访问 1000 页，每页的第一次访问触发 uffd 事件，处理线程"从数据源取回该页"
（demo 里用确定性图案模拟）并用 UFFDIO_COPY 提供。

实测输出：

```
映射虚拟内存: 256 MB (65536 页)
随机访问并校验了 1000 页，全部通过 (耗时 155 ms)
常驻物理内存: 访问前 1820 KB -> 访问后 6056 KB (增长约 4 MB)
```

**对应真实场景**：QEMU post-copy 热迁移（VM 先在目的机跑，页从源机按需
拉取）、CRIU 惰性恢复、Firecracker 快照惰性加载。

### demo2：并发 GC 压缩 —— STW vs uffd（`src/demo2_gc_compact.c`)

模拟 512 页"堆"、4 个 mutator 线程全速读写，GC 每 30ms 搬迁 8 页
（每页 2ms）：

- **阶段 A（STW）**：搬迁期间全局停表，所有 mutator 空转；
- **阶段 B（uffd）**：GC 把搬迁页 MADV_DONTNEED 解除映射；mutator 踩到
  搬迁页时被 uffd 拦截，处理线程等 GC 搬完后从"新位置"恢复该页并唤醒。

实测输出：

```
[阶段 A] stop-the-world GC ...
  mutator 总访问: 58844623 次, 吞吐 11712704 次/秒
  （每轮 GC 全应用暂停约 16 ms）
[阶段 B] userfaultfd 并发压缩 GC（ART CMC GC 的方式）...
  mutator 总访问: 71156397 次, 吞吐 14118333 次/秒
  uffd 拦截缺页: 1312 次, 被搬迁页阻塞累计约 1776 ms (占比 8.81%)
  数据完整性校验(uffd): 全部通过
```

uffd 方案吞吐高约 21%，且停顿只落在真正踩中搬迁页的访问上；其余访问
**零开销**（不需要读屏障指令）。**这就是 Android ART 的 CMC GC
（Concurrent Mark-Compact，Android 13 起默认）做并发堆压缩的核心机制，
详见第四章。**

### demo3：脏页跟踪 —— 增量检查点（`src/demo3_dirty_track.c`)

注册 WP 模式后对 128 页区域全量写保护；应用每轮只写 7 页，每次写入被
uffd 拦截：处理线程在脏页位图记一笔、解除该页保护、写入继续。

实测输出：

```
第 1 轮: 应用写了 7 页, 跟踪到脏页 7 页 -> 本轮检查点只需同步 7/128 页 (省 95%)
第 2 轮: 应用写了 7 页, 跟踪到脏页 7 页 -> 本轮检查点只需同步 7/128 页 (省 95%)
第 3 轮: 应用写了 7 页, 跟踪到脏页 6 页 -> 本轮检查点只需同步 6/128 页 (省 95%)
```

**对应真实场景**：CRIU 增量迁移、QEMU 热迁移 dirty logging、周期性
内存快照。

### demo4：UFFDIO_MOVE vs UFFDIO_COPY（`src/demo4_move.c`)

GC 压缩的本质是"把页从 A 地址挪到 B 地址"。UFFDIO_COPY 要逐字节拷贝；
UFFDIO_MOVE（6.8+）直接把源页的页表项"过户"到目的地址，物理页原地不动。

实测输出（64MB）：

```
UFFDIO_COPY: 681 ms, 数据校验通过
UFFDIO_MOVE: 3 ms, 数据校验通过
MOVE 前后 RSS 变化: +32 页（≈0 说明源页帧被直接过户，零拷贝）
对比: MOVE 比 COPY 快约 227 倍
```

（COPY 含 64MB 的实际内存拷贝与页分配，MOVE 只做页表操作；绝对数值随
机器而变，但量级差距是本质性的。）

### demo5：uffd × io_uring —— 懒加载流水线的吞吐对决（`src/demo5_uffd_iouring.c`)

demo1 的延伸：真实懒加载场景（Firecracker 快照恢复、CRIU lazy-pages）
里，handler 收到缺页事件后要去"取数"——读快照文件/网络拉取/解压，
**每页都有一次 I/O 延迟**。传统 handler 串行处理（read 事件 → pread 取数
→ COPY），N 次缺页总耗时 ≈ N × 单次延迟；io_uring 方案把 uffd 事件
（POLL_ADD）和取数（异步 READ）都挂进事件循环，几十次取数并发在飞，
总耗时 ≈ (N / 并发度) × 单次延迟。只有最后的 UFFDIO_COPY 仍是普通
ioctl——io_uring 没有通用 ioctl op，上游也尚未给 uffd 实现 uring_cmd
（这正是潜在的内核改进点，见第五章）。

负载：64MB 真实数据文件（O_DIRECT 绕过页缓存，实测单次读延迟约
780µs），16 个访问线程并发踩 256 个不同页面制造突发缺页；可叠加
"模拟远端取数/解压延迟"（`UFFD_DEMO5_SIM_US`，µs）。

实测对比（本机，单次取数真实延迟 ~0.8ms + 模拟延迟）：

| 单次取数总延迟 | 阻塞式 handler | io_uring handler | 加速比 |
|---|---|---|---|
| ~0.8ms（仅真实磁盘，SIM_US=0） | 188 ms | 76 ms | **2.5x** |
| ~1.8ms（SIM_US=1000） | 463 ms | 77 ms | **6.0x** |
| ~5.9ms（SIM_US=5000，模拟远端拉取） | 1490 ms | 61 ms | **24.4x** |

规律非常干净：**取数越慢，io_uring 优势越大**——它把 handler 从"串行
等 I/O"变成"流水线并发 I/O"。这正是 Firecracker 生态（buildbuddy 的
chunked+compressed 远端缓存 handler、e2b 等）面临的负载形态。

但要注意它省不掉的部分：缺页陷入 + 缺页线程睡眠/唤醒 + COPY 本身，
这些决定了**单页延迟的下限**（约 300µs/页，见上表 io_uring 列），
io_uring 优化的是 handler 侧的**吞吐**而非单页延迟。对延迟敏感的
同步路径（如 demo2 的 GC 缺页，数据本就在内存里），io_uring 没有意义。

---

## 三、适用场景全景

| 场景 | 代表项目 | uffd 用法 |
|---|---|---|
| 虚拟机 post-copy 热迁移 | QEMU/KVM（uffd 的最初动机，2015） | 目的端注册 MISSING，缺页时向源端请求紧急页，UFFDIO_COPY 注入 |
| 进程/容器热迁移、惰性恢复 | CRIU `--lazy-pages` | 恢复最小状态即启动，页按需从源节点拉取；用到非协作模式与 FORK/UNMAP 等事件 |
| serverless / microVM 快照秒级启动 | Firecracker（`mem_backend: Uffd`）、e2b | uffd fd 交给外部 handler 进程，从快照文件按需填页；研究系统 REAP(ASPLOS'21) 用 uffd 记录工作集做恢复预取 |
| 托管运行时并发 GC | **Android ART**（详见下章） | 压缩阶段拦截对搬迁页的访问 |
| Windows 兼容层 write-watch | Wine/Proton（内核 6.7+ 用 WP_ASYNC） | 模拟 Win32 GetWriteWatch API，供 .NET GC 等做写追踪 |
| 脏页跟踪/增量快照 | CRIU pre-dump、各类研究原型 | WP 模式收集脏页集合 |
| 分布式共享内存 (DSM) | 研究原型（Systex'22 等） | 拦截缺页从远端节点取页 |

**不适合的场景**：对单次访问延迟极度敏感的远端内存（disaggregated
memory）。缺页路径本身开销在微秒级以上，Kona(ASPLOS'21) 等系统正是因为
"远端访问走缺页太慢"（>40µs vs RDMA 3µs）而选择绕过缺页路径。uffd 适合
"命中率高的懒加载/偶发拦截"，不适合把每次访问都变成缺页的设计。

---

## 四、userfaultfd 在 AOSP 中的应用

### 4.1 ART 的 CMC GC：uffd 在 Android 的核心落地场景

**先澄清一个常见误传**：Android 8 (Oreo, 2017) 引入的 Concurrent Copying
(CC) GC **并没有用 uffd**——它在 ARM64 上用的是 Baker 读屏障（每次引用
加载都检查 from-space 位图），x86 上是 table-lookup 读屏障。逐版本核对
`art/runtime/gc/collector/concurrent_copying.cc`（Android 8~16）全文没有
任何 uffd 引用。

真正用 uffd 的是 **CMC（Concurrent Mark-Compact）GC**：

| 时间 | 事件 |
|---|---|
| 2020-04 | AOSP commit `c6bbc26ebe` "Marking phase of Concurrent Mark Compact GC"（Lokesh Gidra） |
| 2022-02 | AOSP commit `5316aeae5c` "**Make moving space compaction concurrent using userfaultfd**" |
| 2022 | **Android 13 起 CMC 默认启用**（内核不支持 uffd 则回退 CC GC）；Android 13 官方发布材料明确宣传 "new garbage collector utilizing the Linux userfaultfd system call" |
| 2023-12 | 经 ART Mainline 把默认开启范围扩大到 Android 12 |

机制（`art/runtime/gc/collector/mark_compact.cc`，与本项目 demo2 完全同构）：

1. `KernelSupportsUffd()` 用 `UFFD_USER_MODE_ONLY` 探测内核支持，要求
   `UFFD_FEATURE_SIGBUS` 和 `MREMAP_DONTUNMAP`；
2. 移动空间（moving space）注册 `UFFDIO_REGISTER_MODE_MISSING`；
3. 压缩时 from-space 页被解除映射；mutator 踩到未处理的页 → 触发
   SIGBUS/缺页 → 由压缩线程用 `UFFDIO_COPY`/`UFFDIO_ZEROPAGE` 填页唤醒。

**为什么用 uffd 换掉读屏障**：Baker 读屏障在**每一次引用加载**都有固定
指令开销，还使编译后代码体积增大约 10%；uffd 方案快路径零开销，代价只
落在少数未压缩页的缺页路径上。

配套工程细节（Android 生态如何为非特权 app 用上 uffd）：

- **权限**：ART 只处理用户态缺页，创建 uffd 时传 `UFFD_USER_MODE_ONLY`，
  因此在默认 `vm.unprivileged_userfaultfd=0` 下也能工作，Android 不需要
  放开该 sysctl；
- **SELinux**：sepolicy 的 `app_domain()` 宏内含 `userfaultfd_use()`，
  即所有 app 域默认被允许；
- **时序**：zygote 在 app 的 seccomp 过滤器安装**之前**就建好 uffd fd
  （见 `runtime/gc/heap.cc` 注释）；
- **内核**：GKI 从 android12-5.4 / android12-5.10 起 `CONFIG_USERFAULTFD=y`
  （更早的 GKI 内核未开，CMC 自动回退 CC）。

值得注意的是，这个场景还**反向驱动了内核演进**：5.11 的
`UFFD_USER_MODE_ONLY`（commit message 明说是为了对齐 Android userland）、
6.9 的 uffd 操作 per-VMA 锁（解决 GC 压缩期 mmap_lock 竞争造成的卡顿）、
以及下面的 UFFDIO_MOVE。

### 4.2 UFFDIO_MOVE：为 ART 堆压缩而生的内核特性

- 内核 **6.8** 合入（commit `adef440691ba`，Andrea Arcangeli 实现、
  Google 的 Suren Baghdasaryan 上游化）；
- commit message 直接写明动机："堆压缩场景下用户态通常有可回收页，
  MOVE 可免去 COPY 的页分配 + memcpy"，并给出 **Pixel 6 实测：压缩线程
  完成时间减少 40% 以上**；
- 公开 AOSP 快照（至 2025-12）中 `mark_compact.cc` 仍只用
  COPY/ZEROPAGE，但 Google 工程师 2025-09 的内核补丁明确写道
  "UFFDIO_MOVE is heavily used in Android as its java garbage collector
  uses it for concurrent heap compaction"——推断已通过 ART Mainline
  灰度发布（ART Mainline 部分开发在内部分支进行）。本项目 demo4 演示的
  正是 COPY→MOVE 的收益。

### 4.3 传闻核实（负结论同样重要）

以下说法在社区流传，但**逐一核实后均未找到证据**：

- ❌ "AVF/pKVM 用 uffd 给受保护虚拟机做惰性内存填充"——pKVM 的实际设计
  是在内核/hypervisor 内处理 host stage-2 缺页，不经过用户态 uffd
  （邻近事实：ChromeOS crosvm 的 vmm-swap 功能用了 uffd，但那不是
  protected VM）；
- ❌ "Android 用 uffd 收集启动期缺页做 I/O 预读"——Android 11 的 IORap
  用的是 perfetto/ftrace 的内核 pagecache tracepoint，不是 uffd；
- ❌ lmkd、Zygote 堆压缩使用 uffd——源码中均无引用。

## 五、后续可以继续用 uffd 改进的方向

1. **ART GC 继续演进（部分已在路上）**
   - UFFDIO_MOVE 全面落地及内核侧配套：2025-09 上游已有"移除 anon_vma
     锁提升 UFFDIO_MOVE 可扩展性"的补丁系列，动机正是真机 field trace
     中 UI 线程因该锁出现 50ms+ 不可中断睡眠；
   - shmem minor fault：`mark_compact.cc` 已预留特性检测，注释指向未来
     **JIT code cache 的并发更新**（代码页按需并发替换而不用停 JIT 线程）。
2. **应用启动加速**：用 uffd 精确记录启动期的缺页序列（页粒度、真实
   访问顺序），指导预读策略与 dex/so 文件布局优化。serverless 领域的
   REAP（ASPLOS'21）已验证该思路（记录函数工作集、恢复时整体预取）；
   Android 目前 IORap 用 ftrace tracepoint，uffd 可提供更细粒度数据。
3. **内存管理精细化**
   - 用 WP 脏页跟踪（demo3）做应用级增量内存快照，支撑"快速冻结/秒级
     恢复"的后台 App 状态保存；
   - 用 WP 做后台 App 的精确工作集统计（比 soft-dirty 按 PTE 追踪、不受
     VMA merge 影响），指导更激进的内存回收/换出策略（crosvm vmm-swap
     已有类似实践）。
4. **共享内存与跨进程场景**：shmem/hugetlbfs 的 MISSING/MINOR/WP 支持已
   齐备（5.13~5.19），可用于跨进程共享堆、共享图形缓冲区的惰性初始化与
   按需同步。
5. **部署形态**：产品化授权优先考虑 `/dev/userfaultfd`（6.1+）按文件权限
   放开，而非全局放开 `vm.unprivileged_userfaultfd`。
6. **uffd × io_uring 的内核侧整合**：目前 io_uring 无法提交 UFFDIO_COPY
   等 ioctl（没有通用 ioctl op，uffd 也未实现 `uring_cmd`）。若给 uffd
   加上 uring_cmd 支持，"缺页事件 → 取数 → 填页"整条流水线可用 linked
   SQE 在内核内闭环，handler 侧 syscall 开销趋近于零。检索 LKML/LWN 未
   发现上游有相关补丁，属于真空地带（demo5 实测的用户态收益见第二章）。

## 六、局限性与使用注意事项

- **单次缺页开销不可忽视**：跨线程唤醒 + ioctl 填页，单次成本在数 µs
  到数十 µs 量级（demo1 实测含调度约 155µs/页，本机偏弱）。设计时要保证
  缺页是"少数路径"，否则应考虑 SIGBUS 模式（UFFD_FEATURE_SIGBUS，
  缺页线程自己处理，省掉跨线程调度）或批量预填充。
- **权限收紧是趋势**：非特权进程默认只能 UFFD_USER_MODE_ONLY；产品化时
  优先考虑 /dev/userfaultfd 授权，而不是把 sysctl 全放开。
- **事件循环要用 O_NONBLOCK fd**：阻塞 read 无法被 close 打断，处理线程
  退出时容易挂死（本项目 demo1 踩过的坑，见 `src/demo1_lazy_load.c` 注释）。
- **UFFDIO_API 每个 fd 只能协商一次**：重复调用返回 EINVAL。
- **并发踩同一页会产生重复事件**：UFFDIO_COPY 撞上已被填充的页返回
  EEXIST，处理线程要容忍（demo2 中有实例）。
- **与 KSM 等机制的交互**：UFFDIO_MOVE 要求页独占，被 KSM 合并的页会
  返回 EBUSY。

## 参考资料

- 内核文档：<https://docs.kernel.org/admin-guide/mm/userfaultfd.html>
- man page：`man 2 userfaultfd`、`man 2 ioctl_userfaultfd`
- QEMU post-copy：<https://wiki.qemu.org/Features/PostCopyLiveMigration>
- CRIU lazy migration：<https://criu.org/Lazy_migration>
- Firecracker snapshot：<https://github.com/firecracker-microvm/firecracker/blob/main/docs/snapshotting/snapshot-support.md>
- UFFDIO_MOVE 补丁系列（LWN）：<https://lwn.net/Articles/952319/>
- uffd per-VMA 锁（LWN）：<https://lwn.net/Articles/961446/>
- ART CMC GC 设计文档（Google 技术公开）：Gidra 等 "Utilizing the Linux
  Userfaultfd System Call in a Compaction Phase of a Garbage Collection
  Process", Technical Disclosure Commons, 2020 —
  <https://www.tdcommons.org/dpubs_series/3671/>
- ART 源码：`art/runtime/gc/collector/mark_compact.cc`、
  `art/runtime/read_barrier_config.h`（AOSP / LineageOS 镜像均可检索）
- UFFDIO_MOVE 主提交：<https://github.com/torvalds/linux/commit/adef440691bab824e39c1b17382322d195e1fab0>
- UFFDIO_MOVE 可扩展性改进（2025，含 Android field trace 动机）：
  <https://lwn.net/Articles/1038678/>
