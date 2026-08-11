#!/bin/bash
# 网络 IO 对照矩阵: threads / epoll / uring × 三种负载 × ROUNDS 轮
# 结果写入 results/results_net.csv
set -u
cd "$(dirname "$0")/.."

ROUNDS=${ROUNDS:-3}
CSV=results/results_net.csv

if [ ! -x ./netbench ]; then make netbench || exit 1; fi

echo "scenario,engine,conns,msg_bytes,msgs_total,rps,bw_mibs,rtt_avg_us,rtt_p50_us,rtt_p99_us,srv_cpu_us_per_kmsg,srv_syscalls" > "$CSV"

port=7950
run() { # scenario engine conns msgs size
    local scen=$1 e=$2 conns=$3 msgs=$4 size=$5
    port=$((port + 1))
    local out
    out=$(timeout 300 ./netbench --engine=$e --conns=$conns --msgs=$msgs --size=$size --port=$port 2>/dev/null | grep '^CSV,') || return
    echo "$scen,${out#CSV,}" >> "$CSV"
    echo "  [$scen] ${out#CSV,}" >&2
}

for round in $(seq 1 "$ROUNDS"); do
    echo "== Round $round / $ROUNDS ==" >&2
    for e in threads epoll uring; do
        # N1: 小包 ping-pong, 中等并发(64B × 64 连接 × 3000 次)
        run pingpong_64c_64B $e 64 3000 64
        # N2: 小包 ping-pong, 高并发(64B × 256 连接 × 1000 次)
        run pingpong_256c_64B $e 256 1000 64
        # N3: 大包 echo(16KiB × 64 连接 × 300 次)
        run bulk_64c_16K $e 64 300 16384
    done
done

echo "== 完成, 结果: $CSV ==" >&2
