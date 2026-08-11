# io_uring 与 AOSP 工程实践

> 一次关于 Linux 新一代异步 I/O 框架的技术分享: 原理、对照实测数据, 以及在 Android/AOSP 中的落地方案。
> 配套代码: 本仓库 `iobench`(零依赖, 刻意用裸 syscall 实现 io_uring 以展示其机制; 生产环境可直接用 AOSP 内置的 liburing, 见 4.1)。
> 实测环境: Linux 7.0.0(x86_64 云主机, 2 vCPU, virtio 磁盘, ext4), 数据均为 3 轮取最优。

---

## 1. 为什么需要 io_uring: Linux I/O 模型的演进

### 1.1 传统方案及其天花板

| 方案 | 模型 | 问题 |
|---|---|---|
| 阻塞 read/write + 线程池 | 用线程换并发 | 每 IO 至少 1 次 syscall; 线程多了调度/栈内存开销大, 线程少了跑不满设备队列 |
| select/poll/epoll | I/O 多路复用 | **只对网络 IO 有意义**; 文件 IO 永远"就绪", 仍需线程池兜底 |
| POSIX AIO (glibc) | 用户态模拟 | glibc 实现就是线程池, 性能无优势, 接口别扭 |
| Linux libaio (`io_submit`) | 内核原生异步 | 只支持 **O_DIRECT**; buffered IO 会退化为阻塞; 每个请求仍要 copy_from_user 拷贝 iocb; 多年停止演进 |
| mmap + 缺页异常 | 零 syscall | 页错误路径开销大(异常、锁、LRU); 大页表抖动; 无法精确控制读ahead; 脏页回写不可控 |

核心矛盾: **存储设备越来越快(NVMe 百万级 IOPS), 而每个 IO 的固定开销(syscall 进出、参数拷贝、上下文切换)没有变**。一次 `pread` 的纯开销大约 1~2µs(CPU), 要用单线程打满一块现代 NVMe, 光 syscall 开销就远超 CPU 能力。

### 1.2 io_uring 的答案

Jens Axboe(Linux block 层维护者)2019 年提出, 5.1 合入主线:

- **提交与完成彻底异步化**: 用户态把请求写进共享内存的提交队列(SQ), 内核把结果写进完成队列(CQ), 双方靠 head/tail 指针 + 内存屏障通信, **正常情况下提交 N 个 IO 只需 0~1 次系统调用**;
- **零拷贝**: ring buffer 是 `mmap` 出来的内核/用户共享页, 请求不再逐次 copy_from_user;
- **一切皆可异步**: read/write/fsync/send/recv/accept/openat/statx/fallocate/timeout... 几乎覆盖全部 IO 相关 syscall, 包括 buffered 文件 IO(内核 io-wq 线程池兜底);
- **可选轮询模式**进一步压延迟: SQPOLL(内核线程轮询提交队列, 提交 0 syscall), IOPOLL(轮询完成, 配合 O_DIRECT + NVMe)。

一句话: **io_uring 把"每个 IO 一次系统调用"变成"一批 IO 一次(甚至零次)系统调用"**, 这和 Android 上 SurfaceFlinger 用共享内存 + fence 批量交换帧缓冲是同一个思想。

## 2. io_uring 核心设计

### 2.1 三个系统调用

```c
int io_uring_setup(unsigned entries, struct io_uring_params *p);   // 创建实例
int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags, ...); // 提交/等待
int io_uring_register(int fd, unsigned opcode, ...);               // 注册缓冲区/文件等
```

`setup` 返回一个 fd, 之后 `mmap` 出三块共享内存: SQ 环、CQ 环、SQE 数组(5.4+ 单次 mmap, `IORING_FEAT_SINGLE_MMAP`)。

### 2.2 提交流程(本项目 `src/engine_uring.c` 的完整实现)

```
用户态                          内核
  │  sqe->opcode=READ            │
  │  sq_array[idx]=idx           │
  │  *sq_tail++  (release)       │
  │──────── io_uring_enter ─────▶│ 消费 SQ, 下发到块层/io-wq
  │                              │
  │  *cq_tail 变化 (acquire) ◀───│ 写 CQE(res, user_data)
  │  *cq_head++                  │
```

