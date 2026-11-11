/**
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Copyright 2020, The George Washington University
 * Author: Gabe Parmer, gparmer@gwu.edu
 */

#include <cos_debug.h>
#include <consts.h>
#include <static_slab.h>
#include <crt.h>

#include <cos_component.h>
#include <cos_kernel_api.h>
#include <cos_defkernel_api.h>
#include <initargs.h>
#include <addr.h>
#include <contigmem.h>

struct cm_asnd;

struct cm_rcv {
	struct crt_rcv  rcv;
	struct cm_comp *sched;
	arcvcap_t       aliased_cap;

	/* Set for rcvs created by capmgr_rcv_create, to release them */
	struct cm_comp *client;      /* the component the rcv (and its aliases) are in */
	coreid_t        core;
	thdcap_t        aliased_thd; /* the rcv thread's capability in client */
	tcap_t          aliased_tc;  /* the rcv tcap's capability in client */
	struct cm_asnd *asnd;        /* the asnd created along with the rcv */
};

/*
 * The kernel cannot deallocate tcaps. The tcaps of released rcvs are
 * kept, along with their alias in the component, and reused for the
 * component's next rcv on that core.
 */
#define CM_TCAP_POOL_SZ 8
struct cm_tcap_pool {
	int n;
	struct {
		tcap_t tc, alias;
	} tcaps[CM_TCAP_POOL_SZ];
};

struct mm_run;
struct mm_runlist {
	struct mm_run *head, *tail;
};

struct cm_comp {
	struct crt_comp comp;
	struct cm_rcv *sched_rcv[NUM_CPU];    /* rcv cap for this scheduler or NULL if not a scheduler */
	struct cm_rcv *sched_parent; /* rcv cap for this scheduler's scheduler, or NULL */

	struct cm_tcap_pool tcap_pool[NUM_CPU];
	struct mm_runlist   va_free; /* freed virtual address ranges of this component, see mm_va_get */
};

struct cm_thd {
	struct crt_thd  thd;

	struct cm_comp *client; /* component thread begins execution in */
	struct cm_comp *sched;	/* The scheduler that has the alias */
	thdcap_t aliased_cap;	/* location thread is aliased into the scheduler. */
	coreid_t core;		/* the core the thread was created on (and runs on) */
};

struct cm_asnd {
	struct crt_asnd asnd;
	asndcap_t       aliased_cap;
};

/*
 * Shared memory should be manager -> client, and between
 * point-to-point channel recipients
 */
#define MM_MAPPINGS_MAX 5

struct mm_mapping {
	SS_STATE_T(struct cm_comp *) comp;
	vaddr_t     addr;
};

typedef unsigned int cbuf_t;
struct mm_page {
	void *page;		/* our (the capmgr's) mapping of the page */
	struct mm_mapping mappings[MM_MAPPINGS_MAX];
	unsigned int hnext;	/* heap pages: next page id in the mm_heap_hash bucket */
	unsigned int snext;	/* shared pages: next page id in the span */
};

/* Span of pages, indexed by cbuf_t */
struct mm_span {
	unsigned int page_off;	/* id of the first page, the others are linked with snext */
	unsigned int n_pages;
	unsigned int nmaps;	/* number of mappings of the span (allocation + maps - unmaps) */
};

SS_STATIC_SLAB(comp, struct cm_comp, MAX_NUM_COMPS);
SS_STATIC_SLAB(thd, struct cm_thd, MAX_NUM_THREADS);
/* These size values are somewhat arbitrarily chosen */
SS_STATIC_SLAB(rcv, struct cm_rcv, MAX_NUM_THREADS);
SS_STATIC_SLAB(asnd, struct cm_asnd, MAX_NUM_THREADS);

/* 64 MiB */
#define MB2PAGES(mb) (round_up_to_page(mb * 1024 * 1024) / PAGE_SIZE)
#define MM_NPAGES (MB2PAGES(9000))
SS_STATIC_SLAB(page, struct mm_page, MM_NPAGES);
SS_STATIC_SLAB(span, struct mm_span, MM_NPAGES);

/* CONTIG_PHY_PAGES: Total size (in pages) of the contiguous physical memory region */
#define CONTIG_PHY_PAGES 300000
static u8_t *contig_phy_pages = 0;        /* Current allocation pointer, advances as pages are allocated */
static u8_t *contig_phy_pages_base = 0;   /* Base address of the contiguous region */

static struct cm_comp *
cm_self(void)
{
	struct cm_comp *c = ss_comp_get(cos_compid());

	assert(c);

	return c;
}

/*
 * Released capability ids of each component's capability table, and
 * released kernel memory, are reused after quiescence (see the
 * resource reclamation section of cos_kernel_api.h).
 */
static struct cos_capid_reclaim cm_capid_reclaim[MAX_NUM_COMPS + 1];
static struct cos_reclaim_ring  cm_kmem_reclaim;

static void
cm_comp_reclaim_init(struct cm_comp *c)
{
	assert(c->comp.id <= MAX_NUM_COMPS);
	cos_capid_reclaim_init(cos_compinfo_get(c->comp.comp_res), &cm_capid_reclaim[c->comp.id]);
}

static struct cm_comp *
cm_comp_self_alloc(char *name)
{
	struct cm_comp *c = ss_comp_alloc_at_id(cos_compid());

	assert(c);
	if (crt_booter_create(&c->comp, name, cos_compid(), 0)) BUG();
	ss_comp_activate(c);

	return c;
}

static struct cm_comp *
cm_comp_alloc_with(char *name, compid_t id, struct crt_comp_resources *resources)
{
	struct cm_comp *c = ss_comp_alloc_at_id(id);

	if (!c) return NULL;
	if (crt_comp_create_with(&c->comp, name, id, resources)) {
		ss_comp_free(c);
		return NULL;
	}
	cm_comp_reclaim_init(c);
	ss_comp_activate(c);

	return c;
}

/*
 * Protects the thread, rcv, and asnd slabs during creation and
 * release, and the tcap pools. A spinlock: see the warning on mm_lock.
 */
static struct ps_lock cm_lock = PS_LOCK_INITIALIZER;

/*
 * Create a rcv end-point (and its thread) in component cm, with its
 * events going to sched. The thread, rcv, and tcap capabilities are
 * aliased into cm.
 */
struct cm_rcv *
cm_rcv_alloc_in(struct cm_comp *cm, struct crt_rcv *sched, thdclosure_index_t closure_id, crt_rcv_flags_t flags, thdcap_t *thdcap, thdid_t *tid)
{
	struct crt_comp *c = &cm->comp;
	struct cm_rcv *r;
	struct crt_rcv_resources res = (struct crt_rcv_resources) { 0 };
	struct cos_compinfo *target_ci = cos_compinfo_get(c->comp_res);
	struct cm_tcap_pool *pool = &cm->tcap_pool[cos_cpuid()];
	crt_rcv_alias_t alias_flags = CRT_RCV_ALIAS_THD | CRT_RCV_ALIAS_RCV | CRT_RCV_ALIAS_TCAP;
	tcap_t tc = 0;
	struct cm_rcv *ret = NULL;

	ps_lock_take(&cm_lock);
	r = ss_rcv_alloc();
	if (!r) ERR_THROW(NULL, done);

	/* Reuse the tcap of a released rcv (it's still aliased in cm) */
	if (pool->n > 0) {
		pool->n--;
		tc          = pool->tcaps[pool->n].tc;
		res.tc      = pool->tcaps[pool->n].alias;
		alias_flags = CRT_RCV_ALIAS_THD | CRT_RCV_ALIAS_RCV;
	}
	if (crt_rcv_create_in_with_tcap(&r->rcv, c, sched, closure_id, flags, tc)) {
		if (tc) pool->n++; /* put the tcap back */
		ss_rcv_free(r);
		ERR_THROW(NULL, done);
	}

	if (crt_rcv_alias_in(&r->rcv, c, &res, alias_flags)) {
		ss_rcv_free(r);
		assert(0);
		ERR_THROW(NULL, done);
	}
	r->aliased_cap = res.rcv;
	r->aliased_thd = res.thd;
	r->aliased_tc  = res.tc;
	r->client      = cm;
	r->core        = cos_cpuid();
	ss_rcv_activate(r);

	*tid = cos_introspect(target_ci, res.thd, THD_GET_TID);
	*thdcap = res.thd;
	ret = r;
done:
	ps_lock_release(&cm_lock);

	return ret;
}

