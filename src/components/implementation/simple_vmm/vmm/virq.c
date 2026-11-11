#include <cos_types.h>
#include <cos_debug.h>
#include <llprint.h>
#include <vmrt.h>
#include <acrn_common.h>
#include <vioapic.h>
#include "virq.h"

void
virq_pulse(struct vmrt_vm_comp *vm, u32_t gsi)
{
	assert(vm && vm->ioapic);
	VMRT_DEBUG("virq: pulse gsi %u\n", gsi);
	vioapic_set_irqline_lock(vm, gsi, GSI_RAISING_PULSE);
}

void
virq_raise_legacy(struct vmrt_vm_vcpu *vcpu, u32_t gsi, u8_t legacy_vector, int autoeoi)
{
	if (vcpu->vm->num_vcpu == 1) {
		lapic_intr_inject(vcpu, legacy_vector, autoeoi);
		return;
	}
	virq_pulse(vcpu->vm, gsi);
}
