#pragma once

#include <vmrt.h>

/*
 * Interrupt lines of emulated devices.
 *
 * A device raises its line on the VM's vIOAPIC; the redirection entry the
 * guest programmed there picks the vector and the destination vCPU(s), and
 * the vlapic posts it to them (vmrt_vcpu_post_intr()). This is what makes
 * device interrupts correct with several vCPUs, where the guest's vector
 * assignment is per CPU and not known in advance.
 */

/* ISA/PCI INTx lines (GSIs) used by the emulated devices */
#define VIRQ_GSI_VIRTIO_NET 0  /* legacy virtio-net reports no INTx pin, so Linux uses IRQ 0 */
#define VIRQ_GSI_SERIAL     4  /* COM1 */
#define VIRQ_GSI_VIRTIO_BLK 10 /* virtio-blk's INTx pin A, routed to IRQ 10 via PCI_INTERRUPT_LINE */

/* Pulse `gsi` as an edge-triggered interrupt. Safe from any thread on any core. */
void virq_pulse(struct vmrt_vm_comp *vm, u32_t gsi);

/*
 * For the two devices that predate multi-vCPU support (serial, virtio-net):
 * with one vCPU, keep injecting the fixed vector `legacy_vector` exactly as
 * before (that is the configuration that has been tested on hardware); with
 * more than one vCPU, use the vIOAPIC line `gsi` instead, because the guest's
 * vector for the line then depends on which CPU Linux bound the IRQ to.
 */
void virq_raise_legacy(struct vmrt_vm_vcpu *vcpu, u32_t gsi, u8_t legacy_vector, int autoeoi);
