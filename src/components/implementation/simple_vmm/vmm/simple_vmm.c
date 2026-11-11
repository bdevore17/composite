#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cos_debug.h>
#include <llprint.h>
#include <cos_component.h>
#include <cos_kernel_api.h>
#include <static_slab.h>
#include <sched.h>
#include <contigmem.h>
#include <shm_bm.h>
#include <vmrt.h>
#include <instr_emul.h>
#include <acrn_common.h>

#include <vlapic.h>
#include <vioapic.h>
#include <sync_lock.h>
#include <cos_time.h>
#include <sync_sem.h>
#include <arpa/inet.h>
#include <netshmem.h>
#include <nic_netio_shmem.h>
#include <devices/vpci/virtio_net_io.h>


/*
 * The guest Linux image is named by the composition script, so a single built
 * component can be composed against different guests:
 *
 *     [[components]]
 *     name = "vmm"
 *     img  = "simple_vmm.vmm"
 *     constants = [{variable = "VM_GUEST_IMAGE",
 *                   value = "\"guest/vmlinux-shell-5.15.107.img\""}]
 *
 * The composer turns that into a -include'd #define at compose time, and
 * recompiles this component, so changing guest is a ./cos compose rather than a
 * rebuild.
 *
 * There is deliberately no default: a composition must say which guest it boots.
 */
#ifndef VM_GUEST_IMAGE
#error "VM_GUEST_IMAGE is not set. Name the guest image in your composition script, e.g. constants = [{variable = \"VM_GUEST_IMAGE\", value = \"\\\"guest/vmlinux-shell-5.15.107.img\\\"\"}] -- see simple_vmm/vm_dev.md"
#endif

INCBIN(vmlinux, VM_GUEST_IMAGE)
/* The realmode stub is built here and is not selectable. */
INCBIN(bios, "guest/guest.img")

/*
 * Number of vCPUs, set by the composition script like the guest image:
 *
 *     constants = [{variable = "VM_GUEST_IMAGE", value = "..."},
 *                  {variable = "VM_NUM_VCPUS",   value = "4"}]
 *
 * vCPU i runs on core i, as a VM thread with its own exit-handler thread, so
 * the VM may have at most NUM_CPU vCPUs. vCPU 0 is the bootstrap processor;
 * the others start in wait-for-SIPI and are brought up by the guest with
 * INIT/SIPI (see vlapic.c and vmrt.c). The guest firmware (vmimg's vmxbooter)
 * discovers the count through CPUID leaf 0BH and lists that many CPUs in the
 * MADT.
 */
#ifndef VM_NUM_VCPUS
#define VM_NUM_VCPUS 1
#endif
#if VM_NUM_VCPUS < 1 || VM_NUM_VCPUS > NUM_CPU || VM_NUM_VCPUS > VMRT_VM_MAX_VCPU
#error "VM_NUM_VCPUS must be between 1 and min(NUM_CPU, VMRT_VM_MAX_VCPU)"
#endif

/* Only one VM component globally managed by this VMM */
static struct vmrt_vm_comp *g_vm;
static u8_t g_num_vcpus = VM_NUM_VCPUS;

/*
 * vCPU creation is done by each vCPU's own core, one core at a time (vCPU 0
 * first); this counts the vCPUs created so far.
 */
static volatile u32_t g_vcpus_created = 0;

#define VM_MAX_COMPS (2)
#define GUEST_MEM_SZ (310*1024*1024)

SS_STATIC_SLAB(vm_comp, struct vmrt_vm_comp, VM_MAX_COMPS);
SS_STATIC_SLAB(vm_lapic, struct acrn_vlapic, VM_MAX_COMPS * VMRT_VM_MAX_VCPU);
SS_STATIC_SLAB(vcpu_inst_ctxt, struct instr_emul_ctxt, VM_MAX_COMPS * VMRT_VM_MAX_VCPU);
SS_STATIC_SLAB(vm_io_apic, struct acrn_vioapics, VM_MAX_COMPS);
SS_STATIC_SLAB(vcpu_mmio_req, struct acrn_mmio_request, VM_MAX_COMPS * VMRT_VM_MAX_VCPU);

struct sync_lock vm_boot_lock;