struct cm_asnd *
cm_asnd_alloc_in(struct crt_comp *c, struct crt_rcv *rcv)
{
	struct cm_asnd *s;
	struct crt_asnd_resources res = { 0 };

	ps_lock_take(&cm_lock);
	s = ss_asnd_alloc();
	ps_lock_release(&cm_lock);

	if (!s) return NULL;
	if (crt_asnd_create(&s->asnd, rcv)) {
		ss_asnd_free(s);
		return NULL;
	}

	if (crt_asnd_alias_in(&s->asnd, c, &res)) {
		ss_asnd_free(s);
		return NULL;
	}
	s->aliased_cap = res.asnd;
	ss_asnd_activate(s);

	return s;
}

struct cm_thd *
cm_thd_alloc_in(struct cm_comp *c, struct cm_comp *sched, thdclosure_index_t closure_id)
{
	struct cm_thd *t;
	struct crt_thd_resources res = { 0 };

	ps_lock_take(&cm_lock);
	t = ss_thd_alloc();
	ps_lock_release(&cm_lock);
	if (!t) return NULL;
	if (crt_thd_create_in(&t->thd, &c->comp, closure_id)) {
		ss_thd_free(t);
		printc("capmgr: couldn't create new thread correctly.\n");
		return NULL;
	}
	if (crt_thd_alias_in(&t->thd, &sched->comp, &res)) {
		printc("capmgr: couldn't alias correctly.\n");
		/* FIXME: reclaim the thread */
		return NULL;
	}

	t->sched       = sched;
	t->aliased_cap = res.cap;
	t->client      = c;
	t->core        = cos_cpuid();
	/* FIXME: should take a reference to the scheduler */
	/* Activate last: capmgr_thd_release finds threads in the slab */
	ss_thd_activate(t);

	return t;
}

/***
 * Memory management and reclamation.
 *
 * Every page we give out is also mapped into our own address space
 * (`mm_page->page`), and given to clients by aliasing (copying) that
 * mapping into their page-table. Freeing a page unmaps the client's
 * mapping; our mapping keeps the frame, so the pool of free memory is
 * simply a set of ranges of our own virtual addresses (`mm_run`s).
 *
 * TLB quiescence: after a page is unmapped, the TLBs of all cores can
 * still hold the old translation, so the previous owner could still
 * access the page. A freed frame (and a freed virtual address range)
 * is therefore only reused after all cores have flushed their TLBs
 * since the unmap. Each free records the time after its unmaps
 * (`ts`), then starts a TLB shootdown (`cos_hw_tlbflush`: flush here,
 * IPI the other cores), and the memory waits on a "pending" FIFO
 * until the kernel reports that every core has flushed after `ts`
 * (`cos_hw_tlb_quiescent`). Reused frames are zeroed before they are
 * handed out. The kernel enforces the same rule for virtual
 * addresses: it refuses to map an unmapped address again before
 * quiescence.
 *
 * Multi-page allocations are taken from a single run of frames that
 * is contiguous in our address space, which, as with the bump
 * allocator, means physically contiguous.
 *
 * All of this is protected by `mm_lock`.
 *
 * WARNING: This is a spinlock. If the scheduler uses any of the
 * memmgr APIs that take this lock, deadlocks and other serious issues
 * can occur.
 * TODO: Requires a redesign to use a different synchronization mechanism.
 */
static struct ps_lock mm_lock = PS_LOCK_INITIALIZER;

/* A range of npages pages starting at addr, freed at time ts */
struct mm_run {
	vaddr_t        addr;
	unsigned long  npages;
	u64_t          ts;
	struct mm_run *next;
};

/*
 * Run descriptors are needed when memory is freed. If they run out
 * (extreme fragmentation), the freed memory is leaked (counted in
 * mm_stats.leaked).
 */
#define MM_NRUNS (1 << 16)
static struct mm_run  mm_runs[MM_NRUNS];
static struct mm_run *mm_runs_freelist;
static unsigned long  mm_runs_used;

/* Frames waiting for TLB quiescence (FIFO, by ts), and reusable frames */
static struct mm_runlist mm_frames_pending, mm_frames_ready;
/* All cores have flushed their TLBs after this time */
static u64_t mm_quiesced_ts;

static struct {
	unsigned long freed, reused, leaked;
} mm_stats;

#define MM_HW_CAP BOOT_CAPTBL_SELF_INITHW_BASE

static struct mm_run *
mm_run_alloc(void)
{
	struct mm_run *r = mm_runs_freelist;

	if (r) {
		mm_runs_freelist = r->next;
	} else {
		if (mm_runs_used == MM_NRUNS) return NULL;
		r = &mm_runs[mm_runs_used++];
	}
	*r = (struct mm_run) { 0 };

	return r;
}

static void
mm_run_free(struct mm_run *r)
{
	r->next          = mm_runs_freelist;
	mm_runs_freelist = r;
}

static void
mm_runlist_append(struct mm_runlist *l, struct mm_run *r)
{
	r->next = NULL;
	if (l->tail) l->tail->next = r;
	else         l->head = r;
	l->tail = r;
}

static void
mm_runlist_remove(struct mm_runlist *l, struct mm_run *r, struct mm_run *prev)
{
	if (prev) prev->next = r->next;
	else      l->head = r->next;
	if (l->tail == r) l->tail = prev;
}

/* Have all cores flushed their TLBs after ts? */
static int
mm_quiescent(u64_t ts)
{
	if (ts <= mm_quiesced_ts) return 1;
	if (cos_hw_tlb_quiescent(MM_HW_CAP, ts) != 1) return 0;
	mm_quiesced_ts = ts;

	return 1;
}

/* Move the pending frames that reached quiescence to the ready list */
static void
mm_frames_promote(void)
{
	struct mm_run *r;

	if (!mm_frames_pending.head) return;
	/* The list is ordered by ts: if the last is quiescent, all are. */
	if (mm_quiescent(mm_frames_pending.tail->ts)) {
		while ((r = mm_frames_pending.head)) {
			mm_runlist_remove(&mm_frames_pending, r, NULL);
			mm_runlist_append(&mm_frames_ready, r);
		}
		return;
	}
	while ((r = mm_frames_pending.head) && mm_quiescent(r->ts)) {
		mm_runlist_remove(&mm_frames_pending, r, NULL);
		mm_runlist_append(&mm_frames_ready, r);
	}
}

/*
 * Return frames (our addresses) to the pool. They were unmapped from
 * a client before ts (and need TLB quiescence), or, if ts == 0, were
 * never given out and are immediately reusable.
 */
