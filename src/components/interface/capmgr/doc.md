## capmgr

The capability manager API that provides dynamic resource allocation, delegation, and revocation.

### Description

A component that is an implementation of this interface is trusted to add and remove resources from the resource tables of components that depend on the `capmgr` interface.

### Usage and Assumptions

Any component that implements this, should also implement `capmgr_create` that enables even components that don't want to invoke any of the functions in this interface (as they are relatively static) to enable the `capmgr` to create threads within them (often in response to creation requests from a `sched`).

### Releasing resources

- `capmgr_thd_release(tid)` releases a thread created with `capmgr_thd_create*` (or `capmgr_initthd_create`).
    The caller must be the component that received the thread's capability: the scheduler that asked for the thread (`capmgr_thd_create_ext`, `capmgr_initthd_create`), or the component itself (`capmgr_thd_create`/`capmgr_thd_create_thunk`).
    The thread's capabilities, kernel memory, and thread id are released and reused.
- `capmgr_rcv_release(tid)` releases a receive end-point created with `capmgr_rcv_create` (identified by its thread's id), along with the asnd created with it.
- Both must be called on the core the thread was created on, and not by the thread being released.
    The caller is responsible for the thread not being in use: e.g. a scheduler first removes it from its run-queues (see `sched_thd_delete`).
- They return `0` on success, `-EINVAL` for an unknown thread or the wrong core, `-EPERM` if the caller doesn't hold the thread, `-EBUSY` for the calling thread, or an error from the kernel.
- Threads created at boot (the components' initial and scheduler threads) cannot be released.

### Component recovery

The capmgr resets components that fault, when it can (see the `capmgr.simple` documentation).
`capmgr_comp_resets(id)` returns how many times component `id` was reset, `-ECOMPRESET` if it faulted and could not be recovered (it remains unavailable: its invocations fail with `-ECOMPRESET`), or `-EINVAL` if it isn't recoverable (a fault in it stops the system).

Clients of a recoverable component see `-ECOMPRESET` (`cos_errno.h`) as the return value of the invocations that were executing in it when it faulted, and of the invocations made while it is being reset.
The component's state was lost: it restarted from its initial state.
`cos_recovery.h` (component library) has `cos_inv_reset(ret)` and `COS_INV_RETRY(tries, call)` to detect the error and retry.
