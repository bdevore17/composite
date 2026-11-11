#ifndef CALL_CONVENTION_H
#define CALL_CONVENTION_H

#include "../../../kernel/include/asm_ipc_defs.h"

/*
 * Functions to maintain calling conventions on invocation and return
 * (i.e. to make sure the registers are appropriately set up).
 */
static inline void
__userregs_setret(struct pt_regs *regs, unsigned long ret)
{
	regs->ax = ret;
}

static inline unsigned long
__userregs_getsp(struct pt_regs *regs)
{
	return regs->bp;
}

static inline unsigned long
__userregs_getip(struct pt_regs *regs)
{
#if defined(__x86_64__)
	return regs->r8;
#elif defined(__i386__)
	return regs->cx;
#endif
}

static inline capid_t
__userregs_getcap(struct pt_regs *regs)
{
	return (regs->ax >> COS_CAPABILITY_OFFSET) - 1;
}

static inline u32_t
__userregs_getop(struct pt_regs *regs)
{
	return regs->ax & ((1 << COS_CAPABILITY_OFFSET) - 1);
}

static inline unsigned long
__userregs_getinvret(struct pt_regs *regs)
{
#if defined(__x86_64__)
	/* r8 holds the return value on invocation return path. */
	return regs->r8;
#elif defined(__i386__)
	/* cx holds the return value on invocation return path. */
	return regs->cx;
#endif
} 

static inline void
__userregs_set(struct pt_regs *regs, unsigned long ret, unsigned long sp, unsigned long ip)
{
#if defined(__x86_64__)
	regs->ax = ret;
	regs->ip = regs->cx = ip;
	regs->sp = regs->bp = sp;

#elif defined(__i386__)
	regs->ax = ret;
	regs->sp = regs->cx = sp;
	regs->ip = regs->dx = ip;
#endif
}

static inline void
__userregs_setinv(struct pt_regs *regs, unsigned long id, unsigned long tok, unsigned long ip)
{ __userregs_set(regs, id, tok, ip); }

static inline void
__userregs_setretvals(struct pt_regs *regs, unsigned long ret, unsigned long ret1, unsigned long ret2, unsigned long ret3)
{
	regs->ax = ret;
	regs->si = ret1;
	regs->di = ret2;
	regs->bx = ret3;
}

static inline void
__userregs_sinvupdate(struct pt_regs *regs)
{
#if defined(__x86_64__)
	regs->r12 = regs->dx;
#elif defined(__i386__)
	/* IPC calling side has 4 args (in order): bx, si, di, dx */
	/* IPC server side receives 4 args: bx, si, di, bp */
	/* So we need to pass the 4th argument. */

	/* regs->bx = regs->bx; */
	/* regs->si = regs->si; */
	/* regs->di = regs->di; */
	regs->bp = regs->dx;
#endif
}

static inline word_t 
__userregs_get1(struct pt_regs *regs)
{
	return regs->bx;
}

static inline word_t 
__userregs_get2(struct pt_regs *regs)
{
	return regs->si;
}

static inline word_t 
__userregs_get3(struct pt_regs *regs)
{
	return regs->di;
}

static inline word_t 
__userregs_get4(struct pt_regs *regs)
{
	return regs->dx;
}

static inline void
copy_gp_regs(struct pt_regs *from, struct pt_regs *to)
{
#define COPY_REG(reg) to->reg = from->reg
#if defined(__x86_64__)
	COPY_REG(r15);
	COPY_REG(r14);
	COPY_REG(r13);
	COPY_REG(r12);
	COPY_REG(r11);
	COPY_REG(r10);
	COPY_REG(r9);
	COPY_REG(r8);
#endif
	COPY_REG(bx);
	COPY_REG(cx);
	COPY_REG(dx);
	COPY_REG(si);
	COPY_REG(di);
	COPY_REG(bp);
	COPY_REG(ax);
#undef COPY_REG
}

