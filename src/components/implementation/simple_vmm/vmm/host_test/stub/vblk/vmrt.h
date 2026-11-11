/* Stand-in for vmrt.h in host-side tests: only what the code under test uses. */
#pragma once
#include <cos_types.h>
#include <stdio.h>
#define VMRT_LOG_LEVEL 2
#define VMRT_LOG(lvl, fmt, args...) do { if ((lvl) <= VMRT_LOG_LEVEL) printf("[vmm] " fmt, ##args); } while (0)
#define VMRT_INFO(fmt, args...)  VMRT_LOG(1, fmt, ##args)
#define VMRT_DEBUG(fmt, args...) VMRT_LOG(2, fmt, ##args)
struct vm_vcpu_shared_region { u64_t ax; };
struct vmrt_vm_comp { void *guest_addr; word_t guest_mem_sz; void *ioapic; };
struct vmrt_vm_vcpu { struct vm_vcpu_shared_region *shared_region; struct vmrt_vm_comp *vm; };
#define VMRT_GPA2HVA(gpa, vm, offset) ((gpa - offset) + (char *)vm->guest_addr)
#define GPA2HVA(gpa, vm) VMRT_GPA2HVA(gpa, vm, PAGE_SIZE_4K)
#define IO_OUT 0
#define IO_IN 1
enum { IO_BYTE = 0, IO_WORD, IO_LONG };
