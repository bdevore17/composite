/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * A server that faults on demand, to test component recovery (see
 * tests.unit_recovery). Its state (the call counter, the heap memory
 * allocated by cos_init) must be reset when it is recovered.
 */

#include <cos_component.h>
#include <llprint.h>
#include <memmgr.h>
#include <faultsrv.h>
#include <ps.h>

/* Allocated (and checked) by each initialization: 8 MiB */
#define HEAP_PAGES 2048

/* (volatile pointers, so that the compiler doesn't see the null dereferences) */
static volatile long *volatile null_ptr = 0, *volatile noncanon_ptr = (volatile long *)0x8000000000000000UL;

static unsigned long counter;
static unsigned long inside;
static long          heap;
static int           initialized;

static void
fault(long type)
{
	volatile long zero = 0;

	switch (type) {
	case FAULTSRV_NULL_WRITE:
		null_ptr[1] = 1;
		break;
	case FAULTSRV_DIV_ZERO:
		/* (the compiler can turn divisions into other instructions) */
		__asm__ __volatile__("xor %%edx, %%edx; mov $1, %%eax; div %%ecx" : : "c"(0) : "eax", "edx");
		break;
	case FAULTSRV_INV_OPCODE:
		__asm__ __volatile__("ud2");
		break;
	case FAULTSRV_GEN_PROT:
		/* A non-canonical address */
		zero = *noncanon_ptr;
		break;
	case FAULTSRV_NULL_READ:
	default:
		zero = *null_ptr;
		break;
	}
	(void)zero;
	printc("FAILURE: faultsrv: fault %ld did not fault\n", type);
}

long
faultsrv_call(void)
{
	if (!initialized) return -1;
	counter++;
	if (counter == FAULTSRV_PERIOD) fault(FAULTSRV_NULL_READ);

	return counter;
}

long
faultsrv_fault(long type)
{
	fault(type);

	return 0;
}

long
faultsrv_spin(long arg, long iters)
{
	volatile long i;

	ps_faa(&inside, 1);
	for (i = 0; i < iters; i++);
	ps_faa(&inside, -1);

	return 2 * arg + 1;
}

long
faultsrv_inside(void)
{
	return ps_load(&inside);
}

long
faultsrv_heap(void)
{
	return heap;
}

long
faultsrv_unrecoverable(void)
{
	vaddr_t addr;

	return memmgr_shared_page_alloc(&addr) ? 0 : -1;
}

long
faultsrv_id(void)
{
	return cos_compid();
}

void
cos_init(void)
{
	char *p;
	int   i;

	p = (char *)memmgr_heap_page_allocn(HEAP_PAGES);
	assert(p);
	heap = (long)p;
	/* New (or reused) memory must be zeroed */
	for (i = 0; i < HEAP_PAGES; i++) {
		long *w = (long *)(p + i * PAGE_SIZE);

		if (w[0] != 0) heap = -1;
		w[0] = i + 1;
	}
	initialized = 1;
}
