#!/bin/sh
# Build RocksDB 11.13.2 with its io_uring reads, against liburing on kqring.
# Run from this directory, after `make` in ../liburing.  Needs cmake,
# gflags, snappy, liblz4 and zstd (pkg install cmake gflags snappy liblz4
# zstd).
#
#   sh build.sh      db_bench (Release) in work/build, and
#                    the tests that use io_uring (Debug) in work/build-debug
set -eu
VER=11.13.2
SHA=6f12f408c4876175b1de73cfefcdd1baf0001ad8d799c1a09320d96e4fd213b5
HERE=$(cd "$(dirname "$0")" && pwd)
LIBURING=$(cd "$HERE/../liburing" && pwd)
W=$HERE/work
INC=$LIBURING/work/liburing-liburing-2.15/src/include
OVL=$LIBURING/freebsd/include
TESTS="env_test prefetch_test block_based_table_reader_test \
    db_tailing_iter_test db_basic_test"

[ -f "$LIBURING/work/out/liburing.a" ] || {
	echo "build liburing first: make -C $LIBURING" >&2; exit 1; }
mkdir -p "$W"
cd "$W"
if [ ! -f rocksdb-$VER.tar.gz ]; then
	fetch -o rocksdb-$VER.tar.gz \
	    https://github.com/facebook/rocksdb/archive/refs/tags/v$VER.tar.gz
fi
[ "$(sha256 -q rocksdb-$VER.tar.gz)" = $SHA ] || {
	echo "rocksdb-$VER.tar.gz: SHA-256 mismatch" >&2; exit 1; }
if [ ! -f src/.patched ]; then
	rm -rf src
	mkdir src
	tar xzf rocksdb-$VER.tar.gz -C src --strip-components 1
	# pthread_t is a pointer on FreeBSD (casts in the io_uring code), and
	# env_test needs io_posix.h wherever io_uring is, not only on Linux.
	patch -s -d src -p1 < "$HERE/freebsd.diff"
	touch src/.patched
fi

# RocksDB's CMake takes liburing wherever find_package(uring) finds it;
# the port's headers need its overlay and config-host.h first.  The port's
# liburing.a isn't position-independent, so no shared library.
configure() {	# build directory, build type
	mkdir -p "$W/$1"
	cd "$W/$1"
	cmake ../src -DCMAKE_BUILD_TYPE="$2" -DPORTABLE=1 	    -DFAIL_ON_WARNINGS=OFF -DROCKSDB_BUILD_SHARED=OFF 	    -DWITH_LIBURING=ON -During_INCLUDE_DIR="$INC" 	    -During_LIBRARIES="$LIBURING/work/out/liburing.a" 	    -DCMAKE_C_FLAGS="-I$OVL -include $OVL/config-host.h" 	    -DCMAKE_CXX_FLAGS="-I$OVL -include $OVL/config-host.h" 	    -DWITH_GFLAGS=ON -DWITH_SNAPPY=ON -DWITH_LZ4=ON -DWITH_ZSTD=ON 	    -DWITH_BENCHMARK_TOOLS=ON -DWITH_TOOLS=ON 	    -DCMAKE_PREFIX_PATH=/usr/local > cmake.log 2>&1 || {
		tail -30 cmake.log >&2; exit 1; }
	grep -q "^uring_LIBRARIES:FILEPATH=$LIBURING/work/out/liburing.a" 	    CMakeCache.txt || {
		echo "cmake did not find liburing (see $1/cmake.log)" >&2
		exit 1; }
}

# Release for db_bench; RocksDB builds its tests only in
# Debug, which also checks its assertions.
configure build Release
make -j"$(sysctl -n hw.ncpu)" db_bench
configure build-debug Debug
make -j"$(sysctl -n hw.ncpu)" $TESTS
echo "built: $W/build (db_bench), $W/build-debug ($TESTS)"
