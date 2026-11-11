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

### Component recovery ("micro-reboot")

When a thread faults in a component that the capmgr can reset (the *server*, S), the system keeps running: S is reset to its initial state and re-initialized, and the invocations of S that were in progress return `-ECOMPRESET` to their callers.
The kernel mechanisms are described in `src/kernel/include/recovery.h`; the capmgr's part is in `capmgr.c` (`cm_recovery_init`, `cm_comp_fault`) and `recovery_entry.S`.

**Which components.**
At initialization, the capmgr registers with the kernel (`cos_comp_recovery_register`) the components it manages that are *passive servers*: not schedulers, not in a shared address space (user-level invocations), and without `main`, `parallel_main`, or `cos_parallel_init` (it asks each component with the `COS_UPCALL_RECOVER_QUERY` upcall, which touches no state).
A passive server only executes in the invocations of its clients (and, once, in its `cos_init`).
Faults in other components stop the system, as before.

**Fault.**
A fault in S, in an invocation (not in a thread's own component), makes the kernel *suspend* S: invocations of S fail with `-ECOMPRESET` from now on, and no thread can return into S.
The faulting thread is redirected to `cm_comp_fault`, as an invocation of the capmgr made by S, on a stack reserved for S (`cm_recovery_stacks`): S's re-initialization runs on this thread, and can invoke the capmgr (e.g. the memmgr) on the thread's normal stack.
The recovery thus executes on the faulting thread, at its priority.

**Reset.**
`cm_comp_fault`:

1. Refuses to reset S if it got resources that a reset can't reclaim: memory other than heap pages (shared memory, contiguous or device memory, shared kernel pages), rcv end-points, or threads other than its initialization threads (closure 0).
    These are recorded when they are given to S, and never cleared.
2. Stops all execution in S (`cos_comp_recovery_quiesce`/`_quiescent`): each core unwinds its current thread if it executes in S, and counts the threads that execute in other components on behalf of S (invocations that S made, e.g. a thread blocked in the scheduler).
    Those are unwound when they return to S, so the capmgr waits for them (up to `CM_RECOVERY_DRAIN_USEC`), and refuses the reset if some never return: they could wait for an event that only S would have triggered.
3. Releases all of S's heap pages (`mm_heap_release_all`, as `memmgr_heap_page_freen`, by runs of contiguous pages so that they are reused for the same allocations).
4. Has the booter reset S's writable memory (data and bss, which include the static stacks) from S's ELF image, and restore the user-level capabilities of S's invocations, which live in its data (`addr_get(id, ADDR_IMAGE_RESET)`, `crt_comp_image_reset`).
5. Re-runs S's initialization (`COS_UPCALL_RECOVER_INIT`, `cos_comp_upcall`): the libc initialization and constructors, then `cos_init`, on the faulting thread.
    If it faults, steps 3-5 are retried (up to `CM_RECOVERY_INIT_TRIES` times).
6. Resumes S and returns from the fault handler in one kernel operation (`cos_comp_recovery_resume`), so that S's handler stack is free before S can fault again.
    The faulting thread returns `-ECOMPRESET` to the caller of the invocation that faulted.

When the reset is refused, S stays suspended forever: its invocations fail with `-ECOMPRESET`, and the rest of the system keeps running.
`capmgr_comp_resets(id)` reports the number of resets of a component (or `-ECOMPRESET` once it failed).
A reset takes 10-40 ms in QEMU without KVM (most of it releasing, re-mapping, and zeroing the server's heap).

**Invocations during a reset** fail fast with `-ECOMPRESET` (the kernel cannot block them).
The client can retry (`COS_INV_RETRY` in `cos_recovery.h`); note that a client that retries with a higher priority than the recovering thread, on the same core, delays the recovery.

**What is not reset, and limits.**

- The initialization threads of S, created at boot by its scheduler, exited after initialization (they are blocked in the scheduler, and never run again); they stay allocated, and can't run in the new S (the kernel refuses to switch to a thread whose own component was reset).
    Recovery creates no thread, capability, or kernel memory, so repeated recoveries leak nothing (tested with hundreds of resets).
- The scheduler's state about S (its initialization state) is unchanged: S is not re-initialized through the `init` interface, and its `init_done` is not called again.
- State that S's clients or servers keep about S (e.g. resources that S allocated in other components, such as locks in the scheduler) is not reclaimed.
- Schedulers, capmgrs, the booter, components in shared address spaces, and components with a `main` (they have threads of their own) can't be recovered.
- Only one capmgr is supported, as for the other capmgr features.

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
