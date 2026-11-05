/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Replaces fio's engines/bsg.c on FreeBSD: the SCSI side of the
 * io_uring_cmd engine needs Linux's bsg(4) driver, which FreeBSD has not.
 */
#include <errno.h>

#include "../fio.h"
#include "bsg.h"

int
fio_bsg_uring_cmd_read_capacity(struct thread_data *td, unsigned int *bs,
    unsigned long long *max_lba)
{
	return (-EOPNOTSUPP);
}

int
fio_bsg_uring_cmd_get_file_size(struct thread_data *td, struct fio_file *f)
{
	return (-EOPNOTSUPP);
}

int
fio_bsg_uring_cmd_prep(struct bsg_uring_cmd *cmd, struct io_u *io_u,
    struct bsg_cmd *bc, bool fua, unsigned int cdb_len,
    enum bsg_write_mode wmode, unsigned int verify_bytchk,
    enum bsg_read_mode rmode)
{
	return (-EOPNOTSUPP);
}
