#!/bin/bash
# 一键跑完整对照矩阵, 结果汇总到 results/results.csv
# 需要 root(drop_caches); 每组配置跑 ROUNDS 轮, 报表时取最优轮次
set -u
cd "$(dirname "$0")/.."

ROUNDS=${ROUNDS:-3}
FSIZE=$((256 * 1024 * 1024))     # 256MiB 测试文件
DATA=results/data.bin
LOG=results/log.bin
CSV=results/results.csv

if [ ! -x ./iobench ]; then make || exit 1; fi

echo "== 准备 256MiB 测试文件 ==" >&2
if [ ! -f "$DATA" ] || [ "$(stat -c%s "$DATA")" != "$FSIZE" ]; then
    ./scratch/mkfile.sh "$DATA" "$FSIZE"
fi
sync

echo "scenario,name,ops,bs,depth,iops,bw_mibs,lat_avg_us,lat_p50_us,lat_p99_us,cpu_us_per_kop,syscalls,pagefaults" > "$CSV"

run() { # scenario extra_args... ; 收集 CSV 行并附加场景列
    local scenario=$1; shift
    local out
    out=$(./iobench "$@" 2>/dev/null | grep '^CSV,') || return
    echo "$scenario,${out#CSV,}" >> "$CSV"
    echo "  [$scenario] ${out#CSV,}" >&2
}

for round in $(seq 1 "$ROUNDS"); do
    echo "== Round $round / $ROUNDS ==" >&2

    # --- 场景 A: 4KiB 随机读, 冷页缓存 ---
    for cfg in "pread 1 " "pread_mt 4 " "pread_mt 16 " "preadv2_nowait 1 " "mmap 1 " \
               "posix_aio 64 " "aio_buffered 64 " "aio_direct 64 " \
               "uring 1 " "uring 32 " "uring 64 " "uring 64 --fixed-buf" "uring 64 --sqpoll"; do
        set -- $cfg; e=$1; d=$2; shift 2
        run randread_cold --pattern=randread --engine=$e --file=$DATA --fsize=$FSIZE \
            --bs=4096 --ops=20000 --depth=$d --drop-caches "$@"
    done

    # --- 场景 B: 4KiB 随机读, 热页缓存 ---
    cat "$DATA" > /dev/null   # 预热
    for cfg in "pread 1 " "pread_mt 4 " "pread_mt 16 " "preadv2_nowait 1 " "mmap 1 " \
               "posix_aio 64 " "aio_buffered 64 " \
               "uring 1 " "uring 32 " "uring 64 " "uring 64 --fixed-buf" "uring 64 --sqpoll"; do
        set -- $cfg; e=$1; d=$2; shift 2
        run randread_warm --pattern=randread --engine=$e --file=$DATA --fsize=$FSIZE \
            --bs=4096 --ops=100000 --depth=$d "$@"
    done

    # --- 场景 C: 256KiB 顺序读, 冷页缓存(模拟 OTA 包读取/快照合并) ---
    for cfg in "pread 1 " "pread_mt 4 " "mmap 1 " "aio_direct 8 " "uring 8 " "uring 32 "; do
        set -- $cfg; e=$1; d=$2; shift 2
        run seqread_cold --pattern=seqread --engine=$e --file=$DATA --fsize=$FSIZE \
            --bs=262144 --ops=1024 --depth=$d --drop-caches "$@"
    done

    # --- 场景 D: 4KiB 日志记录落盘 ---
    run logwrite --pattern=logwrite --engine=write_sync     --file=$LOG --bs=4096 --ops=8192 --depth=1
    run logwrite --pattern=logwrite --engine=write_batch    --file=$LOG --bs=4096 --ops=8192 --depth=32
    run logwrite --pattern=logwrite --engine=uring_logwrite --file=$LOG --bs=4096 --ops=8192 --depth=32
done

echo "== 完成, 结果: $CSV ==" >&2
