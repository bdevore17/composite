/*
 * Start and stop components at runtime.
 *
 * Components marked `boot = "deferred"` in the composition script are
 * built, linked, and created in the system image like all others,
 * including the synchronous invocations from and to them, but they
 * are not initialized at boot. Their scheduler (the server of their
 * `init` dependency) implements this interface to start them, which
 * runs their initialization (`cos_init`, `parallel_init`) and then
 * their `main` (if any), and to stop them again. A component that
 * depends on the scheduler's `loader` interface can start and stop
 * all of the deferred components of that scheduler. That dependency
 * is the authorization: only components that declare it can invoke
 * these functions.
 *
 * Invocations of a deferred component's functions by its clients
 * fail with -ENOENT (returned as the function's first return value;
 * any other return values are undefined) while it isn't running.
 *
 * See the `loader` section of sched.pfprr_quantum_static's doc.md for
 * the details and limitations.
 */

#ifndef LOADER_H
#define LOADER_H

#include <cos_component.h>

/**
 * Start deferred component `id`: reset its memory image, enable
 * invocations to it, and create its initialization thread on the
 * current core, at the priority of the calling thread. Returns once
 * the component has completed its initialization (when its `main`,
 * if any, starts executing).
 *
 * Returns 0 on success, -EINVAL if `id` isn't a deferred component of
 * this scheduler (or is the caller), -EBUSY if it is already running
 * (or being started or stopped), -ENOMEM if its thread can't be
 * created, or another error from the capability manager.
 */
int loader_comp_start(compid_t id);
int COS_STUB_DECL(loader_comp_start)(compid_t id);

/**
 * Stop component `id`, started with `loader_comp_start`: disable
 * invocations to it, then delete its threads and release its memory,
 * so that it can be started again.
 *
 * The component can only be stopped if all of its threads have
 * exited (e.g. its `main` returned) or are blocked in the scheduler
 * (`sched_thd_block`, not on a block point such as a lock), and are
 * on the current core. Otherwise -EBUSY (or -EINVAL for other cores)
 * is returned, and nothing is changed. The caller must ensure that no
 * thread of another component is executing in (or blocked in) the
 * component: this cannot be detected.
 *
 * Returns 0 on success, -EINVAL if `id` isn't a deferred component of
 * this scheduler, -EBUSY if it isn't running or can't be stopped (see
 * above).
 */
int loader_comp_stop(compid_t id);
int COS_STUB_DECL(loader_comp_stop)(compid_t id);

/**
 * The id of the deferred component named `name` (its name in the
 * composition script) that we can start, or 0. Library function
 * using our initargs.
 */
compid_t loader_comp_id(const char *name);

#endif /* LOADER_H */
