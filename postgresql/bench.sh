#!/bin/sh
# pgbench on kqring: io_method=io_uring against PostgreSQL's own worker and
# sync methods, on one cluster, alternated.  kqring.ko must be loaded.
#
#   sh bench.sh [prefix] [scale] [seconds] [rounds]
#
# Each round restarts the server per method (empty shared buffers) and runs
# a cold sequential scan of pgbench_accounts, then pgbench's select-only and
# TPC-B-like scripts with 8 clients.  Output: one line per measurement,
#   method round test value unit
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=$HERE/work
PREFIX=${1:-$W/inst}
SCALE=${2:-40}
SECS=${3:-30}
ROUNDS=${4:-3}
PORT=5499
B=$PREFIX/bin
kldstat -q -n kqring || { echo "kqring.ko is not loaded" >&2; exit 1; }
D=$W/benchdata
psql() { "$B/psql" -h /tmp -p $PORT -U postgres -X -Atq "$@" postgres; }
start() {
	sed -i '' "s/^io_method = .*/io_method = $1/" "$D/postgresql.conf"
	"$B/pg_ctl" -D "$D" -l "$W/bench-server.log" -w start >/dev/null
	[ "$(psql -c 'show io_method')" = "$1" ]
}
stop() { "$B/pg_ctl" -D "$D" -m fast -w stop >/dev/null; }
trap 'stop 2>/dev/null || true' EXIT

if [ ! -d "$D" ]; then
	"$B/initdb" -D "$D" -A trust -U postgres > "$W/bench-initdb.log"
	cat >> "$D/postgresql.conf" <<CONF
io_method = io_uring
listen_addresses = ''
unix_socket_directories = '/tmp'
port = $PORT
max_connections = 50
CONF
	start io_uring
	"$B/pgbench" -h /tmp -p $PORT -U postgres -i -q -s "$SCALE" postgres \
	    > "$W/bench-init.log" 2>&1
	stop
fi

for r in $(seq "$ROUNDS"); do
	for m in io_uring worker sync; do
		start $m
		ms=$(psql -c '\timing on' -c 'select count(*) from pgbench_accounts' |
		    sed -n 's/^Time: \([0-9.]*\) ms.*/\1/p')
		echo "$m $r seqscan_cold $ms ms"
		tps=$("$B/pgbench" -h /tmp -p $PORT -U postgres -S -c 8 -j 8 \
		    -T "$SECS" postgres 2>&1 | sed -n 's/^tps = \([0-9.]*\) .*/\1/p')
		echo "$m $r select_only $tps tps"
		tps=$("$B/pgbench" -h /tmp -p $PORT -U postgres -c 8 -j 8 \
		    -T "$SECS" postgres 2>&1 | sed -n 's/^tps = \([0-9.]*\) .*/\1/p')
		echo "$m $r tpcb $tps tps"
		stop
		if grep -q PANIC "$W/bench-server.log"; then
			echo "PANIC in $W/bench-server.log" >&2; exit 1
		fi
	done
done
