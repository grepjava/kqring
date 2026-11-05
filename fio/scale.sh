#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Multi-threaded scaling with fio: one ring per thread, on targets fast
# enough that the disk isn't what limits them.  Run as root, with kqring.ko
# loaded:
#
#	scale.sh <fio> <zfs dataset> <out dir> [reps] [secs]
#
# Two paths, 4 KiB random reads:
#  - zfscached: a 512 MiB file held in the ARC, shared by all jobs, through
#    the worker pool (16 in flight per job);
#  - mdread: one md(4) malloc disk per job, since md serves each disk from
#    one thread, through the direct bio path (32 in flight per job).
# The dataset and disks are created here and removed at the end.

set -eu
FIO=$1 DS=$2 OUT=$3 REPS=${4:-3} SECS=${5:-10}
MNT=/mnt/fioscale
UNIT0=20

mkdir -p "$OUT"
ulimit -l 65536

cleanup() {
	zfs destroy -r "$DS" 2>/dev/null || true
	for i in 1 2 3 4; do
		mdconfig -d -u $((UNIT0 + i)) 2>/dev/null || true
	done
}
trap cleanup EXIT

COMMON="--thread --group_reporting --direct=1 --time_based \
    --runtime=$SECS --ramp_time=1 --norandommap --randrepeat=0 \
    --output-format=json --rw=randread --bs=4k"

zfs create -o mountpoint=$MNT -o primarycache=all -o compression=off \
    -o recordsize=4k -o atime=off "$DS"
[ "$(zfs get -H -o value mounted "$DS")" = yes ]
FILE=$MNT/fio.dat
"$FIO" --name=layout --ioengine=psync --filename="$FILE" --size=512m \
    --rw=write --bs=1m --output=/dev/null
zpool sync
[ "$(zfs get -Hp -o value used "$DS")" -ge 500000000 ]
# Twice: the second pass makes the blocks frequently used.
dd if="$FILE" of=/dev/null bs=1m 2>/dev/null
dd if="$FILE" of=/dev/null bs=1m 2>/dev/null

for i in 1 2 3 4; do
	u=$((UNIT0 + i))
	mdconfig -a -t malloc -s 256m -u $u
	dd if=/dev/urandom of=/dev/md$u bs=1m count=256 2>/dev/null
	[ -c /dev/md$u ]
done

rep=1
while [ "$rep" -le "$REPS" ]; do
	for j in 1 2 4 8; do
		for eng in io_uring posixaio psync; do
			case $eng in
			psync)	qd=1 ;;
			*)	qd=16 ;;
			esac
			"$FIO" --name=z --ioengine=$eng --iodepth=$qd \
			    --numjobs=$j --filename="$FILE" --size=512m \
			    $COMMON \
			    --output="$OUT/zfscached.$eng.j$j.r$rep.json"
		done
	done
	for j in 1 2 4; do
		for eng in io_uring posixaio; do
			jobs=""
			i=1
			while [ "$i" -le "$j" ]; do
				jobs="$jobs --name=m$i --filename=/dev/md$((UNIT0 + i))"
				i=$((i + 1))
			done
			"$FIO" --ioengine=$eng --iodepth=32 --size=256m $COMMON \
			    --output="$OUT/mdread.$eng.j$j.r$rep.json" $jobs
		done
	done
	rep=$((rep + 1))
done
