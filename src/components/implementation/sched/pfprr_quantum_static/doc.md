## sched.pfprr_quantum_static

A minimal scheduling libary implementation of a scheduler based on

- preemptive, fixed-priority, round-robin scheduling,
- periodic, quantum-based timers, and
- static memory allocation for the threads.

### Description

Export mainly the scheduling and blockpoint APIs as a general-purpose scheduler.

### Usage and Assumptions

See the scheduler API.
This assumes that the macros for the maximum number of threads and the quantum size are properly configured.
Does not yet support hierarchy.

### Thread deletion

`sched_thd_delete` removes the thread from the run-queue and timers (unless it already exited), frees its slab entry, and releases it in the capability manager (`capmgr_thd_release`).
A thread blocked on a scheduler block-point (e.g. a lock) must not be deleted: the block-point would keep a reference to it.

### Starting and stopping components at runtime (`loader`)

The scheduler implements the `loader` interface for the components that have it as their scheduler (their `init` server) and are marked `boot = "deferred"` in the composition script.
They are not part of the boot-time initialization schedule; the composer lists them in the `deferred` initargs.
A component can start and stop them if it depends on the scheduler's `loader` interface: that dependency (the invocation capabilities it creates) is the authorization.
`loader_comp_id(name)` (a library function of the interface) finds a deferred component's id from the `loadable` initargs the composer provides to such clients.

`loader_comp_start(id)`:

1. asks the capmgr to load the component (`capmgr_comp_start`): its memory image is reset from its elf object, and its clients' invocations of it work again;
2. creates its initialization thread, on the current core, at the priority of the calling thread;
3. yields until the component completes its initialization (`cos_init`, then `parallel_init` if any, on this core only): when it starts executing its `main` (or `parallel_main`, on this core only), or when its thread exits if it has none, and returns 0.

`loader_comp_stop(id)`:

1. deletes all of the threads that execute in the component (`sched_comp_thds_delete`), which requires that they are all on the current core, and have exited (e.g. its `main` returned) or are blocked in the scheduler; otherwise it fails with `-EINVAL` or `-EBUSY` and changes nothing (a runnable thread could have been preempted anywhere, e.g. in a critical section of another component);
2. asks the capmgr to stop the component (`capmgr_comp_stop`): its clients' invocations of it return `-ENOENT` again, and its memory is released.

While a deferred component is stopped (or stopping), it can't create threads (`sched_thd_create` returns 0).

Limitations:

- The scheduler cannot detect threads of *other* components that are executing (or blocked) in the component through invocations: the caller of `loader_comp_stop` must ensure there are none, e.g. by stopping the component only once its clients stopped using it. Such a thread would continue in the component's reset image.
- Threads blocked on a block point (e.g. a lock) must not exist when the component is stopped: the block point would keep a reference to them. Block points are never freed, so a component allocating them leaks them on each start.
- Deferred components only execute on the core that started them, and must be stopped on that core.
- `loader_comp_start` waits (yielding) for the initialization: if the component's `cos_init` blocks forever, so does the caller.
