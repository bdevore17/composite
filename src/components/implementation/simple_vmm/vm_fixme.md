This document records some future work or potential fixes needed in the future.  

## The right option to create a VM component
For EPT, having a EPT cap or having a separate operation for PGTBL_CAP for EPT would be the best. And making the flags operations open to kernel api, thus different types of page tables can pass their own flags with the same code path.

However, that's not what the current code is. The current code uses a VM flag bit when creating EPT page tables within the `level` argument in order to reduce workload based on the current kernel and user level codebase.

## Linux image
The ideal choice would be to just use the compressed linux kernel and load it with Composite's ELF loader. But, currently we don't use that for simplicity. Instead, a simple booter loads the Linux image; it lives in the `vmimg` submodule (`vmimg/vmxbooter/`) rather than in the kernel tree, and the kernel itself is unmodified upstream.

## VM Logging System
The vmx log level in the kernel can be controled by `COS_VM_LOG_GLOBAL_LEVEL` flag. Set it to `debug` level will print most verbose information.

## EPT
EPT is the paging structure used to translate guest physical addresses into host physical addresses, thus providing guest physical address virtulization.

To simplify the implementation, we don't use bits above the `MAXPHYADDR` as these bits are controled by their VM-execution controls. By default we don't enable those controls and thus they will be ignored by hardware.

**Guest RAM is still mapped with 4K EPT entries only.** The EPT walk is therefore like a normal 4-level page-table walk, and both use the same 12-MAXPHYADDR bits in an entry to reference the next level. The kernel now reads `IA32_VMX_EPT_VPID_CAP` at boot and logs whether the CPU supports 2MB (bit 16) and 1GB (bit 17) EPT pages (`vmx_ept_caps_init()` in `platform/x86_64/vmx/vmx.c`), but nothing uses them yet.

### Why 2MB/1GB EPT mappings are not implemented yet

Mapping guest RAM with large pages is not a VMM-only change; the kernel's page-table code is 4K-only in ways that matter for memory safety, and none of it can be exercised without VT-x hardware:

1. **The ertrie walker does not stop at superpages.** `chal_pgtbl_mapping_add()` and the other walkers in `platform/x86_64/chal_pgtbl.c` call `pgtbl_lkup_lvl(..., PGTBL_DEPTH)`, which dereferences every level (see the `FIXME` at `chal_pgtbl_mapping_add()`). A PDE with the PS bit (bit 7) set would be followed as if it pointed at a page table, i.e. into guest RAM. Any later lookup, mapping, unmapping or introspection on that range would then read or write "PTEs" that the guest controls -- including the retype/refcount bookkeeping done on unmap. Every walker that can reach an EPT (and, for 1GB pages, the PDPT level) must first be taught to stop at a PS entry.
2. **Frame accounting is per 4K frame.** `retypetbl_ref()` takes an `order` argument but only references the first 4K frame (`GET_RETYPE_ENTRY(idx, ORDER(0))`). A 2MB EPT leaf must take a reference on all 512 frames (with rollback on failure), and the unmap path must drop all of them, or the capability manager could retype frames still mapped into a guest into kernel memory.
3. **There is no operation to install a leaf above level 4.** EPT mappings are created one 4K page at a time by copying a PTE from the capability manager's page table (`chal_pgtbl_cpy()`). A new operation is needed, e.g. `CAPTBL_OP_EPT_LARGE_MAP(src_pgtbl, src_vaddr, ept_pgtbl, gpa, order)` that checks: the EPT capability's type is `PGTBL_TYPE_EPT`; `gpa` and the physical range are aligned to the page size; all 512 (or 512*512) source PTEs are present, user, not `COSFRAME`/`COSKMEM`, and physically contiguous; the CPU supports the size (`vmx_ept_vpid_cap` bit 16/17); the target PDE/PDPTE is not present. It would then reference every frame and install `pa | x86_EPT_VM_DEF | (1 << 7)` with a CAS. User level must allocate the intermediate EPT tables only down to the PD (or PDPT) level for that range.
4. **User-level allocation.** Guest RAM comes from `memmgr_shared_page_allocn_aligned()`, which gives 4K-aligned pages that are not necessarily physically contiguous. Large mappings need 2MB-aligned, physically contiguous chunks: `contigmem` gives contiguity (from a fixed pool, `CONTIG_PHY_PAGES` in capmgr) but not 2MB alignment, so it needs an `align` that is honoured physically, and the VMM must fall back to 4K mappings for whatever part of guest RAM it cannot get in aligned chunks (and for the first 2MB, which holds the 0x1000 BIOS stub mapping, and anything that is not RAM such as the APIC-access page).

