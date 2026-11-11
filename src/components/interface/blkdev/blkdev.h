/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * blkdev: a block device addressed in 512-byte sectors.
 *
 * Data is transferred directly between the device and a shared-memory
 * buffer the client registers once with blkdev_shm_attach (the
 * virtio-blk driver DMAs straight into it). All functions return >= 0
 * on success and -errno on failure.
 */

#ifndef BLKDEV_H
#define BLKDEV_H

#include <cos_component.h>
#include <cos_stubs.h>
#include <errno.h>

#define BLKDEV_SECTOR_SIZE 512

/* Register the client's buffer (a memmgr cbuf of npages pages). */
long blkdev_shm_attach(cbuf_t shm, unsigned long npages);
/* Device size in sectors. */
long blkdev_capacity(void);
/* Transfer nsect sectors starting at sector to/from the buffer at byte offset shm_off. */
long blkdev_read(unsigned long sector, unsigned long nsect, unsigned long shm_off);
long blkdev_write(unsigned long sector, unsigned long nsect, unsigned long shm_off);
/* Make completed writes durable (-ENOTSUP if the device has no write cache control). */
long blkdev_flush(void);

/*
 * Client helper: allocate an npages buffer from memmgr and register it.
 * Returns the buffer's address, or NULL on failure.
 */
void *blkdev_client_shm(unsigned long npages);

#endif /* BLKDEV_H */