static void
mm_frames_put(void *addr, unsigned long npages, u64_t ts)
{
	struct mm_runlist *l = ts ? &mm_frames_pending : &mm_frames_ready;
	struct mm_run     *r = l->tail;

	/* Extend the last run when freeing consecutive pages together */
	if (r && r->ts == ts && r->addr + r->npages * PAGE_SIZE == (vaddr_t)addr) {
		r->npages += npages;
		return;
	}
	r = mm_run_alloc();
	if (!r) {
		mm_stats.leaked += npages;
		return;
	}
	*r = (struct mm_run) {
		.addr   = (vaddr_t)addr,
		.npages = npages,
		.ts     = ts,
	};
	mm_runlist_append(l, r);
}

/* Find npages reusable frames contiguous in our address space, or NULL */
static void *
mm_frames_reuse(unsigned long npages)
{
	struct mm_run *r, *prev = NULL;
	void *ret;

	mm_frames_promote();
	for (r = mm_frames_ready.head; r; prev = r, r = r->next) {
		if (r->npages < npages) continue;

		ret        = (void *)r->addr;
		r->addr   += npages * PAGE_SIZE;
		r->npages -= npages;
		if (r->npages == 0) {
			mm_runlist_remove(&mm_frames_ready, r, prev);
			mm_run_free(r);
		}
		/* Don't leak the previous owner's data */
		memset(ret, 0, npages * PAGE_SIZE);
		mm_stats.reused += npages;

		return ret;
	}

	return NULL;
}

/*
 * Get npages frames, contiguous in our address space: reuse freed
 * frames, else allocate new memory, else wait for the TLB quiescence
 * of the frames being reclaimed.
 */
static void *
mm_frames_get(unsigned long npages)
{
	void *ret;

	ret = mm_frames_reuse(npages);
	if (ret) return ret;
	ret = crt_page_allocn(&cm_self()->comp, npages);
	if (ret) return ret;

	/* Out of memory. Wait for the pending frames. */
	while (mm_frames_pending.head) {
		cos_hw_tlbflush(MM_HW_CAP);
		while (!mm_quiescent(mm_frames_pending.tail->ts)) ;
		ret = mm_frames_reuse(npages);
		if (ret) return ret;
	}

	return NULL;
}

/* Free addresses [addr, addr + npages pages) of component c, unmapped before ts. */
static void
mm_va_put(struct cm_comp *c, vaddr_t addr, unsigned long npages, u64_t ts)
{
	struct mm_run *r = mm_run_alloc();

	/* Without a descriptor, the address range is simply not reused. */
	if (!r) return;
	*r = (struct mm_run) {
		.addr   = addr,
		.npages = npages,
		.ts     = ts,
	};
	mm_runlist_append(&c->va_free, r);
}

/*
 * Find a freed (and quiescent) range of npages in c's address space,
 * aligned on align, or return 0. Reusing addresses avoids growing the
 * component's address space, and page-tables, forever.
 */
static vaddr_t
mm_va_get(struct cm_comp *c, unsigned long npages, unsigned long align)
{
	struct mm_run *r, *prev = NULL;
	vaddr_t ret;

	for (r = c->va_free.head; r; prev = r, r = r->next) {
		/* Ordered by ts: the following ones aren't quiescent either */
		if (!mm_quiescent(r->ts)) break;
		if (r->npages < npages || r->addr % align != 0) continue;

		ret        = r->addr;
		r->addr   += npages * PAGE_SIZE;
		r->npages -= npages;
		if (r->npages == 0) {
			mm_runlist_remove(&c->va_free, r, prev);
			mm_run_free(r);
		}

		return ret;
	}

	return 0;
}

static int
mm_map_at(struct cm_comp *c, vaddr_t addr, void *page)
{
	return call_cap_op(cos_compinfo_get(cm_self()->comp.comp_res)->pgtbl_cap, CAPTBL_OP_CPY, (vaddr_t)page,
	                   cos_compinfo_get(c->comp.comp_res)->pgtbl_cap, addr, COS_PAGE_READABLE | COS_PAGE_WRITABLE);
}

static int
mm_unmap(struct cm_comp *c, vaddr_t addr)
{
	return cos_mem_remove(cos_compinfo_get(c->comp.comp_res)->pgtbl_cap, addr);
}

/*
 * The pages to map: either npages contiguous frames, or the pages of
 * a span (starting with page first, linked with snext).
 */
struct mm_pages_iter {
	void           *frames;
	struct mm_page *p;
	unsigned long   i;
};

static void *
mm_pages_next(struct mm_pages_iter *it)
{
	void *ret;

	if (it->p) {
		ret   = it->p->page;
		it->p = it->p->snext ? ss_page_get(it->p->snext) : NULL;
	} else {
		ret = it->frames + it->i * PAGE_SIZE;
	}
	it->i++;

	return ret;
}

/*
 * Map npages pages (frames, or the span starting at first) into
 * contiguous addresses in c, the first aligned on align. Reuse freed
 * addresses if possible. Returns the address, or 0.
 */
static vaddr_t
mm_map_pages(struct cm_comp *c, void *frames, struct mm_page *first, unsigned long npages, unsigned long align)
{
	struct mm_pages_iter it = { .frames = frames, .p = first, .i = 0 };
	unsigned long i, j;
	vaddr_t addr, a;

	addr = mm_va_get(c, npages, align);
	if (addr) {
		for (i = 0; i < npages; i++) {
			if (mm_map_at(c, addr + i * PAGE_SIZE, mm_pages_next(&it))) break;
		}
		if (i == npages) return addr;

		/*
		 * The kernel can refuse an address it doesn't yet see as
		 * quiescent (if the liveness id of its unmap was recycled).
		 * Undo, and use new addresses instead.
		 */
		for (j = 0; j < i; j++) {
			if (mm_unmap(c, addr + j * PAGE_SIZE)) BUG();
		}
		mm_va_put(c, addr, npages, ps_tsc());
	}

	it = (struct mm_pages_iter) { .frames = frames, .p = first, .i = 0 };
	for (i = 0; i < npages; i++) {
		if (crt_page_aliasn_aligned_in(mm_pages_next(&it), align, 1, &cm_self()->comp, &c->comp, &a)) return 0;
		if (i == 0) addr = a;
		/* We hold mm_lock, so the bump-allocated addresses are contiguous */
		assert(a == addr + i * PAGE_SIZE);
		align = PAGE_SIZE;
	}

	return addr;
}

/*
 * Page metadata allocation. ss_page_alloc is a linear scan of the
 * (huge) page table, so keep recently freed ids, and a high-water
 * mark of the ids we allocated.
 */
#define MM_PAGE_IDS_CACHE (1 << 16)
static unsigned int  mm_page_ids[MM_PAGE_IDS_CACHE];
static unsigned long mm_page_nids;
static unsigned int  mm_page_hw = 1;

static struct mm_page *
mm_page_meta_alloc(void)
{
	struct mm_page *p;

	while (mm_page_nids > 0) {
		p = ss_page_alloc_at_id(mm_page_ids[--mm_page_nids]);
		if (p) return p;
	}
	while (mm_page_hw < MM_NPAGES) {
		p = ss_page_alloc_at_id(mm_page_hw++);
		if (p) return p;
	}

	return ss_page_alloc();
}

static void
mm_page_meta_free(struct mm_page *p)
{
	unsigned int id = ss_page_id(p);

	ss_page_free(p);
	if (mm_page_nids < MM_PAGE_IDS_CACHE) mm_page_ids[mm_page_nids++] = id;
}

