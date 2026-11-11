/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Tests of memory reclamation in the memmgr: freeing heap pages, and
 * unmapping/releasing shared memory (with a peer component). The
 * loops allocate far more memory in total than the capmgr has, so
 * they only succeed if freed memory is actually reused.
 */

#include <cos_component.h>
#include <llprint.h>
#include <memmgr.h>
#include <memmgr_peer.h>

static int failures = 0;

#define TEST_FAIL(...)                           \
	do {                                     \
		printc("FAILURE: " __VA_ARGS__); \
		failures++;                      \
		return;                          \
	} while (0)

/*
 * The capmgr has 2 GiB of memory, 1.2 GiB of which is reserved for
 * contiguous memory: 1.5 GiB of allocations cannot be satisfied
 * without reuse.
 */
#define CHURN_PAGES 256 /* 1 MiB */
#define CHURN_ITER  1536

/* Allocate, touch, and free 1 MiB, many times */
static void
test_heap_churn(void)
{
	unsigned long i, j;
	char *p;
	int ret;

	for (i = 0; i < CHURN_ITER; i++) {
		p = (char *)memmgr_heap_page_allocn(CHURN_PAGES);
		if (!p) TEST_FAIL("heap churn: allocation %lu failed (memory not reused?)\n", i);
		for (j = 0; j < CHURN_PAGES; j++) {
			word_t *w = (word_t *)(p + j * PAGE_SIZE);

			/* Reused memory must be zeroed */
			if (w[0] != 0) TEST_FAIL("heap churn: page %lu of allocation %lu not zeroed\n", j, i);
			w[0] = i + 1;
		}
		if ((ret = memmgr_heap_page_freen((vaddr_t)p, CHURN_PAGES))) TEST_FAIL("heap churn: free %lu: %d\n", i, ret);
	}
	printc("SUCCESS: heap churn, %d MiB allocated, touched, and freed\n", CHURN_ITER * CHURN_PAGES / 256);
}

/* A freed page's frame comes back (after TLB quiescence) */
static void
test_heap_reuse(void)
{
	vaddr_t p, q;
	paddr_t pa;
	int i, ret;

	p = memmgr_heap_page_alloc();
	if (!p) TEST_FAIL("heap reuse: allocation\n");
	pa = memmgr_virt_to_phys(p);
	if ((ret = memmgr_heap_page_free(p))) TEST_FAIL("heap reuse: free %d\n", ret);

	for (i = 0; i < 4096; i++) {
		q = memmgr_heap_page_alloc();
		if (!q) TEST_FAIL("heap reuse: allocation %d\n", i);
		if (memmgr_virt_to_phys(q) == pa) break;
		/* Keep q allocated until the frame we freed comes back */
	}
	if (i == 4096) TEST_FAIL("heap reuse: freed frame never reused\n");
	printc("SUCCESS: freed frame reused after %d allocations\n", i);
}

/* Partial frees, invalid frees, and alignment/contiguity of reused memory */
static void
test_heap_free_ranges(void)
{
	vaddr_t p, a;
	paddr_t pa;
	int i, n, align;

	p = memmgr_heap_page_allocn(8);
	if (!p) TEST_FAIL("free ranges: allocation\n");
	if (memmgr_heap_page_freen(p + 2 * PAGE_SIZE, 4)) TEST_FAIL("free ranges: middle\n");
	if (memmgr_heap_page_freen(p + 2 * PAGE_SIZE, 1) != -EINVAL) TEST_FAIL("free ranges: double free\n");
	/* All or nothing: the range includes a freed page */
	if (memmgr_heap_page_freen(p, 4) != -EINVAL) TEST_FAIL("free ranges: partially freed range\n");
	if (memmgr_heap_page_freen(p, 2)) TEST_FAIL("free ranges: head\n");
	if (memmgr_heap_page_freen(p + 6 * PAGE_SIZE, 2)) TEST_FAIL("free ranges: tail\n");
	if (memmgr_heap_page_free((vaddr_t)&failures) != -EINVAL) TEST_FAIL("free ranges: non-heap page\n");
	printc("SUCCESS: partial, double, and invalid frees\n");

	for (i = 0; i < 64; i++) {
		n     = 1 + i % 16;
		align = PAGE_SIZE << (i % 6);
		a     = memmgr_heap_page_allocn_aligned(n, align);
		if (!a || a % align) TEST_FAIL("aligned reuse: allocation %d not aligned\n", i);
		pa = memmgr_virt_to_phys(a);
		for (int j = 1; j < n; j++) {
			if (memmgr_virt_to_phys(a + j * PAGE_SIZE) != pa + j * PAGE_SIZE) TEST_FAIL("aligned reuse: allocation %d not contiguous\n", i);
		}
		if (memmgr_heap_page_freen(a, n)) TEST_FAIL("aligned reuse: free %d\n", i);
	}
	printc("SUCCESS: aligned and physically contiguous allocations from reused memory\n");
}

