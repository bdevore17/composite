#include <vm_vcpu_context.h>
#include <thd.h>
#include <vmx.h>
#include <vm.h>

/* Abstraction layer for VM support */
#ifdef CONFIG_VMX 

/* Whether the CPU has VMX, so VM threads can be created (vm_env_init()). */
static int vm_env_vmx = 0;

int
vm_env_available(void)
{
	return vm_env_vmx;
}

void
vm_env_init(void)
{
	u32_t a = 1, b = 0, c = 0, d = 0;

	/* CPUID.01H:ECX.VMX[bit 5]; absent e.g. in QEMU without KVM */
	chal_cpuid(&a, &b, &c, &d);
	if (!(c & (1 << 5))) {
		if (get_cpuid() == INIT_CORE) printk("VMX is not supported: VM threads are disabled.\n");
		return;
	}
	vm_env_vmx = 1;
	vmx_env_init();
}

void
vm_thd_init(struct thread *thd, void *vm_pgd, struct cap_vm_vmcb *vmcb)
{
	vmx_thd_init(thd, vm_pgd, vmcb);
}

void
vm_thd_exec(struct thread *thd)
{
	vmx_thd_start_or_resume(thd);
}

#else

int  vm_env_available(void) { return 0; }
void vm_env_init(void) {}
void vm_thd_init(struct thread *thd, void *vm_pgd, struct cap_vm_vmcb *vmcb) {}
void vm_thd_exec(struct thread *thd) {}

#endif