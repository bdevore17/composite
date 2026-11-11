#pragma once

#include <cos_kernel_api.h>
#include <consts.h>

#define VMRT_VM_NAME_SIZE (32)
#define VMRT_VM_MAX_VCPU (16)

/*
 * VMM-side logging. Level 0 = errors only, 1 = notable events (vCPU startup,
 * INIT/SIPI, device setup), 2 = per-event debug (every IPI, every virtio
 * request). Override with -DVMRT_LOG_LEVEL=<n> or a composition constant.
 */
#ifndef VMRT_LOG_LEVEL
#define VMRT_LOG_LEVEL 1
#endif
#define VMRT_LOG(lvl, fmt, args...)                                       \
	do {                                                              \
		if ((lvl) <= VMRT_LOG_LEVEL) printc("[vmm] " fmt, ##args); \
	} while (0)
#define VMRT_INFO(fmt, args...)  VMRT_LOG(1, fmt, ##args)
#define VMRT_DEBUG(fmt, args...) VMRT_LOG(2, fmt, ##args)

/*
 * A minimal spinlock for VMM state shared by vCPU handler threads that run on
 * different cores (device models, the vIOAPIC). Threads on the *same* core can
 * also contend (e.g. the virtio-net RX thread and vCPU 0's handler), so the
 * slow path yields to the scheduler instead of spinning forever against a
 * preempted holder.
 */
struct vmrt_spinlock {
	volatile u32_t locked;
};
#define VMRT_SPINLOCK_INIT { .locked = 0 }
void vmrt_spin_lock(struct vmrt_spinlock *l);
void vmrt_spin_unlock(struct vmrt_spinlock *l);

/*
 * vCPU run states, for INIT/SIPI emulation (SDM Vol.3, "MP Initialization", "Interrupt Command Register").
 *
 *   UNINIT    -- not created yet
 *   WAIT_SIPI -- an application processor after (virtual) power-on or INIT:
 *                its vCPU thread exists but is never run
 *   STARTING  -- a STARTUP IPI arrived (sipi_vector is valid); the vCPU's
 *                own core picks this up and starts the vCPU
 *   RUNNING   -- executing guest code (or in its exit handler)
 */
enum vmrt_vcpu_run_state {
	VMRT_VCPU_UNINIT = 0,
	VMRT_VCPU_WAIT_SIPI,
	VMRT_VCPU_STARTING,
	VMRT_VCPU_RUNNING,
};

/* Words in a 256-bit interrupt-vector bitmap */
#define VMRT_INTR_WORDS (256 / 64)

struct vmrt_vm_vcpu {
	struct vm_vcpu_shared_region *shared_region;
	u64_t next_timer;
	void *lapic_page;

	/*
	 * A opaque pointer points to VMM's specific implementation of lapic.
	 * A VMM is responsible to have its own apic implementation.
	 */
	void *vlapic;

	/* Instruction emulation context */
	void *inst_ctxt;

	void *mmio_request;

	thdid_t tid;
	thdcap_t cap;

	u8_t cpuid;
	u8_t coreid;
	struct vmrt_vm_comp *vm;
	u64_t pending_req;

	u16_t vpid;
	vm_vmcscap_t vmcs_cap;
	vm_msrbitmapcap_t msr_bitmap_cap;
	vm_lapiccap_t lapic_cap;
	vm_shared_mem_t shared_mem_cap; 
	vm_lapicaccesscap_t lapic_access_cap;
	vm_vmcb_t vmcb_cap;
	thdid_t handler_tid;

	/* enum vmrt_vcpu_run_state; changed with atomics, see vmrt_vcpu_*() */
	volatile u32_t run_state;
	/* STARTUP IPI vector, valid in VMRT_VCPU_STARTING */
	volatile u32_t sipi_vector;

	/*
	 * Interrupts requested for this vCPU by anyone (other vCPUs' IPIs, the
	 * vIOAPIC, device threads), one bit per vector. Only the vCPU's own
	 * handler thread moves them into its virtual-APIC page (IRR + RVI), and
	 * it does so only while the guest is stopped, right before VM entry
	 * (vmrt_vcpu_sync_intr()). This is a software stand-in for the posted
	 * interrupt descriptor's PIR: nobody else may write another vCPU's
	 * virtual-APIC page, because the processor updates it concurrently
	 * while that vCPU runs (SDM Vol.3, "Virtual-APIC Page").
	 */
	volatile u64_t pending_intr[VMRT_INTR_WORDS];
};

struct vmrt_vm_comp {
	compid_t comp_id;
	void *guest_addr;
	word_t guest_mem_sz;

	void *ioapic;
	vm_lapicaccesscap_t lapic_access_page;

	char name[VMRT_VM_NAME_SIZE];

	u8_t num_vcpu;
	struct vmrt_vm_vcpu vcpus[VMRT_VM_MAX_VCPU];

	thdid_t tx_thd;
	thdid_t rx_thd;

	/* Serializes guest-visible device emulation across vCPUs (port I/O, MMIO). */
	struct vmrt_spinlock dev_lock;

	int wire_mode;
	u16_t vm_mac_id;
};

#define VMRT_GPA2HVA(gpa, vm, offset) ((gpa - offset) + vm->guest_addr)
#define GPA2HVA(gpa, vm) VMRT_GPA2HVA(gpa, vm, PAGE_SIZE_4K)

typedef void vmrt_exception_handler(struct vmrt_vm_vcpu *vcpu);

void vmrt_vm_create(struct vmrt_vm_comp *vm, char *name, u8_t num_vcpu, u64_t vm_mem_sz);
void vmrt_vm_mem_init(struct vmrt_vm_comp *vm, void *mem);
void vmrt_vm_vcpu_init(struct vmrt_vm_comp *vm, u32_t vcpu_nr);
void vmrt_vm_vcpu_resume(struct vmrt_vm_vcpu *vcpu);
void vmrt_vm_data_copy_to(struct vmrt_vm_comp *vm, char *image, u64_t size, paddr_t gpa);
void vmrt_vm_exception_handler(void *vcpu);
/*
 * Make the vCPU runnable. vmrt_vm_vcpu_init() and vmrt_vm_vcpu_start() must
 * be called on the core the vCPU is to run on: the kernel binds a vCPU
 * thread (and its VMCS) to the core that created it.
 */
void vmrt_vm_vcpu_start(struct vmrt_vm_vcpu *vcpu);
paddr_t vmrt_vm_gva2gpa(struct vmrt_vm_vcpu *vcpu, vaddr_t gva);
static inline struct vmrt_vm_vcpu *vmrt_get_vcpu(struct vmrt_vm_comp *vm, u32_t vcpu_nr) { assert(vcpu_nr < vm->num_vcpu);return &vm->vcpus[vcpu_nr]; }

void lapic_intr_inject(struct vmrt_vm_vcpu *vcpu, u8_t vector, int autoeoi);

/*
 * Request interrupt `vector` on `vcpu`. Safe from any thread on any core. It
 * takes effect at that vCPU's next VM entry: a halted or spinning vCPU exits
 * on HLT/PAUSE and picks it up immediately, a vCPU running guest code picks
 * it up at its next VM exit (at the latest the next host timer interrupt or
 * guest timer reprogramming). See vm_fixme.md ("Interrupt delivery latency").
 */
void vmrt_vcpu_post_intr(struct vmrt_vm_vcpu *vcpu, u8_t vector);
/* Called only by the vCPU's own handler thread, with the guest stopped */
void vmrt_vcpu_sync_intr(struct vmrt_vm_vcpu *vcpu);
/* Drop all requested-but-undelivered interrupts (INIT) */
void vmrt_vcpu_clear_intr(struct vmrt_vm_vcpu *vcpu);

/* INIT/SIPI emulation; the guest-visible side lives in the VMM's vlapic */
void vmrt_vcpu_init_ipi(struct vmrt_vm_vcpu *vcpu);
void vmrt_vcpu_sipi(struct vmrt_vm_vcpu *vcpu, u8_t vector);
/*
 * Run on an application processor vCPU's own core: wait until the guest sent
 * the vCPU a STARTUP IPI, then start it at the SIPI vector's entry point.
 */
void vmrt_vcpu_wait_sipi_and_start(struct vmrt_vm_vcpu *vcpu);

#define INCBIN(name, file) \
    __asm__( \
            ".global incbin_" STR(name) "_start\n" \
            ".balign 16\n" \
            "incbin_" STR(name) "_start:\n" \
            ".incbin \"" file "\"\n" \
            \
            ".global incbin_" STR(name) "_end\n" \
            ".balign 1\n" \
            "incbin_" STR(name) "_end:\n" \
            ".byte 0\n" \
    ); \
    extern const __attribute__((aligned(16))) void* incbin_ ## name ## _start; \
    extern const void* incbin_ ## name ## _end; \

#define GOTO_NEXT_INST(vcpu) { (vcpu)->ip += (vcpu)->inst_length; }

static inline void
chal_cpuid(u32_t *a, u32_t *b, u32_t *c, u32_t *d)
{
	asm volatile("cpuid" : "+a"(*a), "+b"(*b), "+c"(*c), "+d"(*d));
}

void vmrt_dump_vcpu(struct vmrt_vm_vcpu *vcpu);

#define VM_PANIC(vcpu) {	\
    vmrt_dump_vcpu(vcpu);	\
    assert(0);		\
}

#define IO_OUT 0
#define IO_IN 1

enum {
	IO_BYTE = 0,
	IO_WORD,
	IO_LONG,
};

#define LAPIC_BASE_ADDR (0xFEE00000)
#define IOAPIC_BASE_ADDR (0xFEC00000)

#define PGTBL_LVL_4_IDX(addr) ((((u64_t)(addr)) >> 39) & 0x1FF)
#define PGTBL_LVL_3_IDX(addr) ((((u64_t)(addr)) >> 30) & 0x1FF)
#define PGTBL_LVL_2_IDX(addr) ((((u64_t)(addr)) >> 21) & 0x1FF)
#define PGTBL_LVL_1_IDX(addr) ((((u64_t)(addr)) >> 12) & 0x1FF)
#define PGTBL_ENTRY_ADDR_MASK 0X000FFFFFFFFFF000
#define PGTBL_ENTRY_PRESENT(entry) ((u64_t)entry & 0x1)
#define PGTBL_ENTRY_SUPER_PAGE(entry) ((u64_t)entry & 0x80)
