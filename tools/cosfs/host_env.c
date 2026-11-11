/* Host environment for lib/fs. */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "host_env.h"

long host_pages_outstanding;

void *
fs_page_alloc(void)
{
	void *p = NULL;

	if (posix_memalign(&p, FS_PAGE_SIZE, FS_PAGE_SIZE)) return NULL;
	memset(p, 0, FS_PAGE_SIZE);
	host_pages_outstanding++;

	return p;
}

void
fs_page_free(void *p)
{
	host_pages_outstanding--;
	free(p);
}

static int
fdev_read(void *ctx, uint64_t blk, void *buf)
{
	struct host_dev *d = ctx;

	d->nreads++;
	if (d->mem) {
		memcpy(buf, d->mem + blk * COSFS_BLOCK_SIZE, COSFS_BLOCK_SIZE);
		return 0;
	}
	return pread(d->fd, buf, COSFS_BLOCK_SIZE, blk * COSFS_BLOCK_SIZE) == COSFS_BLOCK_SIZE ? 0 : -EIO;
}

static int
fdev_write(void *ctx, uint64_t blk, const void *buf)
{
	struct host_dev *d = ctx;

	d->nwrites++;
	if (d->mem) {
		memcpy(d->mem + blk * COSFS_BLOCK_SIZE, buf, COSFS_BLOCK_SIZE);
		return 0;
	}
	return pwrite(d->fd, buf, COSFS_BLOCK_SIZE, blk * COSFS_BLOCK_SIZE) == COSFS_BLOCK_SIZE ? 0 : -EIO;
}

static int
fdev_flush(void *ctx)
{
	struct host_dev *d = ctx;

	return d->mem ? 0 : (fsync(d->fd) ? -EIO : 0);
}

int
host_dev_open(struct host_dev *d, const char *path, int create, uint64_t size_bytes)
{
	struct stat st;

	memset(d, 0, sizeof(*d));
	d->fd = open(path, O_RDWR | (create ? O_CREAT : 0), 0644);
	if (d->fd < 0) return -errno;
	if (create && size_bytes && ftruncate(d->fd, size_bytes)) return -errno;
	if (fstat(d->fd, &st)) return -errno;
	d->dev = (struct fs_blkdev){.ctx = d, .nblocks = st.st_size / COSFS_BLOCK_SIZE,
	                            .read = fdev_read, .write = fdev_write, .flush = fdev_flush};

	return 0;
}

void
host_dev_mem(struct host_dev *d, char *mem, uint64_t nblocks)
{
	memset(d, 0, sizeof(*d));
	d->fd  = -1;
	d->mem = mem;
	d->dev = (struct fs_blkdev){.ctx = d, .nblocks = nblocks, .read = fdev_read, .write = fdev_write,
	                            .flush = fdev_flush};
}

void
host_dev_close(struct host_dev *d)
{
	if (d->fd >= 0) close(d->fd);
}
