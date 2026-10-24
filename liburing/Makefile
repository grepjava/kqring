# SPDX-License-Identifier: BSD-2-Clause
#
# liburing for FreeBSD on kqring.ko.
#
# Fetches the pinned liburing release, applies the overlay in freebsd/
# (system calls through kqring, Linux header shims, compat.h), and builds
# liburing.a, the examples and liburing's own test programs.  No GNU make
# or configure needed.
#
#	make			liburing.a and the examples
#	make tests		also liburing's tests (those that build here)
#	make check		run them, with kqring.ko loaded
#	make clean		remove work/

LIBURING_VERSION=	2.9
LIBURING_SHA256=	897b1153b55543e8b92a5a3eb9b906537a5fedcf8afaf241f8b8787940c79f8d
DISTFILE=	liburing-${LIBURING_VERSION}.tar.gz
MASTER_SITE=	https://github.com/axboe/liburing/archive/refs/tags

WRKDIR=		${.CURDIR}/work
WRKSRC=		${WRKDIR}/liburing-liburing-${LIBURING_VERSION}
OVERLAY=	${.CURDIR}/freebsd
OUT=		${WRKDIR}/out

CC?=		cc
CFLAGS=		-O2 -g -Wall -Wno-unused-parameter -Wno-sign-compare \
		-Wno-missing-field-initializers -Wno-unknown-pragmas
CPPFLAGS=	-D_GNU_SOURCE \
		-I${OVERLAY}/include -I${WRKSRC}/src/include \
		-include ${OVERLAY}/include/config-host.h
LIB_SRCS=	setup.c queue.c register.c syscall.c version.c
# Test programs only: Linux behaviour they depend on (see the header).
TEST_CPPFLAGS=	-include ${OVERLAY}/include/test-compat.h

.PHONY: all lib examples tests check clean

all: examples

${WRKSRC}/.patched:
	mkdir -p ${WRKDIR}
	cd ${WRKDIR} && [ -f ${DISTFILE} ] || \
	    fetch -o ${DISTFILE} ${MASTER_SITE}/${DISTFILE}
	cd ${WRKDIR} && [ "$$(sha256 -q ${DISTFILE})" = ${LIBURING_SHA256} ] || \
	    { echo "${DISTFILE}: checksum mismatch"; exit 1; }
	cd ${WRKDIR} && tar xzf ${DISTFILE}
	cd ${WRKSRC} && patch -s -p0 < ${OVERLAY}/freebsd.diff
	mkdir -p ${WRKSRC}/src/arch/freebsd
	printf '#ifndef LIBURING_VERSION_H\n#define LIBURING_VERSION_H\n#define IO_URING_VERSION_MAJOR %s\n#define IO_URING_VERSION_MINOR %s\n#endif\n' \
	    ${LIBURING_VERSION:R} ${LIBURING_VERSION:E} \
	    > ${WRKSRC}/src/include/liburing/io_uring_version.h
	touch ${.TARGET}

lib: ${OUT}/liburing.a

# Rebuilt whenever the overlay changes; the tests are relinked by `tests'.
OVERLAY_FILES!=	find ${OVERLAY} -type f
${OUT}/liburing.a: ${WRKSRC}/.patched ${OVERLAY_FILES}
	cp ${OVERLAY}/syscall.h ${WRKSRC}/src/arch/freebsd/syscall.h
	mkdir -p ${OUT}/obj
	cd ${WRKSRC}/src && for s in ${LIB_SRCS}; do \
	    ${CC} ${CFLAGS} ${CPPFLAGS} -DLIBURING_INTERNAL -c $$s \
	    -o ${OUT}/obj/$${s%.c}.o || exit 1; done
	rm -f ${.TARGET}
	ar rcs ${.TARGET} ${LIB_SRCS:S/^/${OUT}\/obj\//:S/.c$/.o/}

# Each example and test is built on its own; one that needs Linux-only
# headers or calls is listed in *.unbuilt rather than failing the build.
examples: lib
	mkdir -p ${OUT}/examples
	: > ${OUT}/examples.unbuilt
	cd ${WRKSRC}/examples && for s in *.c; do \
	    ${CC} ${CFLAGS} ${CPPFLAGS} $$s -o ${OUT}/examples/$${s%.c} \
	    ${OUT}/liburing.a -lpthread -lm >/dev/null 2>&1 || \
	    echo $${s%.c} >> ${OUT}/examples.unbuilt; done
	@echo "examples: $$(ls ${OUT}/examples | wc -l) built," \
	    "$$(wc -l < ${OUT}/examples.unbuilt) not (see ${OUT}/examples.unbuilt)"

# xfail_*.c are liburing's AddressSanitizer tests (built only with ASan
# there): without it they wait for a completion that never comes.
tests: lib
	mkdir -p ${OUT}/test
	rm -f ${OUT}/test/xfail_*.t
	: > ${OUT}/test.unbuilt
	cd ${WRKSRC}/test && ${CC} ${CFLAGS} ${CPPFLAGS} ${TEST_CPPFLAGS} \
	    -c helpers.c -o ${OUT}/test/helpers.o
	cd ${WRKSRC}/test && for s in *.c; do \
	    case $$s in helpers.c|xfail_*) continue;; esac; \
	    ${CC} ${CFLAGS} ${CPPFLAGS} ${TEST_CPPFLAGS} $$s ${OUT}/test/helpers.o \
	    -o ${OUT}/test/$${s%.c}.t ${OUT}/liburing.a -lpthread -lm \
	    >/dev/null 2>&1 || echo $${s%.c} >> ${OUT}/test.unbuilt; done
	@echo "tests: $$(ls ${OUT}/test/*.t | wc -l) built," \
	    "$$(wc -l < ${OUT}/test.unbuilt) not (see ${OUT}/test.unbuilt)"

# liburing's convention: exit 0 pass, 77 skip, anything else fail.
check: tests
	@mkdir -p ${OUT}/logs && cd ${OUT}/test && : > ../check.log && \
	pass=0; skip=0; fail=0; for t in *.t; do \
	    rc=0; timeout 30 ./$$t > ../logs/$${t%.t}.log 2>&1 || rc=$$?; \
	    case $$rc in 0) pass=$$((pass+1)); r=PASS;; \
	    77) skip=$$((skip+1)); r=SKIP;; \
	    *) fail=$$((fail+1)); r="FAIL($$rc)";; esac; \
	    echo "$$r $${t%.t}" >> ../check.log; done; \
	echo "check: $$pass passed, $$skip skipped, $$fail failed" \
	    "(see ${OUT}/check.log)"

clean:
	rm -rf ${WRKDIR}
