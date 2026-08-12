#!/bin/bash
# flame_comm.sh — 采集 eBPF↔用户态通信路径的火焰图
#
# 对 perfbuf / ringbuf 各采两份：
#   producer：perf record 包住 bench_syscall（getpid 压力），
#             捕获 syscall 上下文里的内核态下发路径（bpf_perf_event_output /
#             bpf_ringbuf_output 等帧）
#   consumer：perf record -p 挂在事件消费者 agent 上，看用户态读取路径
# 产物: docs/assets/comm_{perfbuf,ringbuf}_{producer,consumer}.svg
#       + /tmp/comm_*.collapsed（帧计数文本）
# 依赖: perf、/tmp/flamegraph/{stackcollapse-perf.pl,flamegraph.pl}
# 用法: sudo bench/flame_comm.sh [秒数(默认 12)]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FG=/tmp/flamegraph
DUR=${1:-12}
ASSETS="$ROOT/docs/assets"
mkdir -p "$ASSETS"

[ -x "$FG/flamegraph.pl" ] || { echo "缺 /tmp/flamegraph/flamegraph.pl"; exit 1; }

agent_pid=""

stop_agent() {
    [ -n "$agent_pid" ] && kill "$agent_pid" 2>/dev/null || true
    wait "$agent_pid" 2>/dev/null || true
    agent_pid=""
}

capture() {
    local mech=$1 name=$2
    echo "===== $name 机制 ====="

    python3 -u "$ROOT/bench/flame_agent.py" "$mech" 128 > /tmp/comm_agent.out 2>&1 &
    agent_pid=$!
    for i in $(seq 1 30); do grep -q READY /tmp/comm_agent.out && break; sleep 1; done
    grep READY /tmp/comm_agent.out

    echo "  [producer] perf record 包住 bench_syscall（${DUR}s 负载）..."
    taskset -c 0 perf record -F 999 -g -o "/tmp/comm_${name}_prod.data" -- \
        "$ROOT/bench/bench_syscall" $((DUR * 2000000)) > /dev/null

    echo "  [consumer] perf record -p agent（${DUR}s）..."
    perf record -F 99 -g -p "$agent_pid" -o "/tmp/comm_${name}_cons.data" -- sleep "$DUR" &
    local rec=$!
    # 同时给足负载让消费者忙起来
    taskset -c 0 "$ROOT/bench/bench_syscall" $((DUR * 2000000)) > /dev/null
    wait "$rec" 2>/dev/null || true

    stop_agent

    for side in prod cons; do
        perf script -i "/tmp/comm_${name}_${side}.data" 2>/dev/null \
            | "$FG/stackcollapse-perf.pl" > "/tmp/comm_${name}_${side}.collapsed"
        local title="eBPF 事件下发路径 ($name, $side)"
        [ "$side" = cons ] && title="用户态事件消费路径 ($name)"
        "$FG/flamegraph.pl" --title "$title" --width 1200 \
            < "/tmp/comm_${name}_${side}.collapsed" \
            > "$ASSETS/comm_${name}_${side}.svg"
        echo "  -> $ASSETS/comm_${name}_${side}.svg ($(wc -l < /tmp/comm_${name}_${side}.collapsed) 条栈)"
    done
}

capture 1 perfbuf
capture 2 ringbuf
echo "完成"
