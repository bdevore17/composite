/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Kernel-level tests of resource reclamation, run directly on the
 * kernel (this component is constructed by the kernel, and has all
 * resources): releasing threads and asynchronous end-points (with the
 * reuse of their capability slots, kernel memory, and thread ids),
 * and unmapping memory with TLB shootdown and quiescence.
 */

#include <cos_component.h>
#include <cos_kernel_api.h>
#include <cos_thd_init.h>
#include <llprint.h>
#include <ps.h>

static struct cos_compinfo      ci;
static struct cos_capid_reclaim capid_reclaim;
static struct cos_reclaim_ring  kmem_reclaim;

static int failures = 0;

/* More than there are thread ids, so ids and resources must be reused */
#define NITER (3 * MAX_NUM_THREADS)
#define HWCAP BOOT_CAPTBL_SELF_INITHW_BASE

#define TEST_FAIL(...)                    \
	do {                              \
		printc("FAILURE: " __VA_ARGS__); \
		failures++;               \
		return;                   \
	} while (0)

static volatile unsigned long thd_runs = 0;

static void
thd_fn(void *d)
{
	(void)d;
	while (1) {
		thd_runs++;
		cos_thd_switch(BOOT_CAPTBL_SELF_INITTHD_CPU_BASE);
	}
}

/*
 * Create, run, and deallocate threads NITER times. Thread ids must be
 * recycled (the kernel refuses ids >= MAX_NUM_THREADS), and the
 * capability slots and kernel memory reused.
 */
static void
test_thd_release(void)
{
	unsigned long i;
	vaddr_t kmem, kmem_ptr_start = 0, kmem_max = 0;
	thdid_t tid, tid_max = 0;
	thdcap_t t;
	int idx, ret;

	for (i = 0; i < NITER; i++) {
		idx = cos_thd_init_alloc(thd_fn, NULL);
		if (idx < 1) TEST_FAIL("thread release: closure allocation %lu\n", i);
		t = cos_thd_alloc_ext_kmem(&ci, ci.comp_cap, idx, &kmem);
		if (!t) TEST_FAIL("thread release: allocation %lu\n", i);
		tid = cos_introspect(&ci, t, THD_GET_TID);
		if (tid > tid_max) tid_max = tid;
		if (i == 0) kmem_ptr_start = ci.mi.kmem_ptr;

		if (cos_thd_switch(t)) TEST_FAIL("thread release: switch %lu\n", i);
		if (thd_runs != i + 1) TEST_FAIL("thread release: thread %lu didn't run\n", i);

		ret = cos_thd_dealloc(&ci, t, kmem, tid);
		if (ret) TEST_FAIL("thread release: dealloc %lu returned %d\n", i, ret);
		/* The capability is gone (it cannot be reused before quiescence) */
		if (cos_introspect(&ci, t, THD_GET_TID) >= 0) TEST_FAIL("thread release: freed thread %lu still accessible\n", i);
		cos_thd_init_free(idx);
	}
	kmem_max = ci.mi.kmem_ptr - kmem_ptr_start;
	printc("SUCCESS: %d threads created, run, and released (max tid %lu, %lu new kernel pages)\n", NITER,
	       (unsigned long)tid_max, (unsigned long)(kmem_max / PAGE_SIZE));
	/* Without reuse, every thread would need a new kernel page */
	if (kmem_max / PAGE_SIZE >= NITER / 2) TEST_FAIL("thread release: kernel memory not reused\n");
	printc("SUCCESS: thread kernel memory reused\n");
}

/*
 * Create a rcv end-point (thread + arcv) and an asnd to it, send an
 * event, and release them all, NITER times. The kernel cannot free
 * tcaps, so one tcap is reused for all end-points.
 */
static void
test_endpoint_release(void)
{
	unsigned long i;
	vaddr_t kmem;
	thdid_t tid;
	thdcap_t t;
	arcvcap_t r;
	asndcap_t s;
	tcap_t tc;
	int idx, ret;
	capid_t frontier = 0;

	tc = cos_tcap_alloc(&ci);
	if (!tc) TEST_FAIL("endpoint release: tcap allocation\n");

	for (i = 0; i < NITER; i++) {
		idx = cos_thd_init_alloc(thd_fn, NULL);
		if (idx < 1) TEST_FAIL("endpoint release: closure allocation %lu\n", i);
		t = cos_thd_alloc_ext_kmem(&ci, ci.comp_cap, idx, &kmem);
		if (!t) TEST_FAIL("endpoint release: thread allocation %lu\n", i);
		tid = cos_introspect(&ci, t, THD_GET_TID);
		r = cos_arcv_alloc(&ci, t, tc, ci.comp_cap, BOOT_CAPTBL_SELF_INITRCV_CPU_BASE);
		if (!r) TEST_FAIL("endpoint release: arcv allocation %lu\n", i);
		s = cos_asnd_alloc(&ci, r, ci.captbl_cap);
		if (!s) TEST_FAIL("endpoint release: asnd allocation %lu\n", i);
		if (i == 64) frontier = ci.cap_frontier;

		/* Leave a pending event on the end-point */
		if ((ret = cos_asnd(s, 0))) TEST_FAIL("endpoint release: asnd %lu returned %d\n", i, ret);

		/* Dependency order: asnd, arcv (unbinds the thread), thread */
		if ((ret = cos_cap_deactivate(&ci, s, CAP_ASND))) TEST_FAIL("endpoint release: asnd release %lu: %d\n", i, ret);
		if ((ret = cos_cap_deactivate(&ci, r, CAP_ARCV))) TEST_FAIL("endpoint release: arcv release %lu: %d\n", i, ret);
		if ((ret = cos_thd_dealloc(&ci, t, kmem, tid))) TEST_FAIL("endpoint release: thread release %lu: %d\n", i, ret);
		/* (The kernel returns 0 for invocations of missing capabilities, so check the thread) */
		if (cos_introspect(&ci, t, THD_GET_TID) >= 0) TEST_FAIL("endpoint release: freed thread %lu still accessible\n", i);
		cos_thd_init_free(idx);
	}
	printc("SUCCESS: %d rcv/asnd end-points created and released\n", NITER);
	/* Without reuse, each iteration consumes new capability slots */
	if (ci.cap_frontier - frontier > (NITER / 2) * CAPMAX_ENTRY_SZ) TEST_FAIL("endpoint release: capability slots not reused\n");
	printc("SUCCESS: end-point capability slots reused\n");
}

