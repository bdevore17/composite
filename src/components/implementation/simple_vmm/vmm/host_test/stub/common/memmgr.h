#pragma once
#include <stdlib.h>
static inline vaddr_t memmgr_heap_page_allocn(unsigned long n) { return (vaddr_t)aligned_alloc(4096, n * 4096); }
