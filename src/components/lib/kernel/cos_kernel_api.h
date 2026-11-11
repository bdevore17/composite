#ifndef COS_KERNEL_API_H
#define COS_KERNEL_API_H

/*
 * Copyright 2015, Qi Wang and Gabriel Parmer, GWU, gparmer@gwu.edu.
 *
 * This uses a two clause BSD License.
 *
 * This library is a very light wrapper around the system call API.
 * As there is only a single system call (capability invocation), this
 * provides structure to the system calls.  Second, it abstracts one
 * of the most mundane aspects of the system: the management of
 * retyping memory, and of construction of resource tables
 * (page-tables and capability-tables).  It manages the capability and
 * virtual address name-spaces, and assumes that all untyped memory is
 * at a given range of addresses.
 *
 * This library is designed to provide the most functionality without
 * maintaining any fine-grained data-structures (no lists, no arrays,
 * etc...).  Instead, cos_compinfos are created for each component
 * (one for us), and all of the namespaces are tracked within that
 * (statically-sized) structure.  Additionally, a cos_meminfo holds
 * all of the information necessary to allocate memory (of all types).
 * It is likely that we will use the cos_meminfo associated with the
 * cos_compinfo for us (as we have all of the memory), while most
 * other components (i.e. that we load) do *not* have access to
 * memory, thus we shouldn't look in their page-table for memory to
 * use for allocations.
 *
 * For this library to use only static data-structures, we use
 * bump-pointers for managing allocation of each of the namespaces.
 * This means that we *never* deallocate resources (capabilities,
 * kernel resources, virtual addresses, etc...), thus never reuse
 * resources.  Thus this is quite limited in applicability.  However,
 * most embedded systems avoid dynamic allocation, making the
 * simplicity of this abstraction ideally suited to those systems.  It
 * can also be seen as a backend for allocation to layer other
 * allocators on top (that support deallocation).
 *
 * See the micro_booter for an examples of using this API.
 */

#include <cos_component.h>
#include <cos_debug.h>
#include <ps_plat.h>
/* Types mainly used for documentation */
typedef capid_t sinvcap_t;
typedef capid_t sretcap_t;
typedef capid_t asndcap_t;
typedef capid_t arcvcap_t;
typedef capid_t thdcap_t;
typedef capid_t tcap_t;
typedef capid_t compcap_t;
typedef capid_t captblcap_t;
typedef capid_t pgtblcap_t;
typedef capid_t hwcap_t;
typedef capid_t ulkcap_t;
typedef capid_t vm_vmcscap_t;
typedef capid_t vm_msrbitmapcap_t;
typedef capid_t vm_lapicaccesscap_t;
typedef capid_t vm_lapiccap_t;
typedef capid_t vm_shared_mem_t;
typedef capid_t vm_vmcb_t;

/*
 * Deferred reuse of released resources (see "Resource reclamation"
 * below). A FIFO of released capability ids or kernel memory pages,
 * each tagged with the time it was released. An entry is only handed
 * out again after the kernel quiescence period has passed since its
 * release, so that no core can still be in the kernel using the old
 * object.
 */
#define COS_RECLAIM_RING_SZ 512 /* power of 2 */

struct cos_reclaim_ent {
	word_t id;
	u64_t  ts;
};

struct cos_reclaim_ring {
	struct ps_lock         lock;
	unsigned long          head, tail; /* pop at head, push at tail */
	unsigned long          dropped;    /* releases that didn't fit, thus are never reused */
	struct cos_reclaim_ent ents[COS_RECLAIM_RING_SZ];
};

/* One ring per capability-slot size (16, 32 and 64 byte slots) */
#define COS_RECLAIM_CAPSZ_NUM 3
struct cos_capid_reclaim {
	struct cos_reclaim_ring sz[COS_RECLAIM_CAPSZ_NUM];
};

/* Memory source information */
struct cos_meminfo {
	vaddr_t    untyped_ptr, umem_ptr, kmem_ptr;
	vaddr_t    untyped_frontier, umem_frontier, kmem_frontier;
	pgtblcap_t pgtbl_cap;
	/* released kernel memory pages to reuse before bump-allocating new ones (or NULL) */
	struct cos_reclaim_ring *kmem_reclaim;

