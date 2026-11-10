/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 */

/*
 * ZFS's two speeds for a cached file, with pread(2) alone (no kqring).
 *
 * Writes a 64 MiB file as kqring_bench's file group does, then reads
 * random 4 KiB blocks of it for 15 seconds, printing every 200,000 reads
 * the time since the write, the time per read and the dbuf cache's target
 * and size.  Until the transaction group holding the writes syncs
 * (vfs.zfs.txg.timeout, 5 s), the dirty records stay in memory
 * uncompressed and reads are fast.  After it, the records live in the
 * dbuf cache, capped at the ARC target >> vfs.zfs.dbuf.cache_shift (1/32):
 * if the file is bigger than that, each read evicts a 128 KiB record and
 * rebuilds another from the ARC (decompressing or copying it), and
 * reads slow down by an order of magnitude.
 *
 *   zfs_dbuf [-r] [dir]     -r: incompressible data (default: all 'f')
 */

#include <sys/types.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FILE_SIZE	(64U << 20)
#define BLK		4096
#define READS		200000
#define SECONDS		15

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec);
}

static uint64_t
kstat(const char *name)
{
	union { uint64_t u64; uint32_t u32; } v = { 0 };
	size_t len = sizeof(v);

	if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
		return (0);
	return (len == sizeof(v.u32) ? v.u32 : v.u64);
}

int
main(int argc, char **argv)
{
	static char data[BLK], buf[BLK];
	const char *dir = "/tmp";
	char path[1024];
	struct statfs sf;
	uint32_t seed = 12345;
	uint64_t t_write;
	bool random_data = false;
	off_t off;
	int fd, ch;

	while ((ch = getopt(argc, argv, "r")) != -1) {
		if (ch != 'r') {
			fprintf(stderr, "usage: zfs_dbuf [-r] [dir]\n");
			return (2);
		}
		random_data = true;
	}
	if (optind < argc)
		dir = argv[optind];
	snprintf(path, sizeof(path), "%s/zfs_dbuf.XXXXXX", dir);
	fd = mkstemp(path);
	assert(fd >= 0);
	unlink(path);
	assert(fstatfs(fd, &sf) == 0);
	if (strcmp(sf.f_fstypename, "zfs") != 0) {
		fprintf(stderr, "%s is %s, not zfs\n", dir, sf.f_fstypename);
		return (1);
	}

	memset(data, 'f', sizeof(data));
	for (off = 0; off < FILE_SIZE; off += BLK) {
		if (random_data)	/* fresh for each block: lz4 looks back */
			arc4random_buf(data, sizeof(data));
		assert(pwrite(fd, data, BLK, off) == BLK);
	}
	t_write = now_ns();
	for (off = 0; off < FILE_SIZE; off += BLK)
		assert(pread(fd, buf, BLK, off) == BLK);

	printf("# %s data, %u MiB file, txg timeout %ju s, dbuf cache_shift "
	    "%ju, ARC target %ju MiB\n", random_data ? "incompressible" :
	    "all-'f'", FILE_SIZE >> 20, (uintmax_t)kstat("vfs.zfs.txg.timeout"),
	    (uintmax_t)kstat("vfs.zfs.dbuf.cache_shift"),
	    (uintmax_t)(kstat("kstat.zfs.misc.arcstats.c") >> 20));
	printf("# seconds_since_write ns_per_read dbuf_target_mib "
	    "dbuf_size_mib\n");
	while (now_ns() - t_write < (uint64_t)SECONDS * 1000000000) {
		uint64_t t0 = now_ns(), dt;

		for (int i = 0; i < READS; i++) {
			seed = seed * 1103515245U + 12345U;
			off = (off_t)((seed >> 8) % (FILE_SIZE / BLK)) * BLK;
			assert(pread(fd, buf, BLK, off) == BLK);
		}
		dt = now_ns() - t0;
		printf("%.1f %.0f %ju %ju\n", (double)(now_ns() - t_write) / 1e9,
		    (double)dt / READS,
		    (uintmax_t)(kstat("kstat.zfs.misc.dbufstats.cache_target_bytes") >> 20),
		    (uintmax_t)(kstat("kstat.zfs.misc.dbufstats.cache_size_bytes") >> 20));
		fflush(stdout);
	}
	close(fd);
	return (0);
}
