/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Component recovery: a server (tests.unit_faultsrv) faults, the
 * capmgr resets it, and its clients get -ECOMPRESET for the
 * invocations that were in progress, and correct results otherwise.
 *
 * 1. Faults on every FAULTSRV_PERIOD-th call: errors exactly for
 *    those, and the server's state (its call counter) restarts.
 * 2. All fault types.
 * 3. Hundreds of resets: each re-initialization allocates 8 MiB of
 *    heap, more in total than the capmgr has, so the server's memory
 *    must be released and reused, at the same addresses.
 * 4. Faults while clients on all other cores are inside the server.
 * 5. A server that can't be reset (it has shared memory) remains
 *    unavailable after its fault, while the system keeps running.
 */

#include <cos_component.h>
#include <cos_recovery.h>
#include <llprint.h>
#include <barrier.h>
#include <ps.h>
#include <capmgr.h>
#include <faultsrv.h>

#define NPERIODS 8
#define NCHURN 200
#define NCONC 16
#define SPIN_LOOP 200000

static int      failures = 0;
static compid_t srv;

#define TEST_FAIL(...)                           \
	do {                                     \
		printc("FAILURE: " __VA_ARGS__); \
		failures++;                      \
		return;                          \
	} while (0)

/* The resets done since the last call */
static int
resets_since(int *last)
{
	int r = capmgr_comp_resets(srv), d;

	if (r < 0) return r;
	d     = r - *last;
	*last = r;

	return d;
}

static void
test_period(void)
{
	int  i, resets = capmgr_comp_resets(srv);
	long r, expected;

	for (i = 0; i < NPERIODS * FAULTSRV_PERIOD; i++) {
		r        = faultsrv_call();
		expected = (i % FAULTSRV_PERIOD == FAULTSRV_PERIOD - 1) ? -ECOMPRESET : i % FAULTSRV_PERIOD + 1;
		if (r != expected) TEST_FAIL("period: call %d returned %ld instead of %ld\n", i, r, expected);
	}
	if ((r = resets_since(&resets)) != NPERIODS) TEST_FAIL("period: %ld resets instead of %d\n", r, NPERIODS);
	printc("SUCCESS: %d calls, -ECOMPRESET exactly for the %d faulting ones, state reset after each\n",
	       NPERIODS * FAULTSRV_PERIOD, NPERIODS);
}

static void
test_fault_types(void)
{
	int  t, resets = capmgr_comp_resets(srv);
	long r;

	for (t = 0; t < FAULTSRV_NTYPES; t++) {
		if ((r = faultsrv_call()) != 1 && r != 2)
			TEST_FAIL("fault types: call before fault %d returned %ld\n", t, r);
		if ((r = faultsrv_fault(t)) != -ECOMPRESET) TEST_FAIL("fault types: fault %d returned %ld\n", t, r);
		if ((r = faultsrv_call()) != 1) TEST_FAIL("fault types: state not reset after fault %d (%ld)\n", t, r);
		if ((r = resets_since(&resets)) != 1) TEST_FAIL("fault types: %ld resets for fault %d\n", r, t);
	}
	printc("SUCCESS: recovered from %d fault types (page faults, divide error, invalid opcode, protection fault)\n",
	       FAULTSRV_NTYPES);
}

static void
test_churn(void)
{
	int  i, resets = capmgr_comp_resets(srv);
	long r, heap, min = 0, max = 0;

	for (i = 0; i < NCHURN; i++) {
		if ((r = faultsrv_fault(i % FAULTSRV_NTYPES)) != -ECOMPRESET)
			TEST_FAIL("churn: fault %d returned %ld\n", i, r);
		heap = faultsrv_heap();
		if (heap == -1) TEST_FAIL("churn: the heap of reset %d wasn't zeroed\n", i);
		if (heap <= 0) TEST_FAIL("churn: no heap after reset %d (%ld)\n", i, heap);
		if (i == 0 || heap < min) min = heap;
		if (i == 0 || heap > max) max = heap;
		if ((r = faultsrv_call()) != 1) TEST_FAIL("churn: state not reset after reset %d (%ld)\n", i, r);
	}
	if ((r = resets_since(&resets)) != NCHURN) TEST_FAIL("churn: %ld resets instead of %d\n", r, NCHURN);
	/* Freed addresses are reused (after TLB quiescence): a few heap locations only */
	if (max - min > 8 * 8 * 1024 * 1024) TEST_FAIL("churn: heap addresses not reused (0x%lx - 0x%lx)\n", min, max);
	printc("SUCCESS: %d resets, %d MiB of heap allocated by re-initializations (memory reused, addresses 0x%lx - "
	       "0x%lx)\n",
	       NCHURN, NCHURN * 8, min, max);
}