	capid_t	   second_lvl_pgtbl_cap;
	vaddr_t	   second_lvl_pgtbl_addr;
};

/* Component captbl/pgtbl allocation information */
struct cos_compinfo {
	/* capabilities to higher-order capability tables (or -1) */
	capid_t pgtbl_cap, captbl_cap, comp_cap;
	/* the frontier of unallocated caps, and the allocated captbl range */
	capid_t cap_frontier, caprange_frontier;
	/* the frontier for each of the various sizes of capability per core! */
	capid_t cap16_frontier[NUM_CPU], cap32_frontier[NUM_CPU], cap64_frontier[NUM_CPU];
	/* heap pointer equivalent, and range of allocated PTEs */
	vaddr_t vas_frontier;
	vaddr_t vasrange_frontier[COS_PGTBL_DEPTH - 1]; 
	/* the source of memory */
	struct cos_compinfo *memsrc; /* might be self-referential */
	struct cos_meminfo   mi;     /* only populated for the component with real memory */

	struct ps_lock cap_lock, mem_lock; /* locks to make the cap frontier and mem frontier updates and expands atomic */
	struct ps_lock va_lock; /* lock to make the vas frontier and bump expands for vas atomic */
	/* shared comp cap */
	capid_t comp_cap_shared;
	capid_t pgtbl_cap_shared;

	u8_t comp_type;

	/* released capability ids to reuse before bump-allocating new ones (or NULL) */
	struct cos_capid_reclaim *capid_reclaim;
};

#define COMP_TYPE_DEF (0)
#define COMP_TYPE_VM (1)

#define PGTBL_TYPE_DEF (0)
#define PGTBL_TYPE_EPT (1)
#define PGTBL_LVL_FLAG_VM (1UL << 31)

void cos_compinfo_init(struct cos_compinfo *ci, pgtblcap_t pgtbl_cap, captblcap_t captbl_cap, compcap_t comp_cap,
                       vaddr_t heap_ptr, capid_t cap_frontier, struct cos_compinfo *ci_resources);/*
 * This only needs be called on compinfos that are managing resources
 * (i.e. likely only one).  All of the capabilities will be relative
 * to this component's captbls.
 */
void cos_meminfo_init(struct cos_meminfo *mi, vaddr_t untyped_ptr, unsigned long untyped_sz, pgtblcap_t pgtbl_cap);
void cos_meminfo_alloc(struct cos_compinfo *ci, vaddr_t untyped_ptr, unsigned long untyped_sz);
/* expand *only* the pgtbl-internal nodes */
vaddr_t cos_pgtbl_intern_alloc(struct cos_compinfo *ci, pgtblcap_t cipgtbl, vaddr_t mem_ptr, unsigned long mem_sz);
/*
 * Expand the page-table with a node at lvl, and return the pgtbl
 * capability to that node.  This also adjusts the frontier, so it
 * should be set to round_to_pgd_page(mem_ptr) before being called.
 */
pgtblcap_t cos_pgtbl_intern_expand(struct cos_compinfo *ci, vaddr_t mem_ptr, int lvl);
/*
 * Use a given pgtbl internal node to expand ci's page-table.  Adjusts
 * frontier as above.
 */
int cos_pgtbl_intern_expandwith(struct cos_compinfo *ci, pgtblcap_t intern, vaddr_t mem);
vaddr_t cos_shared_kernel_page_alloc(struct cos_compinfo *ci, vaddr_t *resource);
vaddr_t cos_shared_kernel_page_alloc_at(struct cos_compinfo *ci, vaddr_t mem_ptr);
int cos_comp_alloc_shared(struct cos_compinfo *ci_og, pgtblcap_t ptc, vaddr_t entry, struct cos_compinfo *ci_resources, prot_domain_t protdom);

/*
 * This uses the next three functions to allocate a new component and
 * correctly populate ci (allocating all resources from ci_resources).
 */
int         cos_compinfo_alloc(struct cos_compinfo *ci, vaddr_t heap_ptr, capid_t cap_frontier, vaddr_t entry,
                               struct cos_compinfo *ci_resources, prot_domain_t protdom);
