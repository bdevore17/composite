#ifndef CHAL_PLAT_H
#define CHAL_PLAT_H

int chal_tlb_lockdown(unsigned long entryid, unsigned long vaddr, unsigned long paddr);
int chal_l1flush(void);
int chal_tlbstall(void);
int chal_tlbstall_recount(int a);
int chal_tlbflush(int a);

/* This flushes all levels of cache of the current logical CPU. */
static inline void
chal_flush_cache(void)
{
	asm volatile("wbinvd" : : : "memory");
}

/*
 * Invalidate *all* TLB entries on the current core: global and
 * non-global entries, for every PCID.  Any write to CR4 that changes
 * CR4.PGE does exactly this (Intel SDM Vol. 3A, 4.10.4.1), and it works
 * whether or not PCIDs are enabled, so we toggle PGE twice.  This is
 * expensive (the kernel's global entries are lost as well), and is
 * only used for TLB shootdowns when memory is reclaimed.
 */
static inline void
chal_flush_tlb_global(void)
{
	unsigned long cr4;

	asm volatile("mov %%cr4, %0" : "=r"(cr4));
	asm volatile("mov %0, %%cr4" : : "r"(cr4 ^ (1UL << 7)) : "memory");
	asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}
static inline void
chal_remote_tlb_flush(int target_cpu)
{
}
/*
 * Flush all user-level translations on this core, for all address
 * spaces (PCIDs). User mappings are not global, but with PCIDs
 * enabled a CR3 reload only flushes the current PCID, so we use the
 * full flush.
 */
static inline void
chal_flush_tlb(void)
{
	chal_flush_tlb_global();
}

static inline void *
chal_pa2va(paddr_t address)
{
	return (void *)(address + COS_MEM_KERN_START_VA);
}

static inline paddr_t
chal_va2pa(void *address)
{
	return (paddr_t)((unsigned long)address - COS_MEM_KERN_START_VA);
}

#endif /* CHAL_PLAT_H */
