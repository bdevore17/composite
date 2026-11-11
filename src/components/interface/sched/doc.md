## sched

Scheduler interface that enables thread creation, synchronization, and timing.

### Description

Components that implement this provide scheduling services, and implement some scheduling policy.
Most schedulers are going to depend on the `sl` and `sl_capmgr` libraries.

### Usage and Assumptions

- It is quite common for components to depend on both `sched` and `init` if they want the scheduler to initialize them, and to schedule them.
- Most scheduler implementations will also require you depend on the `sl` libraries, and the current software abstractions depend on a `capmgr` for thread creation.

### Thread deletion

`sched_thd_delete(tid)` deletes a thread that the caller created with `sched_thd_create*`, and releases its resources (scheduler state, and through the `capmgr`, the kernel thread, its capabilities, and its thread id).
It can delete a thread that is blocked, preempted, or that called `sched_thd_exit` (exited threads are not freed until they are deleted).
It must be called on the thread's core, and a thread cannot delete itself.
