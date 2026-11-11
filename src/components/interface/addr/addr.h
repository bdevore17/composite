/*
 * This API is really meant to only be used by capmgrs. It allows the
 * capmgr to get key addresses within another component they
 * oversee. For the time being, this is limited to the heap pointer
 * and capability frontier. The constructor (booter) also resets a
 * component's image for component recovery (ADDR_IMAGE_RESET).
 */

#ifndef ADDR_H
#define ADDR_H

typedef enum {
	ADDR_HEAP_FRONTIER,
	ADDR_CAPTBL_FRONTIER,
	/*
	 * Not an address: component recovery. Reset the writable
	 * memory (data and bss) of component `id` to its initial
	 * state, from its image, including the user-level
	 * capabilities of its synchronous invocations. Only while `id`
	 * cannot execute (it is suspended, see the capmgr's
	 * documentation). Returns 0 on success, and (unsigned long)-EINVAL
	 * if the component can't be reset (e.g. it isn't in its own
	 * address space).
	 *
	 * This is not a separate function because each function of
	 * this interface is a synchronous invocation in the capmgr's
	 * capability table, allocated by the booter, and the capmgr's
	 * capability frontier must stay clear of the end of the
	 * capability table's second-level node (see
	 * __capid_captbl_check_expand).
	 */
	ADDR_IMAGE_RESET,
} addr_t;

unsigned long addr_get(compid_t id, addr_t type);

#endif /* ADDR_H */
