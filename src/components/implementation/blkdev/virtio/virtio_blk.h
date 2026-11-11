/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Definitions for the legacy (virtio 0.9.5 / "transitional") PCI
 * interface of virtio-blk. References: Virtual I/O Device (VIRTIO)
 * Version 1.1, sections 2.6 (split virtqueues), 4.1.4.8 (legacy PCI
 * interface) and 5.2 (block device).
 */

#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <cos_types.h>

#define VIRTIO_PCI_VENDOR 0x1af4
#define VIRTIO_PCI_DEV_BLK_LEGACY 0x1001 /* transitional virtio-blk */
#define VIRTIO_PCI_DEV_BLK_MODERN 0x1042 /* modern-only virtio-blk (not supported) */

/* Legacy I/O BAR register offsets */
#define VIRTIO_PCI_HOST_FEATURES 0x00  /* 32 bit, RO */
#define VIRTIO_PCI_GUEST_FEATURES 0x04 /* 32 bit */
#define VIRTIO_PCI_QUEUE_PFN 0x08      /* 32 bit: physical page number of the queue */
#define VIRTIO_PCI_QUEUE_NUM 0x0c      /* 16 bit, RO: queue size */
#define VIRTIO_PCI_QUEUE_SEL 0x0e      /* 16 bit */
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10   /* 16 bit */
#define VIRTIO_PCI_STATUS 0x12         /* 8 bit */
#define VIRTIO_PCI_ISR 0x13            /* 8 bit, read clears */
#define VIRTIO_PCI_CONFIG 0x14         /* device config (MSI-X disabled) */

#define VIRTIO_PCI_QUEUE_ADDR_SHIFT 12
#define VIRTIO_PCI_VRING_ALIGN 4096

/* Device status bits */
#define VIRTIO_STATUS_ACK 1
#define VIRTIO_STATUS_DRIVER 2
#define VIRTIO_STATUS_DRIVER_OK 4
#define VIRTIO_STATUS_FAILED 128

/* virtio-blk feature bits */
#define VIRTIO_BLK_F_SIZE_MAX 1
#define VIRTIO_BLK_F_SEG_MAX 2
#define VIRTIO_BLK_F_RO 5
#define VIRTIO_BLK_F_FLUSH 9

/* virtio-blk device config layout (offsets from VIRTIO_PCI_CONFIG) */
#define VIRTIO_BLK_CFG_CAPACITY 0 /* 64 bit, in 512-byte sectors */
#define VIRTIO_BLK_CFG_SIZE_MAX 8 /* 32 bit */
#define VIRTIO_BLK_CFG_SEG_MAX 12 /* 32 bit */

/* Request types and status */
#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_S_OK 0

struct virtio_blk_req_hdr {
	u32_t type;
	u32_t reserved;
	u64_t sector;
};

/* Split virtqueue */
#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2 /* device writes (vs. reads) this buffer */

struct vring_desc {
	u64_t addr;
	u32_t len;
	u16_t flags;
	u16_t next;
};

struct vring_avail {
	u16_t flags;
	u16_t idx;
	u16_t ring[];
};

struct vring_used_elem {
	u32_t id;
	u32_t len;
};

struct vring_used {
	u16_t                  flags;
	u16_t                  idx;
	struct vring_used_elem ring[];
};

#define VRING_AVAIL_F_NO_INTERRUPT 1

#endif /* VIRTIO_BLK_H */