static struct cm_comp *
mm_mapping_comp(struct mm_mapping *m)
{
	if (!ss_state_is_allocated(m->comp)) return NULL;

	return (struct cm_comp *)ss_state_val_get(m->comp);
}

/*
 * Heap pages are found by (component, address) on free, using a hash
 * table of page ids chained through mm_page->hnext.
 */
#define MM_HASH_SZ (1 << 16)
static unsigned int mm_heap_hash[MM_HASH_SZ];

static unsigned long
mm_heap_hash_idx(struct cm_comp *c, vaddr_t addr)
{
	return ((addr >> PAGE_ORDER) ^ ((unsigned long)c->comp.id * 2654435761UL)) % MM_HASH_SZ;
}

static void
mm_heap_insert(struct cm_comp *c, struct mm_page *p)
{
	unsigned long h = mm_heap_hash_idx(c, p->mappings[0].addr);

	p->hnext        = mm_heap_hash[h];
	mm_heap_hash[h] = ss_page_id(p);
}

/* Find (and if remove, unlink) c's heap page at addr */
static struct mm_page *
mm_heap_lookup(struct cm_comp *c, vaddr_t addr, int remove)
{
	unsigned long h = mm_heap_hash_idx(c, addr);
	unsigned int *id;
	struct mm_page *p;

	for (id = &mm_heap_hash[h]; *id; id = &p->hnext) {
		p = ss_page_get(*id);
		assert(p);
		if (p->mappings[0].addr != addr || mm_mapping_comp(&p->mappings[0]) != c) continue;
		if (remove) *id = p->hnext;

		return p;
	}

	return NULL;
}

/**
 * Allocate num_pages pages, mapped into c at contiguous addresses
 * (the first aligned on align), and contiguous in physical memory.
 * Returns the first page, the others being linked by snext.
 *
 * - @heap - if set, register the pages for memmgr_heap_page_free
 */
static struct mm_page *
mm_pages_alloc(struct cm_comp *c, unsigned long num_pages, unsigned long align, int heap)
{
	struct mm_page *first = NULL, *prev = NULL, *p;
	struct mm_mapping *m;
	void   *frames;
	vaddr_t addr;
	unsigned long i;

	if (num_pages == 0 || align % PAGE_SIZE != 0) return NULL;
	frames = mm_frames_get(num_pages);
	if (!frames) return NULL;

	addr = mm_map_pages(c, frames, NULL, num_pages, align);
	if (!addr) {
		mm_frames_put(frames, num_pages, 0);
		return NULL;
	}

	for (i = 0; i < num_pages; i++) {
		p = mm_page_meta_alloc();
		if (!p) BUG();	/* The page metadata is sized for all of memory */

		m = &p->mappings[0];
		if (ss_state_alloc(&m->comp)) BUG();
		p->page = frames + i * PAGE_SIZE;
		m->addr = addr + i * PAGE_SIZE;
		ss_state_activate_with(&m->comp, (word_t)c);
		ss_page_activate(p);

		if (heap) mm_heap_insert(c, p);
		if (prev) prev->snext = ss_page_id(p);
		else      first = p;
		prev = p;
	}

	return first;
}

static vaddr_t
__memmgr_virt_to_phys(compid_t id, vaddr_t vaddr)
{
	struct cm_comp *c;

	c = ss_comp_get(id);
	if (!c) return 0;

	return call_cap_op(c->comp.comp_res->ci.pgtbl_cap, CAPTBL_OP_INTROSPECT, (vaddr_t)vaddr, 0, 0, 0);
}

vaddr_t
memmgr_virt_to_phys(vaddr_t vaddr)
{
	return __memmgr_virt_to_phys(cos_inv_token(), vaddr);
}

static void
contigmem_check(compid_t id, vaddr_t vaddr, int npages)
{
	vaddr_t paddr_pre = 0, paddr_next = 0;

	paddr_pre = __memmgr_virt_to_phys(id, vaddr);

	for (int i = 1; i < npages; i++) {
		paddr_next = __memmgr_virt_to_phys(id, vaddr + i * PAGE_SIZE);
		assert(paddr_next - paddr_pre == PAGE_SIZE);

		paddr_pre = paddr_next;
	}
}

/*
 * Track npages pages of the contiguous memory, starting at page and
 * mapped in c at vaddr. Returns the first page, the others are linked
 * with snext.
 */
static struct mm_page *
contigmem_pages_track(struct cm_comp *c, void *page, vaddr_t vaddr, unsigned long npages)
{
	struct mm_page *p, *initial = NULL, *prev = NULL;
	struct mm_mapping *m;
	unsigned long i;

	for (i = 0; i < npages; i++) {
		p = mm_page_meta_alloc();
		assert(p);

		m = &p->mappings[0];
		if (ss_state_alloc(&m->comp)) BUG();

		p->page = page + i * PAGE_SIZE;
		m->addr = vaddr + i * PAGE_SIZE;

		ss_state_activate_with(&m->comp, (word_t)c);
		ss_page_activate(p);

		if (prev) prev->snext = ss_page_id(p);
		else      initial = p;
		prev = p;
	}

	return initial;
}

vaddr_t
contigmem_alloc(unsigned long npages)
{
	struct cm_comp *c;
	vaddr_t vaddr;
	void *page;

	c = ss_comp_get(cos_inv_token());
	if (!c) return 0;

	ps_lock_take(&mm_lock);
	page = contig_phy_pages;
	if (crt_page_aliasn_aligned_in(page, PAGE_SIZE, npages, &cm_self()->comp, &c->comp, &vaddr)) BUG();
	contigmem_pages_track(c, page, vaddr, npages);

	contigmem_check(cos_inv_token(), (vaddr_t)vaddr, npages);
	contig_phy_pages += npages * PAGE_SIZE;
	assert((word_t)contig_phy_pages < (word_t)contig_phy_pages_base + CONTIG_PHY_PAGES * PAGE_SIZE);
	ps_lock_release(&mm_lock);

	return vaddr;
}

static cbuf_t
contigmem_shared_alloc_aligned_flags(unsigned long npages, unsigned long align, vaddr_t *pgaddr, int readonly)
{
	struct cm_comp *c;
	struct mm_page *initial;
	struct mm_span *s;
	vaddr_t vaddr;
	void *page;
	cbuf_t ret;

	c = ss_comp_get(cos_inv_token());
	if (!c) return 0;

	ps_lock_take(&mm_lock);
	s = ss_span_alloc();
	if (!s) ERR_THROW(0, done);

	page = contig_phy_pages;
	if (readonly) {
		if (crt_ro_page_aliasn_aligned_in(page, align, npages, &cm_self()->comp, &c->comp, &vaddr)) BUG();
	} else {
		if (crt_page_aliasn_aligned_in(page, align, npages, &cm_self()->comp, &c->comp, &vaddr)) BUG();
	}
	initial = contigmem_pages_track(c, page, vaddr, npages);

	s->page_off = ss_page_id(initial);
	s->n_pages  = npages;
	s->nmaps    = 1;
	ss_span_activate(s);

	ret = ss_span_id(s);

	*pgaddr = initial->mappings[0].addr;

	contigmem_check(cos_inv_token(), (vaddr_t)vaddr, npages);
	contig_phy_pages += npages * PAGE_SIZE;
	assert((word_t)contig_phy_pages < (word_t)contig_phy_pages_base + CONTIG_PHY_PAGES * PAGE_SIZE);
done:
	ps_lock_release(&mm_lock);

	return ret;
}

