/**
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Entry point of the component recovery fault handler (see doc.md):
 * the kernel redirects a thread that faulted in a recoverable
 * component here, as a synchronous invocation of the handler sinv
 * that we registered for the component, whose token is the
 * component's id:
 *
 * - %rax: thread id | core id << 16
 * - %rbp: the invocation token (the component id)
 * - %rbx, %rsi, %rdi, %r12: the arguments (fault type, address,
 *   instruction pointer, faulting thread id)
 *
 * The handler can't use the thread's normal stack in this component:
 * the component's re-initialization (which the handler runs on this
 * thread) can invoke us, on that stack. Instead, each component has a
 * handler stack (cm_recovery_stacks, indexed by component id),
 * which only one thread uses at a time: the kernel redirects a single
 * fault per recovery, and the handler leaves the stack (resuming the
 * component and returning in one operation) before the component can
 * fault again. The top of the stack has the same layout as the normal
 * invocation stacks (core id, thread id, token), so that cos_thdid(),
 * cos_cpuid() and cos_inv_token() work as usual.
 */

#include <consts.h>
#include "../../../../kernel/include/asm_ipc_defs.h"

.text
.globl cm_recovery_entry
.type  cm_recovery_entry, @function
.align 16
cm_recovery_entry:
	/* The top of this component's handler stack */
	movq	%rbp, %rcx
	addq	$1, %rcx
	shlq	$MAX_STACK_SZ_BYTE_ORDER, %rcx
	movabs	$cm_recovery_stacks, %rsp
	addq	%rcx, %rsp
	/* core id, thread id, and token, as in COS_ASM_GET_STACK_INVTOKEN */
	movq	%rax, %rdx
	shrq	$16, %rdx
	pushq	%rdx
	andq	$0xffff, %rax
	pushq	%rax
	pushq	%rbp
	xorq	%rbp, %rbp
	/* C arguments: (%rbx, %rsi, %rdi, %r12) */
	movq	%rdi, %rdx
	movq	%r12, %rcx
	movq	%rbx, %rdi
	andq	$~0xf, %rsp
	call	cm_comp_fault
	/* Return (only if the component isn't recovered, see cm_comp_fault) */
	movq	%rax, %r8
	movq	$RET_CAP, %rax
	syscall

.section .note.GNU-stack,"",@progbits
