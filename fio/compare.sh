#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# kqring on FreeBSD against io_uring on Linux, on one machine: run this on
# each OS, with the same fio build, and compare the two output directories.
# Run as root; on FreeBSD with kqring.ko loaded.
#
#	compare.sh <fio> <xbench> <raw partition> <out dir> [reps] [secs]
#
# <xbench> is tests/kqring_xbench built on this OS.  The raw partition is
# only read (fio --readonly); give the same one on both OSes, and one
# neither uses (a spare or the other OS's swap).
#
# What it runs, 4 KiB random reads unless said otherwise:
#  - kqring_xbench: NOPs, pipe and AF_UNIX reads, each against the plain
#    system call, for each OS's own baseline;
#  - a RAM disk (md(4) malloc, Linux brd), 256 MiB: the ring and the block
#    layer with no device in the way;
#  - the raw partition: the same SSD through each OS's block layer;
#  - a 256 MiB file on tmpfs: cached file reads on the same kind of
#    filesystem on both.
# Engines: io_uring at 1 and 32 in flight, posixaio at 32, psync (one
# thread per job), at 1 and 4 jobs (the SSD at 1 job).  Each fio result is
# kept as JSON, with the machine's details in system.txt.

set -eu
FIO=$1 XB=$2 RAW=$3 OUT=$4 REPS=${5:-3} SECS=${6:-10}
OS=$(uname -s)
mkdir -p "$OUT"
[ "$(id -u)" = 0 ] || { echo "run as root" >&2; exit 1; }
[ -x "$FIO" ] && [ -x "$XB" ] || { echo "no fio or xbench" >&2; exit 1; }
[ -c "$RAW" ] || [ -b "$RAW" ] || { echo "$RAW: not a device" >&2; exit 1; }
ulimit -l 65536 || true

# What the results depend on.
{
	echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
	uname -a
	"$FIO" --version
	case $OS in
	FreeBSD)
		sysctl -n hw.model hw.ncpu hw.physmem
		kldstat -v -n kqring | head -n 3
		sysctl vm.pmap.pti hw.ibrs_disable hw.mds_disable \
		    hw.spec_store_bypass_disable 2>/dev/null || true
		swapctl -l
		;;
	Linux)
		grep -m1 'model name' /proc/cpuinfo
		nproc
		grep MemTotal /proc/meminfo
		cat /proc/cmdline
		grep -r . /sys/devices/system/cpu/vulnerabilities/ || true
		cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor \
		    2>/dev/null || true
		cat /proc/swaps
		;;
	esac
} > "$OUT/system.txt" 2>&1

"$XB" 5 > "$OUT/xbench.txt"

TMPD=$(mktemp -d /tmp/kqcmp.XXXXXX)
RAM=
cleanup() {
	case $OS in
	FreeBSD)
		[ -n "$RAM" ] && mdconfig -d -u "${RAM#/dev/md}" 2>/dev/null
		umount "$TMPD" 2>/dev/null
		;;
	Linux)
		umount "$TMPD" 2>/dev/null
		[ -n "$RAM" ] && rmmod brd 2>/dev/null
		;;
	esac
	rmdir "$TMPD" 2>/dev/null || true
}
trap cleanup EXIT
case $OS in
FreeBSD)
	RAM=/dev/$(mdconfig -a -t malloc -s 256m)
	mount -t tmpfs -o size=300m tmpfs "$TMPD"
	;;
Linux)
	lsmod | grep -q '^brd ' && { echo "brd already loaded" >&2; exit 1; }
	modprobe brd rd_nr=1 rd_size=262144 max_part=0
	RAM=/dev/ram0
	mount -t tmpfs -o size=300m tmpfs "$TMPD"
	;;
esac
dd if=/dev/urandom of="$RAM" bs=1m count=256 2>/dev/null ||
    dd if=/dev/urandom of="$RAM" bs=1M count=256 2>/dev/null
"$FIO" --name=layout --ioengine=psync --filename="$TMPD/fio.dat" \
    --size=256m --rw=write --bs=1m --output=/dev/null

COMMON="--thread --group_reporting --time_based --runtime=$SECS \
    --ramp_time=1 --norandommap --randrepeat=0 --rw=randread --bs=4k \
    --output-format=json"

one() {	# target engine qd jobs rep
	case $1 in
	ram)	f=$RAM; sz=256m; d=1; ro="" ;;
	ssd)	f=$RAW; sz=8g; d=1; ro="--readonly" ;;
	tmpfs)	f=$TMPD/fio.dat; sz=256m; d=0; ro="" ;;
	esac
	"$FIO" --name=x --ioengine=$2 --iodepth=$3 --numjobs=$4 \
	    --filename="$f" --size=$sz --direct=$d $ro $COMMON \
	    --output="$OUT/$1.$2.qd$3.j$4.r$5.json"
}

rep=1
while [ "$rep" -le "$REPS" ]; do
	for t in ram tmpfs; do
		for j in 1 4; do
			one $t io_uring 1 $j $rep
			one $t io_uring 32 $j $rep
			one $t posixaio 32 $j $rep
			one $t psync 1 $j $rep
		done
	done
	one ssd io_uring 1 1 $rep
	one ssd io_uring 32 1 $rep
	one ssd posixaio 32 1 $rep
	one ssd psync 1 1 $rep
	echo "rep $rep done $(date +%H:%M:%S)"
	rep=$((rep + 1))
done
