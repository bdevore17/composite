#pragma once

/* Abstraction layer for vm support*/
#include <vmx_vmcs.h>

typedef enum {
	VM_THD_STATE_STOPPED    = 1,
	VM_THD_STATE_RUNNING    = 1 << 1, 
} vm_thd_state_t;

struct vm_vcpu_context {
	vm_thd_state_t state;
	struct vmx_vmcs vmcs;
};

struct thread;
struct cap_vm_vmcb;

void vm_env_init(void);
/* Can VM threads be created on this CPU? (needs VMX) */
int  vm_env_available(void);
void vm_thd_init(struct thread *thd, void *vm_pgd, struct cap_vm_vmcb *vmcb);
void vm_thd_exec(struct thread *thd);
