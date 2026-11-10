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
		log "$f done"
	done
done
