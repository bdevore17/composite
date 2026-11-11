/**
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * Component fault isolation and recovery: see include/recovery.h for
 * the design.
 */

#include "include/shared/cos_types.h"
#include "include/captbl.h"
#include "include/thd.h"
#include "include/inv.h"
#include "include/component.h"
#include "include/liveness_tbl.h"
#include "include/chal/call_convention.h"
#include "include/chal/cpuid.h"
#include "include/chal/defs.h"
#include "include/chal/chal_proto.h"
#include "include/recovery.h"

#define COMP_RECOVERY_MAX MAX_NUM_COMPS

/* A recoverable component, and its fault handler */
struct comp_recovery {
	livenessid_t     lid; /* the component (its liveness id), 0 if the slot is free */
	struct comp_info handler;
	vaddr_t          handler_entry;
	invtoken_t       handler_token;
	unsigned long    nfaults;
};

static struct comp_recovery comp_recovery_tbl[COMP_RECOVERY_MAX];

/* All threads of each core (see thd_core_list_add) */
struct list thd_core_list[NUM_CPU];

/*
 * Quiescence requests: each core records the last request it
 * processed (ack), and, per registered component (slot), how many of
 * its threads execute in other components on behalf of the component
 * (nested), as of that request.
 */
static int recovery_req;
static struct {
	int          ack;
	unsigned int nested[COMP_RECOVERY_MAX];
} CACHE_ALIGNED recovery_core[NUM_CPU];

/* How threads were unwound (for diagnostics): see comp_recovery_fault's message */
enum
{
	UNWIND_FAULT,
	UNWIND_IPI,
	UNWIND_SWITCH,
	UNWIND_RET,
	UNWIND_MAX
};
static int recovery_unwinds[UNWIND_MAX];

/* The number of cores that booted (lapic.c) */
extern volatile int ncpus;

#define LOAD(x) (*(volatile typeof(x) *)&(x))
/* An allocated, but not yet published, registration slot */
#define LID_RESERVED 0xFFFFFFFF

static int
comp_recovery_slot(livenessid_t lid)
{
	int i;

	if (lid == 0) return -1;
	for (i = 0; i < COMP_RECOVERY_MAX; i++) {
		if (LOAD(comp_recovery_tbl[i].lid) == lid) return i;
	}

	return -1;
}

static inline int
frame_valid(struct invstk_entry *f)
{
	return ltbl_frame_valid(&f->comp_info.liveness);
}

/*
 * Unwind thd's invocation stack from its top frame, `top`: pop it if
 * `drop_top`, then all frames that cannot be returned to, and set up
 * regs to return from the invocation made by the first valid frame,
 * with -ECOMPRESET (and the registers it saved when it invoked).
 * Returns the index of that frame (top if there is nothing to
 * unwind), or -1 if there is none (nothing is changed then).
 */
static int
invstk_unwind(struct thread *thd, int top, int drop_top, struct pt_regs *regs)
{
	struct invstk_entry *f;
	int                  i = drop_top ? top - 1 : top;

	while (i >= 0 && !frame_valid(&thd->invstk[i])) i--;
	if (i < 0) return -1;
	if (i == top) return top;

	f = &thd->invstk[i];
	__userregs_inv_unwind(regs, -ECOMPRESET, f->sp, f->ip, f->saved_regs);
	/* Drop the user-level invocations made above this frame */
	if (thd->ulk_invstk) thd->ulk_invstk->top = thd->invstk[i + 1].ulk_stkoff;
	f->flags = 0;

	return i;
}

/* The current thread was unwound to frame i: switch to its component */
static void
curr_unwound(struct thread *thd, int i, struct cos_cpu_local_info *cos_info)
{
	struct invstk_entry *f = &thd->invstk[i];

	cos_info->invstk_top = i;
	pgtbl_update(&f->comp_info.pgtblinfo);
	chal_protdom_write(f->protdom);
}

int
comp_recovery_sret_unwind(struct thread *thd, struct pt_regs *regs, struct cos_cpu_local_info *cos_info)
{
	int i;

	i = invstk_unwind(thd, curr_invstk_top(cos_info), 0, regs);
	if (i < 0) return -1;
	curr_unwound(thd, i, cos_info);
	cos_faa(&recovery_unwinds[UNWIND_RET], 1);

	return 0;
}

