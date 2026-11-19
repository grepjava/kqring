# SPDX-License-Identifier: BSD-2-Clause

CC?=		cc
CFLAGS=		-O2 -Wall -Wextra -Werror -std=c11 -I../include -pthread

all: kqring_test kqring_bench kqring_mt_bench kqring_stress kqring_diskbench \
    kqring_fuzz kqring_diff kqring_xbench kqring_twlat kqring_pagecheck \
    kqring_pagecheck_last zfs_dbuf

kqring_test: kqring_test.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_test kqring_test.c

kqring_bench: kqring_bench.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_bench kqring_bench.c

kqring_mt_bench: kqring_mt_bench.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_mt_bench kqring_mt_bench.c

kqring_stress: kqring_stress.c kqring_leak.h ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_stress kqring_stress.c

kqring_diskbench: kqring_diskbench.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_diskbench kqring_diskbench.c

kqring_fuzz: kqring_fuzz.c kqring_leak.h ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_fuzz kqring_fuzz.c

kqring_diff: kqring_diff.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_diff kqring_diff.c

kqring_xbench: kqring_xbench.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_xbench kqring_xbench.c

kqring_twlat: kqring_twlat.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_twlat kqring_twlat.c

kqring_pagecheck: kqring_pagecheck.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_pagecheck kqring_pagecheck.c

kqring_pagecheck_last: kqring_pagecheck.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -DPAGECHECK_LAST -o kqring_pagecheck_last \
	    kqring_pagecheck.c

zfs_dbuf: zfs_dbuf.c
	$(CC) $(CFLAGS) -o zfs_dbuf zfs_dbuf.c

clean:
	rm -f kqring_test kqring_bench kqring_mt_bench kqring_stress \
	    kqring_diskbench kqring_fuzz kqring_diff kqring_xbench kqring_twlat \
	    kqring_pagecheck \
	    zfs_dbuf
