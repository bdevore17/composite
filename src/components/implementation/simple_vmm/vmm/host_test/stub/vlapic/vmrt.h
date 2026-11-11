/* Stand-in for vmrt.h in host-side tests: only what the code under test uses. */
#pragma once
#include <cos_types.h>
#include <stdio.h>
#define VMRT_VM_MAX_VCPU 16
#define VMRT_LOG_LEVEL 2
#define VMRT_LOG(lvl, fmt, args...) do { if ((lvl) <= VMRT_LOG_LEVEL) printf("[vmm] " fmt, ##args); } while (0)
#define VMRT_INFO(fmt, args...)  VMRT_LOG(1, fmt, ##args)
#define VMRT_DEBUG(fmt, args...) VMRT_LOG(2, fmt, ##args)
struct vm_pi_desc { u64_t pir[4]; u64_t control; };
struct vm_vcpu_shared_region { u64_t ax; u64_t qualification; u16_t interrupt_status; u16_t inject_evt; struct vm_pi_desc pi_desc; };
struct vmrt_vm_comp;
struct vmrt_vm_vcpu { struct vm_vcpu_shared_region *shared_region; void *lapic_page; void *vlapic; u8_t cpuid; struct vmrt_vm_comp *vm; u64_t pending_req; };
struct vmrt_vm_comp { u8_t num_vcpu; struct vmrt_vm_vcpu vcpus[VMRT_VM_MAX_VCPU]; int wire_mode; void *ioapic; };
static inline struct vmrt_vm_vcpu *vmrt_get_vcpu(struct vmrt_vm_comp *vm, u32_t n) { return &vm->vcpus[n]; }
void vmrt_vcpu_post_intr(struct vmrt_vm_vcpu *vcpu, u8_t vector);
void vmrt_vcpu_init_ipi(struct vmrt_vm_vcpu *vcpu);
void vmrt_vcpu_sipi(struct vmrt_vm_vcpu *vcpu, u8_t vector);
void vmrt_dump_vcpu(struct vmrt_vm_vcpu *vcpu);
#define VM_PANIC(v) do { vmrt_dump_vcpu(v); assert(0); } while (0)
