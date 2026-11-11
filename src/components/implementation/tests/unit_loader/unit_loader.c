/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Starting and stopping components at runtime (the `loader` interface
 * of the scheduler) with two deferred components:
 *
 * - loadsrv (tests.unit_loader_server), a server: its clients'
 *   invocations fail before it is started, work once it is, and fail
 *   again once it is stopped. It is then started and stopped hundreds
 *   of times. Each time, it allocates more memory, and creates more
 *   threads, than are available in total if they weren't released
 *   when it is stopped, and checks that its memory image was reset.
 * - loadmain (tests.unit_loader_main), with a main that runs, and
 *   finishes, each time it is started.
 */

#include <cos_component.h>
#include <llprint.h>
#include <sched.h>
#include <loader.h>
#include <loadtest.h>
#include <memmgr.h>

#define NCYCLES 500

static int failures = 0;

#define TEST_FAIL(...)                           \
	do {                                     \
		printc("FAILURE: " __VA_ARGS__); \
		failures++;                      \
		return;                          \
	} while (0)

static compid_t srv, mainc;

static void
test_lookup(void)
{
	srv   = loader_comp_id("loadsrv");
	mainc = loader_comp_id("loadmain");
	if (!srv || !mainc || srv == mainc) TEST_FAIL("lookup: ids %lu and %lu\n", srv, mainc);
	if (loader_comp_id("client") || loader_comp_id("sched"))
		TEST_FAIL("lookup: a boot-time component is loadable\n");
	printc("SUCCESS: deferred components loadsrv (%lu) and loadmain (%lu) found\n", srv, mainc);
}

static void
test_invalid(void)
{
	int ret;

	if ((ret = loader_comp_stop(srv)) != -EBUSY) TEST_FAIL("invalid: stop before start returned %d\n", ret);
	if ((ret = loader_comp_start(cos_compid())) != -EINVAL) TEST_FAIL("invalid: started ourself (%d)\n", ret);
	if ((ret = loader_comp_start(1)) != -EINVAL) TEST_FAIL("invalid: started the booter (%d)\n", ret);
	if ((ret = loader_comp_start(MAX_NUM_COMPS + 1)) != -EINVAL) TEST_FAIL("invalid: started a bad id (%d)\n", ret);
	if ((ret = loader_comp_stop(cos_compid())) != -EINVAL) TEST_FAIL("invalid: stopped ourself (%d)\n", ret);
	printc("SUCCESS: invalid starts and stops rejected\n");
}

/* (1) a deferred server: invocations fail until it is started, and again once it is stopped */
static void
test_server(void)
{
	long    r, shared;
	vaddr_t addr;
	int     ret;

	if ((r = loadtest_calls()) != -ENOENT) TEST_FAIL("server: invocation before start returned %ld\n", r);
	if ((ret = loader_comp_start(srv))) TEST_FAIL("server: start returned %d\n", ret);
	if ((ret = loader_comp_start(srv)) != -EBUSY) TEST_FAIL("server: second start returned %d\n", ret);
	if ((r = loadtest_calls()) != 1) TEST_FAIL("server: first invocation returned %ld\n", r);
	if ((r = loadtest_calls()) != 2) TEST_FAIL("server: second invocation returned %ld\n", r);
	if (loadtest_fresh() != 1) TEST_FAIL("server: initial image not fresh\n");
	if (!loadtest_heap()) TEST_FAIL("server: initialization failed\n");
	/* Shared memory that we still map is not released when the server is stopped */
	shared = loadtest_shared();
	if (!shared || memmgr_shared_page_map(shared, &addr) == 0)
		TEST_FAIL("server: could not map its shared memory\n");
	if (*(long *)addr != LOADTEST_SHARED_MAGIC) TEST_FAIL("server: shared memory content\n");
	if ((ret = loader_comp_stop(srv))) TEST_FAIL("server: stop returned %d\n", ret);
	if (*(long *)addr != LOADTEST_SHARED_MAGIC) TEST_FAIL("server: shared memory lost on stop\n");
	if ((ret = memmgr_shared_page_unmap(shared, addr))) TEST_FAIL("server: shared unmap returned %d\n", ret);
	if ((ret = loader_comp_stop(srv)) != -EBUSY) TEST_FAIL("server: second stop returned %d\n", ret);
	if ((r = loadtest_calls()) != -ENOENT) TEST_FAIL("server: invocation after stop returned %ld\n", r);
	printc("SUCCESS: deferred server: -ENOENT before start, invoked once started, -ENOENT once stopped, shared "
	       "memory kept\n");
}