captblcap_t cos_captbl_alloc(struct cos_compinfo *ci);
pgtblcap_t  cos_pgtbl_alloc(struct cos_compinfo *ci, u8_t type);
compcap_t   cos_comp_alloc(struct cos_compinfo *ci, captblcap_t ctc, pgtblcap_t ptc, vaddr_t entry, prot_domain_t protdom);

void       cos_ulk_info_init(struct cos_compinfo *ci);
pgtblcap_t cos_ulk_pgtbl_create(struct cos_compinfo *ci, pgtblcap_t *secondlvl);
ulkcap_t   cos_ulk_page_alloc(struct cos_compinfo *ci, pgtblcap_t ulkpt, vaddr_t uaddr);
int        cos_ulk_map_in(pgtblcap_t ptc);

void cos_comp_capfrontier_update(struct cos_compinfo *ci, capid_t cap_frontier, int try_expand);

typedef void (*cos_thd_fn_t)(void *);
thdcap_t cos_thd_alloc(struct cos_compinfo *ci, compcap_t comp, cos_thd_fn_t fn, void *data);
thdcap_t cos_thd_alloc_ext(struct cos_compinfo *ci, compcap_t comp, thdclosure_index_t idx);

vaddr_t cos_vm_kernel_page_create(struct cos_compinfo *ci);

/* Create the initial (cos_init) thread */
thdcap_t  cos_initthd_alloc(struct cos_compinfo *ci, compcap_t comp);

sinvcap_t cos_sinv_alloc(struct cos_compinfo *srcci, compcap_t dstcomp, vaddr_t entry, invtoken_t token);
arcvcap_t cos_arcv_alloc(struct cos_compinfo *ci, thdcap_t thdcap, tcap_t tcapcap, compcap_t compcap, arcvcap_t enotif);
asndcap_t cos_asnd_alloc(struct cos_compinfo *ci, arcvcap_t arcvcap, captblcap_t ctcap);

capid_t cos_vm_vmcs_alloc(struct cos_compinfo *ci, vaddr_t kmem);
capid_t cos_vm_msr_bitmap_alloc(struct cos_compinfo *ci, vaddr_t kmem);
capid_t cos_vm_lapic_alloc(struct cos_compinfo *ci, vaddr_t kmem);
capid_t cos_vm_shared_region_alloc(struct cos_compinfo *ci, vaddr_t kmem);
capid_t cos_vm_lapic_access_alloc(struct cos_compinfo *ci, vaddr_t kmem);
capid_t cos_vm_vmcb_alloc(struct cos_compinfo *ci, vm_vmcscap_t vmcs_cap, vm_msrbitmapcap_t msr_bitmap_cap, vm_lapicaccesscap_t lapic_access_cap, vm_lapiccap_t lapic_cap, vm_shared_mem_t shared_mem_cap, thdcap_t handler_cap, word_t vpid);

void *cos_page_bump_alloc(struct cos_compinfo *ci);
void *cos_page_bump_allocn(struct cos_compinfo *ci, size_t sz);
void *cos_page_bump_allocn_aligned(struct cos_compinfo *ci, size_t sz, size_t align);

capid_t cos_cap_cpy(struct cos_compinfo *dstci, struct cos_compinfo *srcci, cap_t srcctype, capid_t srccap);
int     cos_cap_cpy_at(struct cos_compinfo *dstci, capid_t dstcap, struct cos_compinfo *srcci, capid_t srccap);

int cos_thd_switch(thdcap_t c);
int cos_thd_wakeup(thdcap_t thd, tcap_t tc, tcap_prio_t prio, tcap_res_t res);
#define CAP_NULL 0
sched_tok_t cos_sched_sync(void);
/*
 * returns 0 on success and errno on failure:
 * -EBUSY: if rcv has pending notifications and if current thread is the thread associated with rcv.
 * -EAGAIN: if stok is outdated
 * -EPERM: if tcap is not active (has no budget left)
 * -EINVAL: any other error
 */
int cos_switch(thdcap_t c, tcap_t t, tcap_prio_t p, tcap_time_t r, arcvcap_t rcv, sched_tok_t stok);
int cos_thd_mod(struct cos_compinfo *ci, thdcap_t c, void *tls_addr); /* set tls addr of thd in captbl */

