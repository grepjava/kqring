#!/bin/sh
# The file group on ZFS with the dbuf cache at 1/32 (cache_shift 5, the
# default) and 1/8 (3) of the ARC target, alternated, 5 runs each.  Run as
# root from the repository root, after make: sh docs/bench/data/zfs-dbuf/run.sh USER
set -eu
U=${1:?usage: run.sh user-to-run-the-benchmark-as}
OUT=${OUT:-zfs-dbuf.out}
log() { echo "$(date +%T) $*" | tee -a "$OUT/progress.log"; }
fail() { log "FAIL: $*"; exit 1; }
mkdir -p "$OUT" && chown "$U" "$OUT"
[ "$(df -T /tmp | awk 'NR==2{print $2}')" = zfs ] || fail "/tmp is not zfs"
[ "$(sysctl -n vfs.zfs.dbuf.cache_shift)" = 5 ] || fail "cache_shift is not 5"
kldstat -q -n kqring && fail "kqring is already loaded"
trap 'sysctl vfs.zfs.dbuf.cache_shift=5 >/dev/null; kldunload kqring 2>/dev/null || true' EXIT
kldload ./kmod/kqring.ko
for i in 1 2 3 4 5; do
	for sh in 5 3; do
		sysctl vfs.zfs.dbuf.cache_shift=$sh >/dev/null
		f="$OUT/file-s$sh.$i.out"
		sh -c "ulimit -l 8192; exec sudo -u $U ./tests/kqring_bench file" > "$f"
		[ "$(grep -c '^ROW file_4k' "$f")" = 4 ] || fail "rows missing in $f"
		# At 1/8 the file must fit the dbuf cache, and at 1/32 not, or the
		# two columns measure the same thing. The cache follows the ARC
		# target, which grows only as the ARC fills: run after other I/O.
		t=$(sed -n 's/^# file_4k start:.*dbuf cache target \([0-9]*\) MiB.*file \([0-9]*\) MiB.*/\1 \2/p' "$f")
		set -- $t
		[ $# = 2 ] || fail "no dbuf cache target in $f"
		if [ $sh = 3 ]; then [ "$1" -gt "$2" ] || fail "$f: file ($2 MiB) doesn't fit the dbuf cache ($1 MiB)"
		else [ "$1" -lt "$2" ] || fail "$f: file ($2 MiB) fits the dbuf cache ($1 MiB)"; fi
		log "$f done"
	done
done
