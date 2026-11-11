/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Copyright 2018, The George Washington University
 * Author: Phani Gadepalli, phanikishoreg@gwu.edu
 */

#ifndef MEMMGR_H
#define MEMMGR_H

#include <cos_types.h>
#include <cos_component.h>
#include <cos_stubs.h>

vaddr_t       memmgr_heap_page_alloc(void);
vaddr_t       memmgr_virt_to_phys(vaddr_t vaddr);
vaddr_t       memmgr_map_phys_to_virt(paddr_t paddr, size_t size);

vaddr_t       memmgr_heap_page_allocn(unsigned long num_pages);
vaddr_t       COS_STUB_DECL(memmgr_heap_page_allocn)(unsigned long num_pages);

vaddr_t       memmgr_heap_page_allocn_aligned(unsigned long num_pages, unsigned long align);
vaddr_t       COS_STUB_DECL(memmgr_heap_page_allocn_aligned)(unsigned long num_pages, unsigned long align);

/*
 * Free heap pages from memmgr_heap_page_alloc*: unmap num_pages pages
 * starting at addr. A range may be a part of an allocation. Returns 0
 * on success, -EINVAL (and frees nothing) if any page in the range is
 * not one of our heap pages.
 */
int           memmgr_heap_page_free(vaddr_t addr);
int           memmgr_heap_page_freen(vaddr_t addr, unsigned long num_pages);
int           COS_STUB_DECL(memmgr_heap_page_freen)(vaddr_t addr, unsigned long num_pages);

cbuf_t        memmgr_shared_page_alloc(vaddr_t *pgaddr);

cbuf_t        memmgr_shared_page_allocn(unsigned long num_pages, vaddr_t *pgaddr);
cbuf_t        COS_STUB_DECL(memmgr_shared_page_allocn)(unsigned long num_pages, vaddr_t *pgaddr);

cbuf_t        memmgr_shared_page_allocn_aligned(unsigned long num_pages, unsigned long align, vaddr_t *pgaddr);
cbuf_t        COS_STUB_DECL(memmgr_shared_page_allocn_aligned)(unsigned long num_pages, unsigned long align, vaddr_t *pgaddr);

unsigned long memmgr_shared_page_map(cbuf_t id, vaddr_t *pgaddr);
unsigned long COS_STUB_DECL(memmgr_shared_page_map)(cbuf_t id, vaddr_t *pgaddr);

unsigned long memmgr_shared_page_map_aligned(cbuf_t id, unsigned long align, vaddr_t *pgaddr);
unsigned long COS_STUB_DECL(memmgr_shared_page_map_aligned)(cbuf_t id, unsigned long align, vaddr_t *pgaddr);

unsigned long memmgr_shared_page_map_aligned_in_vm(cbuf_t id, unsigned long align, vaddr_t *pgaddr, compid_t cid);
unsigned long COS_STUB_DECL(memmgr_shared_page_map_aligned_in_vm)(cbuf_t id, unsigned long align, vaddr_t *pgaddr, compid_t cid);

/*
 * Unmap our mapping of shared memory id at addr (the address returned
 * by memmgr_shared_page_alloc* or memmgr_shared_page_map*). Each
 * allocation and each map creates one mapping; once the last mapping
 * of id is unmapped, the memory is freed and id becomes invalid.
 * Returns 0 on success, -EINVAL if we have no such mapping.
 */
int           memmgr_shared_page_unmap(cbuf_t id, vaddr_t addr);
int           COS_STUB_DECL(memmgr_shared_page_unmap)(cbuf_t id, vaddr_t addr);

#endif /* MEMMGR_H */
