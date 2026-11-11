#!/bin/sh
# db_bench: batched MultiGet with RocksDB's io_uring reads on kqring
# against the same reads with io_uring off (one pread(2) each).  Run as
# root, after build.sh, with kqring.ko loaded.
#
# The database goes in a ZFS dataset made for the run, as fio/bench.sh
# makes its own: 4 KiB records, RocksDB's block size (a 4 KiB block read
# from 128 KiB records reads 128 KiB, and the disk's bandwidth is the
# limit), and only metadata cached, so every block read goes to the disk.
#
#   sh bench.sh [pool]     default pool: zroot; output in results/bench.out
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
B=$HERE/work/build
POOL=${1:-zroot}
DS=$POOL/kqring-rocksbench
MNT=/mnt/rocksbench
DB=$MNT/db
OUT=$HERE/results/bench.out
NUM=4000000		# 1000-byte values: about 4 GB
ROUNDS=3

[ "$(id -u)" -eq 0 ] || { echo "run as root (zfs create)" >&2; exit 1; }
kldstat -q -m kqring || { echo "load kqring.ko first" >&2; exit 1; }
[ -x "$B/db_bench" ] || { echo "run build.sh first" >&2; exit 1; }
mkdir -p "$HERE/results"
zfs create -o mountpoint=$MNT -o recordsize=4k -o primarycache=metadata \
    -o compression=off -o atime=off "$DS"
trap 'zfs destroy -r "$DS"' EXIT
[ "$(zfs get -H -o value mounted "$DS")" = yes ]

COMMON="--db=$DB --num=$NUM --value_size=1000 --compression_type=none
    --compression_ratio=1 --block_size=4096 --cache_size=8388608
    --statistics=0 --histogram=1"
{
	echo "# $(date '+%F %T') $(uname -r) $(sysctl -n hw.model)"
	echo "# $DS: recordsize 4K, primarycache metadata, compression off"
} > "$OUT"
"$B/db_bench" $COMMON --benchmarks=fillrandom,compact --threads=1 \
    >> "$OUT" 2>&1
zpool sync "$POOL"
echo "# database $(du -sh "$DB" | cut -f1)" >> "$OUT"

# MultiGet of 32 keys at a time: io_uring submits a batch's block reads
# together; without it RocksDB reads them one after another.
for r in $(seq 1 $ROUNDS); do
	for t in 1 4; do
		for u in true false; do
			echo "## round $r threads $t io_uring $u" >> "$OUT"
			"$B/db_bench" $COMMON --use_existing_db=1 \
			    --benchmarks=multireadrandom --multiread_batched=1 \
			    --batch_size=32 --threads=$t --duration=60 \
			    --io_uring_enabled=$u >> "$OUT" 2>&1
		done
	done
done
grep -E '^## |^multireadrandom' "$OUT"
# Every block read is checked against its checksum.
n=$(grep -ciE 'corruption|io error' "$OUT" || true)
echo "corruption or I/O errors: $n" | tee -a "$OUT"
[ "$n" -eq 0 ]
