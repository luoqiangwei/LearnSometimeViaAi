# 内存泄漏诊断报告

- 进程: `leak_demo` (pid=157783)
- 生成时间: 2026-08-12 00:15:15
- 触发时 RSS: 56.1MB（阈值 T1=30MB / T2=55MB）
- 从武装抓栈到触发: 2.0s
- 处置: SIGKILL

## RSS 轨迹（采样间隔 0.5s）

```
00:15:13      38.7 MB  #########
00:15:14      43.0 MB  ##########
00:15:14      47.5 MB  ###########
00:15:15      52.0 MB  ############
00:15:15      56.1 MB  ##############
```

## Top 10 泄漏堆栈

按用户态页错误采样数排序（采样率 1/1，每采样约对应 4KB 的 RSS 增长）。共采样 4454 次。

### #1  采样 4225 次（约 16.5MB，占 94.9%）

```
libc.so.6+0x1a8653
parse_request_body+0x27 [leak_demo]
handle_http_request+0x1c [leak_demo]
http_worker+0x15 [leak_demo]
libc.so.6+0xa407a
libc.so.6+0x13772c
```

### #2  采样 151 次（约 0.6MB，占 3.4%）

```
libc.so.6+0x1a8653
handle_ws_connection+0xd [leak_demo]
ws_worker+0x15 [leak_demo]
libc.so.6+0xa407a
libc.so.6+0x13772c
```

### #3  采样 67 次（约 0.3MB，占 1.5%）

```
libc.so.6+0xb1321
libc.so.6+0xb3317
libc.so.6+0xb3f6d
cache_store_entry+0x1e [leak_demo]
parse_request_body+0x27 [leak_demo]
handle_http_request+0x1c [leak_demo]
http_worker+0x15 [leak_demo]
libc.so.6+0xa407a
libc.so.6+0x13772c
```

### #4  采样 11 次（约 0.0MB，占 0.2%）

```
libc.so.6+0xb3580
libc.so.6+0xb3f6d
session_init+0x16 [leak_demo]
handle_ws_connection+0xd [leak_demo]
ws_worker+0x15 [leak_demo]
libc.so.6+0xa407a
libc.so.6+0x13772c
```

## 关联事件（最近 50 条）

```
00:15:13.676 [target] 开始跟踪 pid=157783 comm=leak_demo
00:15:13.766 [THRESH-1] pid=157783 comm=leak_demo RSS=38.7MB >= 30MB，开始抓取用户态堆栈
00:15:15.785 [THRESH-2] pid=157783 comm=leak_demo RSS=56.1MB >= 55MB，生成泄漏报告...
```