int
comp_recovery_thd_resume(struct thread *thd)
{
	int i;

	if (likely(frame_valid(&thd->invstk[thd->invstk_top]))) return 0;
	if (thd->thd_type != THD_TYPE_HOST) return -1;

	i = invstk_unwind(thd, thd->invstk_top, 0, &thd->regs);
	if (i < 0) return -1;
	thd->invstk_top = i;
	cos_faa(&recovery_unwinds[UNWIND_SWITCH], 1);
	/*
	 * A receive (in the component) that is abandoned returns the
	 * error, not the events, which stay pending.
	 */
	thd->state &= ~THD_STATE_RCVING;

	return 0;
}

int
comp_recovery_fault(struct pt_regs *regs, cos_flt_off type, unsigned long addr)
{
	struct cos_cpu_local_info *cos_info = cos_cpu_local_info();
	struct thread             *thd      = thd_current(cos_info);
	struct comp_recovery      *r;
	struct invstk_entry       *f, *h;
	livenessid_t               lid;
	unsigned long              fault_ip;
	u64_t                     *ep, e;
	int                        top, i, slot, redirect;

	if (!__userregs_from_user(regs) || !thd || thd->thd_type != THD_TYPE_HOST) return 0;
	top = curr_invstk_top(cos_info);
	/* A fault in the thread's own component can't be returned from */
	if (top == 0) return 0;
	f = &thd->invstk[top];
	/* Faults in user-level invocations (shared address spaces) aren't supported */
	if (thd->ulk_invstk && thd->ulk_invstk->top > f->ulk_stkoff) return 0;
	lid  = f->comp_info.liveness.id;
	slot = comp_recovery_slot(lid);
	if (slot < 0) return 0;
	r = &comp_recovery_tbl[slot];

	/*
	 * Can we redirect the thread to the fault handler? Its
	 * component must not already be on the thread's invocation
	 * stack (it would reuse the stack of that invocation).
	 */
	redirect = top + 1 < THD_INVSTK_MAXSZ && ltbl_isalive(&r->handler.liveness);
	for (i = 0; redirect && i <= top; i++) {
		if (thd->invstk[i].comp_info.liveness.id == r->handler.liveness.id) redirect = 0;
	}

	/* Suspend the component, unless it already is */
	ep = ltbl_epoch_ptr(lid);
	while (1) {
		e = *ep;
		if (e & LTBL_EPOCH_SUSPENDED) {
			redirect = 0;
			break;
		}
		/* We can't recover the component: keep the previous behavior */
		if (!redirect) return 0;
		if (cos_cas((unsigned long *)ep, (unsigned long)e, (unsigned long)(e | LTBL_EPOCH_SUSPENDED))) break;
	}

	if (!redirect) {
		/*
		 * The component is being recovered (we probably faulted
		 * because of its corrupted state, or during its
		 * re-initialization): return the error to the caller.
		 */
		i = invstk_unwind(thd, top, 1, regs);
		if (i < 0) return 0;
		curr_unwound(thd, i, cos_info);
		cos_faa(&recovery_unwinds[UNWIND_FAULT], 1);

		return 1;
	}

	fault_ip = regs->ip;
	r->nfaults++;
	if (r->nfaults <= 3 || r->nfaults % 100 == 0) {
		printk("cos: thread %d faulted in a recoverable component (liveness id %d, fault %d @ ip %p, addr %p), "
		       "redirecting to its manager (fault #%lu).\n",
		       thd->tid, lid, type, (void *)fault_ip, (void *)addr, r->nfaults);
		printk("cos: threads unwound so far: %d at faults, %d by IPI, %d when switched to, %d on return.\n",
		       recovery_unwinds[UNWIND_FAULT], recovery_unwinds[UNWIND_IPI], recovery_unwinds[UNWIND_SWITCH],
		       recovery_unwinds[UNWIND_RET]);
	}

	/* The faulting frame, as if it invoked the handler (it will never be returned to) */
	f->ip      = fault_ip;
	f->sp      = regs->sp;
	f->protdom = chal_protdom_read();
	__userregs_inv_save(regs, f->saved_regs);
	f->flags |= INVSTK_FAULTED;

	h = &thd->invstk[top + 1];
	memcpy(&h->comp_info, &r->handler, sizeof(struct comp_info));
	h->comp_info.liveness.epoch = *ltbl_epoch_ptr(r->handler.liveness.id);
	h->ip = h->sp = 0;
	h->flags      = 0;
	h->protdom    = r->handler.pgtblinfo.protdom;
	h->ulk_stkoff = thd->ulk_invstk ? thd->ulk_invstk->top : 0;
	curr_invstk_inc(cos_info);

	pgtbl_update(&h->comp_info.pgtblinfo);
	chal_protdom_write(h->comp_info.pgtblinfo.protdom);
	__userregs_setinv(regs, thd->tid | (get_cpuid() << 16), r->handler_token, r->handler_entry);
	__userregs_setinvargs(regs, type, addr, fault_ip, thd->tid);

	return 1;
}