static inline void
copy_all_regs(struct pt_regs *from, struct pt_regs *to)
{
#define COPY_REG(reg) to->reg = from->reg	
#if defined(__x86_64__)
	COPY_REG(r15);
	COPY_REG(r14);
	COPY_REG(r13);
	COPY_REG(r12);
	COPY_REG(r11);
	COPY_REG(r10);
	COPY_REG(r9);
	COPY_REG(r8);
#endif
	COPY_REG(bx);
	COPY_REG(cx);
	COPY_REG(dx);
	COPY_REG(si);
	COPY_REG(di);
	COPY_REG(bp);
	COPY_REG(ax);
/*
 * These four segment registers don't need to be copied
 * because user space cannot modified them, and the kernel
 * also doesn't need to change its value.
 */
	// COPY_REG(ds);
	// COPY_REG(es);
	// COPY_REG(fs);
	// COPY_REG(gs);

	COPY_REG(orig_ax);
	COPY_REG(ip);
	COPY_REG(cs);
	COPY_REG(flags);
	COPY_REG(sp);
	COPY_REG(ss);
#undef COPY_REG
}

static inline void
regs_upcall_setup(struct pt_regs *r, unsigned long entry_addr, int option, int id, int arg1, int arg2, int arg3)
{
#if defined(__x86_64__)
	/* $0x3200 : enable interrupt, and iopl is set to 3, the same as user's CPL */
	r->r11	= 0x3200;
	r->r12	= option;

	r->bx	= arg1;
	r->di	= arg2;
	r->si	= arg3;

	r->ip	= r->cx	= entry_addr;
	r->ax	= id;
#elif defined(__i386__)
	r->cx = option;

	r->bx = arg1;
	r->di = arg2;
	r->si = arg3;

	r->ip = r->dx = entry_addr;
	r->ax         = id; // thd id + cpu id
#endif
	return;
}

/*
 * Component recovery: a synchronous invocation that is unwound (see
 * recovery.h) returns to its caller without the server having
 * returned. The registers that a normal return preserves (the
 * callee-saved registers of the ABI, which the server's code
 * preserves, and that the client's invocation stub relies on) are
 * saved in the caller's invocation stack frame on each invocation,
 * and restored on unwinding.
 */
#define INVSTK_SAVED_REGS 4

static inline void
__userregs_inv_save(struct pt_regs *regs, unsigned long *saved)
{
#if defined(__x86_64__)
	saved[0] = regs->bx;
	saved[1] = regs->r13;
	saved[2] = regs->r14;
	saved[3] = regs->r15;
#elif defined(__i386__)
	saved[0] = regs->bx;
	saved[1] = regs->si;
	saved[2] = regs->di;
	saved[3] = 0;
#endif
}

/*
 * Return from an unwound invocation: to ip/sp (as for a normal
 * return), with the return value ret, the secondary return values
 * zeroed, and the saved registers restored.
 */
static inline void
__userregs_inv_unwind(struct pt_regs *regs, unsigned long ret, unsigned long sp, unsigned long ip, unsigned long *saved)
{
	__userregs_set(regs, ret, sp, ip);
#if defined(__x86_64__)
	regs->si  = 0;
	regs->di  = 0;
	regs->bx  = saved[0];
	regs->r13 = saved[1];
	regs->r14 = saved[2];
	regs->r15 = saved[3];
#elif defined(__i386__)
	regs->bx = saved[0];
	regs->si = saved[1];
	regs->di = saved[2];
#endif
}

/* The arguments of a synchronous invocation, as the server receives them (see __userregs_sinvupdate) */
static inline void
__userregs_setinvargs(struct pt_regs *regs, unsigned long a1, unsigned long a2, unsigned long a3, unsigned long a4)
{
#if defined(__x86_64__)
	regs->bx  = a1;
	regs->si  = a2;
	regs->di  = a3;
	regs->r12 = a4;
#elif defined(__i386__)
	regs->bx = a1;
	regs->si = a2;
	regs->di = a3;
	regs->bp = a4;
#endif
}

/* The return value of an invocation return (sret) */
static inline void
__userregs_setinvret(struct pt_regs *regs, unsigned long ret)
{
#if defined(__x86_64__)
	regs->r8 = ret;
#elif defined(__i386__)
	regs->cx = ret;
#endif
}

/* Did the trap/interrupt with these registers come from user-level? */
static inline int
__userregs_from_user(struct pt_regs *regs)
{
	return (regs->cs & 3) == 3;
}

#endif /* CALL_CONVENTION_H */