cbuf_t
contigmem_ro_shared_alloc_aligned(unsigned long npages, unsigned long align, vaddr_t *pgaddr)
{
	return contigmem_shared_alloc_aligned_flags(npages, align, pgaddr, 1);
}

cbuf_t
contigmem_shared_alloc_aligned(unsigned long npages, unsigned long align, vaddr_t *pgaddr)
{
	return contigmem_shared_alloc_aligned_flags(npages, align, pgaddr, 0);
}

vaddr_t
memmgr_heap_page_allocn_aligned(unsigned long num_pages, unsigned long align)
{
	struct cm_comp *c;
	struct mm_page *p;
	vaddr_t ret = 0;

	c = ss_comp_get(cos_inv_token());
	if (!c) return 0;

	ps_lock_take(&mm_lock);
	p = mm_pages_alloc(c, num_pages, align, 1);
	if (p) ret = p->mappings[0].addr;
	ps_lock_release(&mm_lock);

	return ret;
}

vaddr_t
memmgr_heap_page_allocn(unsigned long num_pages)
{
	return memmgr_heap_page_allocn_aligned(num_pages, PAGE_SIZE);
}

/*
 * Make the memory freed (unmapped) since the last shootdown reusable:
 * record when the unmaps completed, and start the TLB shootdown.
 */
static u64_t
mm_shootdown(void)
{
	u64_t ts = ps_tsc();

	/* The local flush in the shootdown is timestamped after ts */
	cos_hw_tlbflush(MM_HW_CAP);

	return ts;
}

int
memmgr_heap_page_freen(vaddr_t addr, unsigned long num_pages)
{
	struct cm_comp *c;
	struct mm_page *p;
	unsigned long i;
	u64_t ts;

	c = ss_comp_get(cos_inv_token());
	if (!c || num_pages == 0 || addr % PAGE_SIZE != 0) return -EINVAL;

	ps_lock_take(&mm_lock);
	/* All or nothing: first check that all pages are our heap pages */
	for (i = 0; i < num_pages; i++) {
		if (!mm_heap_lookup(c, addr + i * PAGE_SIZE, 0)) {
			ps_lock_release(&mm_lock);
			return -EINVAL;
		}
	}
	for (i = 0; i < num_pages; i++) {
		if (mm_unmap(c, addr + i * PAGE_SIZE)) BUG();
	}
	ts = mm_shootdown();
	for (i = 0; i < num_pages; i++) {
		p = mm_heap_lookup(c, addr + i * PAGE_SIZE, 1);
		assert(p);

		mm_frames_put(p->page, 1, ts);
		ss_state_free(&p->mappings[0].comp);
		mm_page_meta_free(p);
	}
	mm_va_put(c, addr, num_pages, ts);
	mm_stats.freed += num_pages;
	ps_lock_release(&mm_lock);

	return 0;
}

vaddr_t
memmgr_map_phys_to_virt(paddr_t paddr, size_t size)
{
	struct cm_comp *c;

	c = ss_comp_get(cos_inv_token());
	if (!c) return 0;

	return (vaddr_t)cos_hw_map(&c->comp.comp_res->ci, BOOT_CAPTBL_SELF_INITHW_BASE, paddr, size);
}

cbuf_t
memmgr_shared_page_allocn_aligned(unsigned long num_pages, unsigned long align, vaddr_t *pgaddr)
{
	struct cm_comp *c;
	struct mm_page *p;
	struct mm_span *s;
	cbuf_t ret = 0;

	*pgaddr = 0;
	c = ss_comp_get(cos_inv_token());
	if (!c) return 0;

	ps_lock_take(&mm_lock);
	s = ss_span_alloc();
	if (!s) ERR_THROW(0, done);
	p = mm_pages_alloc(c, num_pages, align, 0);
	if (!p) {
		ss_span_free(s);
		ERR_THROW(0, done);
	}

	s->page_off = ss_page_id(p);
	s->n_pages  = num_pages;
	s->nmaps    = 1;
	ss_span_activate(s);

	ret = ss_span_id(s);

	*pgaddr = p->mappings[0].addr;
done:
	ps_lock_release(&mm_lock);

	return ret;
}

cbuf_t
memmgr_shared_page_allocn(unsigned long num_pages, vaddr_t *pgaddr)
{
	return memmgr_shared_page_allocn_aligned(num_pages, PAGE_SIZE, pgaddr);
}

static struct mm_page *
mm_span_page_next(struct mm_page *p)
{
	return p->snext ? ss_page_get(p->snext) : NULL;
}

/*
 * Map span s into c (one more mapping of each page), the first page
 * aligned on align. The number of mappings of each page is limited by
 * `MM_MAPPINGS_MAX`. Returns the address, or 0.
 */
static vaddr_t
mm_span_map(struct mm_span *s, struct cm_comp *c, unsigned long align)
{
	struct mm_page *p;
	struct mm_mapping *m;
	vaddr_t addr;
	unsigned int i;
	int slot = 0;

	/*
	 * Every mapping maps all pages of the span, so all pages
	 * have the same free mapping slots.
	 */
	p = ss_page_get(s->page_off);
	assert(p);
	for (i = 1; i < MM_MAPPINGS_MAX; i++) {
		if (ss_state_is_free(p->mappings[i].comp)) {
			slot = i;
			break;
		}
	}
	if (!slot) return 0;

	addr = mm_map_pages(c, NULL, p, s->n_pages, align);
	if (!addr) return 0;

	for (i = 0; p; p = mm_span_page_next(p), i++) {
		m = &p->mappings[slot];
		if (ss_state_alloc(&m->comp)) BUG();
		m->addr = addr + i * PAGE_SIZE;
		ss_state_activate_with(&m->comp, (word_t)c);
	}
	assert(i == s->n_pages);
	s->nmaps++;

	return addr;
}

unsigned long
memmgr_shared_page_map_aligned_in_vm(cbuf_t id, unsigned long align, vaddr_t *pgaddr, compid_t cid)
{
	struct cm_comp *c;
	struct mm_span *s;
	unsigned long ret = 0;
	compid_t vmm = (compid_t)cos_inv_token();

	*pgaddr = 0;
	c = ss_comp_get(cid);
	if (!c) return 0;

	/* Only the vmm of this VM is allowed to call this interface */
	assert(vmm == c->comp.vm_comp_info.vmm_comp_id);

	ps_lock_take(&mm_lock);
	s = ss_span_get(id);
	if (!s) ERR_THROW(0, done);
	*pgaddr = mm_span_map(s, c, align);
	if (*pgaddr) ret = s->n_pages;
done:
	ps_lock_release(&mm_lock);

	return ret;
}

unsigned long
memmgr_shared_page_map_aligned(cbuf_t id, unsigned long align, vaddr_t *pgaddr)
{
	struct cm_comp *c;
	struct mm_span *s;
	unsigned long ret = 0;

	*pgaddr = 0;
	c = ss_comp_get(cos_inv_token());
	if (!c) return 0;

	ps_lock_take(&mm_lock);
	s = ss_span_get(id);
	if (!s) ERR_THROW(0, done);
	*pgaddr = mm_span_map(s, c, align);
	if (*pgaddr) ret = s->n_pages;
done:
	ps_lock_release(&mm_lock);

	return ret;
}

unsigned long
memmgr_shared_page_map(cbuf_t id, vaddr_t *pgaddr)
{
	return memmgr_shared_page_map_aligned(id, PAGE_SIZE, pgaddr);
}