/*
 * Count, per recoverable component, the threads of this core that
 * execute in other components on behalf of it (have an invalid frame
 * below their top one). Frames of threads redirected to a fault
 * handler (the handler executes on behalf of the manager), and base
 * frames (threads of the component itself, that can never run
 * again), don't count.
 */
static void
recovery_scan(struct thread *curr, struct cos_cpu_local_info *cos_info)
{
	unsigned int      nested[COMP_RECOVERY_MAX] = {0};
	struct list_node *n;
	struct list      *l = thd_list_core();
	int               i, slot;

	for (n = l->l.next; n != &l->l; n = n->next) {
		struct thread *t    = n->container;
		u64_t          seen = 0;
		int            top;

		if (t->thd_type != THD_TYPE_HOST) continue;
		top = (t == curr) ? curr_invstk_top(cos_info) : t->invstk_top;
		for (i = 1; i < top; i++) {
			struct invstk_entry *f = &t->invstk[i];

			if ((f->flags & INVSTK_FAULTED) || frame_valid(f)) continue;
			slot = comp_recovery_slot(f->comp_info.liveness.id);
			if (slot < 0 || (seen & (1ULL << slot))) continue;
			seen |= 1ULL << slot;
			nested[slot]++;
		}
	}
	/* Publish complete counts (readers may see a mix of two complete scans) */
	for (i = 0; i < COMP_RECOVERY_MAX; i++) recovery_core[get_cpuid()].nested[i] = nested[i];
}

int
comp_recovery_ipi(struct pt_regs *regs)
{
	struct cos_cpu_local_info *cos_info = cos_cpu_local_info();
	struct thread             *thd      = thd_current(cos_info);
	int                        req      = LOAD(recovery_req);
	int                        i;

	/* Stop executing in invalidated frames: return to the caller */
	if (__userregs_from_user(regs) && thd && thd->thd_type == THD_TYPE_HOST) {
		int top = curr_invstk_top(cos_info);

		if (!frame_valid(&thd->invstk[top])) {
			i = invstk_unwind(thd, top, 0, regs);
			if (i >= 0) {
				curr_unwound(thd, i, cos_info);
				cos_faa(&recovery_unwinds[UNWIND_IPI], 1);
			}
		}
	}
	recovery_scan(thd, cos_info);
	cos_mem_fence();
	recovery_core[get_cpuid()].ack = req;

	return 1;
}

/* Has every core processed quiescence request req? */
static int
recovery_quiescent(int req)
{
	int i;

	for (i = 0; i < ncpus; i++) {
		if (LOAD(recovery_core[i].ack) - req < 0) return 0;
	}

	return 1;
}

int
comp_recovery_op(struct captbl *ct, struct cap_comp *compc, syscall_op_t op, struct thread *thd, struct pt_regs *regs,
                 struct cos_cpu_local_info *cos_info, int *thd_switch)
{
	livenessid_t lid  = compc->info.liveness.id;
	u64_t       *ep   = ltbl_epoch_ptr(lid);
	int          slot = comp_recovery_slot(lid);