/* (3) start/stop cycles: memory, threads, and capabilities are released */
static void
test_cycles(void)
{
	long heap0 = 0, heap, tid, max_tid = 0;
	int  i, ret;

	for (i = 0; i < NCYCLES; i++) {
		if ((ret = loader_comp_start(srv))) TEST_FAIL("cycles: start %d returned %d\n", i, ret);
		/* The image is reset each time: counters restart */
		if (loadtest_calls() != 1) TEST_FAIL("cycles: start %d: state not reset\n", i);
		if (loadtest_fresh() != 1) TEST_FAIL("cycles: start %d: image not fresh\n", i);
		heap = loadtest_heap();
		if (!heap) TEST_FAIL("cycles: start %d: initialization failed (memory or threads not released?)\n", i);
		/* Its addresses are reused: its address space doesn't grow */
		if (i == 0)
			heap0 = heap;
		else if (heap != heap0)
			TEST_FAIL("cycles: start %d: heap at 0x%lx, not 0x%lx\n", i, heap, heap0);
		tid = loadtest_init_tid();
		if (tid > max_tid) max_tid = tid;
		if ((ret = loader_comp_stop(srv))) TEST_FAIL("cycles: stop %d returned %d\n", i, ret);
		if (loadtest_calls() != -ENOENT) TEST_FAIL("cycles: stop %d: still invocable\n", i);
	}
	printc("SUCCESS: %d start/stop cycles of the server (%d MiB, %d threads created; max tid %ld)\n", NCYCLES,
	       NCYCLES * 4, NCYCLES * 2, max_tid);
}

/* (2) a deferred component with a main that runs, and finishes */
static void
test_main(void)
{
	long r;
	int  i, j, ret;

	if ((ret = loader_comp_start(srv))) TEST_FAIL("main: server start returned %d\n", ret);
	for (i = 0; i < 3; i++) {
		loadtest_report(0);
		if ((ret = loader_comp_start(mainc))) TEST_FAIL("main: start %d returned %d\n", i, ret);
		for (j = 0; loadtest_reported() != LOADTEST_MAIN_STARTED && j < 10000; j++) sched_thd_yield();
		if (loadtest_reported() != LOADTEST_MAIN_STARTED) TEST_FAIL("main: %d didn't start\n", i);
		/* Its main is runnable: it can't be stopped */
		if ((ret = loader_comp_stop(mainc)) != -EBUSY)
			TEST_FAIL("main: stop of a running main %d returned %d\n", i, ret);
		loadtest_report(LOADTEST_MAIN_GO);
		/* Once its main returned, it is stopped */
		for (j = 0; (ret = loader_comp_stop(mainc)) == -EBUSY && j < 10000; j++) sched_thd_yield();
		if (ret) TEST_FAIL("main: stop %d returned %d\n", i, ret);
		/* Its main executed once since it was (re)started */
		if ((r = loadtest_reported()) != LOADTEST_MAIN_DONE + 1) TEST_FAIL("main: %d reported %ld\n", i, r);
	}
	if ((ret = loader_comp_stop(srv))) TEST_FAIL("main: server stop returned %d\n", ret);
	printc("SUCCESS: deferred component's main ran and finished, 3 times\n");
}

int
main(void)
{
	printc("Runtime component loading tests\n");
	test_lookup();
	if (!failures) test_invalid();
	if (!failures) test_server();
	if (!failures) test_main();
	if (!failures) test_cycles();
	printc("%s\n", failures ? "TEST FAILED" : "TEST PASSED");

	while (1) sched_thd_block(0);

	return 0;
}
