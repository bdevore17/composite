#include <memmgr.h>
#include <cos_thd_init.h>

vaddr_t
memmgr_heap_page_alloc(void)
{
	return memmgr_heap_page_allocn(1);
}

int
memmgr_heap_page_free(vaddr_t addr)
{
	return memmgr_heap_page_freen(addr, 1);
}

cbuf_t
memmgr_shared_page_alloc(vaddr_t *pgaddr)
{
	return memmgr_shared_page_allocn(1, pgaddr);
}
