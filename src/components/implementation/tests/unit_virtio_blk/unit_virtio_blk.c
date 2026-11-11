/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * unit_virtio_blk: raw sector I/O through the blkdev interface
 * (blkdev.virtio). Run with a scratch disk image of at least 16 MiB:
 *
 *   qemu-system-x86_64 ... -drive file=blk.img,if=virtio,format=raw
 *
 * Besides in-guest round trips, it leaves a marker the host can check
 * afterwards: sectors [MARK_SECTOR, MARK_SECTOR + MARK_NSECT) hold the
 * test pattern with seed MARK_SEED, i.e.
 *
 *   tools/cosfs/cosfs verify-raw blk.img $((2048*512)) $((256*512)) 11
 */

#include <cos_component.h>
#include <llprint.h>
#include <string.h>
#include <blkdev.h>
#include <unit_vfs_ramfs/vfs_test.h>

#define SHM_PAGES 64 /* 256 KiB */
#define SHM_SIZE (SHM_PAGES * PAGE_SIZE)
#define MARK_SECTOR 2048
#define MARK_NSECT 256
#define MARK_SEED 11

static unsigned char *shm;
static long           capacity;

static void
fill(unsigned long off, unsigned long len, unsigned long base, unsigned seed)
{
	unsigned long i;

	for (i = 0; i < len; i++) shm[off + i] = vt_pattern(base + i, seed);
}

static int
check(unsigned long off, unsigned long len, unsigned long base, unsigned seed)
{
	unsigned long i;

	for (i = 0; i < len; i++) {
		if (shm[off + i] != vt_pattern(base + i, seed)) return 0;
	}
	return 1;
}

static void
test_info(void)
{
	capacity = blkdev_capacity();
	printc("unit_virtio_blk: capacity %ld sectors\n", capacity);
	VT_CHECK(capacity >= 16 * 1024 * 1024 / BLKDEV_SECTOR_SIZE);
}

static void
test_single_sector(void)
{
	fill(0, 512, 0, 1);
	VT_CHECK(blkdev_write(0, 1, 0) == 1);
	memset(shm + 4096, 0, 512);
	VT_CHECK(blkdev_read(0, 1, 4096) == 1);
	VT_CHECK(check(4096, 512, 0, 1));
}

static void
test_multi_sector(void)
{
	/* 64 KiB, page aligned */
	fill(0, 65536, 1000 * 512, 2);
	VT_CHECK(blkdev_write(1000, 128, 0) == 128);
	memset(shm + 65536, 0, 65536);
	VT_CHECK(blkdev_read(1000, 128, 65536) == 128);
	VT_CHECK(check(65536, 65536, 1000 * 512, 2));
	/* read a sub-range at an odd sector */
	memset(shm, 0, 4096);
	VT_CHECK(blkdev_read(1003, 5, 0) == 5);
	VT_CHECK(check(0, 5 * 512, 1003 * 512, 2));
}

static void
test_large_unaligned(void)
{
	/* nearly the whole buffer at a non-page-aligned, non-sector-aligned offset: many segments, split requests */
	unsigned long off = 100, nsect = (SHM_SIZE - off) / 512;

	fill(off, nsect * 512, 5000 * 512, 3);
	VT_CHECK(blkdev_write(5000, nsect, off) == (long)nsect);
	memset(shm, 0, SHM_SIZE);
	VT_CHECK(blkdev_read(5000, nsect, off) == (long)nsect);
	VT_CHECK(check(off, nsect * 512, 5000 * 512, 3));
	/* read it back again at a different alignment */
	memset(shm, 0, SHM_SIZE);
	VT_CHECK(blkdev_read(5000, nsect, 3000) == (long)nsect);
	VT_CHECK(check(3000, nsect * 512, 5000 * 512, 3));
}

static void
test_overwrite(void)
{
	fill(0, 8 * 512, 0, 4);
	VT_CHECK(blkdev_write(3000, 8, 0) == 8);
	fill(0, 2 * 512, 0, 5);
	VT_CHECK(blkdev_write(3003, 2, 0) == 2);
	memset(shm, 0, 8 * 512);
	VT_CHECK(blkdev_read(3000, 8, 0) == 8);
	VT_CHECK(check(0, 3 * 512, 0, 4));
	VT_CHECK(check(3 * 512, 2 * 512, 0, 5));
	VT_CHECK(check(5 * 512, 3 * 512, 5 * 512, 4));
}

static void
test_bounds(void)
{
	VT_CHECK(blkdev_read(capacity - 1, 1, 0) == 1);
	VT_CHECK(blkdev_write(capacity - 1, 1, 0) == 1);
	VT_CHECK(blkdev_read(capacity, 1, 0) == -EINVAL);
	VT_CHECK(blkdev_read(capacity - 1, 2, 0) == -EINVAL);
	VT_CHECK(blkdev_read(0, 0, 0) == -EINVAL);
	VT_CHECK(blkdev_read(0, SHM_SIZE / 512 + 1, 0) == -EINVAL);
	VT_CHECK(blkdev_read(0, 1, SHM_SIZE - 100) == -EINVAL);
	VT_CHECK(blkdev_read(0, 1, SHM_SIZE) == -EINVAL);
	VT_CHECK(blkdev_read(0, 1, (unsigned long)-1) == -EINVAL);
}

static void
test_marker_and_flush(void)
{
	fill(0, MARK_NSECT * 512, 0, MARK_SEED);
	VT_CHECK(blkdev_write(MARK_SECTOR, MARK_NSECT, 0) == MARK_NSECT);
	VT_CHECK(blkdev_flush() == 0);
	memset(shm, 0, MARK_NSECT * 512);
	VT_CHECK(blkdev_read(MARK_SECTOR, MARK_NSECT, 0) == MARK_NSECT);
	VT_CHECK(check(0, MARK_NSECT * 512, 0, MARK_SEED));
}

int
main(void)
{
	shm = blkdev_client_shm(SHM_PAGES);
	if (!shm) {
		printc("FAILURE: cannot set up the blkdev buffer (is a virtio-blk disk attached?)\n");
		VT_DONE();
		return 0;
	}
	VT_CASE(test_info);
	VT_CASE(test_single_sector);
	VT_CASE(test_multi_sector);
	VT_CASE(test_large_unaligned);
	VT_CASE(test_overwrite);
	VT_CASE(test_bounds);
	VT_CASE(test_marker_and_flush);
	VT_DONE();

	return 0;
}
