#ifndef NET_ENGINES_H
#define NET_ENGINES_H

#include <stddef.h>

/*
 * echo 服务器引擎(在子进程中运行):
 * listen 后向 sync_fd 写 1 字节表示就绪, 收到 SIGINT 后停止,
 * 退出前向 stats_fd 写入本进程关键系统调用总次数。
 */
int net_engine_run(const char *engine, int port, size_t bufsize, int sync_fd, int stats_fd);

#endif
