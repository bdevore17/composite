/**
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 *
 * Component fault isolation and recovery ("micro-reboot").
 *
 * A component's manager (the capmgr) can register it as recoverable
 * (CAPTBL_OP_COMP_RECOVERY_REGISTER, on the component's capability),
 * providing a synchronous invocation capability to its fault handler.
 * When a thread faults in a registered component, S, while executing
 * in a synchronous invocation (i.e. not in its own component):
 *
 * 1. S is *suspended*: a bit in its liveness entry's epoch (see
 *    LTBL_EPOCH_*) makes all invocations of it fail fast with
 *    -ECOMPRESET, and makes every invocation stack frame of S
 *    invalid, so that no thread can return into S anymore.
 * 2. The faulting thread is redirected to the manager's fault handler
 *    (a new invocation stack frame, as if S had invoked it), on a
 *    stack of the handler's choosing (the handler's entry gets the
 *    registered token, and can select a stack per component).
 * 3. The handler asks all cores to stop executing in S
 *    (CAPTBL_OP_COMP_RECOVERY_QUIESCE, an IPI to all other cores):
 *    each core unwinds its current thread if it executes in a frame
 *    of S that is no longer valid, so that it returns -ECOMPRESET to
 *    its caller, and counts the threads that execute in other
 *    components on behalf of S (invocations made by S, which can't
 *    be unwound until they return to S). Once all cores have done so
 *    (CAPTBL_OP_COMP_RECOVERY_QUIESCENT), no core executes in S, and
 *    none will until S is resumed.
 * 4. The handler resets S's memory and resources, and re-runs its
 *    initialization with an upcall into S on its own thread
 *    (CAPTBL_OP_COMP_UPCALL, which may enter S while it is
 *    suspended: the frame records the suspended epoch).
 * 5. The handler resumes S (CAPTBL_OP_COMP_RECOVERY_RESUME), which
 *    increments its generation, and returns from the handler in the
 *    same operation (so that the handler's stack is free before S
 *    can fault again). The faulting thread then returns into S's
 *    (stale) frame, which unwinds it to its caller with -ECOMPRESET.
 *
 * Threads that are not running when S is suspended, and that have
 * frames of S, are unwound *lazily*: when they are switched to (if
 * their top frame is invalid), or when they return to an invalid
 * frame (sret). Unwinding pops all invalid frames, and returns from
 * the invocation made by the first valid frame with -ECOMPRESET,
 * restoring the registers that a normal return preserves (saved in
 * each frame on invocation).
 *
 * Faults that are not recoverable keep the previous behavior (the
 * kernel stops): faults in unregistered components, in a thread's own
 * component (which can't be returned from), in user-level
 * invocations, or that can't be redirected to the handler (whose
 * component is already on the faulting thread's invocation stack,
 * or whose invocation stack is full).
 */

#ifndef RECOVERY_H
#define RECOVERY_H

#include "shared/cos_types.h"
#include "component.h"
#include "thd.h"

/* Fault in user-level (from an exception handler). Returns 1 if handled, 0 otherwise */
int comp_recovery_fault(struct pt_regs *regs, cos_flt_off type, unsigned long addr);
/* The quiescence IPI (HW_LAPIC_IPI_RECOVERY) */
int comp_recovery_ipi(struct pt_regs *regs);
/*
 * Before thd (not the current thread) is switched to: unwind it if
 * its top frame was invalidated. Returns 0 if it can run, -1 if not.
 */
int comp_recovery_thd_resume(struct thread *thd);
/* CAPTBL_OP_COMP_* operations on a component capability */
int comp_recovery_op(struct captbl *ct, struct cap_comp *compc, syscall_op_t op, struct thread *thd,
                     struct pt_regs *regs, struct cos_cpu_local_info *cos_info, int *thd_switch);

/* platform: send the recovery IPI to all other cores */
void chal_recovery_ipi_broadcast(void);

#endif /* RECOVERY_H */
