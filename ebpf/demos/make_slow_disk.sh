#!/bin/bash
# make_slow_disk.sh — 用 dm-delay 造一块“慢盘”，让读线程稳定进入 D 态
#
# 原理：device-mapper 的 delay target 把每次 I/O 延迟指定毫秒，
#       读该盘的线程会阻塞在不可中断睡眠（D 态）约 500ms。
# 用法：
#   sudo ./make_slow_disk.sh start     # 创建慢盘并启动一个持续读的进程
#   sudo ./make_slow_disk.sh stop      # 停止读进程并清理
set -euo pipefail

IMG="$(cd "$(dirname "$0")" && pwd)/.slowdisk.img"
LOOPDEV_FILE="$(cd "$(dirname "$0")" && pwd)/.slowdisk.loopdev"
PIDFILE="$(cd "$(dirname "$0")" && pwd)/.slowdisk.reader.pid"
DMNAME="ebpf_slowdisk"
DELAY_MS=500

start() {
    modprobe dm-delay 2>/dev/null || true

    echo "[1/4] 创建 64MB 镜像文件"
    dd if=/dev/zero of="$IMG" bs=1M count=64 status=none

    echo "[2/4] 绑定 loop 设备"
    LOOPDEV=$(losetup -f --show "$IMG")
    echo "$LOOPDEV" > "$LOOPDEV_FILE"
    SECTORS=$(blockdev --getsz "$LOOPDEV")

    echo "[3/4] 创建 dm-delay 设备 /dev/mapper/$DMNAME（每次 I/O 延迟 ${DELAY_MS}ms）"
    echo "0 $SECTORS delay $LOOPDEV 0 $DELAY_MS" | dmsetup create "$DMNAME"

    echo "[4/4] 启动持续读进程（dd iflag=direct，每次读都会 D 态 ${DELAY_MS}ms）"
    ( while true; do
          dd if="/dev/mapper/$DMNAME" of=/dev/null bs=4k count=16 iflag=direct status=none 2>/dev/null || sleep 1
      done ) &
    READER=$!
    echo "$READER" > "$PIDFILE"
    echo "慢盘就绪，读进程 pid=$READER，可用 tracker 观察其 D 态"
}

stop() {
    echo "清理慢盘..."
    [ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE")" 2>/dev/null || true
    # dd 是 reader 的子进程，一并清掉
    pkill -f "dd if=/dev/mapper/$DMNAME" 2>/dev/null || true
    sleep 0.5
    dmsetup remove "$DMNAME" 2>/dev/null || true
    [ -f "$LOOPDEV_FILE" ] && losetup -d "$(cat "$LOOPDEV_FILE")" 2>/dev/null || true
    rm -f "$IMG" "$LOOPDEV_FILE" "$PIDFILE"
    echo "清理完成"
}

case "${1:-}" in
    start) start ;;
    stop)  stop  ;;
    *) echo "用法: $0 start|stop"; exit 1 ;;
esac