/*
 * returns 0 on success and errno on failure (the rcv thread will not be sent a notification):
 * -EBUSY: if rcv has pending notifications and if current thread is the thread associated with rcv.
 * -EAGAIN: if stok is outdated
 * -EPERM: if tcap is not active (has no budget left)
 * -EINVAL: any other error
 */
int cos_sched_asnd(asndcap_t snd, tcap_time_t timeout, arcvcap_t srcv, sched_tok_t stok);
/* returns 0 on success and -EINVAL on failure */
int cos_asnd(asndcap_t snd, int yield);
/* returns non-zero if there are still pending events (i.e. there have been pending snds) */
int cos_rcv(arcvcap_t rcv, rcv_flags_t flags, int *rcvd);
/* returns the same value as cos_rcv, but also information about scheduling events */
int cos_sched_rcv(arcvcap_t rcv, rcv_flags_t flags, tcap_time_t timeout, int *rcvd, thdid_t *thdid, int *blocked, cycles_t *cycles, tcap_time_t *thd_timeout);

int cos_introspect(struct cos_compinfo *ci, capid_t cap, unsigned long op);

vaddr_t cos_mem_alias(struct cos_compinfo *dstci, struct cos_compinfo *srcci, vaddr_t src, unsigned long perm_flags);
vaddr_t cos_mem_aliasn(struct cos_compinfo *dstci, struct cos_compinfo *srcci, vaddr_t src, size_t sz, unsigned long perm_flags);
vaddr_t cos_mem_aliasn_aligned(struct cos_compinfo *dstci, struct cos_compinfo *srcci, vaddr_t src, size_t sz, size_t align, unsigned long perm_flags);
int     cos_mem_alias_at(struct cos_compinfo *dstci, vaddr_t dst, struct cos_compinfo *srcci, vaddr_t src, unsigned long perm_flags);
int     cos_mem_alias_atn(struct cos_compinfo *dstci, vaddr_t dst, struct cos_compinfo *srcci, vaddr_t src, size_t sz, unsigned long perm_flags);
vaddr_t cos_mem_move(struct cos_compinfo *dstci, struct cos_compinfo *srcci, vaddr_t src);
int     cos_mem_move_at(struct cos_compinfo *dstci, vaddr_t dst, struct cos_compinfo *srcci, vaddr_t src);
/*
 * Unmap the page at addr from pgtbl pt. The frame stays referenced by
 * its other mappings. The TLBs of all cores might still cache the
 * translation: don't reuse the frame for anyone else before
 * cos_hw_tlb_quiescent says that all cores have flushed since the
 * unmap (see cos_hw_tlbflush).
 */
int     cos_mem_remove(pgtblcap_t pt, vaddr_t addr);

/* Tcap operations */
tcap_t cos_tcap_alloc(struct cos_compinfo *ci);
/*
 * returns 0 on success and errno on failure:
 * -EPERM: if src tcap is not active (has no budget left)
 * -EINVAL: any other error
 */
int cos_tcap_transfer(tcap_t src, arcvcap_t dst, tcap_res_t res, tcap_prio_t prio);
/*
 * returns 0 on success and errno on failure:
 * -EPERM: if src tcap is not active (has no budget left)
 * -EINVAL: any other error
 */
int cos_tcap_delegate(asndcap_t dst, tcap_t src, tcap_res_t res, tcap_prio_t prio, tcap_deleg_flags_t flags);
int cos_tcap_merge(tcap_t dst, tcap_t rm);

/* Hardware (interrupts) operations */
hwcap_t cos_hw_alloc(struct cos_compinfo *ci, u32_t bitmap);
int     cos_hw_attach(hwcap_t hwc, hwid_t hwid, arcvcap_t rcvcap);
int     cos_hw_detach(hwcap_t hwc, hwid_t hwid);
void   *cos_hw_map(struct cos_compinfo *ci, hwcap_t hwc, paddr_t pa, unsigned int len);
int     cos_hw_cycles_per_usec(hwcap_t hwc);
int     cos_hw_cycles_thresh(hwcap_t hwc);
int     cos_hw_tlb_lockdown(hwcap_t hwc, unsigned long entryid, unsigned long vaddr, unsigned long paddr);
int     cos_hw_l1flush(hwcap_t hwc);
/*
 * Start a TLB shootdown: flush this core's TLB and send an IPI asking
 * all other cores to flush theirs. Doesn't wait for the other cores.
 */
