/*
 * RAM disk backend for virtio-blk: the disk is a buffer in the VMM's own
 * memory, lost when the system stops.
 */
#include <cos_types.h>
#include <cos_debug.h>
#include <llprint.h>
#include <string.h>
#include <memmgr.h>
#include <errno.h>
#include "vblk_backend.h"

static int
ramdisk_read(struct vblk_backend *be, u64_t off, void *buf, u32_t len)
{
	assert(off + len <= be->size);
	memcpy(buf, (char *)be->priv + off, len);
	return 0;
}

static int
ramdisk_write(struct vblk_backend *be, u64_t off, const void *buf, u32_t len)
{
	assert(off + len <= be->size);
	memcpy((char *)be->priv + off, buf, len);
	return 0;
}

static int
ramdisk_flush(struct vblk_backend *be)
{
	/* Nothing is cached anywhere: writes are "durable" when they return. */
	return 0;
}

static const struct vblk_backend_ops ramdisk_ops = {
	.read  = ramdisk_read,
	.write = ramdisk_write,
	.flush = ramdisk_flush,
};

int
vblk_ramdisk_create(struct vblk_backend *be, u64_t size)
{
	unsigned long npages;
	void *mem;

	if (size == 0 || size % PAGE_SIZE_4K != 0) return -EINVAL;
	npages = size / PAGE_SIZE_4K;

	mem = (void *)memmgr_heap_page_allocn(npages);
	if (!mem) return -ENOMEM;
	memset(mem, 0, size);

	be->ops      = &ramdisk_ops;
	be->name     = "cos-vmm-ramdisk";
	be->size     = size;
	be->readonly = 0;
	be->priv     = mem;

	return 0;
}
