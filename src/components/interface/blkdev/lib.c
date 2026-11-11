/*
 * Redistribution of this file is permitted under the BSD two clause license.
 */

#include <memmgr.h>
#include <blkdev.h>

void *
blkdev_client_shm(unsigned long npages)
{
	vaddr_t addr = 0;
	cbuf_t  id;

	id = memmgr_shared_page_allocn(npages, &addr);
	if (!id || !addr) return NULL;
	if (blkdev_shm_attach(id, npages) < 0) return NULL;

	return (void *)addr;
}