int
memmgr_shared_page_unmap(cbuf_t id, vaddr_t addr)
{
	struct cm_comp *c;
	struct mm_span *s;
	struct mm_page *p, *next;
	unsigned int i;
	int slot = -1, ret = 0;
	u64_t ts;

	c = ss_comp_get(cos_inv_token());
	if (!c) return -EINVAL;

	ps_lock_take(&mm_lock);
	s = ss_span_get(id);
	if (!s) ERR_THROW(-EINVAL, done);

	/* Which of the span's mappings is ours at addr? */
	p = ss_page_get(s->page_off);
	assert(p);
	for (i = 0; i < MM_MAPPINGS_MAX; i++) {
		if (mm_mapping_comp(&p->mappings[i]) == c && p->mappings[i].addr == addr) {
			slot = i;
			break;
		}
	}
	if (slot < 0) ERR_THROW(-EINVAL, done);

	for (; p; p = mm_span_page_next(p)) {
		if (mm_unmap(c, p->mappings[slot].addr)) BUG();
		ss_state_free(&p->mappings[slot].comp);
	}
	ts = mm_shootdown();
	mm_va_put(c, addr, s->n_pages, ts);

	/* Last mapping: free the memory and the span */
	if (--s->nmaps == 0) {
		for (p = ss_page_get(s->page_off); p; p = next) {
			next = mm_span_page_next(p);
			mm_frames_put(p->page, 1, ts);
			mm_page_meta_free(p);
		}
		mm_stats.freed += s->n_pages;
		ss_span_free(s);
	}
done:
	ps_lock_release(&mm_lock);

	return ret;
}

static compid_t
capmgr_comp_sched_hier_get(compid_t cid)
{
#define SCHED_STR_SZ 36 /* base-10 32 bit int + "sched_hierarchy/" */
	char *sched;
	char serialized[SCHED_STR_SZ];

	snprintf(serialized, SCHED_STR_SZ, "scheduler_hierarchy/%ld", cid);
	sched = args_get(serialized);
	if (!sched) return 0;

	return atoi(sched);
}

static compid_t
capmgr_comp_sched_get(compid_t cid)
{
#define INIT_STR_SZ 35 /* base-10 32 bit int + "init_hierarchy/" */
	char *sched;
	char serialized[SCHED_STR_SZ];

	snprintf(serialized, SCHED_STR_SZ, "init_hierarchy/%ld", cid);
	sched = args_get(serialized);
	if (!sched) return 0;

	return atoi(sched);
}

