#!/bin/sh
# The raw-disk stalls of up to 1 s in fio/bench.sh's first run of a rep: the
# SSD flushing the previous rep's ZFS writes, or kqring?  fio random 4 KiB
# reads, one in flight, 20 s, read-only on /dev/ada0: three times each for
# kqring and psync on a quiet disk (after zpool sync and 10 s), then twice
# each straight after 20 s of random ZFS writes at 32 in flight.  Waits for
# psync169.  Run as root.
K=/home/grepjava/kqring-p6
FIO=/home/grepjava/kqring/fio/work/fio-fio-3.43/fio
O=/home/grepjava/p6data/stall170
S=$O/steps
mkdir -p "$O" && : > "$S"
log() { echo "$(date +%H:%M:%S) $*" >> "$S"; }
while ! grep -q "ALLDONE\|FAILED" /home/grepjava/p6data/steps169 2>/dev/null; do sleep 20; done
zfs list zroot/fiostall >/dev/null 2>&1 && { log "FAILED precondition: dataset exists"; exit 1; }
kldstat -q -m kqring && { log "FAILED precondition: loaded"; exit 1; }
cleanup() { zfs destroy -r zroot/fiostall 2>/dev/null; kldunload kqring 2>/dev/null; }
trap cleanup EXIT
kldload $K/kmod/kqring.ko || { log "FAILED load"; exit 1; }
ulimit -l 65536
rd() {	# label engine
	"$FIO" --name=s --ioengine=$2 --iodepth=1 --direct=1 --readonly --filename=/dev/ada0 \
	    --size=64g --rw=randread --bs=4k --time_based --runtime=20 --ramp_time=1 \
	    --norandommap --randrepeat=0 --output-format=json --output=$O/$1.json || { log "FAILED fio $1"; exit 1; }
	log "$1 $(python3 -c "import json; r=json.load(open('$O/$1.json'))['jobs'][0]['read']; c=r['clat_ns']; print(round(r['iops']), 'iops p99.99', round(c['percentile']['99.990000']/1000), 'us max', round(c['max']/1000), 'us')")"
}
zpool sync; sleep 10
for n in 1 2 3; do rd quiet.kqring.$n io_uring; rd quiet.psync.$n psync; done
zfs create -o mountpoint=/mnt/fiostall -o primarycache=metadata -o compression=off -o recordsize=4k -o atime=off zroot/fiostall || { log "FAILED create"; exit 1; }
"$FIO" --name=layout --ioengine=psync --filename=/mnt/fiostall/f --size=2g --rw=write --bs=1m --output=/dev/null || exit 1
zpool sync
for n in 1 2; do
	for e in io_uring psync; do
		"$FIO" --name=w --ioengine=io_uring --iodepth=32 --direct=1 --filename=/mnt/fiostall/f --size=2g \
		    --rw=randwrite --bs=4k --time_based --runtime=20 --output=/dev/null || { log "FAILED writes"; exit 1; }
		rd afterwrites.$e.$n $e
	done
done
kldunload kqring || log "FAILED unload"
log ALLDONE
