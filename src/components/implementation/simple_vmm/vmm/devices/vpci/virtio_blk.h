#pragma once

/*
 * virtio-blk, legacy ("transitional", virtio 0.9.5) PCI transport: an I/O BAR
 * holding the common virtio header followed by struct virtio_blk_config,
 * INTx interrupts, a single request queue. This is the transport the guest's
 * CONFIG_VIRTIO_PCI_LEGACY driver binds to, the same one virtio-net uses.
 *
 * The disk contents come from a vblk_backend (devices/vblk/vblk_backend.h);
 * by default a RAM disk of VM_BLK_DISK_MB megabytes (composition constant,
 * 0 removes the device).
 */

#include <cos_types.h>
#include <vmrt.h>

/* Initial I/O BAR; the guest may move it (we decode wherever BAR0 points) */
#define VIRTIO_BLK_IO_ADDR 0x8000
/* Header (0x14 bytes) + device configuration; the decoded window */
#define VIRTIO_BLK_IO_SIZE 0x40

/* Request queue size; legacy devices dictate it (power of 2) */
#define VIRTIO_BLK_QUEUE_SIZE 128

void virtio_blk_init(struct vmrt_vm_comp *vm);
int  virtio_blk_handles_port(u16_t port);
void virtio_blk_handler(u16_t port, int dir, int sz, struct vmrt_vm_vcpu *vcpu);
/*
 * Raise the completion interrupt for requests already pushed to the used
 * ring. Called with the VM's device lock held; for an asynchronous backend
 * this is the hook its completion path would use.
 */
void virtio_blk_complete(void);
