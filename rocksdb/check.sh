#!/bin/sh
# RocksDB's tests that use io_uring, on kqring (the Debug build).  Run
# after build.sh, with kqring.ko loaded.  Logs go to results/.
#
#   sh check.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
B=$HERE/work/build-debug
O=$HERE/results
T=$HERE/work/tmp
TESTS="env_test prefetch_test block_based_table_reader_test \
    db_tailing_iter_test db_basic_test"

kldstat -q -m kqring || { echo "load kqring.ko first" >&2; exit 1; }
for t in $TESTS; do
	[ -x "$B/$t" ] || { echo "$B/$t missing: run build.sh" >&2; exit 1; }
done
mkdir -p "$O"
rm -rf "$T"
mkdir -p "$T"
export TEST_TMPDIR=$T
: > "$O/check.txt"
rc=0

# The rings are kqring's: under ktrace(1), a test that reads through real
# io_uring makes kqring's system calls (lkmnosys to kdump): setups of 256
# entries (first argument 0x100), and enters and registrations on a ring.
ktrace -i -t c -f "$T/ktrace.out" "$B/env_test" \
    --gtest_filter='TestAsyncRead.InterleavingIOUringOperations' \
    > /dev/null 2>&1
n=$(kdump -f "$T/ktrace.out" | grep 'CALL  lkmnosys(' |
    grep -vc 'lkmnosys(0x100,')
echo "env_test InterleavingIOUringOperations: $n kqring calls on rings" |
    tee -a "$O/check.txt"
[ "$n" -gt 0 ] || rc=1
rm -f "$T/ktrace.out"

for t in $TESTS; do
	timeout 3600 "$B/$t" > "$O/$t.log" 2>&1
	r=$?
	echo "$t: exit $r, $(grep -E '^\[  (PASSED|FAILED)  \]' "$O/$t.log" |
	    tr '\n' ' ')" | tee -a "$O/check.txt"
	[ $r -eq 0 ] || rc=1
done

rm -rf "$T"
exit $rc