/*
 * Unmap a page, and check that its address can only be mapped again
 * once all cores flushed their TLBs (TLB shootdown + quiescence).
 */
#define NUNMAP 64
static void
test_unmap_quiescence(void)
{
	unsigned long i, early_remaps = 0;
	volatile word_t *page, *alias;
	u64_t ts, start;
	int ret;

	page = cos_page_bump_alloc(&ci);
	if (!page) TEST_FAIL("unmap: page allocation\n");
	alias = (word_t *)cos_mem_alias(&ci, &ci, (vaddr_t)page, COS_PAGE_READABLE | COS_PAGE_WRITABLE);
	if (!alias) TEST_FAIL("unmap: alias\n");

	for (i = 0; i < NUNMAP; i++) {
		*alias = i;
		if (*page != i) TEST_FAIL("unmap: alias %lu not shared\n", i);

		if ((ret = cos_mem_remove(ci.pgtbl_cap, (vaddr_t)alias))) TEST_FAIL("unmap: remove %lu: %d\n", i, ret);
		ts = ps_tsc();
		if (cos_mem_remove(ci.pgtbl_cap, (vaddr_t)alias) == 0) TEST_FAIL("unmap: double remove %lu succeeded\n", i);

		/* No core flushed its TLB yet: the address isn't reusable */
		ret = call_cap_op(ci.pgtbl_cap, CAPTBL_OP_CPY, (vaddr_t)page, ci.pgtbl_cap, (vaddr_t)alias,
		                  COS_PAGE_READABLE | COS_PAGE_WRITABLE);
		if (ret == 0) early_remaps++;
		else if (ret != -EQUIESCENCE) TEST_FAIL("unmap: early remap %lu returned %d\n", i, ret);
		if (ret != 0) {
			if (cos_hw_tlb_quiescent(HWCAP, ts) == 1) early_remaps++;

			/* Shootdown, then wait for all cores to flush */
			if (cos_hw_tlbflush(HWCAP)) TEST_FAIL("unmap: tlb flush %lu\n", i);
			start = ps_tsc();
			while (cos_hw_tlb_quiescent(HWCAP, ts) != 1) {
				if (ps_tsc() - start > 10000ULL * 1000 * 1000) TEST_FAIL("unmap: no quiescence %lu\n", i);
			}

			ret = call_cap_op(ci.pgtbl_cap, CAPTBL_OP_CPY, (vaddr_t)page, ci.pgtbl_cap, (vaddr_t)alias,
			                  COS_PAGE_READABLE | COS_PAGE_WRITABLE);
			if (ret) TEST_FAIL("unmap: remap %lu after quiescence returned %d\n", i, ret);
		}
	}
	if (*alias != NUNMAP - 1) TEST_FAIL("unmap: final remap not shared\n");
	/* Something else would have to flush all TLBs for these to happen */
	if (early_remaps) TEST_FAIL("unmap: %lu addresses reusable before the TLB shootdown\n", early_remaps);
	printc("SUCCESS: %d unmaps, addresses reused only after TLB shootdown and quiescence\n", NUNMAP);
}

void
cos_init(void)
{
	cos_meminfo_init(&ci.mi, BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
	cos_compinfo_init(&ci, BOOT_CAPTBL_SELF_PT, BOOT_CAPTBL_SELF_CT, BOOT_CAPTBL_SELF_COMP,
			  (vaddr_t)cos_get_heap_ptr(), BOOT_CAPTBL_FREE, &ci);
	cos_capid_reclaim_init(&ci, &capid_reclaim);
	cos_kmem_reclaim_init(&ci, &kmem_reclaim);
}

int
main(void)
{
	/*
	 * main runs only on the initialization core (whichever core
	 * started first). The others spin at user-level (init_exit),
	 * where they take the TLB shootdown IPIs.
	 */

	printc("Resource reclamation (kernel-level) tests\n");
	test_thd_release();
	test_endpoint_release();
	test_unmap_quiescence();

	printc("%s\n", failures ? "TEST FAILED" : "TEST PASSED");
	SPIN();

	return 0;
}
