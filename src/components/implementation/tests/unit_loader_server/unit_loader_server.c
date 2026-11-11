/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * A server that is started and stopped at runtime (`boot = "deferred"`)
 * by the unit_loader test. Each time it is started, its cos_init
 * checks that its memory image is in its initial state, allocates
 * memory (heap and shared), and creates a thread that blocks, all of
 * which must be released when it is stopped.
 */

#include <cos_component.h>
#include <llprint.h>
#include <sched.h>
#include <memmgr.h>
#include <loadtest.h>

/* Enough that the test exhausts memory if it isn't released on stop */
#define LOADTEST_HEAP_PAGES 1024
#define LOADTEST_SHARED_PAGES 4
#define HIGH_PRIORITY (32 - 1 - 10)

#define DATA_INIT 0x10ad5e7e

static long         data_marker = DATA_INIT; /* data: must be reset on each start */
static long         bss_marker;              /* bss: must be zero on each start */
static long         calls, fresh, reported;
static thdid_t      init_tid;
static vaddr_t      heap;
static cbuf_t       shared_id;
static volatile int blocked_ran;

static void
blocked_fn(void *d)
{
	blocked_ran = 1;
	/* Deleted, while blocked, when the server is stopped */
	while (1) sched_thd_block(0);
}

long
loadtest_calls(void)
{
	return ++calls;
}

long
loadtest_fresh(void)
{
	return fresh;
}

long
loadtest_init_tid(void)
{
	return init_tid;
}

long
loadtest_heap(void)
{
	return heap;
}

long
loadtest_shared(void)
{
	return shared_id;
}

long
loadtest_report(long v)
{
	reported = v;

	return 0;
}

long
loadtest_reported(void)
{
	return reported;
}

void
cos_init(void)
{
	vaddr_t       shared;
	thdid_t       tid;
	unsigned long i;

	fresh       = (data_marker == DATA_INIT && bss_marker == 0);
	data_marker = 0;
	bss_marker  = 1;
	init_tid    = cos_thdid();

	heap = memmgr_heap_page_allocn(LOADTEST_HEAP_PAGES);
	if (heap) {
		/* touch every page */
		for (i = 0; i < LOADTEST_HEAP_PAGES; i++) ((long *)(heap + i * PAGE_SIZE))[0] = i;
	}
	shared_id = memmgr_shared_page_allocn(LOADTEST_SHARED_PAGES, &shared);
	if (shared_id)
		*(long *)shared = LOADTEST_SHARED_MAGIC;
	else
		heap = 0;

	tid = sched_thd_create(blocked_fn, NULL);
	if (!tid) {
		heap = 0;
		return;
	}
	sched_thd_param_set(tid, sched_param_pack(SCHEDP_PRIO, HIGH_PRIORITY));
	while (!blocked_ran) sched_thd_yield();
}
