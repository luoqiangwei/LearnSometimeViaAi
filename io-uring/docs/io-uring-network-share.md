# io_uring 网络编程与 AOSP 落地(专题二)

> 上一篇([io-uring-aosp-share.md](io-uring-aosp-share.md))覆盖了文件 IO; 本篇聚焦**网络 IO**:
> thread-per-connection / epoll / io_uring 三种服务器模型的对照实测。
> 配套工具: 本仓库 `netbench`(loopback TCP echo 压测, 服务器进程独立统计 CPU 与 syscall)。

---

## 1. 网络 IO 模型的演进: 从 C10K 到 C10M

| 模型 | 每消息 syscall | 并发扩展瓶颈 |
|---|---|---|
| thread-per-connection | 2(recv/send) | 线程数 = 连接数: 栈内存、调度、上下文切换; 万级连接即崩 |
| select/poll | 2 + 每次扫描 O(n) | fd 集合线性扫描, 位图大小限制 |
| epoll | 2 + epoll_wait 摊薄 + 每连接 epoll_ctl ×3 | 每连接至少 ADD/MOD/DEL 三次 ctl; ET 模式 EAGAIN 排空浪费调用; 每消息 recv/send 仍是 2 次 syscall |
| **io_uring** | **摊薄到 ≪1** | 队列深度与连接数解耦; accept/recv/send 全部走共享 ring |

epoll 是 2002 年的答案, 解决了"哪个 fd 就绪"的发现问题, 但**数据收发本身仍是一消息一 syscall**,
连接管理(epoll_ctl)也是逐连接 syscall。io_uring 把"发现 + 收发 + 建连"全部并入批量化的提交/完成队列。

## 2. io_uring 的网络武器库

| 特性 | 说明 | 内核版本 |
|---|---|---|
| `IORING_OP_ACCEPT` | 异步 accept; 加 `IORING_ACCEPT_MULTISHOT` 后**一次武装, 持续产新连接** | 5.5 / 5.19(multishot) |
| `IORING_OP_RECV/SEND` | 异步收发, 等价 recv/send | 5.6 |
| `IORING_OP_RECVMSG/SENDMSG` | 带辅助数据 | 5.3+ |
| multishot recv + provided buffers | `IORING_RECV_MULTISHOT` + buffer ring: 内核持续接收并自动从 buffer ring 取缓冲, **收到数据才产生 CQE, 不耗 SQE** | 5.19 / 6.0 |
| `IORING_OP_SEND_ZC` | 零拷贝发送(绕过 skb 数据拷贝) | 6.0 |
| `IORING_OP_SOCKET/CONNECT` | 连 socket 创建、TCP 三次握手发起都能异步化 | 5.19+ |
| `IORING_OP_TIMEOUT` / `ASYNC_CANCEL` / `POLL_ADD` | 超时、取消、任意 fd 就绪监听 | 5.5+ |

**架构层面的杀手锏: 统一事件源。** socket、文件、管道、定时器、eventfd 全部挂在同一个 ring 上,
一个线程一个 `io_uring_enter` 处理所有事件 —— 取代 epoll + timerfd + eventfd + signalfd 的组合拼装。
这与 Android native 层的 `Looper`(epoll + eventfd + timer)结构一一对应, Looper 类机制可以整体迁移到单个 ring 上。

## 3. Benchmark 设计(netbench)

```
父进程(客户端)                     子进程(被测服务器)
 C 个线程, 每线程 1 条连接          threads: accept + 每连接一线程, 阻塞读写
 write(B) → read(B)  ping-pong     epoll:   ET 非阻塞, 事件循环
 记录每次 RTT                       uring:   multishot accept + RECV/SEND, 单线程
      │ kill(SIGINT) + wait4 ──▶   报告: rusage(CPU) + syscall 计数(插桩)
```

- 服务器是**独立子进程**: CPU 用 `wait4` 的 rusage 精确归属, 不与客户端混淆;
- 三种引擎跑**完全相同**的负载; 引擎的 syscall 数在代码里逐点插桩(epoll 含 epoll_ctl/epoll_wait/EAGAIN 排空, uring 含 io_uring_enter + setsockopt);
- 场景: N1 = 64B × 64 连接 × 3000 次 ping-pong; N2 = 64B × 256 连接 × 1000 次; N3 = 16KiB × 64 连接 × 300 次。每组 3 轮取最优;
- **局限**: loopback 没有真实网卡中断/软 IRQ, 测的是**模型开销**(syscall、CPU、调度); 2 vCPU 共享环境, 客户端与服务器互相抢核, 绝对值仅作参考, 看相对关系。

