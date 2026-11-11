/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * vfs.diskfs: the VFS server with ramfs at "/" and the on-disk cosfs
 * file system at "/disk", stored on the blkdev the component depends
 * on (e.g. blkdev.virtio).
 *
 * A device whose first block is all zeros is formatted on boot. A
 * device holding anything else that isn't a valid cosfs is left alone
 * (and /disk is not mounted), so a wrong disk is never overwritten.
 */

#include <cos_component.h>
#include <llprint.h>
#include <string.h>
#include <blkdev.h>
#include <vfs_server.h>
#include <ramfs.h>
#include <cosfs.h>

#define DISK_SHM_PAGES 2

static char            *disk_shm;
static struct fs_blkdev disk;

static int
disk_read(void *ctx, uint64_t blk, void *buf)
{
	if (blkdev_read(blk * COSFS_SECTORS_PER_BLOCK, COSFS_SECTORS_PER_BLOCK, 0) != COSFS_SECTORS_PER_BLOCK) {
		return -EIO;
	}
	memcpy(buf, disk_shm, COSFS_BLOCK_SIZE);

	return 0;
}

static int
disk_write(void *ctx, uint64_t blk, const void *buf)
{
	memcpy(disk_shm, buf, COSFS_BLOCK_SIZE);
	if (blkdev_write(blk * COSFS_SECTORS_PER_BLOCK, COSFS_SECTORS_PER_BLOCK, 0) != COSFS_SECTORS_PER_BLOCK) {
		return -EIO;
	}

	return 0;
}

static int
disk_flush(void *ctx)
{
	long ret = blkdev_flush();

	return (ret == 0 || ret == -ENOTSUP) ? 0 : -EIO;
}

static void
mount_disk(struct vfs_core *v)
{
	struct cosfs *fs;
	long          cap;
	int           ret;

	cap = blkdev_capacity();
	if (cap <= 0) {
		printc("vfs.diskfs: no block device (%ld); /disk not mounted\n", cap);
		return;
	}
	disk_shm = blkdev_client_shm(DISK_SHM_PAGES);
	if (!disk_shm) {
		printc("vfs.diskfs: cannot set up the blkdev buffer; /disk not mounted\n");
		return;
	}
	disk = (struct fs_blkdev){.nblocks = cap / COSFS_SECTORS_PER_BLOCK,
	                          .read    = disk_read,
	                          .write   = disk_write,
	                          .flush   = disk_flush};

	ret = cosfs_dev_blank(&disk);
	if (ret < 0) {
		printc("vfs.diskfs: cannot read the disk (%d)\n", ret);
		return;
	}
	if (ret == 1) {
		printc("vfs.diskfs: blank disk, formatting %llu blocks\n", (unsigned long long)disk.nblocks);
		if ((ret = cosfs_mkfs(&disk, 0))) {
			printc("vfs.diskfs: mkfs failed (%d)\n", ret);
			return;
		}
	}
	fs = cosfs_mount(&disk, &ret);
	if (!fs) {
		printc("vfs.diskfs: the disk does not hold a valid cosfs (%d); /disk not mounted\n", ret);
		return;
	}
	if ((ret = vfs_core_mount(v, "/disk", &cosfs_ops, fs))) {
		printc("vfs.diskfs: mount failed (%d)\n", ret);
		return;
	}
	printc("vfs.diskfs: cosfs mounted at /disk (%llu of %llu blocks free)\n",
	       (unsigned long long)cosfs_free_blocks(fs), (unsigned long long)disk.nblocks);
}

void
cos_init(void)
{
	struct vfs_core *v = vfs_server_init();
	struct ramfs    *rfs;

	rfs = ramfs_create(0);
	if (!rfs || vfs_core_mount(v, "/", &ramfs_ops, rfs)) {
		printc("vfs.diskfs: cannot create the root file system\n");
		BUG();
	}
	mount_disk(v);
}
