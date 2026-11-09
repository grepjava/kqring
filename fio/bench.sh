#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# The storage benchmarks in BENCHMARK.MD: fio on a real disk, the io_uring
# engine (kqring) against posixaio (aio(4)) and psync.  Run as root, with
# kqring.ko loaded:
#
#	bench.sh <fio> <read-only device> <zfs dataset> <out dir> [reps] [secs]
#
# The device is only read (fio's --readonly).  All writes go to a file in
# the dataset, which this script creates and destroys, with data caching
# off so that reads reach the disk.  psync runs depth threads, one I/O each.
# Before each read run it waits until the disk has had no writes for 5
# seconds, so the reads don't meet the flush of the previous writes.

set -eu
FIO=$1 DEV=$2 DS=$3 OUT=$4 REPS=${5:-3} SECS=${6:-20}
SIZE=8g

mkdir -p "$OUT"
ulimit -l 65536

# The disk under DEV, for iostat: ada0 for /dev/ada0 or /dev/ada0p4.
DISK=$(basename "$DEV" | sed 's/p[0-9]*$//')

# Wait for the disk to go quiet: ZFS flushes random writes for a while after
# they return, and the SSD then stalls reads for up to a second at a time,
# whichever engine issues them.  Sync, then wait until iostat shows no writes
# for 5 seconds in a row (at most 2 minutes).
settle() {
	zpool sync
	quiet=0 t=0
	while [ "$quiet" -lt 5 ] && [ "$t" -lt 120 ]; do
		w=$(iostat -x -d -w 1 -c 2 "$DISK" |
		    awk -v d="$DISK" '$1 == d { w = $3 } END { printf "%d", w }')
		if [ "$w" -eq 0 ]; then quiet=$((quiet + 1)); else quiet=0; fi
		t=$((t + 1))
	done
}

run() {	# name engine depth extra...
	name=$1 eng=$2 qd=$3
	shift 3
	# Reads measure the disk, not the last writes' flush.
	case $name in *read*) settle ;; esac
	case $eng in
	psync)	par="--numjobs=$qd --iodepth=1 --thread --group_reporting" ;;
	*)	par="--iodepth=$qd" ;;
	esac
	"$FIO" --name="$name" --ioengine="$eng" $par --direct=1 \
	    --time_based --runtime="$SECS" --ramp_time=2 --norandommap \
	    --randrepeat=0 --output-format=json "$@" \
	    --output="$OUT/$name.$eng.q$qd.r$rep.json"
}

MNT=/mnt/fiobench
zfs create -o mountpoint=$MNT -o primarycache=metadata -o compression=off \
    -o recordsize=4k -o atime=off "$DS"
trap 'zfs destroy -r "$DS"' EXIT
[ "$(zfs get -H -o value mounted "$DS")" = yes ]
FILE=$MNT/fio.dat
"$FIO" --name=layout --ioengine=psync --filename="$FILE" --size=$SIZE \
    --rw=write --bs=1m --output=/dev/null
# The numbers mean nothing unless the data is really in this dataset.
zpool sync
[ "$(zfs get -Hp -o value used "$DS")" -ge 8000000000 ]

rep=1
while [ "$rep" -le "$REPS" ]; do
	for qd in 1 8 32; do
		for eng in io_uring posixaio psync; do
			run rawread4k $eng $qd --readonly --filename="$DEV" \
			    --size=64g --rw=randread --bs=4k
		done
	done
	for eng in io_uring posixaio; do
		run rawseq128k $eng 32 --readonly --filename="$DEV" \
		    --size=64g --rw=read --bs=128k
	done
	run rawread4k_fixed io_uring 32 --readonly --filename="$DEV" \
	    --size=64g --rw=randread --bs=4k --fixedbufs --registerfiles
	run rawread4k_sqpoll io_uring 32 --readonly --filename="$DEV" \
	    --size=64g --rw=randread --bs=4k --sqthread_poll
	for qd in 1 32; do
		for eng in io_uring posixaio psync; do
			run zfsread4k $eng $qd --filename="$FILE" --size=$SIZE \
			    --rw=randread --bs=4k
			run zfswrite4k $eng $qd --filename="$FILE" \
			    --size=$SIZE --rw=randwrite --bs=4k
		done
	done
	rep=$((rep + 1))
done
