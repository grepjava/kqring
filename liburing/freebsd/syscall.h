/* SPDX-License-Identifier: MIT */
/*
 * liburing system calls on FreeBSD, through kqring.ko.
 *
 * kqring registers kqring_setup(2), kqring_enter(2) and kqring_register(2)
 * in free system-call slots when it loads; their numbers are in sysctl
 * kern.kqring.sys_{setup,enter,register}.  They take io_uring_setup(2),
 * io_uring_enter(2) and io_uring_register(2)'s arguments.  The other
 * wrappers are the libc calls, as in arch/generic/syscall.h.
 *
 * Setup passes the flags through: without IORING_SETUP_SINGLE_ISSUER any
 * thread of the process may use the ring.  kqring runs completions in an
 * enter, as DEFER_TASKRUN does, and liburing enters to reap them when
 * IORING_SQ_TASKRUN is set.
 */
#ifndef LIBURING_ARCH_FREEBSD_SYSCALL_H
#define LIBURING_ARCH_FREEBSD_SYSCALL_H

#include <sys/types.h>
#include <sys/sysctl.h>
#include <fcntl.h>

enum { __KQRING_SYS_SETUP, __KQRING_SYS_ENTER, __KQRING_SYS_REGISTER };

static inline long __kqring_sysnum(int which)
{
	static const char *const names[] = {
		"kern.kqring.sys_setup",
		"kern.kqring.sys_enter",
		"kern.kqring.sys_register",
	};
	static int nums[] = { -2, -2, -2 };	/* -2: not looked up yet */
	int n = __atomic_load_n(&nums[which], __ATOMIC_RELAXED);

	if (n == -2) {
		size_t len = sizeof(n);
		int e = errno;

		if (sysctlbyname(names[which], &n, &len, NULL, 0) != 0 ||
		    n <= 0)
			n = -1;
		errno = e;
		__atomic_store_n(&nums[which], n, __ATOMIC_RELAXED);
	}
	return n;
}

/* A kqring system call: the result, or -errno (-ENOSYS without kqring). */
static inline long __kqring_syscall(int which, long a1, long a2, long a3,
				    long a4, long a5, long a6)
{
	long num = __kqring_sysnum(which);

	if (num <= 0)
		return -ENOSYS;
#if defined(__x86_64__)
	{
		/* syscall(2) would fetch the sixth argument from the stack. */
		register long r10 __asm__("r10") = a4;
		register long r8 __asm__("r8") = a5;
		register long r9 __asm__("r9") = a6;
		unsigned char failed;

		__asm__ __volatile__("syscall; setc %1"
		    : "+a"(num), "=r"(failed), "+D"(a1), "+S"(a2), "+d"(a3),
		      "+r"(r10), "+r"(r8), "+r"(r9)
		    :
		    : "rcx", "r11", "memory", "cc");
		return failed ? -num : num;
	}
#else
	{
		long ret = syscall((int)num, a1, a2, a3, a4, a5, a6);

		return ret < 0 ? -errno : ret;
	}
#endif
}

static inline int __sys_io_uring_register(unsigned int fd, unsigned int opcode,
					  const void *arg, unsigned int nr_args)
{
	return (int) __kqring_syscall(__KQRING_SYS_REGISTER, (long) fd,
				      (long) opcode, (long) arg,
				      (long) nr_args, 0, 0);
}

static inline int __sys_io_uring_setup(unsigned int entries,
				       struct io_uring_params *p)
{
	return (int) __kqring_syscall(__KQRING_SYS_SETUP, (long) entries,
				      (long) p, 0, 0, 0, 0);
}

static inline int __sys_io_uring_enter2(unsigned int fd, unsigned int to_submit,
					unsigned int min_complete,
					unsigned int flags, sigset_t *sig,
					size_t sz)
{
	return (int) __kqring_syscall(__KQRING_SYS_ENTER, (long) fd,
				      (long) to_submit, (long) min_complete,
				      (long) flags, (long) sig, (long) sz);
}

static inline int __sys_io_uring_enter(unsigned int fd, unsigned int to_submit,
				       unsigned int min_complete,
				       unsigned int flags, sigset_t *sig)
{
	return __sys_io_uring_enter2(fd, to_submit, min_complete, flags, sig,
				     _NSIG / 8);
}

static inline int __sys_open(const char *pathname, int flags, mode_t mode)
{
	int ret = open(pathname, flags, mode);

	return (ret < 0) ? -errno : ret;
}

static inline ssize_t __sys_read(int fd, void *buffer, size_t size)
{
	ssize_t ret = read(fd, buffer, size);

	return (ret < 0) ? -errno : ret;
}

static inline void *__sys_mmap(void *addr, size_t length, int prot, int flags,
			       int fd, off_t offset)
{
	void *ret = mmap(addr, length, prot, flags, fd, offset);

	return (ret == MAP_FAILED) ? ERR_PTR(-errno) : ret;
}

static inline int __sys_munmap(void *addr, size_t length)
{
	int ret = munmap(addr, length);

	return (ret < 0) ? -errno : ret;
}

/* MADV_DONTFORK (see compat.h) is minherit(2)'s INHERIT_NONE here. */
static inline int __sys_madvise(void *addr, size_t length, int advice)
{
	int ret;

	if (advice == MADV_DONTFORK)
		ret = minherit(addr, length, INHERIT_NONE);
	else if (advice == MADV_DOFORK)
		ret = minherit(addr, length, INHERIT_SHARE);
	else
		ret = madvise(addr, length, advice);
	return (ret < 0) ? -errno : ret;
}

static inline int __sys_getrlimit(int resource, struct rlimit *rlim)
{
	int ret = getrlimit(resource, rlim);

	return (ret < 0) ? -errno : ret;
}

static inline int __sys_setrlimit(int resource, const struct rlimit *rlim)
{
	int ret = setrlimit(resource, rlim);

	return (ret < 0) ? -errno : ret;
}

static inline int __sys_close(int fd)
{
	int ret = close(fd);

	return (ret < 0) ? -errno : ret;
}

#endif /* LIBURING_ARCH_FREEBSD_SYSCALL_H */
