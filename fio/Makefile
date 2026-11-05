# SPDX-License-Identifier: BSD-2-Clause
#
# fio for FreeBSD with its io_uring engine on kqring.ko.
#
# Fetches the pinned fio release, applies the overlay in freebsd/ and builds
# it.  Needs gmake(1) (devel/gmake), which fio's own build requires.
#
#	make			work/fio-fio-${FIO_VERSION}/fio
#	make clean		remove work/

FIO_VERSION=	3.43
FIO_SHA256=	efa49b3f36eda9adf29294f27a87d7e457747d676cd0f73996d6365357832cfe
DISTFILE=	fio-${FIO_VERSION}.tar.gz
MASTER_SITE=	https://github.com/axboe/fio/archive/refs/tags

WRKDIR=		${.CURDIR}/work
WRKSRC=		${WRKDIR}/fio-fio-${FIO_VERSION}
OVERLAY=	${.CURDIR}/freebsd
EXTFLAGS=	-I${OVERLAY} -I${OVERLAY}/include -DIOPRIO_CLASS_RT=1 \
		-DO_LARGEFILE=0

.PHONY: all clean

all: ${WRKSRC}/.patched
	cd ${WRKSRC} && gmake -j`sysctl -n hw.ncpu` EXTFLAGS="${EXTFLAGS}"
	@echo "built ${WRKSRC}/fio"

${WRKSRC}/.patched:
	mkdir -p ${WRKDIR}
	cd ${WRKDIR} && fetch -q -o ${DISTFILE} \
	    ${MASTER_SITE}/fio-${FIO_VERSION}.tar.gz
	[ "`sha256 -q ${WRKDIR}/${DISTFILE}`" = "${FIO_SHA256}" ]
	cd ${WRKDIR} && tar xzf ${DISTFILE}
	cd ${WRKSRC} && patch -s -p0 < ${OVERLAY}/fio.diff
	cp ${OVERLAY}/bsg.c ${WRKSRC}/engines/bsg.c
	cd ${WRKSRC} && ./configure --disable-native > configure.log
	touch ${.TARGET}

clean:
	rm -rf ${WRKDIR}
