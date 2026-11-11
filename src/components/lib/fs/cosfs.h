/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * cosfs: a small on-disk file system (format in cosfs_format.h) on
 * top of an abstract block device, so the same code runs in the VFS
 * component (over the blkdev interface) and on the host (over an
 * image file, see tools/cosfs).
 */

#ifndef COSFS_H
#define COSFS_H

#include "fs.h"
#include "cosfs_format.h"

/* A device of nblocks COSFS_BLOCK_SIZE-byte blocks. Callbacks return 0 or -errno. */
struct fs_blkdev {
	void    *ctx;
	uint64_t nblocks;
	int    (*read)(void *ctx, uint64_t blk, void *buf);
	int    (*write)(void *ctx, uint64_t blk, const void *buf);
	int    (*flush)(void *ctx);
};

struct cosfs;

extern const struct fs_ops cosfs_ops;

/*
 * Write an empty file system to the device. ninodes == 0 picks a
 * default (one inode per 4 blocks). Returns 0 or -errno.
 */
int cosfs_mkfs(struct fs_blkdev *dev, uint32_t ninodes);

/* Is block 0 of the device all zeros (i.e. never formatted)? 1 yes, 0 no, <0 error */
int cosfs_dev_blank(struct fs_blkdev *dev);

/* Mount the file system on dev. Returns NULL and sets *err on failure. */
struct cosfs *cosfs_mount(struct fs_blkdev *dev, int *err);

/* Free blocks currently available (for statistics and tests). */
uint64_t cosfs_free_blocks(struct cosfs *fs);

#endif /* COSFS_H */