## 4. 实测数据与分析

> 完整数据: `results/results_net.csv`; 图表: `results/net_*.png`。每组 3 轮取最优。
> RTT 含客户端线程调度噪声(2 vCPU 共享环境), 重点看 RPS、服务端 CPU、服务端 syscall 三列。

### 场景 N1: 64B 消息 ping-pong, 64 并发连接(19.2 万条消息)

| 引擎 | RPS | RTT 平均 | RTT p99 | 服务端 CPU us/千条 | 服务端 syscall |
|---|---|---|---|---|---|
| threads | 91,026 | 647 us | 2,221 us | 10,094 | 384,193 |
| epoll | 93,644 | 670 us | 2,574 us | 9,658 | 593,657 |
| **uring** | **98,841** | 642 us | **1,547 us** | **7,332** | **6,483** |

### 场景 N2: 64B 消息 ping-pong, 256 并发连接(25.6 万条消息)

| 引擎 | RPS | RTT 平均 | RTT p99 | 服务端 CPU us/千条 | 服务端 syscall |
|---|---|---|---|---|---|
| threads | **91,677** | 2,355 us | 6,589 us | 10,277 | 512,769 |
| epoll | 87,530 | 2,781 us | 7,124 us | 9,900 | 750,658 |
| uring | 84,499 | 2,956 us | **5,173 us** | **7,674** | **2,554** |

![N2 RPS](../results/net_pingpong_256c_64B_rps.png)
![N2 服务端 syscall 数](../results/net_pingpong_256c_64B_sys.png)

### 场景 N3: 16KiB 消息 echo, 64 并发连接(1.92 万条)

| 引擎 | RPS | 带宽 | 服务端 CPU us/千条 | 服务端 syscall |
|---|---|---|---|---|
| threads | **77,646** | **1,213 MiB/s** | 12,405 | 38,593 |
| epoll | 64,566 | 1,009 MiB/s | 14,543 | 57,432 |
| uring | 58,278 | 911 MiB/s | 12,826 | **697** |

**解读:**

1. **syscall 数量级碾压, 且并发越高优势越大**: N2 中 io_uring 全程仅 **2,554 次** enter,
   平均**一次系统调用处理约 100 条消息**的收发; epoll 是 75 万次(≈3 次/消息: wait+recv+send),
   threads 是 51 万次(2 次/消息)——**293 倍 / 200 倍差距**。这正是 io_uring 的设计承诺。
2. **服务端 CPU 稳定省 20~30%**(N1: 7.3k vs epoll 9.7k us/kmsg; N2: 7.7k vs 9.9k)。
   对 Android 的意义: 同样的代理吞吐, 省下的 CPU 就是电量和前台响应。
3. **RPS 在 loopback 上拉不开差距**(±10% 以内): 瓶颈在客户端线程与调度(2 vCPU 跑 64~256 个客户端线程),
   服务端模型效率不直接等于吞吐 —— 真实收益要放到"CPU 受限的移动设备"语境里看, 省 CPU ≈ 省功耗。
4. **大包吞吐场景(16KiB)threads 反而最高**: 传输 CPU 被数据拷贝主导, threads 模型能吃满双核并行拷贝,
   单线程 uring 吃亏。生产写法的下一步是 `SEND_ZC`(零拷贝发送)与 provided buffers, 或在多核上跑多 ring。
5. **threads 模型在 ~256 连接规模并未崩溃** —— 它的墙在 C10K+(线程内存与调度), 本环境测不出来;
   但如果业务连接数就是几百级, 它的简单性仍是合理选项。别为用而用。
6. epoll 的 RTT p99 在三个场景中 consistently 最差(2.6/7.1/4.0ms): ET 排空 + 事件循环串行处理在
   高负载下的队头阻塞; io_uring 的完成队列天然批量, p99 在 N1/N2 均为最好。

### 网络篇结论汇总

