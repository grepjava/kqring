#!/bin/sh
# Run PostgreSQL's regression tests with io_method=io_uring on kqring.
# kqring.ko must be loaded.  Uses a scratch cluster in work/data, listening
# only on a Unix socket in /tmp, port 5499.
#
#   sh check.sh [prefix]          default prefix: $PWD/work/inst
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
W=$HERE/work
PREFIX=${1:-$W/inst}
PORT=5499
kldstat -q -n kqring || { echo "kqring.ko is not loaded" >&2; exit 1; }
rm -rf "$W/data"
"$PREFIX/bin/initdb" -D "$W/data" -A trust -U postgres > "$W/initdb.log"
cat >> "$W/data/postgresql.conf" <<CONF
io_method = io_uring
listen_addresses = ''
unix_socket_directories = '/tmp'
port = $PORT
CONF
"$PREFIX/bin/pg_ctl" -D "$W/data" -l "$W/server.log" -w start
trap '"$PREFIX/bin/pg_ctl" -D "$W/data" -m fast -w stop' EXIT
[ "$("$PREFIX/bin/psql" -h /tmp -p $PORT -U postgres -Atc 'show io_method' postgres)" = io_uring ]
cd "$W/postgresql-"*/
PGHOST=/tmp PGPORT=$PORT PGUSER=postgres gmake installcheck-parallel
if grep -q PANIC "$W/server.log"; then
	echo "PANIC in $W/server.log" >&2; exit 1
fi
