#ifndef MEMMGR_PEER_H
#define MEMMGR_PEER_H

/***
 * A test interface: a peer component that maps, checks, and unmaps
 * shared memory (memmgr_shared_page_*) allocated by its client. Used
 * to test the release of shared memory across components.
 */

#include <cos_component.h>
#include <cos_stubs.h>
#include <memmgr.h>

/*
 * Map the npages pages of shared memory id, check that the first word
 * of page i is `id + i` (written by the client), and write `~(id + i)`
 * into its second word. Returns 0 on success, 1 if the mapping
 * failed, 2 if the contents are wrong.
 */
int memmgr_peer_map(cbuf_t id, unsigned long npages);
int COS_STUB_DECL(memmgr_peer_map)(cbuf_t id, unsigned long npages);

/* Unmap our mapping of id. Returns memmgr_shared_page_unmap's result. */
int memmgr_peer_unmap(cbuf_t id);
int COS_STUB_DECL(memmgr_peer_unmap)(cbuf_t id);

/* Try to map id, expecting it to be released: returns the number of pages mapped (0 expected). */
unsigned long memmgr_peer_map_released(cbuf_t id);
unsigned long COS_STUB_DECL(memmgr_peer_map_released)(cbuf_t id);

#endif /* MEMMGR_PEER_H */