void 
pause_handler(struct vmrt_vm_vcpu *vcpu)
{
	sched_thd_yield();
	GOTO_NEXT_INST(vcpu->shared_region);
}

void 
hlt_handler(struct vmrt_vm_vcpu *vcpu)
{
	sched_thd_yield();
	GOTO_NEXT_INST(vcpu->shared_region);
}

void
mmio_init(struct vmrt_vm_vcpu *vcpu)
{
	vcpu->mmio_request = ss_vcpu_mmio_req_alloc();
	assert(vcpu->mmio_request);

	ss_vcpu_mmio_req_activate(vcpu->mmio_request);
}

void
ioapic_init(struct vmrt_vm_comp *vm)
{
	vm->ioapic = ss_vm_io_apic_alloc();
	assert(vm->ioapic);

	vioapic_init(vm);
	ss_vm_io_apic_activate(vm->ioapic);
}

void
iinst_ctxt_init(struct vmrt_vm_vcpu *vcpu)
{
	vcpu->inst_ctxt = ss_vcpu_inst_ctxt_alloc();
	assert(vcpu->inst_ctxt);

	ss_vcpu_inst_ctxt_activate(vcpu->inst_ctxt);
}

void
lapic_init(struct vmrt_vm_vcpu *vcpu)
{
	struct acrn_vlapic *vlapic = ss_vm_lapic_alloc();
	assert(vlapic);

	/* A vcpu can only have one vlapic */
	assert(vcpu->vlapic == NULL);

	vcpu->vlapic = vlapic;
	vlapic->apic_page = vcpu->lapic_page;
	vlapic->vcpu = vcpu;

	/* Status: enable APIC and vector be 0xFF */
	vlapic_reset(vlapic);

	ss_vm_lapic_activate(vlapic);
	return;
}

struct vmrt_vm_comp *
vm_comp_create(void)
{
	u64_t guest_mem_sz = GUEST_MEM_SZ;
	u64_t num_vcpu = g_num_vcpus;
	void *start;
	void *end;
	cbuf_t shm_id;
	void  *mem, *vm_mem;
	size_t sz;

	struct vmrt_vm_comp *vm = ss_vm_comp_alloc();
	assert(vm);
	
	vmrt_vm_create(vm, "vmlinux-5.15", num_vcpu, guest_mem_sz);

	/* Allocate memory for the VM */
	shm_id	= memmgr_shared_page_allocn_aligned(guest_mem_sz / PAGE_SIZE_4K, PAGE_SIZE_4K, (vaddr_t *)&mem);
	/* Make the memory accessible to VM */
	int ret = memmgr_shared_page_map_aligned_in_vm(shm_id, PAGE_SIZE_4K, (vaddr_t *)&vm_mem, vm->comp_id);
	vmrt_vm_mem_init(vm, mem);
	printc("created VM with %u cpus, memory size: %luMB, at host vaddr: %p\n", vm->num_vcpu, vm->guest_mem_sz/1024/1024, vm->guest_addr);

	ss_vm_comp_activate(vm);

	start = &incbin_bios_start;
	end = &incbin_bios_end;
	sz = end - start + 1;

	printc("BIOS image start: %p, end: %p, size: %lu(%luKB)\n", start, end, sz, sz/1024);
	vmrt_vm_data_copy_to(vm, start, sz, PAGE_SIZE_4K);
	
	start = &incbin_vmlinux_start;
	end = &incbin_vmlinux_end;
	sz = end - start + 1;

	printc("Guest Linux image start: %p, end: %p, size: %lu(%luMB)\n", start, end, sz, sz/1024/1024);
	#define GUEST_IMAGE_ADDR 0x100000
	vmrt_vm_data_copy_to(vm, start, sz, GUEST_IMAGE_ADDR);

	ioapic_init(vm);
	vm->dev_lock.locked = 0;

	printc("Guest(%s) image has been loaded into the VM component\n", vm->name);

	return vm;
}

/* Weak usage of nicmgr interface */
void __attribute__((weak)) nic_netio_shmem_map(cbuf_t shm_id);

