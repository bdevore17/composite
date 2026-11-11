## capmgr.simple

A capability manager that focuses on simplicity.

### Description

The main area of simplification for this component is in statically limiting the number of delegations for each resource.
We can allocate a maximum, predefined, number of resources, and share them a maximum number of times.
Revocation will remove up to that many shared references.
This results in an implementation that is more amenable to analysis, but might be too limiting for larger systems.
It is somewhat surprising how far you can get with maximum three delegations, however in an RTOS.
The intuition here is that if we are mainly using channels for communication, they are most often one-to-one (SPSC), thus require sharing only between the channel manager, and two clients.

### Usage and Assumptions

- Assumes that the capability image upon initialization includes all resources that we'd have as if we were boot up on directly on the kernel.
    This includes resource table references to ourselves.
- Also assumes that `initargs` have been set up by the `composer` to tell us where the capabilities are that correspond to our clients.
- Assumes a maximum number of resources, and delegations for those resources.

### Resource reclamation

**Memory.**
Every page given to a client is also mapped in the capmgr; clients get an alias of that mapping.
Freeing (`memmgr_heap_page_free[n]`, or the last `memmgr_shared_page_unmap` of shared memory) unmaps the client's mapping (`cos_mem_remove`), while the capmgr's own mapping keeps the frame: the free pool is a list of runs of capmgr addresses.
A freed frame can still be reached through stale TLB entries of the previous owner until every core has flushed its TLB.
So each free records the time after its unmaps, starts a TLB shootdown (`cos_hw_tlbflush`: local flush plus an IPI to all cores), and the frames wait on a pending FIFO until the kernel reports that all cores flushed after that time (`cos_hw_tlb_quiescent`).
Only then are they reused (zeroed) for new allocations.
The freed client address ranges are kept per component and reused under the same condition (which the kernel also enforces when mapping).
Multi-page allocations take a single run of frames that is contiguous in the capmgr, thus physically contiguous.
When there is no fresh memory left, allocations wait for the pending frames.

**Capabilities and threads.**
The capmgr registers reclamation rings with `cos_kernel_api` for its own capability table, each client's, and its kernel memory, so that released capability slots and kernel pages are reused after the kernel quiescence period.
`capmgr_thd_release` and `capmgr_rcv_release` remove the aliases in the client first, then the capmgr's own capabilities (asnd, then arcv, then thread), which releases the thread's kernel memory and thread id.
The kernel can't free tcaps, so a released rcv's tcap (and its alias in the client) is pooled per component and core (up to `CM_TCAP_POOL_SZ`), and reused by the component's next rcv.

**Limits.**
Run descriptors (`MM_NRUNS`) and the reclamation rings are statically sized; if they overflow, the freed resource is leaked rather than reused (counted in `mm_stats.leaked` / the ring's `dropped`).
`contigmem_alloc` memory and memory mapped into VMs with `memmgr_shared_page_map_aligned_in_vm` (no unmap for VMs) are not reclaimed.
The capmgr's locks are spinlocks (see the warning in the code).

### Component teardown (not implemented)

Tearing down a component (e.g. `capmgr_comp_terminate(compid)`, for crash recovery or unloading) is not implemented.
What it needs, in order:

1. *Stop execution in it.*
    Its scheduler must delete all threads that *begin* in the component (`sched_thd_delete`), and the rcvs/asnds created for it (`capmgr_rcv_release`).
    Threads of other components that are currently *inside* the component through synchronous invocations are the hard part: their invocation stacks reference the component's `comp_info` (captbl/pgtbl).
    They must be unwound (made to return an error to their caller) before the component's tables are freed, which needs kernel support to inspect and pop invocation stack frames of a non-current thread (e.g. a `CAPTBL_OP_THDINVSTK_UNWIND(compcap)`), and a way to make such threads fault/return if they are executing.
2. *Revoke the paths into it.*
    Deactivate the sinv capabilities that clients hold to it (`CAPTBL_OP_SINVDEACTIVATE` in each client's captbl; the crt tracks them in `crt_comp->sinvs`, but the capmgr only knows the sinvs it created, not those set up by the booter, so the composer's `initargs` must describe them).
    Shared memory: unmap all its mappings (heap pages via the page metadata, shared spans via their mapping slots; spans whose last mapping it held are freed).
3. *Free its resource tables.*
    Its capability table and page table are kernel objects of several levels: each internal node must be frozen (`CAPTBL_OP_CAPKMEM_FREEZE`), wait for quiescence (TLB quiescence for the top-level page table, kernel quiescence otherwise), and be deactivated bottom-up with `CAPTBL_OP_{CAP,PG}TBLDEACTIVATE(_ROOT)`, whose kernel paths (`kmem_deact_pre`/`chal_pgtbl_deact_pre`) exist but have never been exercised on x86_64 (e.g. `chal_pgtbl_mapping_scan` only handles 32-bit entries, and `kmem_deact_pre` checks `!p_pte` instead of `!*p_pte`).
    The capmgr does not keep the capabilities to the internal nodes it created with `__bump_mem_expand_intern`/`cos_captbl_alloc`, so `cos_compinfo` must record them.
    Finally deactivate the component capability (`CAPTBL_OP_COMPDEACTIVATE`) and the capmgr's own copies, and free the `cm_comp`.
4. *Image memory.*
    The component's image (code/data) pages were mapped by the booter, not the capmgr; they can only be reclaimed if the booter delegates them (or the capmgr loads components itself).
