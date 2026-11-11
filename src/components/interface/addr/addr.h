/*
 * This API is really meant to only be used by capmgrs. It allows the
 * capmgr to get key addresses within another component they
 * oversee. For the time being, this is limited to the heap pointer
 * and capability frontier.
 *
 * It also lets the capmgr load and unload the components that are
 * started at runtime (`boot = "deferred"` in the composition script),
 * whose memory images and invocations are kept by the booter.
 */

#ifndef ADDR_H
#define ADDR_H

typedef enum {
	ADDR_HEAP_FRONTIER,
	ADDR_CAPTBL_FRONTIER,
} addr_t;

unsigned long addr_get(compid_t id, addr_t type);

/**
 * Load deferred component `id`: reset its memory image to its
 * initial state (from its elf object), restore the client side
 * (ucaps) of its own invocations, and activate its clients'
 * invocations to it. The booter creates deferred components at boot
 * like all others, but redirects the invocations to them to
 * `addr_comp_absent` until they are loaded. The component must not
 * be executing (it was never started, or was stopped).
 *
 * Only capmgrs can call this; they serialize the calls for a
 * component (see capmgr_comp_start).
 *
 * Returns 0 on success, -EPERM if the caller isn't a capmgr, -EINVAL
 * if `id` isn't a deferred component, -EBUSY if it is loaded.
 */
int addr_comp_load(compid_t id);

/**
 * Unload deferred component `id`: redirect its clients' invocations
 * to it to `addr_comp_absent`, so that they fail (with -ENOENT). Its
 * memory is reset by the next `addr_comp_load`.
 *
 * Returns 0 on success, -EPERM if the caller isn't a capmgr, -EINVAL
 * if `id` isn't a deferred component, -EBUSY if it isn't loaded.
 */
int addr_comp_unload(compid_t id);

/**
 * The function that the invocations of a deferred component that
 * isn't loaded execute instead: they return -ENOENT (as the function's
 * first return value, a full word; other return values are
 * undefined). Not meant to be called directly.
 */
long addr_comp_absent(void);

#endif /* ADDR_H */
