#!/bin/sh
# Build PostgreSQL 18.6 with io_method=io_uring, against liburing on kqring.
# Run from this directory, after `make` in ../liburing.  Needs gmake, bison,
# flex, perl5 and pkgconf (pkg install gmake bison flex perl5 pkgconf).
#
#   sh build.sh [prefix]          default prefix: $PWD/work/inst
set -eu
VER=18.6
SHA=555610c24d53e4316da5b7d3fc25c279d96856d5e0e23ee308c328c5fa881d9f
HERE=$(cd "$(dirname "$0")" && pwd)
LIBURING=$HERE/../liburing
W=$HERE/work
PREFIX=${1:-$W/inst}
INC=$LIBURING/work/liburing-liburing-2.15/src/include

[ -f "$LIBURING/work/out/liburing.a" ] || {
	echo "build liburing first: make -C $LIBURING" >&2; exit 1; }
mkdir -p "$W/pc"
cd "$W"
if [ ! -f postgresql-$VER.tar.bz2 ]; then
	fetch https://ftp.postgresql.org/pub/source/v$VER/postgresql-$VER.tar.bz2
fi
[ "$(sha256 -q postgresql-$VER.tar.bz2)" = $SHA ] || {
	echo "postgresql-$VER.tar.bz2: SHA-256 mismatch" >&2; exit 1; }

# PostgreSQL finds liburing with pkg-config.
cat > pc/liburing.pc <<PC
Name: liburing
Description: liburing 2.15 on kqring
Version: 2.15
Cflags: -I$INC -I$LIBURING/freebsd/include -include $LIBURING/freebsd/include/config-host.h
Libs: $LIBURING/work/out/liburing.a -lpthread
PC

rm -rf postgresql-$VER
tar xjf postgresql-$VER.tar.bz2
cd postgresql-$VER
PKG_CONFIG_PATH=$W/pc ./configure --prefix="$PREFIX" --with-liburing \
    --without-icu --enable-debug --enable-cassert \
    CPPFLAGS=-I/usr/local/include LDFLAGS=-L/usr/local/lib
grep -q '^#define USE_LIBURING 1' src/include/pg_config.h || {
	echo "configure did not enable liburing" >&2; exit 1; }
gmake -j"$(sysctl -n hw.ncpu)"
gmake install
echo "installed in $PREFIX"