/*
 * Shared memory between us and the peer: both unmap, in both orders;
 * the memory is released with the last mapping.
 */
#define SHARED_PAGES 256
#define SHARED_ITER  1536
static void
test_shared_release(void)
{
	unsigned long i, j;
	vaddr_t addr;
	cbuf_t id;
	word_t *w;
	int ret;

	for (i = 0; i < SHARED_ITER; i++) {
		id = memmgr_shared_page_allocn(SHARED_PAGES, &addr);
		if (!id || !addr) TEST_FAIL("shared: allocation %lu failed (memory not reused?)\n", i);
		for (j = 0; j < SHARED_PAGES; j++) {
			w = (word_t *)(addr + j * PAGE_SIZE);
			if (w[0] != 0 || w[1] != 0) TEST_FAIL("shared: allocation %lu not zeroed\n", i);
			w[0] = id + j;
		}
		if ((ret = memmgr_peer_map(id, SHARED_PAGES))) TEST_FAIL("shared: peer map %lu: %d\n", i, ret);
		for (j = 0; j < SHARED_PAGES; j++) {
			w = (word_t *)(addr + j * PAGE_SIZE);
			if (w[1] != ~(id + j)) TEST_FAIL("shared: peer's write %lu not visible\n", i);
		}
		if (memmgr_shared_page_unmap(id, addr + PAGE_SIZE) != -EINVAL) TEST_FAIL("shared: unmap of a wrong address\n");

		if (i % 2) {
			/* Peer first: we still have the memory */
			if ((ret = memmgr_peer_unmap(id))) TEST_FAIL("shared: peer unmap %lu: %d\n", i, ret);
			w = (word_t *)addr;
			if (w[0] != id) TEST_FAIL("shared: memory lost after peer unmap %lu\n", i);
			if ((ret = memmgr_shared_page_unmap(id, addr))) TEST_FAIL("shared: unmap %lu: %d\n", i, ret);
		} else {
			/* Us first: the peer still has the memory */
			if ((ret = memmgr_shared_page_unmap(id, addr))) TEST_FAIL("shared: unmap %lu: %d\n", i, ret);
			if ((ret = memmgr_peer_unmap(id))) TEST_FAIL("shared: peer unmap %lu: %d\n", i, ret);
		}
		/* Both mappings are gone: the id is released */
		if (memmgr_peer_map_released(id) != 0) TEST_FAIL("shared: released memory %lu still mappable\n", i);
		if (memmgr_shared_page_unmap(id, addr) != -EINVAL) TEST_FAIL("shared: double unmap %lu\n", i);
	}
	printc("SUCCESS: shared memory, %d MiB allocated, shared, and released\n", SHARED_ITER * SHARED_PAGES / 256);
}

int
main(void)
{
	printc("Memory reclamation (memmgr) tests\n");
	test_heap_reuse();
	test_heap_free_ranges();
	test_heap_churn();
	test_shared_release();
	printc("%s\n", failures ? "TEST FAILED" : "TEST PASSED");

	return 0;
}
