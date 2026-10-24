# SPDX-License-Identifier: BSD-2-Clause

CC?=		cc
CFLAGS=		-O2 -Wall -Wextra -Werror -std=c11 -I../include -pthread

all: kqring_test kqring_bench

kqring_test: kqring_test.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_test kqring_test.c

kqring_bench: kqring_bench.c ../include/sys/kqring.h
	$(CC) $(CFLAGS) -o kqring_bench kqring_bench.c

clean:
	rm -f kqring_test kqring_bench
