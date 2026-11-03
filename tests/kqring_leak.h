/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Jayakumar Thanakodi
 *
 * Leak checks shared by kqring_stress and kqring_fuzz: once every ring
 * and descriptor is closed, no socket should be left that nothing refers
 * to, nor user-wired pages or kqring malloc memory beyond the baseline.
 */
#ifndef KQRING_LEAK_H
#define KQRING_LEAK_H

#include <sys/param.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/un.h>
#include <sys/unpcb.h>
#include <netinet/in.h>
#include <netinet/in_pcb.h>
#include <netinet/tcp.h>
#include <netinet/tcp_var.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static long
sysctl_num(const char *name)
{
	union {
		uint32_t	u32;
		uint64_t	u64;
	} v;
	size_t len = sizeof(v);

	memset(&v, 0, sizeof(v));
	if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
		return (-1);
	return (len == sizeof(v.u32) ? (long)v.u32 : (long)v.u64);
}

/* InUse of malloc type "kqring" (vmstat -m), or -1. */
static long
kqring_malloc_inuse(void)
{
	char line[256], name[64];
	long inuse = -1, n;
	FILE *f = popen("vmstat -m", "r");

	if (f == NULL)
		return (-1);
	while (fgets(line, sizeof(line), f) != NULL)
		if (sscanf(line, " %63s %ld", name, &n) == 2 &&
		    strcmp(name, "kqring") == 0)
			inuse = n;
	pclose(f);
	return (inuse);
}

/*
 * Sockets of ours that no descriptor refers to, less those TCP itself
 * holds after close(2) to deliver what is still queued (INP_SOCKREF).  On
 * a host whose packet filter drops some of that traffic they linger for
 * hours with no process or kqring involved.  Counting only our uid and
 * only unreferenced sockets keeps other processes' churn out of it; a
 * reference kqring failed to drop shows up here.
 */
static void *
sysctl_blob(const char *name, size_t *lenp)
{
	void *buf = NULL;
	size_t len;

	for (int i = 0; i < 10; i++) {
		if (sysctlbyname(name, NULL, &len, NULL, 0) != 0)
			break;
		len += len / 4 + 4096;
		free(buf);
		if ((buf = malloc(len)) == NULL)
			break;
		if (sysctlbyname(name, buf, &len, NULL, 0) == 0) {
			*lenp = len;
			return (buf);
		}
		if (errno != ENOMEM)
			break;
	}
	free(buf);
	return (NULL);
}

static int
kvaddr_cmp(const void *a, const void *b)
{
	kvaddr_t x = *(const kvaddr_t *)a, y = *(const kvaddr_t *)b;

	return (x < y ? -1 : x > y);
}

static long
open_sockets(void)
{
	static const char *const lists[] = { "net.inet.tcp.pcblist",
	    "net.local.stream.pcblist", "net.local.dgram.pcblist",
	    "net.local.seqpacket.pcblist" };
	struct xfile *xf;
	kvaddr_t *held;
	size_t len, nheld = 0;
	uid_t uid = getuid();
	long n = 0;

	if ((xf = sysctl_blob("kern.file", &len)) == NULL)
		return (-1);
	if ((held = calloc(len / sizeof(*xf) + 1, sizeof(*held))) == NULL) {
		free(xf);
		return (-1);
	}
	for (size_t i = 0; i < len / sizeof(*xf); i++)
		if (xf[i].xf_type == DTYPE_SOCKET)
			held[nheld++] = xf[i].xf_data;
	free(xf);
	qsort(held, nheld, sizeof(*held), kvaddr_cmp);

	for (size_t l = 0; l < nitems(lists); l++) {
		char *buf, *p, *end;

		if ((buf = sysctl_blob(lists[l], &len)) == NULL)
			continue;
		end = buf + len;
		for (p = buf + ((struct xinpgen *)buf)->xig_len;
		    p + sizeof(struct xinpgen) < end &&
		    ((struct xinpgen *)p)->xig_len > sizeof(struct xinpgen);
		    p += ((struct xinpgen *)p)->xig_len) {
			const struct xsocket *xso;

			if (l == 0) {
				const struct xtcpcb *xt = (const void *)p;

				if (xt->xt_inp.inp_flags & INP_SOCKREF)
					continue;
				xso = &xt->xt_inp.xi_socket;
			} else
				xso = &((const struct xunpcb *)p)->xu_socket;
			if (xso->xso_so == 0 || xso->so_uid != uid ||
			    bsearch(&xso->xso_so, held, nheld, sizeof(*held),
			    kvaddr_cmp) != NULL)
				continue;
			n++;
		}
		free(buf);
	}
	free(held);
	return (n);
}

/* Wait (up to 3 min) for things to drain, then compare with the baseline. */
static int
check_leaks(long socks0, long wired0, long mem0)
{
	long socks = 0, wired = 0, mem = 0;

	for (int i = 0; i < 360; i++) {
		socks = open_sockets();
		wired = sysctl_num("vm.stats.vm.v_user_wire_count");
		mem = kqring_malloc_inuse();
		if (socks <= socks0 && wired <= wired0 && mem <= mem0)
			break;
		usleep(500000);
	}
	printf("leak check: unreferenced sockets %ld -> %ld, user-wired pages %ld -> %ld, "
	    "kqring malloc in use %ld -> %ld\n", socks0, socks, wired0, wired,
	    mem0, mem);
	if (socks > socks0 || wired > wired0 || mem > mem0) {
		fprintf(stderr, "%s: FAIL: resources still held "
		    "after everything was closed\n", getprogname());
		return (1);
	}
	return (0);
}


#endif /* !KQRING_LEAK_H */
