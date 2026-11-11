## memmgr

The memory manager API: heap memory, and shared memory between components.

### Description

Memory is allocated in pages, mapped into the caller's address space.

- `memmgr_heap_page_alloc[n][_aligned]` allocate private (heap) pages. Multi-page allocations are virtually and physically contiguous.
- `memmgr_heap_page_free[n](addr[, n])` free heap pages: the pages are unmapped from the caller, and their memory is reused (zeroed) for later allocations.
    A range can be part of an allocation.
    Freeing is all or nothing: if any page in the range isn't one of the caller's heap pages, `-EINVAL` is returned and nothing is freed.
- `memmgr_shared_page_alloc[n][_aligned]` allocate shared memory, returning an id (`cbuf_t`) and the address of the caller's mapping.
    Other components map it with `memmgr_shared_page_map[_aligned](id, &addr)`.
- `memmgr_shared_page_unmap(id, addr)` removes the caller's mapping of `id` at `addr` (as returned by the allocation or map).
    Shared memory is reference counted by its mappings: the allocation and each map create one mapping, and the memory is freed when the last one is unmapped, at which point `id` becomes invalid (maps of it fail).
- `memmgr_virt_to_phys` and `memmgr_map_phys_to_virt` translate addresses and map device memory.

### Usage and Assumptions

- After a free or unmap, the addresses are no longer valid in the caller: accesses fault.
- Freed memory (and the freed virtual addresses) is only reused after all cores have flushed their TLBs, so that no stale translation can reach it (see `capmgr.simple`).
- Memory allocated through `contigmem` can be unmapped with `memmgr_shared_page_unmap` (shared allocations), but contiguous heap allocations (`contigmem_alloc`) cannot be freed.
- The number of mappings of each shared page is limited by the implementation (`capmgr.simple`: 5, including the allocation).
