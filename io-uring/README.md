# io-uring-aosp-demo

面向 AOSP 工程实践的 io_uring 分享配套项目: 零依赖的对照 benchmark 工具集,
io_uring 部分刻意用裸 syscall 封装(不依赖 liburing, 展示 SQ/CQ 机制; AOSP 系统组件生产上直接用内置的 external/liburing, snapuserd 即是)。

- **iobench**: 文件 IO 对照 —— `pread` / 多线程 `pread` / `preadv2(RWF_NOWAIT)` / `mmap` / **libaio**(O_DIRECT 与 buffered)/ **POSIX AIO**(glibc)/ `io_uring`(含 SQPOLL、注册缓冲区)
- **netbench**: 网络 IO 对照 —— thread-per-connection / epoll(ET) / io_uring(multishot accept)

量化指标: IOPS/吞吐、延迟(avg/p50/p99)、每千次 IO 的 CPU 开销、系统调用总次数。

## 目录结构

```
├── Makefile
├── src/
│   ├── iobench.c        # 文件 IO 入口: 参数解析与引擎调度
│   ├── netbench.c       # 网络 IO 入口: fork 服务器子进程 + 客户端负载发生
│   ├── net_engines.[ch] # threads / epoll / uring 三种 echo 服务器引擎
│   ├── common.[ch]      # 计时/随机偏移/结果统计
│   ├── uring_min.[ch]   # 最小 io_uring 封装(setup/mmap/enter/register, 两个工具共用)
│   ├── engine_sync.c    # pread、pread_mt、write+fdatasync、攒批写
│   ├── engine_mmap.c    # mmap + memcpy
│   ├── engine_aio.c     # libaio(io_submit, direct/buffered)、POSIX AIO、RWF_NOWAIT
│   └── engine_uring.c   # 文件 IO 的 uring 引擎(读/日志落盘)
├── tools/
│   ├── run_bench.sh     # 文件 IO 对照矩阵(3 轮, 结果落 CSV)
│   ├── run_netbench.sh  # 网络 IO 对照矩阵
│   ├── gen_charts.py    # 文件 CSV → markdown 表格 + PNG
│   ├── gen_net_charts.py# 网络 CSV → markdown 表格 + PNG
│   └── uring_probe.c    # 能力探针: 检测 io_uring 可用性(降级判断用)
├── docs/
│   ├── io-uring-aosp-share.md     # 专题一: 原理 + 文件 IO + AOSP 落地
│   └── io-uring-network-share.md  # 专题二: 网络 IO 模型对照 + AOSP 落地
└── results/             # benchmark 产物(csv/表格/图)
```

## 构建

```sh
make            # iobench; 需要 Linux ≥ 5.1, gcc, 无第三方依赖
make netbench   # 网络压测
make uring_probe
```

## 文件 IO 单点测试

```sh
# 4K 随机读, io_uring 深度 64, 冷页缓存(需 root)
./iobench --pattern=randread --engine=uring --file=results/data.bin \
          --fsize=268435456 --bs=4096 --ops=20000 --depth=64 --drop-caches

# 日志落盘: io_uring 一批 32 条写 + 1 次 DRAIN fsync
./iobench --pattern=logwrite --engine=uring_logwrite --file=results/log.bin \
          --bs=4096 --ops=8192 --depth=32
```

引擎: `pread` `pread_mt`(线程数=depth) `preadv2_nowait` `mmap` `posix_aio` `aio_buffered`
`aio_direct`(O_DIRECT) `uring`(可选 `--sqpoll` `--fixed-buf`) `write_sync` `write_batch` `uring_logwrite`

## 网络 IO 单点测试

```sh
# 64 并发连接, 每连接 3000 次 64B ping-pong, 对比三种服务器模型
./netbench --engine=uring --conns=64 --msgs=3000 --size=64
./netbench --engine=epoll --conns=64 --msgs=3000 --size=64 --port=7801
```

引擎: `threads` `epoll` `uring`

## 完整对照实验

```sh
sudo ./tools/run_bench.sh        # 文件 IO 矩阵, 约 3~5 分钟
./tools/run_netbench.sh          # 网络 IO 矩阵, 约 2~3 分钟
.venv/bin/python tools/gen_charts.py
.venv/bin/python tools/gen_net_charts.py
```

## 场景与 AOSP 落地映射

| 场景 | 模式 | 对应 AOSP 场景 |
|---|---|---|
| A | 4KiB 随机读(冷缓存) | 应用冷启动资源加载、SQLite 随机页读、缩略图批量加载 |
| B | 4KiB 随机读(热缓存) | 高频元数据/页缓存命中路径, 纯 syscall 开销对照 |
| C | 256KiB 顺序读(冷缓存) | OTA 包读取、Virtual A/B 快照合并 COPY、大文件校验 |
| D | 4KiB 日志落盘(含 fsync) | logd 持久化、SQLite WAL commit、事件日志批量落盘 |
| N1/N2 | 小包 ping-pong(64B) | 高并发 IPC/代理: 投屏、文件传输服务、vendor 自定义协议栈 |
| N3 | 大包 echo(16KiB) | 大流量传输: adb push/pull、媒体投送 |

详细分析与实测数据:
[docs/io-uring-aosp-share.md](docs/io-uring-aosp-share.md)(文件篇)、
[docs/io-uring-network-share.md](docs/io-uring-network-share.md)(网络篇)。