static void
capmgr_execution_init(int is_init_core)
{
	struct initargs cap_entries, exec_entries, curr;
	struct initargs_iter i;
	vaddr_t vasfr = 0;
	capid_t capfr = 0;
	int ret, cont;

	/* Create execution in the relevant components */
	ret = args_get_entry("execute", &exec_entries);
	assert(!ret);
	if (is_init_core) printc("Capmgr: %d components that need execution\n", args_len(&exec_entries));
	for (cont = args_iter(&exec_entries, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		struct cm_comp    *cmc;
		struct crt_comp   *comp;
		int      keylen;
		compid_t id        = atoi(args_key(&curr, &keylen));
		char    *exec_type = args_value(&curr);
		struct crt_comp_exec_context ctxt = { 0 };

		assert(exec_type);
		assert(id != cos_compid());
		cmc  = ss_comp_get(id);
		assert(cmc);
		comp = &cmc->comp;

		if (!strcmp(exec_type, "sched")) {
			struct cm_rcv *r = ss_rcv_alloc();

			assert(r);
			if (crt_comp_exec(comp, crt_comp_exec_sched_init(&ctxt, &r->rcv))) BUG();
			ss_rcv_activate(r);
			cmc->sched_rcv[cos_cpuid()] = r;
			if (is_init_core) printc("\tCreated scheduling execution for %ld\n", id);
		} else if (!strcmp(exec_type, "init")) {
			struct cm_thd *t = ss_thd_alloc();

			assert(t);
			if (crt_comp_exec(comp, crt_comp_exec_thd_init(&ctxt, &t->thd))) BUG();
			ss_thd_activate(t);
			if (is_init_core) printc("\tCreated thread for %ld\n", id);
		} else {
			printc("Error: Found unknown execution schedule type %s.\n", exec_type);
			BUG();
		}
	}

	return;
}

static void
capmgr_comp_init(void)
{
	struct initargs cap_entries, exec_entries, shared_comps, curr;
	struct initargs_iter i;
	vaddr_t vasfr = 0;
	capid_t capfr = 0;
	int ret, cont;
	struct cm_comp *comp;

	int remaining = 0;
	int num_comps = 0;

	/* ...then those that we're responsible for... */
	ret = args_get_entry("captbl", &cap_entries);
	assert(!ret);
	printc("Capmgr: processing %d capabilities for components that have already been booted\n", args_len(&cap_entries));

	for (cont = args_iter(&cap_entries, &i, &curr) ; cont ; ) {
		compid_t sched_id;
		compid_t id = 0;
		struct crt_comp_resources comp_res = { 0 };
		int keylen;
		int j;
		char id_serialized[16]; 	/* serialization of the id number */
		char *name;

		for (j = 0 ; j < 3 ; j++, cont = args_iter_next(&i, &curr)) {
			capid_t capid = atoi(args_key(&curr, &keylen));
			char   *type  = args_get_from("type", &curr);

			assert(capid && type);

			if (j == 0) id = atoi(args_get_from("target", &curr));
			else        assert((compid_t)atoi(args_get_from("target", &curr)) == id);

			if (!strcmp(type, "comp")) {
				comp_res.compc = capid;
			} else if (!strcmp(type, "captbl")) {
				comp_res.ctc = capid;
			} else if (!strcmp(type, "pgtbl")) {
				comp_res.ptc = capid;
			} else {
				BUG();
			}
		}
		assert(id);

		assert(comp_res.compc && comp_res.ctc && comp_res.ptc);
		sched_id = capmgr_comp_sched_get(id);
		if (sched_id == 0) {
			sched_id = capmgr_comp_sched_hier_get(id);
		}
		comp_res.heap_ptr        = addr_get(id, ADDR_HEAP_FRONTIER);
		comp_res.captbl_frontier = addr_get(id, ADDR_CAPTBL_FRONTIER);

		snprintf(id_serialized, 20, "names/%ld", id);
		name = args_get(id_serialized);
		assert(name);
		printc("\tCreating component %s: id %ld\n", name, id);
		printc("\t\tcaptbl:%ld, pgtbl:%ld, comp:%ld, captbl/pgtbl frontiers %d & %lx, sched %ld\n",
		       comp_res.ctc, comp_res.ptc, comp_res.compc, comp_res.captbl_frontier, comp_res.heap_ptr, sched_id);
		comp = cm_comp_alloc_with(name, id, &comp_res);
		assert(comp);
	}

	/* Create ULK memory region for UL sinvs and map it into comps that need it */
	ret = crt_ulk_init();
	assert(!ret);
	ret = args_get_entry("addrspc_shared", &shared_comps);
	assert(!ret);
	for (cont = args_iter(&shared_comps, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		compid_t        id  = atoi(args_value(&curr));
		struct cm_comp *cmc = ss_comp_get(id);

		/* if this fails, we already aliased the ICB memory into this shared pt */
		crt_ulk_map_in(&cmc->comp);
	}

	return;
}

static inline struct crt_comp *
crtcomp_get(compid_t id)
{
	struct cm_comp *c = ss_comp_get(id);

	assert(c);

	return &c->comp;
}

void
capmgr_set_tls(thdcap_t cap, void* tls_addr)
{
	compid_t cid = (compid_t)cos_inv_token();
	struct crt_comp* c = crtcomp_get(cid);

	cos_thd_mod(&c->comp_res->ci, cap, tls_addr);
}

void
init_done(int parallel_init, init_main_t main_type)
{
	compid_t client = (compid_t)cos_inv_token();
	struct crt_comp *c;

	assert(client > 0 && client <= MAX_NUM_COMPS);
	c = crtcomp_get(client);

	crt_compinit_done(c, parallel_init, main_type);

	return;
}


void
init_exit(int retval)
{
	compid_t client = (compid_t)cos_inv_token();
	struct crt_comp *c;

	assert(client > 0 && client <= MAX_NUM_COMPS);
	c = crtcomp_get(client);
	assert(c);

	crt_compinit_exit(c, retval);

	while (1) ;
}

thdcap_t
capmgr_thd_create_ext(spdid_t client, thdclosure_index_t idx, thdid_t *tid)
{
	compid_t schedid = (compid_t)cos_inv_token();
	struct cm_thd *t;
	struct cm_comp *s, *c;

	c = ss_comp_get(client);

	if (schedid != capmgr_comp_sched_get(client)) {
		if (c->comp.flags & CRT_COMP_VM) {
			schedid = c->comp.vm_comp_info.vmm_comp_id;
		} else {
			/* don't have permission to create execution in that component. */
			printc("capmgr: Component asking to create thread from %ld in %ld -- no permission.\n",
			schedid, (compid_t)client);
			return 0;
		}
	}

	s = ss_comp_get(schedid);
	if (!c || !s) return 0;
	t = cm_thd_alloc_in(c, s, idx);
	if (!t) {
		/* TODO: release resources */
		return 0;
	}
	*tid = t->thd.tid;

	return t->aliased_cap;
}

thdcap_t
capmgr_initthd_create(spdid_t client, thdid_t *tid)
{
	return capmgr_thd_create_ext(client, 0, tid);
}

thdcap_t  capmgr_initaep_create(spdid_t child, struct cos_aep_info *aep, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax, asndcap_t *sndret) { BUG(); return 0; }

thdcap_t
capmgr_thd_create_thunk(thdclosure_index_t idx, thdid_t *tid)
{
	compid_t client = (compid_t)cos_inv_token();
	struct cm_thd *t;
	struct cm_comp *c;

	assert(client > 0 && client <= MAX_NUM_COMPS);
	c = ss_comp_get(client);
	t = cm_thd_alloc_in(c, c, idx);
	if (!t) {
		/* TODO: release resources */
		return 0;
	}

	*tid = t->thd.tid;

	return t->aliased_cap;
}

arcvcap_t
capmgr_rcv_create(thdclosure_index_t idx, crt_rcv_flags_t flags, asndcap_t *asnd, thdcap_t *thdcap, thdid_t *tid)
{
	compid_t sched = (compid_t)cos_inv_token();
	struct cm_comp   *c;
	struct cm_rcv    *r;
	struct cm_asnd   *s;
	arcvcap_t  child_rcv;

	/* TODO: Should replace this static check with one that uses the initargs to validate that the call is coming from the scheduler. */
	c = ss_comp_get(sched);
	if (!c || !c->sched_rcv[cos_cpuid()]) return 0;

	r = cm_rcv_alloc_in(c, &c->sched_rcv[cos_cpuid()]->rcv, idx, flags, thdcap, tid);
	if (!r) return 0;

	s = cm_asnd_alloc_in(&c->comp, &r->rcv);
	assert(s);
	r->asnd = s;
	*asnd = s->aliased_cap;

	return r->aliased_cap;
}

/*
 * Common checks for releasing a thread: it must exist on this core,
 * and not be the thread executing this call (the kernel would refuse
 * to free it anyway).
 */
static int
cm_release_check(thdid_t tid, coreid_t core)
{
	if (core != cos_cpuid()) return -EINVAL;
	if (tid == cos_thdid()) return -EBUSY;

	return 0;
}

int
capmgr_thd_release(thdid_t tid)
{
	compid_t caller = (compid_t)cos_inv_token();
	struct cm_thd *t = NULL;
	int i, ret;

	ps_lock_take(&cm_lock);
	for (i = 1; i <= MAX_NUM_THREADS; i++) {
		t = ss_thd_get(i);
		if (t && t->thd.tid == tid) break;
		t = NULL;
	}
	if (!t) ERR_THROW(-EINVAL, done);
	/*
	 * Only the component the thread capability was given to (the
	 * scheduler) can release it. Threads created at boot (no
	 * sched) cannot be released.
	 */
	if (!t->sched || t->sched->comp.id != caller) ERR_THROW(-EPERM, done);
	if ((ret = cm_release_check(tid, t->core))) goto done;

	/* First the scheduler's capability, then the thread itself */
	ret = crt_cap_alias_free(&t->sched->comp, t->aliased_cap, CAP_THD);
	if (ret) goto done;
	ret = crt_thd_free(&t->thd);
	if (ret) {
		/* The alias is gone, so the thread can't be used, but its memory is leaked. */
		printc("capmgr: could not free thread %lu (err %d).\n", (unsigned long)tid, ret);
		goto done;
	}
	ss_thd_free(t);
done:
	ps_lock_release(&cm_lock);

	return ret;
}

int
capmgr_rcv_release(thdid_t tid)
{
	compid_t caller = (compid_t)cos_inv_token();
	struct cm_rcv  *r = NULL;
	struct cm_asnd *s;
	struct cm_comp *c;
	struct cm_tcap_pool *pool;
	tcap_t tc;
	int i, ret;

	ps_lock_take(&cm_lock);
	for (i = 1; i <= MAX_NUM_THREADS; i++) {
		r = ss_rcv_get(i);
		if (r && r->client && r->client->comp.id == caller && r->rcv.thd.tid == tid) break;
		r = NULL;
	}
	if (!r) ERR_THROW(-EINVAL, done);
	if ((ret = cm_release_check(tid, r->core))) goto done;
	c = r->client;

	/*
	 * Dependency order: the asnd (that sends to the rcv), then the
	 * arcv (the kernel only frees threads not bound to an arcv),
	 * then the thread. Aliases before the originals.
	 */
	s = r->asnd;
	if (s) {
		if ((ret = crt_cap_alias_free(&c->comp, s->aliased_cap, CAP_ASND))) goto done;
		if ((ret = crt_asnd_free(&s->asnd))) goto done;
		ss_asnd_free(s);
		r->asnd = NULL;
	}
	/* Deactivating the first copy of the arcv unbinds it in the kernel */
	if ((ret = crt_cap_alias_free(&c->comp, r->aliased_cap, CAP_ARCV))) goto done;
	if ((ret = crt_cap_alias_free(&c->comp, r->aliased_thd, CAP_THD))) goto done;
	if ((ret = crt_rcv_free(&r->rcv, &tc))) {
		printc("capmgr: could not free rcv thread %lu (err %d).\n", (unsigned long)tid, ret);
		goto done;
	}

	/* Keep the tcap (and its alias in c) for c's next rcv on this core */
	pool = &c->tcap_pool[r->core];
	if (pool->n < CM_TCAP_POOL_SZ) {
		pool->tcaps[pool->n].tc    = tc;
		pool->tcaps[pool->n].alias = r->aliased_tc;
		pool->n++;
	}
	ss_rcv_free(r);
done:
	ps_lock_release(&cm_lock);

	return ret;
}

static struct cm_thd *
capmgr_get_thd(thdid_t tid)
{
	/* This can only be used on the slow path */
	struct cm_thd *thd;
	for (int i = 1; i < MAX_NUM_THREADS; i++) {
		thd = ss_thd_get(i);
		if (thd == NULL) continue;
		if (thd->thd.tid == tid) return thd;
	}
	
	return NULL;
}

compid_t
capmgr_vm_comp_create(u64_t mem_sz)
{
	/* TODO: allocate name rather than make it static */
	char *name = "vmlinux";
	prot_domain_t protdom = 0;
	compid_t id = crt_comp_id_new();

	/* Allocate a new comp */
	struct cm_comp *c = ss_comp_alloc_at_id(id);
	assert(c);

	crt_comp_vm_create(&c->comp, name, id, protdom);
	cm_comp_reclaim_init(c);

	ss_comp_activate(c);

	return id;
}

vaddr_t
capmgr_vm_shared_kernel_page_create_at(compid_t comp_id, vaddr_t addr)
{
	struct cm_comp *vmm = ss_comp_get(cos_inv_token());
	struct cm_comp *vm = ss_comp_get(comp_id);

	assert(vmm && vm);
	assert(vm->comp.vm_comp_info.vmm_comp_id == vmm->comp.id);

	return crt_comp_shared_kernel_page_alloc_at(&vm->comp, addr);
}

vaddr_t
capmgr_shared_kernel_page_create(vaddr_t *resource)
{
	struct cm_comp *c = ss_comp_get(cos_inv_token());
	assert(c);

	return crt_comp_shared_kernel_page_alloc(&c->comp, resource);
}

capid_t
capmgr_vm_vmcs_create(void)
{
	struct cm_comp *c = ss_comp_get(cos_inv_token());
	assert(c);

	return crt_vm_vmcs_create(&c->comp);
}

capid_t
capmgr_vm_msr_bitmap_create(void)
{
	struct cm_comp *c = ss_comp_get(cos_inv_token());
	assert(c);

	return crt_vm_msr_bitmap_create(&c->comp);
}

capid_t
capmgr_vm_lapic_create(vaddr_t *page)
{
	struct cm_comp *c = ss_comp_get(cos_inv_token());
	assert(c);

	return crt_vm_lapic_create(&c->comp, page);
}

capid_t
capmgr_vm_shared_region_create(vaddr_t *page)
{
	struct cm_comp *c = ss_comp_get(cos_inv_token());
	assert(c);

	return crt_vm_shared_region_create(&c->comp, page);
}

/* lapic access page is very special: all vcpu needs to set the same mem page */
capid_t
capmgr_vm_lapic_access_create(vaddr_t mem)
{
	struct cm_comp *c = ss_comp_get(cos_inv_token());
	assert(c);

	return crt_vm_lapic_access_create(&c->comp, mem);
}

capid_t
capmgr_vm_vmcb_create(vm_vmcscap_t vmcs_cap, vm_msrbitmapcap_t msr_bitmap_cap, vm_lapicaccesscap_t lapic_access_cap, vm_lapiccap_t lapic_cap, vm_shared_mem_t shared_mem_cap, thdid_t handler_thd_id, word_t vpid)
{
	struct cm_thd *handler_thd;
	struct cm_comp *c = ss_comp_get(cos_inv_token());
	assert(c);

	handler_thd = capmgr_get_thd(handler_thd_id);

	return crt_vm_vmcb_create(&c->comp, vmcs_cap, msr_bitmap_cap, lapic_access_cap, lapic_cap, shared_mem_cap, handler_thd->thd.cap, vpid);
}

thdcap_t
capmgr_vm_vcpu_create(compid_t vm_comp, vm_vmcb_t vmcb_cap, thdid_t *tid)
{
	/*
	 * Set vmcb_cap as the closure_id to pass this cap into kernel,
	 * this is OK since vm thread doesn't need closure_id, and the 
	 * existing thead creation api has used all arguments(thus it's
	 * not easy to modify it).
	 */
	return capmgr_thd_create_ext(vm_comp, vmcb_cap, tid);
}

thdcap_t  capmgr_aep_create_thunk(struct cos_aep_info *a, thdclosure_index_t idx, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax) { BUG(); return 0; }
thdcap_t  capmgr_aep_create_ext(spdid_t child, struct cos_aep_info *a, thdclosure_index_t idx, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax, arcvcap_t *extrcv) { BUG(); return 0; }
asndcap_t capmgr_asnd_create(spdid_t child, thdid_t t) { BUG(); return 0; }
asndcap_t capmgr_asnd_rcv_create(arcvcap_t rcv) { BUG(); return 0; }
asndcap_t capmgr_asnd_key_create(cos_channelkey_t key) { BUG(); return 0; }

void capmgr_create_noop(void) { return; }

void
cos_init(void)
{
	struct cos_defcompinfo *defci = cos_defcompinfo_curr_get();
	struct cos_compinfo    *ci    = cos_compinfo_get(defci);
	struct initargs curr, comps;
	struct initargs_iter i;
	int cont, found_shared = 0;
	int ret;

	/* 
	 * FIXME: This is a hack since the booter's __thdid_alloc is
	 *        initialized the same as ours. Need a better solution.
	 */
	extern unsigned long __thdid_alloc;
	__thdid_alloc = NUM_CPU * 4;

	printc("Starting the capability manager.\n");
	assert(atol(args_get("captbl_end")) >= BOOT_CAPTBL_FREE);

	/* Example code to walk through the components in shared address spaces */
	printc("Components in shared address spaces: ");
	ret = args_get_entry("addrspc_shared", &comps);
	assert(!ret);
	for (cont = args_iter(&comps, &i, &curr) ; cont ; cont = args_iter_next(&i, &curr)) {
		compid_t id = atoi(args_value(&curr));

		found_shared = 1;
		printc("%ld ", id);
	}
	if (!found_shared) {
		printc("none");
	}
	printc("\n");

	/* Get our house in order. Initialize ourself and our data-structures */
	cos_meminfo_init(&(ci->mi), BOOT_MEM_KM_BASE, COS_MEM_KERN_PA_SZ, BOOT_CAPTBL_SELF_UNTYPED_PT);
	cos_defcompinfo_init();

	/*
	 * FIXME: this is a hack. The captbl_end variable does *not*
	 * take into account the synchronous invocations yet. That is
	 * because we don't want to modify the image to include it
	 * after we've sealed in all initargs and sinvs. Regardless,
	 * that is the *correct* approach.
	 */
	cos_comp_capfrontier_update(ci, addr_get(cos_compid(), ADDR_CAPTBL_FRONTIER), 0);
	if (!cm_comp_self_alloc("capmgr")) BUG();
	cm_comp_reclaim_init(cm_self());
	cos_kmem_reclaim_init(ci, &cm_kmem_reclaim);
	/* Initialize the other component's for which we're responsible */
	capmgr_comp_init();

	/* Reserve some continuous pages */
	contig_phy_pages = crt_page_allocn(&cm_self()->comp, CONTIG_PHY_PAGES);
	contig_phy_pages_base = contig_phy_pages;
	contigmem_check(cos_compid(), (vaddr_t)contig_phy_pages, CONTIG_PHY_PAGES);

	return;
}

void
cos_parallel_init(coreid_t cid, int init_core, int ncores)
{
	cos_defcompinfo_sched_init();
	capmgr_execution_init(init_core);
}

void
parallel_main(coreid_t cid)
{
	crt_compinit_execute(crtcomp_get);
}