| 情况 | 建议 |
|---|---|
| 系统侧高并发代理/转发(vendor IPC、投屏、文件传输) | **用 io_uring**, 生产写法上 multishot recv + provided buffers, 收益: CPU 省 20~30%, syscall 省 1~2 个数量级 |
| 连接数 ≤ 几百的内部服务 | threads/epoll 都够用, io_uring 是优化不是必需 |
| 大流量单连接传输 | 先评估 SEND_ZC; 单线程 ring 打不过多线程拷贝 |
| 短连接密集(建连成本高) | **multishot accept 一次武装永久生效**, 对比 epoll 每连接 3 次 epoll_ctl, 优势最大 |
| 三方 App | **用不了**(seccomp, 见专题一) |

## 5. AOSP 网络场景落地评估

先说结论: **AOSP 系统侧网络 daemon 大多是低并发管理面**(netd、mdnsd、logd 的 socket),
epoll 已绰绰有余; io_uring 的价值点在**高并发 / 高吞吐的数据面代理**:

| 候选场景 | 并发特征 | 评估 |
|---|---|---|
| adbd(adb over TCP/Wi-Fi 调试) | 单连接高吞吐 | 收益有限, 但模型可简化(收发一体 ring) |
| 文件传输类(MTP/文件共享/cast 投屏服务) | 中并发 + 大流量 | **值得**: SEND/RECV 批量化 + 可叠加 SEND_ZC; 收发与文件读写可共用一个 ring(sendfile 的 io_uring 化) |
| vendor 自定义高吞吐 IPC(车载、电视盒子投屏/镜像) | 高并发小包 | **最值得**: 对照实测中 uring 在高并发下 syscall 与 CPU 优势最大 |
| netd / mdnsd / logd 等管理面 daemon | 低并发 | 不建议, 徒增复杂度 |
| 三方 App | — | **用不了**(seccomp 拦截, 见专题一第 4.2 节) |

另一个被低估的角度: **统一事件源**。Android native daemon 里 `Looper`(epoll+eventfd+timerfd)
处理 socket、定时器、跨线程唤醒三件套; 迁到 io_uring 后, 文件读写、socket、timeout(POLL_ADD/TIMEOUT)
全在一个等待点上, 事件循环代码和 syscall 数同时收敛。这对长生命周期、事件来源杂的系统服务(如
statsd、incidentd 这类既收 socket 又写文件的)是结构性简化。

## 6. 工程建议(网络篇)

1. **连接管理成本**: epoll 模型下每连接至少 3 次 epoll_ctl + N 次 recv/send; io_uring 下建连由
   multishot accept 一次武装完成, 收发走 ring —— 短连接比例越高, io_uring 优势越大;
2. **multishot recv + provided buffers 是生产级写法**(本次 demo 用单发 RECV 是为可读性):
   消除反复武装 recv 的 SQE, 且内核自动分配接收缓冲, 进一步压低 SQE 消耗;
3. **失败级联**: RECV/SEND 出错(-ECONNRESET 等)直接体现在 CQE res, 关闭逻辑要覆盖所有 op;
4. **TCP_僵尸连接**: io_uring 没有内建 keepalive 语义, 用 IORING_OP_TIMEOUT 给每轮 recv 挂超时;
5. **调试**: `ss -tinp` 看连接队列; `strace -c -e io_uring_enter` 对比提交频率; perf 看 CPU 收益;
6. 社区参考: DragonflyDB 整体架构建立在 io_uring 上; ScyllaDB(seastar)、QEMU、RockDB(MultiRead)
   均已生产使用; tokio-uring / glommio 提供 Rust 运行时。

## 7. 参考资料

- [LPC2022: io_uring in Android OTA](https://lpc.events/event/16/contributions/1331/attachments/951/1867/LPC2022%20-%20io_uring%20in%20Android%20OTA.pdf)
- [Google Security Blog: Learnings from kCTF VRP's 42 Linux kernel exploits](https://security.googleblog.com/2023/06/learnings-from-kctf-vrps-42-linux.html)
- Jens Axboe, [Efficient IO with io_uring](https://kernel.dk/io_uring.pdf)
- [liburing 源码中的 echo 示例与 man 页](https://github.com/axboe/liburing)(`man io_uring_prep_multishot_accept` 等)
- 专题一: [io-uring-aosp-share.md](io-uring-aosp-share.md)
- 本仓库 `netbench` 源码与 `tools/run_netbench.sh`
