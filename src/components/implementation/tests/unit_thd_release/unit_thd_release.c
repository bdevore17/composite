/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Thread deletion through the scheduler (sched_thd_delete) and the
 * capmgr (capmgr_thd_release). More threads are created than there
 * are thread ids (MAX_NUM_THREADS), so ids, capabilities, and kernel
 * memory must be reclaimed and reused.
 */

#include <cos_component.h>
#include <cos_thd_init.h>
#include <llprint.h>
#include <sched.h>
#include <capmgr.h>
#include <cos_time.h>

#define NITER (3 * MAX_NUM_THREADS)
#define SL_FPRR_NPRIOS 32
#define HIGH_PRIORITY (SL_FPRR_NPRIOS - 1 - 10)

static int failures = 0;
static volatile unsigned long ran = 0;

#define TEST_FAIL(...)                           \
	do {                                     \
		printc("FAILURE: " __VA_ARGS__); \
		failures++;                      \
		return;                          \
	} while (0)

static void
thd_fn(void *d)
{
	unsigned long i = (unsigned long)d;

	ran++;
	/* Threads are deleted while blocked, or after they exited */
	if (i % 2) sched_thd_exit();
	while (1) sched_thd_block(0);
}

static void
test_sched_thd_delete(void)
{
	unsigned long i, j;
	thdid_t tid, max_tid = 0;
	int ret;

	for (i = 0; i < NITER; i++) {
		tid = sched_thd_create(thd_fn, (void *)i);
		if (!tid) TEST_FAIL("sched delete: creation %lu\n", i);
		if (tid > max_tid) max_tid = tid;
		sched_thd_param_set(tid, sched_param_pack(SCHEDP_PRIO, HIGH_PRIORITY));
		/* Let it run until it blocks or exits */
		for (j = 0; ran != i + 1 && j < 1000; j++) {
			sched_thd_block_timeout(0, time_now() + time_usec2cyc(100));
		}
		if (ran != i + 1) TEST_FAIL("sched delete: thread %lu didn't run\n", i);

		if (i == 0) {
			/* Only the scheduler, which has the thread, can release it */
			if (capmgr_thd_release(tid) != -EPERM) TEST_FAIL("sched delete: release by non-scheduler\n");
		}
		if ((ret = sched_thd_delete(tid))) TEST_FAIL("sched delete: delete %lu returned %d\n", i, ret);
		if (sched_thd_delete(tid) == 0) TEST_FAIL("sched delete: double delete %lu\n", i);
	}
	if (sched_thd_delete(cos_thdid()) == 0) TEST_FAIL("sched delete: deleted ourself\n");
	printc("SUCCESS: %d threads created, run, and deleted (max tid %lu)\n", NITER, (unsigned long)max_tid);
}

/* Threads created by the capmgr in this component, never run */
static void
test_capmgr_thd_release(void)
{
	thdclosure_index_t idx;
	unsigned long i;
	thdcap_t cap;
	thdid_t tid;
	int ret;

	/* The threads never run, so they never consume the closure */
	idx = cos_thd_init_alloc(thd_fn, NULL);
	if (idx < 1) TEST_FAIL("capmgr release: closure\n");
	for (i = 0; i < NITER; i++) {
		cap = capmgr_thd_create_thunk(idx, &tid);
		if (!cap) TEST_FAIL("capmgr release: creation %lu\n", i);
		if ((ret = capmgr_thd_release(tid))) TEST_FAIL("capmgr release: release %lu returned %d\n", i, ret);
		if (capmgr_thd_release(tid) != -EINVAL) TEST_FAIL("capmgr release: double release %lu\n", i);
	}
	if (capmgr_thd_release(cos_thdid()) == 0) TEST_FAIL("capmgr release: released a boot thread\n");
	cos_thd_init_free(idx);
	printc("SUCCESS: %d threads created and released through the capmgr\n", NITER);
}

int
main(void)
{
	printc("Thread release tests\n");
	test_sched_thd_delete();
	test_capmgr_thd_release();
	printc("%s\n", failures ? "TEST FAILED" : "TEST PASSED");

	while (1) sched_thd_block(0);

	return 0;
}