	switch (op) {
	case CAPTBL_OP_COMP_RECOVERY_REGISTER: {
		capid_t               sinvcap = __userregs_get1(regs);
		struct cap_sinv      *s       = (struct cap_sinv *)captbl_lkup(ct, sinvcap);
		struct comp_recovery *r;
		int                   i;

		if (!CAP_TYPECHK(s, CAP_SINV)) return -EINVAL;
		/* The handler must be in another component */
		if (s->comp_info.liveness.id == lid) return -EINVAL;
		if (slot < 0) {
			for (i = 0; i < COMP_RECOVERY_MAX; i++) {
				if (LOAD(comp_recovery_tbl[i].lid) == 0
				    && cos_cas_32((u32_t *)&comp_recovery_tbl[i].lid, 0, LID_RESERVED))
					break;
			}
			if (i == COMP_RECOVERY_MAX) return -ENOMEM;
			slot = i;
		}
		/* Only faults after the registration is complete see the slot */
		r                = &comp_recovery_tbl[slot];
		r->handler       = s->comp_info;
		r->handler_entry = s->entry_addr;
		r->handler_token = s->token;
		cos_mem_fence();
		r->lid = lid;

		return 0;
	}
	case CAPTBL_OP_COMP_UPCALL: {
		word_t type = __userregs_get1(regs);
		word_t a1   = __userregs_get2(regs);
		word_t a2   = __userregs_get3(regs);
		word_t a3   = __userregs_get4(regs);
		u64_t  e    = *ep;

		/* The component must be alive, but can be suspended (that's the point) */
		if ((e & LTBL_EPOCH_LIVE_MASK) != compc->info.liveness.epoch) return -EINVAL;
		if (thd_invstk_push(thd, &compc->info, e, __userregs_getip(regs), __userregs_getsp(regs), regs,
		                    cos_info)) {
			return -ENOMEM;
		}
		thd->invstk[curr_invstk_top(cos_info)].protdom = compc->info.pgtblinfo.protdom;
		pgtbl_update(&compc->info.pgtblinfo);
		chal_protdom_write(compc->info.pgtblinfo.protdom);

		/* As for a thread creation upcall, returning with an sret */
		regs_upcall_setup(regs, compc->entry_addr, type, thd->tid | (get_cpuid() << 16), a1, a2, a3);
		regs->bp = regs->sp = 0;
		*thd_switch         = 1;

		return 0;
	}
	case CAPTBL_OP_COMP_RECOVERY_QUIESCE: {
		int req;

		if (slot < 0) return -EINVAL;
		req = cos_faa(&recovery_req, 1) + 1;
		/* This core (the current thread is the caller, not in the component) */
		recovery_scan(thd, cos_info);
		recovery_core[get_cpuid()].ack = req;
		chal_recovery_ipi_broadcast();

		return req;
	}
	case CAPTBL_OP_COMP_RECOVERY_QUIESCENT: {
		int          req = __userregs_get1(regs);
		unsigned int n   = 0;
		int          i;

		if (slot < 0) return -EINVAL;
		if (!recovery_quiescent(req)) return -EAGAIN;
		cos_mem_fence();
		for (i = 0; i < ncpus; i++) n += LOAD(recovery_core[i].nested[slot]);

		return n;
	}
	case CAPTBL_OP_COMP_RECOVERY_RESUME: {
		word_t retval = __userregs_get1(regs);
		u64_t  e      = *ep, gen;

		if (slot < 0 || !(e & LTBL_EPOCH_SUSPENDED)) return -EINVAL;
		/* A new generation: all frames of the previous ones are invalid */
		gen = ((e & LTBL_EPOCH_GEN_MASK) + (1ULL << LTBL_EPOCH_GEN_SHIFT)) & LTBL_EPOCH_GEN_MASK;
		if (!cos_cas((unsigned long *)ep, (unsigned long)e,
		             (unsigned long)((e & LTBL_EPOCH_LIVE_MASK) | gen))) {
			return -ECASFAIL;
		}

		/* Return from the current invocation (the fault handler) */
		__userregs_setinvret(regs, retval);
		sret_ret(thd, regs, cos_info);
		*thd_switch = 1;

		return 0;
	}
	default:
		return -EINVAL;
	}
}
