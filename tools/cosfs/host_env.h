/* Host environment for lib/fs: page allocation and an image-file block device. */
#ifndef HOST_ENV_H
#define HOST_ENV_H

#include <stdio.h>
#include "cosfs.h"

/* Block device over a file (fd) or over memory (mem). */
struct host_dev {
	struct fs_blkdev dev;
	int              fd;
	char            *mem;
	long             nreads, nwrites;
};

int  host_dev_open(struct host_dev *d, const char *path, int create, uint64_t size_bytes);
void host_dev_mem(struct host_dev *d, char *mem, uint64_t nblocks);
void host_dev_close(struct host_dev *d);

/* fsck (fsck.c): returns the number of errors found, prints them to out if non-NULL. */
int cosfs_fsck(struct fs_blkdev *dev, FILE *out, int verbose);

/* The deterministic test-data pattern also used by the in-QEMU tests. */
static inline unsigned char
cosfs_pattern(uint64_t i, unsigned seed)
{
	return (unsigned char)((i * 131 + (i >> 9) * 7 + seed * 17 + 3) & 0xff);
}

#endif
