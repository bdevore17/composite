/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * The peer of tests.unit_memmgr_free: maps, checks, and unmaps shared
 * memory allocated by the test.
 */

#include <cos_component.h>
#include <llprint.h>
#include <memmgr.h>
#include <memmgr_peer.h>

#define PEER_MAX_IDS 64

static struct {
	cbuf_t  id;
	vaddr_t addr;
} mappings[PEER_MAX_IDS];

int
memmgr_peer_map(cbuf_t id, unsigned long npages)
{
	vaddr_t addr;
	unsigned long i;
	int slot;

	slot = id % PEER_MAX_IDS;
	if (mappings[slot].id) return 1;
	if (memmgr_shared_page_map(id, &addr) != npages || !addr) return 1;
	mappings[slot].id   = id;
	mappings[slot].addr = addr;

	for (i = 0; i < npages; i++) {
		word_t *w = (word_t *)(addr + i * PAGE_SIZE);

		if (w[0] != id + i) return 2;
		w[1] = ~(id + i);
	}

	return 0;
}

int
memmgr_peer_unmap(cbuf_t id)
{
	int slot = id % PEER_MAX_IDS;
	int ret;

	if (mappings[slot].id != id) return -EINVAL;
	ret = memmgr_shared_page_unmap(id, mappings[slot].addr);
	if (!ret) mappings[slot].id = 0;

	return ret;
}

unsigned long
memmgr_peer_map_released(cbuf_t id)
{
	vaddr_t addr;

	return memmgr_shared_page_map(id, &addr);
}

/*
 * We only serve invocations: no main. A parallel initialization
 * function tells the runtime that returning from initialization is
 * expected.
 */
void
cos_parallel_init(coreid_t cid, int init_core, int ncores)
{
}