关键细节:

- SQ 里存的是**索引**, 真正的请求结构 SQE(64 字节)在独立数组中;
- SQE 通过 `user_data` 携带用户上下文, 原样透传到 CQE —— 不需要任何回调注册;
- 一次 `io_uring_enter(fd, N, M, GETEVENTS)` 语义: 提交 N 个新请求, 并阻塞等到至少 M 个完成。提交和等待合并成一次 syscall;
- 内存序: tail 用 release 写、head 用 acquire 读(C11 `__atomic` 即可)。

### 2.3 三种工作模式

| 模式 | 提交路径 | 完成路径 | 适用 |
|---|---|---|---|
| 默认(中断驱动) | `io_uring_enter` | 中断 → CQE | 大多数场景, 本仓库默认 |
| `IORING_SETUP_SQPOLL` | 内核线程轮询 SQ, **提交 0 syscall**(仅睡眠唤醒/等完成时才 enter) | 中断 → CQE | 高频提交; 需 CAP_SYS_NICE; SQ 线程白吃一个核 |
| `IORING_SETUP_IOPOLL` | `io_uring_enter` | 轮询完成队列, 无中断 | 极低延迟 NVMe + O_DIRECT; 本环境 virtio 不支持, 未测 |

### 2.4 降低每-IO 固定开销的高级特性

- **注册缓冲区** `IORING_REGISTER_BUFFERS` + `READ_FIXED`: 内核预先 pin 住用户页并建好映射, 省去每次 IO 的 get_user_pages/建页表开销; 本仓库 `--fixed-buf`;
- **注册文件** `IORING_REGISTER_FILES`: 省去每次 IO 的 fget/fput 引用计数;
- **`IOSQE_IO_DRAIN` / `IOSQE_IO_LINK`**: 表达请求间顺序依赖(如 "写完再 fsync", 本项目日志场景即用 DRAIN);
- **`IORING_OP_FSYNC`(DATASYNC)**: fsync 本身也异步化, 不阻塞调用线程;
- **multishot / provided buffers / timeout / poll**: 网络编程利器(未在本次实测范围)。

### 2.5 buffered IO 的异步兜底

buffered read 若命中页缓存, 内核在 `io_uring_enter` 内联完成(不下发); 未命中或写路径, 则 offload 到内核 io-wq 线程池, 完成后写 CQE。**用户态全程无感** —— 这正是相对 libaio 的本质优势: 语义上真异步, 工程上不用管 O_DIRECT 对齐。

## 3. io_uring 与其他方案对照(理论)

| 维度 | pread 线程池 | mmap | libaio | **io_uring** |
|---|---|---|---|---|
| 每千次 IO syscall 数 | ~1000 | ~0(但页错误 ~1000) | ~1000/submit | **可低至 ~1** |
| buffered IO 真异步 | ✗(线程兜底) | ✗ | ✗ | ✓ |
| O_DIRECT 要求 | 无 | 无 | 有 | 无(可选) |
| 并发扩展方式 | 加线程 | 加线程(页错误串行化在 mmap_lock) | 队列 | 队列 |
| CPU 开销/IO | 高(syscall+调度) | 中(缺页异常) | 中 | **低** |
| 编程复杂度 | 低 | 低 | 中 | 中高(需自己管 ring) |
| 内核版本要求 | 任意 | 任意 | 老 | ≥5.1, 新特性要 5.6+/5.11+ |

## 4. io_uring 在 AOSP 中的现状

### 4.1 内核与用户态依赖: 全部就位