int     cos_hw_tlbflush(hwcap_t hwc);
/*
 * Have all cores flushed their TLBs since timestamp ts (a value of
 * ps_tsc() taken after an unmap)? 1 if yes, 0 if no, < 0 on error.
 */
int     cos_hw_tlb_quiescent(hwcap_t hwc, u64_t ts);
int     cos_hw_tlbstall(hwcap_t hwc);
int     cos_hw_tlbstall_recount(hwcap_t hwc);
void    cos_hw_shutdown(hwcap_t hwc);


capid_t cos_capid_bump_alloc(struct cos_compinfo *ci, cap_t cap);

/**************** [Resource reclamation] ****************
 *
 * The allocation functions above bump-allocate capability ids,
 * kernel memory, and thread ids, and never reuse them. A manager
 * (e.g. the capmgr) that wants to release resources:
 *
 * 1. registers reclamation rings with cos_capid_reclaim_init (per
 *    compinfo whose capability table it manages) and
 *    cos_kmem_reclaim_init (on the compinfo with the memory),
 * 2. deactivates capabilities with cos_cap_deactivate (copies) and
 *    cos_thd_dealloc (the last reference to a thread, releasing its
 *    kernel memory).
 *
 * Released capability ids and kernel pages are reused by the normal
 * allocation functions once the kernel quiescence period
 * (KERN_QUIESCENCE_CYCLES) has passed since their release. Capability
 * slots are deactivated with liveness id 0, so the kernel itself
 * doesn't delay their reuse: that is enforced by the rings instead.
 * Thread ids are recycled through cos_thd_id_free.
 */
void cos_capid_reclaim_init(struct cos_compinfo *ci, struct cos_capid_reclaim *r);
void cos_kmem_reclaim_init(struct cos_compinfo *ci, struct cos_reclaim_ring *r);
/* Return a capability id (whose slot is already deactivated) / kernel page for reuse. 0 on success. */
int  cos_capid_free(struct cos_compinfo *ci, capid_t cap, cap_t type);
int  cos_kmem_free(struct cos_compinfo *ci, vaddr_t kmem);
/*
 * Deactivate capability cap of type in ci's capability table, and
 * return its id for reuse. Not for the last reference to a thread
 * (use cos_thd_dealloc). Supports CAP_THD, CAP_ARCV, CAP_ASND, and
 * CAP_SINV. Returns 0 on success, the kernel's error otherwise.
 */
int  cos_cap_deactivate(struct cos_compinfo *ci, capid_t cap, cap_t type);
/*
 * Deactivate the last capability to a thread (in ci's capability
 * table, ci being the compinfo that has the memory), release the
 * thread's kernel memory kmem (as returned by cos_thd_alloc_ext_kmem
 * or cos_initthd_alloc_kmem), and its thread id tid. All copies of
 * the capability must have been deactivated first, and the thread
 * must not be bound to an arcv. Must execute on the thread's core.
 */
int  cos_thd_dealloc(struct cos_compinfo *ci, thdcap_t thd, vaddr_t kmem, thdid_t tid);
/* Thread allocation that also returns the thread's kernel memory, for cos_thd_dealloc */
thdcap_t cos_thd_alloc_ext_kmem(struct cos_compinfo *ci, compcap_t comp, thdclosure_index_t idx, vaddr_t *kmem);
thdcap_t cos_initthd_alloc_kmem(struct cos_compinfo *ci, compcap_t comp, vaddr_t *kmem);
/* Make a thread id available for reuse (the thread must be deallocated). */
void cos_thd_id_free(thdid_t tid);

pgtblcap_t cos_shared_pgtbl_alloc(void);
u32_t cos_cons_into_shared_pgtbl(struct cos_compinfo *ci, pgtblcap_t top_lvl);

#endif /* COS_KERNEL_API_H */
