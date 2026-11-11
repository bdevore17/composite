#pragma once

/*
 * Storage backends for the virtio-blk device model (devices/vpci/virtio_blk.c).
 *
 * The device model parses virtqueue requests and hands the backend plain
 * reads, writes and flushes of byte ranges; it knows nothing about where the
 * data lives. The first backend is a RAM disk inside the VMM
 * (vblk_ramdisk.c). A backend that forwards to a block-device component (a
 * native virtio-blk/NVMe driver behind a block interface) only has to provide
 * these three operations; see the notes on asynchronous completion below.
 *
 * Contract:
 *   - `off` and `len` are multiples of VBLK_SECTOR_SIZE, and
 *     off + len <= size (the device model checks this before calling);
 *   - `buf` points into guest memory and is only valid during the call;
 *   - operations return 0 on success or a negative errno, which the guest
 *     sees as VIRTIO_BLK_S_IOERR;
 *   - operations are called with the VM's device lock held, from the exit
 *     handler of the vCPU that notified the queue, so they must not block
 *     for long. They are synchronous for now: the request completes (and the
 *     guest is interrupted) when the call returns. An asynchronous backend
 *     would copy or pin the buffers, return, and complete later through
 *     virtio_blk_complete() with the device lock taken -- the device model
 *     already separates request parsing from completion for that reason.
 */

#include <cos_types.h>

#define VBLK_SECTOR_SIZE 512ULL

struct vblk_backend;

struct vblk_backend_ops {
	int (*read)(struct vblk_backend *be, u64_t off, void *buf, u32_t len);
	int (*write)(struct vblk_backend *be, u64_t off, const void *buf, u32_t len);
	int (*flush)(struct vblk_backend *be);
};

struct vblk_backend {
	const struct vblk_backend_ops *ops;
	/* Shown to the guest as the disk's serial number (VIRTIO_BLK_T_GET_ID) */
	const char *name;
	/* Capacity in bytes, a multiple of VBLK_SECTOR_SIZE */
	u64_t size;
	int   readonly;
	void *priv;
};

/* A zero-filled RAM disk of `size` bytes, allocated from the memory manager. */
int vblk_ramdisk_create(struct vblk_backend *be, u64_t size);