void
cos_init(void)
{
	struct vmrt_vm_vcpu *vcpu;
	g_vm = vm_comp_create();
	printc("VMM: Created VM with %d vCPUs: comp_id=%lu, %p\n", g_num_vcpus, g_vm->comp_id, g_vm);

	if (!nic_netio_shmem_map) {
		printc("VMM: Networking disabled: nicmgr interface not found\n");
	}
}

/*
 * Create vCPU `cid` on this core: the kernel binds the vCPU's VM thread and
 * its VMCS to the core that creates it.
 */
static void
vcpu_create_here(coreid_t cid)
{
	struct vmrt_vm_vcpu *vcpu;

	printc("VMM: Initializing vCPU %d on core %d\n", cid, cid);
	vmrt_vm_vcpu_init(g_vm, cid);
	vcpu = vmrt_get_vcpu(g_vm, cid);
	assert(vcpu->coreid == cos_coreid());
	lapic_init(vcpu);
	iinst_ctxt_init(vcpu);
	mmio_init(vcpu);
	printc("VMM: vCPU %d initialized (tid=%lu), lapic, inst_ctxt, mmio done\n", cid, vcpu->tid);
}

void
cos_parallel_init(coreid_t cid, int init_core, int ncores)
{
	assert(ncores >= g_num_vcpus);

	if (cid == 0) {
		/*
		 * The TX/RX threads are created here rather than in cos_init()
		 * because a thread can only be switched to by the scheduler on the
		 * core it was created on -- cap_switch rejects a cross-core switch
		 * with -EINVAL. cos_init() runs on whichever core happens to
		 * initialise the component, while parallel_main() sets these
		 * threads' scheduling parameters from core 0, which put core-N
		 * threads on core 0's runqueue and tripped the assert in
		 * slm_private.h when the scheduler picked one.
		 */
		if (nic_netio_shmem_map) {
			/* Create TX thread for transmitting packets to NIC */
			g_vm->tx_thd = sched_thd_create(virtio_tx_task, NULL);
			printc("VMM: Created TX thread, tid=%lu\n", g_vm->tx_thd);
			/* Create RX thread for receiving packets from NIC */
			g_vm->rx_thd = sched_thd_create(virtio_rx_task, NULL);
			printc("VMM: Created RX thread, tid=%lu\n", g_vm->rx_thd);
		}
	}

	if (cid < g_num_vcpus) {
		/*
		 * One core at a time, in vCPU order: the capability manager and the
		 * static slabs are not known to tolerate concurrent VM resource
		 * creation, and this is a one-off cost at boot.
		 */
		while (__atomic_load_n(&g_vcpus_created, __ATOMIC_ACQUIRE) != (u32_t)cid) sched_thd_yield();
		vcpu_create_here(cid);
		__atomic_store_n(&g_vcpus_created, cid + 1, __ATOMIC_RELEASE);
	}

	return;
}

void
parallel_main(coreid_t cid)
{
	struct vmrt_vm_vcpu *vcpu;
	if (cid == 0 && nic_netio_shmem_map) {
		sched_thd_param_set(g_vm->rx_thd, sched_param_pack(SCHEDP_PRIO, 31));
		sched_thd_param_set(g_vm->tx_thd, sched_param_pack(SCHEDP_PRIO, 31));
	}

	if (cid < g_num_vcpus) {
		/* Every vCPU must exist before any of them runs: vCPU 0 sends IPIs to the others. */
		while (__atomic_load_n(&g_vcpus_created, __ATOMIC_ACQUIRE) != g_num_vcpus) sched_thd_yield();
		vcpu = vmrt_get_vcpu(g_vm, cid);

		if (cid == 0) {
			printc("VMM: Starting vCPU 0 (BSP) on core %d (tid=%lu), %d vCPU(s) total\n", cid, vcpu->tid,
			       g_num_vcpus);
			vmrt_vm_vcpu_start(vcpu);
		} else {
			/* An application processor: runs only once the guest INIT/SIPIs it */
			printc("VMM: vCPU %d (AP) on core %d waiting for INIT/SIPI\n", cid, cid);
			vmrt_vcpu_wait_sipi_and_start(vcpu);
			printc("VMM: Core %d started vCPU %d (tid=%lu)\n", cid, cid, vcpu->tid);
		}
	}
	sched_thd_block(0);
	/* Should not be here, or there is a bug in the scheduler! */
	assert(0);
}