- **内核**: Android GKI 内核(android12-5.10 及以后)默认 `CONFIG_IO_URING=y`;
- **管控能力**: 内核 6.6 起提供 `kernel.io_uring_disabled` sysctl(0=允许 / 1=禁非特权 / 2=全禁,
  见 [Phoronix 报道](https://www.phoronix.com/news/Linux-6.6-sysctl-IO_uring)); 内核亦已补齐 io_uring
  的 LSM 钩子(SELinux 新增 `io_uring` 对象类, 覆盖 `sqpoll` / `cmd` 权限), Android 借此把 io_uring
  收编为白名单系统进程专用 —— "按进程精细管控"在内核层面已经备齐;
- **用户态库**: AOSP 内置 [`external/liburing`](https://android.googlesource.com/platform/external/liburing/)
  和 Google 的 C++ 封装 [`liburing_cpp`](https://github.com/google/liburing_cpp), 系统组件可直接静态链接,
  **不必像本项目 demo 一样手撸裸 syscall**(本项目是为了展示机制); bionic 本身不封装 io_uring, 对 App 也不开放。

### 4.2 安全侧: 对 App 全面封禁, 系统进程白名单

io_uring 是近年 Linux 内核最大的攻击面之一。Google 安全团队 2023-06 披露: **2022 年 kCTF(内核漏洞奖励计划)收到的 exploit 中 60% 针对 io_uring**, 为此支付的奖金约 $1M。对策([Google Security Blog](https://security.googleblog.com/2023/06/learnings-from-kctf-vrps-42-linux.html)):

- **Android**: seccomp-bpf 过滤器使 app 进程**完全无法调用** io_uring 相关 syscall; 后续版本用 SELinux 把 io_uring 限制在**少数系统进程**白名单内;
- ChromeOS: 直接禁用; Google 生产服务器: 默认禁用。

典型 CVE: CVE-2023-21400(Pixel 7 上有完整提权 PoC)、CVE-2023-2598(注册缓冲区越界)、CVE-2024-0582(UAF) 等。

**结论一: App 层(Native 也一样)用不了 io_uring, 不要把它写进面向三方 App 的方案里。落地空间在系统侧: 系统原生守护进程、HAL、vendor 组件。**

### 4.3 已落地模块盘点: 目前是 snapuserd 一枝独秀

盘点 AOSP 源码, **大规模生产使用 io_uring 的系统模块目前就是 snapuserd**(Virtual A/B 的用户态快照守护进程,
`system/core/fs_mgr/libsnapshot/snapuserd/`), 围绕它的数据链都已 io_uring 化:

| 组件 | io_uring 用法 | 证据 |
|---|---|---|
| 快照**合并**(merge worker) | 批量下发 COPY 操作的 read/write/fsync | [LPC2022](https://lpc.events/event/16/contributions/1331/attachments/951/1867/LPC2022%20-%20io_uring%20in%20Android%20OTA.pdf): perf 火焰图显示 `MergeOrderedOpsAsync` → `io_uring_submit_and_wait` |
| 合并期**预读**(readahead worker) | 异步批量预读 COW 块 | 同上: `ReadAhead::RunThread` 走 io_uring 提交 |
| 构建依赖 | 静态链接 `liburing` + `liburing_cpp` | [snapuserd/Android.bp](https://github.com/aosp-mirror/platform_system_core/blob/main/fs_mgr/libsnapshot/snapuserd/Android.bp) 的 `static_libs` 明确列出 |
| 运行开关 | 系统属性 `ro.virtual_ab.io_uring.enabled`(默认开启) | 设备日志可见 `update_engine: io_uring for snapshots enabled` |

**为什么值得做**: 虚拟 A/B 的合并是海量小块 COPY —— 一次 ~200MB 增量 OTA, SYSTEM 约 15 万个 COPY 操作、
PRODUCT 约 35 万个, 每个 COPY = read + write + fsync 三次 syscall, 合计 **150 万次系统调用**(算上
SYSTEM_EXT/VENDOR 超 200 万)。合并完成前根文件系统挂载在 dm-user 上, 所有 IO 都过 snapuserd, 所以
**合并速度直接等于用户感知的"OTA 后首次开机卡顿时长"**。

**效果**(Pixel 6 / Android T, Google 实测): 增量 OTA 合并 **120~180s → 60~75s, 缩短约 40~50%**,
同时 CPU 占用与线程数显著下降(对应我们场景 C 实测: 同样的批量 COPY, syscall 数可压缩 30 倍以上)。

其余系统组件(logd、installd、adbd 等)**目前均未使用 io_uring** —— 这正是第 5 章的机会清单。

### 4.4 演进方向: ublk

上游 ublk(5.19+)是基于 io_uring 命令通道的用户态块设备框架("块设备版 FUSE"), Android 团队已原型验证用 ublk-loop 替代 out-of-tree 的 dm-user 跑 OTA COPY。对 vendor 而言, 这意味着用户态存储逻辑(压缩、加密、去重)可以用 io_uring 高性能地挂进块层。

## 5. 还有哪些模块可以改进(机会清单)

结合第 7 章实测数据, 按"收益 × 改动成本"盘点 AOSP 系统组件(均为我们的工程评估, 非官方路线图):

### 5.1 第一梯队: 收益明确、改动可控

| 模块 | 当前 IO 模式 | 对应实测 | 预期收益 | 备注 |
|---|---|---|---|---|
| **日志/事件落盘类**: logd 持久化、statsd、incidentd、tombstoned | write + 定期 fsync | 场景 D | 批量提交 + DRAIN 异步 fsync: syscall 降 1~2 个数量级, 吞吐潜力 ~25x(攒批) | 语义简单, 有 snapuserd 先例可循; **首推练手对象** |
| **OTA 类批量 COPY**(已落地) | 逐块 read/write/fsync | 场景 C | Google 已验证 40~50% | 已落地, 可作内部布道案例 |
| **iorap 启动预读** | 逐文件 readahead/read | 场景 C | 预读批量化, 降低启动关键路径的 syscall 与 CPU 抖动 | 深度 16~32 即可 |

### 5.2 第二梯队: 收益真实但要动架构

| 模块 | 当前 IO 模式 | 对应实测 | 预期收益 | 风险/前提 |
|---|---|---|---|---|
| **installd / dex2oat**(应用安装、OTA 后 dexopt) | 大量小文件顺序读写、逐文件 fsync | 场景 A/B/D | 安装与"应用优化中"阶段 CPU 降 25~57%; 批量 fsync 收敛落盘次数 | 改动面大, 需灰度; 用户感知强(安装速度) |
| **MediaProvider 缩略图/媒体扫描** | 逐文件打开读头部 | 场景 A | 批量随机读, CPU 减半 | 深度匹配 eMMC/UFS 队列 |
| **adbd 文件传输**(adb push/pull) | sendfile/逐块读写 | 场景 N3/C | 需叠加 SEND_ZC/零拷贝才有大收益(专题二 N3 的教训: 单 ring 打不过多线程拷贝) | 中; 适合作为 SEND_ZC 试验田 |
| **FUSE 外置存储守护进程** | /dev/fuse 读请求 → 后端文件 IO, 两个等待点 | 场景 N1 | fuse 请求与后端 IO 共用一个 ring, 事件循环收敛 | 中高; 注意 passthrough 场景已绕过 daemon |

### 5.3 不建议碰

| 模块 | 原因 |
|---|---|
| netd / mdnsd / keystore / gatekeeper | 低并发管理面, epoll/阻塞 IO 已足够, 换 io_uring 只增复杂度 |
| 三方 App(含其 native 库) | seccomp 直接拦截(SIGSYS), Google 红线, 见 4.2 |
| 深度 1 的同步小读写路径 | 实测无收益(专题一场景 A: uring d1 ≈ pread), 批量化才是收益来源 |

**结论二: 系统侧最划算的切入点是 "日志/WAL 批量落盘" 和 "OTA 类批量 COPY" —— 前者零风险改动小, 后者已有 Google 自己背书; 第二梯队里 installd/dexopt 的用户感知最强, 适合有灰度能力的团队跟进。**

## 6. Benchmark 方法论

- **工具**: 本仓库 `iobench`, 所有引擎跑**同一组固定种子的偏移序列**, 保证访问模式逐字节一致;
- **指标**: IOPS / 带宽、平均与 p99 延迟、**每千次 IO 的 CPU 微秒**(getrusage, 含内核态)、**系统调用总次数**(io_uring 侧精确计数 io_uring_enter)、页错误数;
- **场景**:
  - A: 4KiB 随机读 × 20000 次, 运行前 `drop_caches`(冷缓存);
  - B: 4KiB 随机读 × 100000 次, 页缓存预热(纯开销对照);
  - C: 256KiB 顺序读 × 1024 次, 冷缓存(模拟 OTA 包读取/快照合并);
  - D: 4KiB 日志记录 × 8192 条, 每条必须落盘(fdatasync 语义);
- 每组配置跑 3 轮取最优; 环境为 2 vCPU 云主机 + virtio 磁盘, 随机 IO 上限约 2.4k IOPS, **绝对值仅作参考, 看相对关系**。

## 7. 实测数据与分析

> 完整数据: `results/results.csv`; 图表: `results/*.png`。每组 3 轮取最优。

### 场景 A: 4KiB 随机读(冷页缓存)—— 模拟冷启动资源加载 / SQLite 页缺失

| 引擎 | 深度 | IOPS | 平均延迟 | CPU us/千次IO | 系统调用 |
|---|---|---|---|---|---|
| pread | 1 | 1,540 | 649 us | 18,100 | 20,000 |
| pread_mt | 4 线程 | 2,063 | 1.94 ms | 14,881 | 20,000 |
| pread_mt | 16 线程 | 2,331 | 6.86 ms | 13,303 | 20,000 |
| preadv2_nowait | 1 | 1,572 | 636 us | 18,933 | 37,165(86% EAGAIN 空转) |
| mmap | 1 | 1,590 | 629 us | 20,009 | 2(+17,273 页错误) |
| posix_aio(glibc) | 64 | 1,602 | 39.9 ms* | 35,061 | -(内部线程池) |
| libaio(buffered) | 64 | 1,583 | 40.4 ms* | 17,592 | 626 |
| libaio(O_DIRECT) | 64 | 2,062 | 31.0 ms* | **3,604** | 1,237 |
| uring | 1 | 1,570 | 637 us | 22,797 | 20,000 |
| uring | 32 | 2,352 | 13.6 ms* | 7,892 | 1,215 |
| uring | 64 | 2,392 | 26.5 ms* | 7,991 | 628 |
| uring+fixed buf | 64 | 2,391 | 26.8 ms* | 8,108 | 628 |
| uring+sqpoll | 64 | 2,393 | 27.1 ms* | 415,757(!) | 619 |

![冷随机读 IOPS](../results/randread_cold_iops.png)
![冷随机读 CPU](../results/randread_cold_cpu.png)

**解读:**

1. **磁盘是瓶颈时, 所有"深度>1"方案吞吐趋同**(≈2.3k IOPS, 即这块 virtio 盘的上限)——io_uring 不能变出磁盘带宽, 它的收益在**达成同等吞吐所花的代价**: CPU 比 pread 省 **56%**(8.0k vs 18.1k us/kop), 系统调用少 **32 倍**(628 vs 20,000)。对应到 Android: OTA 合并/媒体扫描跑在后台时, 省下的 CPU 就是前台流畅度和电量。
2. **深度 1 的 io_uring 没有收益甚至略亏**(CPU 22.8k vs pread 18.1k)——收益来自**批量化**, 不是来自"异步"两个字。这是最常见的误用。
3. **mmap 冷路径并不便宜**: 2 次 syscall 换来 1.7 万次页错误, CPU 最高一档。它只是把开销从 syscall 挪到了异常处理。
4. **SQPOLL 在共享 CPU 上是反模式**: 完成等待路径的内核轮询被计入调用者 stime, CPU 暴涨数十倍。SQPOLL 需要独立核 + 高速 NVMe 才划算; 移动端 big.LITTLE 架构请直接用默认模式。
5. *深度 N 引擎的"平均延迟" ≈ N/IOPS(排队窗口, Little's law), 不是单次服务时间, 不能直接和 depth=1 引擎比延迟。*
6. AIO 家族的逐条验尸见下文"为什么非得是 io_uring"一节。

### 场景 B: 4KiB 随机读(热页缓存)—— 纯开销对照

| 引擎 | 深度 | IOPS | p50 延迟 | CPU us/千次IO | 系统调用 |
|---|---|---|---|---|---|
| pread | 1 | 716,439 | 1.3 us | 1,396 | 100,000 |
| pread_mt | 4 线程 | **1,204,124** | 1.4 us | 1,627 | 100,000 |
| pread_mt | 16 线程 | 1,115,546 | 1.5 us | 1,745 | 100,000 |
| preadv2_nowait | 1 | 663,970 | 1.3 us | 1,505 | 100,000 |
| mmap | 1 | **1,469,813** | 0.5 us | **680** | 2 |
| posix_aio(glibc) | 64 | 178,002 | 351 us | 6,796 | -(内部线程池) |
| libaio(buffered) | 64 | 864,522 | 69.4 us* | 1,149 | 3,126 |
| uring | 1 | 590,447 | 1.5 us | 1,692 | 100,000 |
| uring | 32 | 936,327 | 32.1 us* | 1,068 | 3,125 |
| uring | 64 | 946,706 | 63.7 us* | **1,054** | **1,563** |
| uring+fixed buf | 64 | 917,188 | 66.3 us* | 1,090 | 1,563 |
| uring+sqpoll | 64 | 782,011 | 71.1 us* | 1,452 | 2,072 |

**解读:**

1. 热缓存小读的王者是 **mmap**(纯 memcpy, 0.6µs/op)——如果数据确定在页缓存里, 不需要 io_uring;
2. **io_uring 是单核效率王**: 1 个线程跑到 95 万 IOPS, 每次 IO 的 CPU 最低(1,054us/kop, 比 pread 省 25%); pread_mt(4) 总吞吐更高, 但那是约 2 个核烧出来的(1,627us/kop × 120 万 ≈ 2.0 核), 折算单核效率只有 io_uring 的六成;
3. 深度 64 时 **64 次 IO 才摊 1 次 syscall**(100k 次 IO 仅 1,563 次 enter);
4. SQPOLL 在 2 vCPU 上吞吐反降 17% —— 再次验证: 忙核系统别用 SQPOLL;
5. **posix_aio 热路径垫底**(17.8 万 IOPS, CPU 6.8k): glibc 内部每 IO 一次线程交接, 纯开销;
   **libaio buffered 热路径意外能打**(86 万 IOPS)——缓存命中时 io_submit 内联完成, 路径与 io_uring 的热缓存内联完成本质相同; 但它的短板在冷路径(见场景 A 与下文)。

### 场景 C: 256KiB 顺序读(冷页缓存)—— 模拟 OTA 包读取 / 快照合并 COPY

| 引擎 | 深度 | 带宽 | CPU us/千次IO | 系统调用 |
|---|---|---|---|---|
| read | 1 | 106.8 MiB/s | 86,992 | 1,024 |
| pread_mt | 4 线程 | 97.7 MiB/s | 90,973 | 1,024 |
| mmap | 1 | 102.7 MiB/s | 81,003 | 2 |
| libaio(O_DIRECT) | 8 | 97.9 MiB/s | **25,885** | 514 |
| uring | 8 | 107.2 MiB/s | 95,645 | 131 |
| uring | 32 | 107.0 MiB/s | 114,004 | **35** |

![顺序读带宽](../results/seqread_cold_bw.png)

**解读:** 大块顺序读全部打满磁盘(≈100MiB/s), CPU 被 256KiB 的页缓存 memcpy 主导; 唯一例外是 **libaio O_DIRECT——绕过页缓存没有 memcpy, CPU 只有别人的 1/3**, 这正是 O_DIRECT 的价值(代价是放弃缓存)。
io_uring 的意义在于: **35 次 syscall 完成别人 1,024 次的工作**。把这个比例代回 snapuserd 的 COPY 操作(read+write+fsync = 3 syscall/op): 150 万次 syscall 压缩到几万次 enter, 再叠加写路径批量下发, 就是 Google 实测 **120~180s → 60~75s** 的来源。

### 场景 D: 4KiB 日志记录落盘(含持久化)—— 模拟 logd / WAL commit

| 引擎 | 批大小 | 吞吐(rec/s) | 批延迟 | CPU us/千条 | 系统调用 |
|---|---|---|---|---|---|
| write + fdatasync 每条 | 1 | 472 | 2.12 ms/条 | 58,126 | 16,384 |
| 攒批写 + fdatasync | 32 | 11,467 | 2.79 ms/批 | 6,729 | 8,448 |
| **uring DRAIN 批写** | 32 | **11,457** | 2.79 ms/批 | 8,767 | **256** |

![日志落盘吞吐](../results/logwrite_iops.png)
![日志落盘 syscall 数](../results/logwrite_syscalls.png)

**解读:**

1. **每条 fsync 是性能杀手**: journal commit 延迟(~2ms)把吞吐锁死在 472 rec/s; 攒批后 11.5k rec/s, **提升 24 倍** —— 这部分与 io_uring 无关, 是"批量换吞吐"的普适规律;
2. io_uring 的增量价值: 同样攒 32 条, **一次 `io_uring_enter` 提交 32 个写 + 1 个带 `IOSQE_IO_DRAIN` 的 fsync**, 8192 条记录全程只有 **256 次 syscall**(比手工攒批再少 33 倍), 且 fsync 本身异步执行, 不阻塞业务线程;
3. CPU 上两者同量级(journal 提交的内核工作占大头), io_uring 略高的部分可在真实场景用注册缓冲区/文件进一步压。

### 为什么非得是 io_uring: "前任"方案实测验尸

| 方案 | 实测表现(场景 A/B) | 结论 |
|---|---|---|
| **preadv2 + RWF_NOWAIT** | 冷缓存 **86% 请求 EAGAIN 回退**, syscall 数全场最高(37,165: 2 万次有效 + 1.7 万次空转); 热缓存 ≈ pread | 只是"页缓存命中"的快路径, 未命中就甩锅给调用方自己回退 —— **不构成异步方案** |
| **POSIX AIO**(glibc) | 冷读 CPU 是 pread 的 **1.9 倍**(35k vs 18k us/kop); 热读 IOPS 全场垫底(17.8 万, 仅为 pread 的 1/4) | 用户态线程池模拟, 每次 IO 一次线程交接; **没有内核批量, 只有额外开销** |
| **libaio buffered** | 冷读 ≈ pread(1,583 IOPS, 请求在 `io_submit` 里**同步执行**); 热读能打(86 万 IOPS, 内联完成) | 名义 AIO, 冷路径实际同步; "syscall 变少(626)"只是把阻塞搬进了 submit; 接口十余年停滞 |
| **libaio O_DIRECT** | 真异步, CPU 全场最低(3.6k us/kop, 无页缓存 memcpy), 但冷读 IOPS 仍低于 uring d64(2,062 vs 2,392) | 唯一能打的前任, 但代价是**绕过页缓存**(对齐约束、放弃缓存、预读自己管), 且功能面窄(无 fsync 批量/link/multishot/网络) |
| **io_uring** | 冷读 IOPS 最高(2,392)、CPU 比 pread 省 56%、syscall 省 32 倍 | **唯一同时满足: buffered 真异步 + 批量提交收割 + 统一文件/网络/定时器 + 内核主线活跃演进** |

两点补充, 避免误读:

- libaio O_DIRECT 的 CPU 优势来自 O_DIRECT 本身, 不是 libaio —— **io_uring 同样支持 O_DIRECT**(open 时加标志即可), 还能再叠 IOPOLL, 这条路线上限更高;
- libaio buffered 热路径追平 io_uring, 恰好反证了 io_uring 的价值定位: **优势不在"缓存命中的热路径"(那里大家都内联完成), 而在冷路径的真异步与统一的工程语义**。

所以答案不是"io_uring 跑分最高", 而是: **pread 家族没有并发, 线程池用 CPU 换并发, mmap 用页错误换 syscall, NOWAIT 只管热缓存, POSIX AIO 是假异步, libaio 只能 O_DIRECT —— 每个方案都用一种硬伤换一项收益; io_uring 是第一个不用做这种交换的接口。**

### 结论汇总: 什么时候用 io_uring

| 情况 | 建议 |
|---|---|
| 系统进程, 批量小块 IO(OTA/日志/扫描) | **用**, 深度 32~64, 收益: CPU 省 25~56%, syscall 省 1~2 个数量级 |
| 需要"写完落盘"语义的批量提交 | **用 DRAIN/link + 异步 FSYNC**, 语义等价 write+fdatasync 攒批, syscall 再省 33 倍 |
| 极致 CPU 敏感且可放弃页缓存 | libaio O_DIRECT 也能打, 但 **io_uring + O_DIRECT + IOPOLL 上限更高**且主线活跃 |
| 深度 1 的同步读改写 | **不用**, 批量化才是收益来源, 深度 1 无利可图 |
| 数据确定在页缓存的热路径小读 | mmap/pread 即可, 别为用而用 |
| 冷路径"异步" | RWF_NOWAIT / POSIX AIO / buffered libaio 实测全部不及格(见验尸表), 只有 io_uring |
| App 进程(含 Native) | **用不了**, seccomp 拦截(SIGSYS), 这是 Google 红线 |
| SQPOLL | 共享/忙核 CPU 上**不要用**; 独立核 + NVMe 才考虑 |

## 8. 工程落地 Checklist

1. **权限**: 确认目标进程在 io_uring 的 SELinux/seccomp 白名单内(系统进程);  vendor 进程需加 sepolicy, 参考 snapuserd 的 `io_uring` 相关 allow 规则;
2. **内核版本**: 基础读写 ≥5.1 即可, `READ_FIXED`/multishot 等特性注意 5.6/5.11/6.0 的版本线; GKI 设备按 android12-5.10 起基本都全;
3. **降级策略**: 运行时用一次 `IORING_OP_NOP` 探测, 失败(EPERM/ENOSYS)则回退 pread/线程池路径 —— 本项目 `uring_probe` 即此思路;
4. **深度选择**: 从 32 起步, 对照设备队列深度调; 过深只涨延迟不涨吞吐;
5. **失败处理**: link/DRAIN 链上前者失败会级联取消后续请求(CQE res = -ECANCELED), 重试逻辑要覆盖;
6. **调试**: `strace -e io_uring_enter` 观察提交频率; `/proc/<pid>/fdinfo` 看 ring 状态; perf 火焰图确认 CPU 收益;
7. **不要在 App 层使用**: 会被 seccomp 杀掉(SIGSYS), 且这是 Google 明确的红线。

## 9. 参考资料

- [LPC2022: io_uring in Android OTA(Akilesh Kailash, Google)](https://lpc.events/event/16/contributions/1331/attachments/951/1867/LPC2022%20-%20io_uring%20in%20Android%20OTA.pdf)
- [Google Security Blog: Learnings from kCTF VRP's 42 Linux kernel exploits(2023-06)](https://security.googleblog.com/2023/06/learnings-from-kctf-vrps-42-linux.html)
- [Phoronix: Google Limiting IO_uring Use Due To Security Vulnerabilities](https://www.phoronix.com/news/Google-Restricting-IO_uring)
- [Phoronix: Linux 6.6 sysctl 可全局禁用 io_uring](https://www.phoronix.com/news/Linux-6.6-sysctl-IO_uring)
- AOSP 源码: [snapuserd/Android.bp(liburing + liburing_cpp 依赖)](https://github.com/aosp-mirror/platform_system_core/blob/main/fs_mgr/libsnapshot/snapuserd/Android.bp)、[external/liburing](https://android.googlesource.com/platform/external/liburing/)、[google/liburing_cpp](https://github.com/google/liburing_cpp)
- Jens Axboe, [Efficient IO with io_uring](https://kernel.dk/io_uring.pdf)
- `man io_uring_setup / io_uring_enter / io_uring_register`
- 本仓库源码与实测脚本