Only then can `vm_comp_create()` map guest RAM 2MB (or 1GB) at a time. The guest-side code (`vmrt_vm_gva2gpa()`) already handles large *guest* pages.

## Multi-vCPU

`VM_NUM_VCPUS` (composition constant) sets the vCPU count; vCPU *i* runs on core *i*. Things to know:

- **Interrupt delivery latency.** Interrupts for a vCPU (IPIs from other vCPUs, vIOAPIC interrupts, device threads) are posted to that vCPU's request bitmap and moved into its virtual-APIC page by its own exit handler right before the next VM entry (`vmrt_vcpu_sync_intr()`). A halted vCPU (HLT exits, the handler yields and re-enters) or a spinning one (PAUSE exits) picks them up almost at once. A vCPU busy running guest code only sees them at its next VM exit; if it does not exit on its own (I/O, MSR writes, PAUSE), that is the next host timer interrupt on its core, i.e. the scheduler quantum of `sched.pfprr_quantum_static`. This is the same mechanism that delivers a busy vCPU's own guest timer interrupts, unchanged from the single-vCPU VMM. Linux tolerates the delay, but cross-CPU work (TLB shootdowns, `smp_call_function`) on a busy target can take up to a quantum. The fix is a real kick: either the posted-interrupt mechanism (`PROCESS_POSTED_INTERRUPTS` is already enabled with notification vector `HW_LAPIC_POSTED_INTR`, but the host would have to send that vector to the target core, and `posted_intr_handler()` must handle it arriving in host context), or an asynchronous send to the vCPU's core, which the kernel now tolerates while a guest runs (see below).
- **IPIs to a core running a guest.** A Composite asynchronous-send IPI (`HW_LAPIC_IPI_ASND`) that arrives while a guest runs used to `assert`. It is now re-raised as a self-IPI and processed in host context when the exit handler thread runs (`vmx_exit.c`). This matters as soon as the VMM's threads wake each other across cores.
- **INIT to a running vCPU** (Linux CPU offlining) is ignored: there is no vCPU reset. Don't offline guest CPUs.
- **NMI IPIs** are dropped with a message (Linux uses them for backtraces and for stopping CPUs on panic/reboot).
- **Level-triggered interrupts** are delivered edge-style (no TMR, no EOI broadcast to the vIOAPIC); all emulated devices use edge semantics.
- **Device interrupts** go through the vIOAPIC with more than one vCPU (the guest's vector for an IRQ depends on which CPU it is bound to). With one vCPU the serial port and virtio-net keep their historical fixed vectors (34, 33).

## virtio-blk

`devices/vpci/virtio_blk.c` is a legacy virtio-pci block device (bus 0, device 2, I/O BAR at 0x8000, IRQ 10) backed by a RAM disk of `VM_BLK_DISK_MB` megabytes (default 16; 0 removes the device). Requests are synchronous, run on the vCPU that kicked the queue under the device lock. The storage is behind `devices/vblk/vblk_backend.h`, so a backend that talks to a block-device component can replace the RAM disk; an asynchronous backend would complete requests through `virtio_blk_complete()`. Not implemented: indirect descriptors, `VIRTIO_RING_F_EVENT_IDX`, discard/write-zeroes, multiple queues, MSI-X, the modern (virtio 1.0) transport.

## MSR emulation
We don't have MTRR MSR emulation, the Linux kernel can know this and handle them correctly by setting the MTRR MSRs to be 0.