static struct simple_barrier barrier_start = SIMPLE_BARRIER_INITVAL, barrier_conc = SIMPLE_BARRIER_INITVAL,
                             barrier_end = SIMPLE_BARRIER_INITVAL;
static volatile int  conc_done;
static unsigned long conc_ok, conc_reset, conc_bad, conc_unwound;

/* Clients on the other cores: invocations that last, and are inside the server when it faults */
static void
conc_client(coreid_t cid)
{
	unsigned long ok = 0, reset = 0, bad = 0, unwound = 0;
	cycles_t      s, e, min_ok = ~0ULL;
	long          i, r;
	volatile int  d;

	for (i = 0; !conc_done; i++) {
		rdtscll(s);
		r = faultsrv_spin(i, SPIN_LOOP);
		rdtscll(e);
		if (r == 2 * i + 1) {
			ok++;
			if (e - s < min_ok) min_ok = e - s;
		} else if (r == -ECOMPRESET) {
			reset++;
			/*
			 * An invocation that failed after most of a
			 * successful one's duration was inside the
			 * server, and was unwound (instead of failing
			 * fast as the server was being reset).
			 */
			if (min_ok != ~0ULL && e - s > min_ok / 2) unwound++;
			for (d = 0; d < 1000; d++);
		} else {
			bad++;
			printc("FAILURE: concurrent: core %u got %ld for %ld\n", cid, r, i);
		}
	}
	ps_faa(&conc_ok, ok);
	ps_faa(&conc_reset, reset);
	ps_faa(&conc_bad, bad);
	ps_faa(&conc_unwound, unwound);
}

static void
test_concurrent(void)
{
	int           i, resets = capmgr_comp_resets(srv);
	long          r, inside, inside_faults = 0;
	unsigned long w;

	for (i = 0; i < NCONC; i++) {
		/* Fault while other clients are inside the server */
		for (w = 0; (inside = faultsrv_inside()) <= 0 && w < 100000000; w++);
		if (inside > 0) inside_faults++;
		if ((r = faultsrv_fault(FAULTSRV_NULL_READ)) != -ECOMPRESET) {
			printc("FAILURE: concurrent: fault %d returned %ld\n", i, r);
			failures++;
		}
	}
	conc_done = 1;
	simple_barrier(&barrier_conc);

	if (conc_bad) TEST_FAIL("concurrent: %lu wrong results\n", conc_bad);
	if ((r = resets_since(&resets)) != NCONC) TEST_FAIL("concurrent: %ld resets instead of %d\n", r, NCONC);
	if (inside_faults == 0) TEST_FAIL("concurrent: no fault with clients inside the server\n");
	if (conc_unwound == 0) TEST_FAIL("concurrent: no client invocation was unwound from inside the server\n");
	if (conc_ok == 0) TEST_FAIL("concurrent: no client invocation succeeded\n");
	if ((r = faultsrv_spin(1, 0)) != 3) TEST_FAIL("concurrent: server broken after the test (%ld)\n", r);
	printc("SUCCESS: %d faults (%ld with other clients inside the server) on %d cores: %lu correct results, "
	       "%lu -ECOMPRESET (%lu unwound from inside the server)\n",
	       NCONC, inside_faults, NUM_CPU, conc_ok, conc_reset, conc_unwound);
}

static void
test_unrecoverable(void)
{
	long r;

	if ((r = faultsrv_unrecoverable()) != 0) TEST_FAIL("unrecoverable: shared memory allocation (%ld)\n", r);
	if ((r = faultsrv_fault(FAULTSRV_NULL_READ)) != -ECOMPRESET)
		TEST_FAIL("unrecoverable: fault returned %ld\n", r);
	if ((r = capmgr_comp_resets(srv)) != -ECOMPRESET) TEST_FAIL("unrecoverable: server not failed (%ld)\n", r);
	if ((r = COS_INV_RETRY(3, faultsrv_call())) != -ECOMPRESET)
		TEST_FAIL("unrecoverable: server available (%ld)\n", r);
	printc("SUCCESS: a server with shared memory isn't reset, and remains unavailable\n");
}

void
parallel_main(coreid_t cid, int init_core, int ncores)
{
	if (init_core) {
		long r;

		printc("Component recovery tests (server %ld).\n", faultsrv_id());
		srv = faultsrv_id();
		if ((r = capmgr_comp_resets(srv)) != 0) {
			printc("FAILURE: the server isn't recoverable (%ld)\n", r);
			failures++;
		} else {
			test_period();
			test_fault_types();
			test_churn();
		}
	}
	simple_barrier(&barrier_start);

	if (init_core)
		test_concurrent();
	else
		conc_client(cid);
	if (!init_core) simple_barrier(&barrier_conc);
	simple_barrier(&barrier_end);

	if (init_core) {
		test_unrecoverable();
		if (failures)
			printc("TEST FAILED (%d failures)\n", failures);
		else
			printc("TEST PASSED\n");
	}
}
