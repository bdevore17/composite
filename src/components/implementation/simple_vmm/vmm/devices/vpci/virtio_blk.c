/*
 * virtio-blk device model: legacy virtio PCI transport, one request queue,
 * synchronous requests against a vblk_backend (RAM disk by default).
 *
 * References: "Virtual I/O Device (VIRTIO) Version 1.1", sections 2.6
 * (split virtqueues), 4.1.4.8 (legacy PCI interface) and 5.2 (block device);
 * the legacy register layout is that of Linux's virtio_pci_legacy.c.
 *
 * Concurrency: all register accesses arrive through io_handler(), which holds
 * the VM's device lock, so at most one vCPU is in here at a time. Requests are
 * processed on the vCPU that kicked the queue, inside that kick.
 *
 * Everything in guest memory (ring indices, descriptors, request headers) is
 * guest-controlled: it is validated, and a malformed request fails that
 * request rather than the VMM.
 */
#include <cos_types.h>
#include <cos_debug.h>
#include <llprint.h>
#include <string.h>
#include <vmrt.h>

#include "vpci.h"
#include "virtio_blk.h"
#include "../vblk/vblk_backend.h"
#include "../../virq.h"

#ifndef VM_BLK_DISK_MB
#define VM_BLK_DISK_MB 16
#endif

#define VIRTIO_VENDOR_ID       0x1AF4
#define VIRTIO_BLK_LEGACY_DEV  0x1001 /* transitional virtio-blk */
#define VIRTIO_TYPE_BLOCK      2
#define PCI_CLASS_STORAGE      0x01
#define PCI_SUBCLASS_STORAGE_SCSI 0x00

/* Legacy virtio header register offsets (Linux include/uapi/linux/virtio_pci.h) */
#define VPCI_HOST_FEATURES  0x00 /* 32, RO */
#define VPCI_GUEST_FEATURES 0x04 /* 32, RW */
#define VPCI_QUEUE_PFN      0x08 /* 32, RW */
#define VPCI_QUEUE_NUM      0x0C /* 16, RO */
#define VPCI_QUEUE_SEL      0x0E /* 16, RW */
#define VPCI_QUEUE_NOTIFY   0x10 /* 16, WO */
#define VPCI_STATUS         0x12 /*  8, RW */
#define VPCI_ISR            0x13 /*  8, RO, read clears */
#define VPCI_CONFIG         0x14 /* device config, no MSI-X */

#define VIRTIO_PCI_QUEUE_ADDR_SHIFT 12
#define VIRTIO_PCI_VRING_ALIGN      4096

/* Feature bits (virtio 1.1, 5.2.3) */
#define VIRTIO_BLK_F_SEG_MAX  2
#define VIRTIO_BLK_F_RO       5
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH    9

/* Request types and status values (virtio 1.1, 5.2.6) */
#define VIRTIO_BLK_T_IN     0
#define VIRTIO_BLK_T_OUT    1
#define VIRTIO_BLK_T_FLUSH  4
#define VIRTIO_BLK_T_GET_ID 8
#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2
#define VIRTIO_BLK_ID_BYTES 20

#define VRING_DESC_F_NEXT          1
#define VRING_DESC_F_WRITE         2
#define VRING_DESC_F_INDIRECT      4
#define VRING_AVAIL_F_NO_INTERRUPT 1

#define VIRTIO_CONFIG_S_DRIVER_OK 4

/* Data segments per request we advertise (seg_max); + header + status */
#define VBLK_SEG_MAX  (VIRTIO_BLK_QUEUE_SIZE - 2)
#define VBLK_MAX_SEGS (VBLK_SEG_MAX + 2)

struct vblk_desc {
	u64_t addr;
	u32_t len;
	u16_t flags;
	u16_t next;
} __attribute__((packed));

struct vblk_avail {
	u16_t flags;
	u16_t idx;
	u16_t ring[];
} __attribute__((packed));

struct vblk_used_elem {
	u32_t id;
	u32_t len;
} __attribute__((packed));

struct vblk_used {
	u16_t flags;
	u16_t idx;
	struct vblk_used_elem ring[];
} __attribute__((packed));

/* Legacy layout of struct virtio_blk_config, as far as we fill it in */
struct virtio_blk_config {
	u64_t capacity; /* in 512-byte sectors */
	u32_t size_max;
	u32_t seg_max;
	struct {
		u16_t cylinders;
		u8_t  heads;
		u8_t  sectors;
	} geometry;
	u32_t blk_size;
	u8_t  physical_block_exp;
	u8_t  alignment_offset;
	u16_t min_io_size;
	u32_t opt_io_size;
} __attribute__((packed));

struct virtio_blk_outhdr {
	u32_t type;
	u32_t ioprio;
	u64_t sector;
} __attribute__((packed));

/* One guest buffer of a request, translated */
struct vblk_seg {
	char *hva;
	u32_t len;
	int   writable; /* device-writable (VRING_DESC_F_WRITE) */
};

struct vblk_dev {
	struct vmrt_vm_comp       *vm;
	struct vpci_config_space  *pci; /* live PCI config space (BAR0) */
	struct vblk_backend        be;

	/* virtio header registers */
	u32_t host_features;
	u32_t guest_features;
	u32_t queue_pfn;
	u16_t queue_sel;
	u8_t  status;
	u8_t  isr;

	struct virtio_blk_config config;

	/* the request queue, valid while queue_pfn != 0 */
	volatile struct vblk_desc  *desc;
	volatile struct vblk_avail *avail;
	volatile struct vblk_used  *used;
	u16_t last_avail;

	/* statistics, for the debug log */
	u64_t nreq, nread, nwrite, nerr;
};

static struct vblk_dev vblk;
static int vblk_present = 0;

static struct vpci_config_type0 virtio_blk_pci = {
	.header.vendor_id     = VIRTIO_VENDOR_ID,
	.header.device_id     = VIRTIO_BLK_LEGACY_DEV,
	.header.command       = 0,
	.header.status        = 0,
	.header.revision_id   = 0,
	.header.prog_if       = 0,
	.header.subclass      = PCI_SUBCLASS_STORAGE_SCSI,
	.header.class_code    = PCI_CLASS_STORAGE,
	.header.header_type   = PCI_HDR_TYPE_DEV,
	/* Legacy virtio: a single I/O BAR, no capabilities, no MSI-X */
	.bars[0].io_bar.fixed_bit = 1,
	.bars[0].io_bar.reserved  = 0,
	.bars[0].io_bar.base_addr = VIRTIO_BLK_IO_ADDR >> 2,
	.subsystem_vendor_id  = VIRTIO_VENDOR_ID,
	.subsystem_id         = VIRTIO_TYPE_BLOCK,
	.cap_pointer          = 0,
	/*
	 * INTx pin A, routed (there is no _PRT or PIRQ table for the guest) to
	 * the ISA IRQ in the interrupt line register, which Linux uses as is.
	 */
	.interrupt_line       = VIRQ_GSI_VIRTIO_BLK,
	.interrupt_pin        = 1,
};

/* Guest-physical to host-virtual for [gpa, gpa + len), or NULL if not all guest RAM */
static void *
vblk_gpa2hva(u64_t gpa, u64_t len)
{
	/* Guest RAM starts at GPA PAGE_SIZE_4K in this VMM (see GPA2HVA()). */
	u64_t ram_start = PAGE_SIZE_4K;
	u64_t ram_end   = PAGE_SIZE_4K + vblk.vm->guest_mem_sz;

	if (gpa < ram_start || gpa >= ram_end) return NULL;
	if (len > ram_end - gpa) return NULL;

	return GPA2HVA(gpa, vblk.vm);
}

static inline u16_t
vblk_iobase(void)
{
	/* BAR0 of an I/O BAR: bits 31:2 are the address, bit 0 the I/O flag */
	return (u16_t)(((u32_t *)vblk.pci)[OFFSET_BAR0 / 4] & 0xFFFC);
}

static void
vblk_queue_reset(void)
{
	vblk.queue_pfn  = 0;
	vblk.desc       = NULL;
	vblk.avail      = NULL;
	vblk.used       = NULL;
	vblk.last_avail = 0;
}

static void
vblk_reset(void)
{
	VMRT_INFO("virtio-blk: device reset\n");
	vblk.guest_features = 0;
	vblk.queue_sel      = 0;
	vblk.status         = 0;
	vblk.isr            = 0;
	vblk_queue_reset();
}

/* The guest wrote the queue's page frame number: map the legacy vring (virtio 1.1, 2.6.2) */
static void
vblk_queue_setup(u32_t pfn)
{
	u64_t  qsz = VIRTIO_BLK_QUEUE_SIZE;
	u64_t  gpa, used_off, size;
	char  *hva;

	if (vblk.queue_sel != 0) return; /* only queue 0 exists */
	if (pfn == 0) {
		vblk_queue_reset();
		return;
	}

	gpa      = (u64_t)pfn << VIRTIO_PCI_QUEUE_ADDR_SHIFT;
	used_off = qsz * sizeof(struct vblk_desc) + (3 + qsz) * sizeof(u16_t);
	used_off = (used_off + VIRTIO_PCI_VRING_ALIGN - 1) & ~(u64_t)(VIRTIO_PCI_VRING_ALIGN - 1);
	size     = used_off + 3 * sizeof(u16_t) + qsz * sizeof(struct vblk_used_elem);

	hva = vblk_gpa2hva(gpa, size);
	if (!hva) {
		printc("virtio-blk: queue pfn 0x%x outside guest RAM, queue disabled\n", pfn);
		vblk_queue_reset();
		return;
	}

	vblk.queue_pfn  = pfn;
	vblk.desc       = (volatile struct vblk_desc *)hva;
	vblk.avail      = (volatile struct vblk_avail *)(hva + qsz * sizeof(struct vblk_desc));
	vblk.used       = (volatile struct vblk_used *)(hva + used_off);
	vblk.last_avail = 0;
	VMRT_INFO("virtio-blk: queue 0 at gpa 0x%llx, %llu entries\n", (unsigned long long)gpa,
	          (unsigned long long)qsz);
}

/*
 * Walk the descriptor chain at `head` into `segs`. Returns the number of
 * segments, or -1 if the chain is malformed.
 */
static int
vblk_chain(u16_t head, struct vblk_seg *segs)
{
	u16_t idx = head;
	int   n;

	for (n = 0; n < VBLK_MAX_SEGS; n++) {
		struct vblk_desc d;

		if (idx >= VIRTIO_BLK_QUEUE_SIZE) return -1;
		d = *(struct vblk_desc *)&vblk.desc[idx];
		/* We do not offer VIRTIO_RING_F_INDIRECT_DESC */
		if (d.flags & VRING_DESC_F_INDIRECT) return -1;

		segs[n].hva      = vblk_gpa2hva(d.addr, d.len);
		segs[n].len      = d.len;
		segs[n].writable = !!(d.flags & VRING_DESC_F_WRITE);
		if (!segs[n].hva && d.len) return -1;

		if (!(d.flags & VRING_DESC_F_NEXT)) return n + 1;
		idx = d.next;
	}

	return -1; /* too long, or a loop */
}

/*
 * Execute one request. The layout (virtio 1.1, 5.2.6) is a 16-byte header the
 * device reads, the data, and one status byte the device writes; we accept
 * the header and data split over descriptors any way the driver likes, as
 * long as each data segment is a whole number of sectors (Linux's always
 * are). Returns the number of bytes written into guest buffers.
 */
static u32_t
vblk_process(u16_t head)
{
	struct vblk_seg segs[VBLK_MAX_SEGS];
	struct virtio_blk_outhdr hdr;
	u8_t  *status_p;
	u8_t   status  = VIRTIO_BLK_S_OK;
	u32_t  written = 0;
	u64_t  off;
	int    n, i, first_data, last_data;
	u32_t  hdr_done = 0;

	n = vblk_chain(head, segs);
	if (n < 2) {
		printc("virtio-blk: malformed descriptor chain at %u, request dropped\n", head);
		vblk.nerr++;
		return 0;
	}

	/* Status: the last byte of the last descriptor, which must be device-writable */
	if (!segs[n - 1].writable || segs[n - 1].len < 1) {
		printc("virtio-blk: request %u has no status byte, dropped\n", head);
		vblk.nerr++;
		return 0;
	}
	status_p = (u8_t *)segs[n - 1].hva + segs[n - 1].len - 1;
	segs[n - 1].len--;

	/* Header: the first 16 device-readable bytes */
	for (i = 0; i < n && hdr_done < sizeof(hdr); i++) {
		u32_t take = segs[i].len < sizeof(hdr) - hdr_done ? segs[i].len : sizeof(hdr) - hdr_done;

		if (segs[i].writable) break;
		memcpy((char *)&hdr + hdr_done, segs[i].hva, take);
		hdr_done      += take;
		segs[i].hva   += take;
		segs[i].len   -= take;
	}
	if (hdr_done < sizeof(hdr)) {
		status = VIRTIO_BLK_S_IOERR;
		goto done;
	}
	first_data = i;
	/* a header descriptor that also carried data */
	if (first_data > 0 && segs[first_data - 1].len) first_data--;
	last_data = n - 1; /* the status descriptor may carry data before the status byte */

	vblk.nreq++;
	switch (hdr.type) {
	case VIRTIO_BLK_T_IN:
	case VIRTIO_BLK_T_OUT: {
		int   is_read = (hdr.type == VIRTIO_BLK_T_IN);
		u64_t total   = 0;

		if (!is_read && vblk.be.readonly) {
			status = VIRTIO_BLK_S_IOERR;
			break;
		}
		/* Validate the whole request before touching the disk or the guest */
		for (i = first_data; i <= last_data; i++) {
			if (segs[i].len == 0) continue;
			if (segs[i].writable != is_read || segs[i].len % VBLK_SECTOR_SIZE) {
				status = VIRTIO_BLK_S_IOERR;
				break;
			}
			total += segs[i].len;
		}
		if (status != VIRTIO_BLK_S_OK) break;
		if (hdr.sector > vblk.be.size / VBLK_SECTOR_SIZE
		    || total > vblk.be.size - hdr.sector * VBLK_SECTOR_SIZE) {
			VMRT_INFO("virtio-blk: %s beyond end of disk (sector %llu, %llu bytes)\n",
			          is_read ? "read" : "write", (unsigned long long)hdr.sector,
			          (unsigned long long)total);
			status = VIRTIO_BLK_S_IOERR;
			break;
		}

		off = hdr.sector * VBLK_SECTOR_SIZE;
		for (i = first_data; i <= last_data; i++) {
			int r;

			if (segs[i].len == 0) continue;
			r = is_read ? vblk.be.ops->read(&vblk.be, off, segs[i].hva, segs[i].len)
			            : vblk.be.ops->write(&vblk.be, off, segs[i].hva, segs[i].len);
			if (r) {
				status = VIRTIO_BLK_S_IOERR;
				break;
			}
			off += segs[i].len;
			if (is_read) written += segs[i].len;
		}
		if (is_read) vblk.nread++;
		else         vblk.nwrite++;
		VMRT_DEBUG("virtio-blk: %s sector %llu, %llu bytes: %u\n", is_read ? "read" : "write",
		           (unsigned long long)hdr.sector, (unsigned long long)total, status);
		break;
	}
	case VIRTIO_BLK_T_FLUSH:
		if (vblk.be.ops->flush(&vblk.be)) status = VIRTIO_BLK_S_IOERR;
		break;
	case VIRTIO_BLK_T_GET_ID: {
		/* Up to 20 bytes of serial number, not necessarily NUL-terminated */
		u32_t idlen = strlen(vblk.be.name);

		if (idlen > VIRTIO_BLK_ID_BYTES) idlen = VIRTIO_BLK_ID_BYTES;
		for (i = first_data; i <= last_data && segs[i].len == 0; i++)
			;
		if (i > last_data || !segs[i].writable) {
			status = VIRTIO_BLK_S_IOERR;
			break;
		}
		if (idlen > segs[i].len) idlen = segs[i].len;
		memset(segs[i].hva, 0, segs[i].len < VIRTIO_BLK_ID_BYTES ? segs[i].len : VIRTIO_BLK_ID_BYTES);
		memcpy(segs[i].hva, vblk.be.name, idlen);
		written = segs[i].len < VIRTIO_BLK_ID_BYTES ? segs[i].len : VIRTIO_BLK_ID_BYTES;
		break;
	}
	default:
		VMRT_DEBUG("virtio-blk: unsupported request type %u\n", hdr.type);
		status = VIRTIO_BLK_S_UNSUPP;
		break;
	}

done:
	if (status != VIRTIO_BLK_S_OK) vblk.nerr++;
	*status_p = status;

	return written + 1;
}

/* Publish a completed request in the used ring (virtio 1.1, 2.6.8) */
static void
vblk_push_used(u16_t head, u32_t len)
{
	u16_t uidx = vblk.used->idx;

	vblk.used->ring[uidx % VIRTIO_BLK_QUEUE_SIZE].id  = head;
	vblk.used->ring[uidx % VIRTIO_BLK_QUEUE_SIZE].len = len;
	/* The element must be visible before the index (x86: stores are ordered; stop the compiler) */
	__atomic_thread_fence(__ATOMIC_RELEASE);
	vblk.used->idx = uidx + 1;
}

/* The guest kicked the queue: run every available request */
static void
vblk_notify(void)
{
	int completed = 0;

	if (!vblk.desc || !(vblk.status & VIRTIO_CONFIG_S_DRIVER_OK)) return;

	while (1) {
		u16_t avail_idx = vblk.avail->idx;
		u16_t head;

		/* Read the ring entries only after the index that covers them */
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (vblk.last_avail == avail_idx) break;
		if ((u16_t)(avail_idx - vblk.last_avail) > VIRTIO_BLK_QUEUE_SIZE) {
			printc("virtio-blk: avail index %u is %u ahead of us, driver confused; queue stopped\n",
			       avail_idx, (u16_t)(avail_idx - vblk.last_avail));
			vblk_queue_reset();
			return;
		}

		head = vblk.avail->ring[vblk.last_avail % VIRTIO_BLK_QUEUE_SIZE];
		vblk.last_avail++;
		vblk_push_used(head, vblk_process(head));
		completed++;
	}

	if (completed) virtio_blk_complete();
}

/*
 * Interrupt the guest after requests were completed, unless it asked not to
 * be (VRING_AVAIL_F_NO_INTERRUPT). Called with the device lock held.
 */
void
virtio_blk_complete(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (!vblk.avail || (vblk.avail->flags & VRING_AVAIL_F_NO_INTERRUPT)) return;
	vblk.isr |= 1;
	virq_pulse(vblk.vm, VIRQ_GSI_VIRTIO_BLK);
}

int
virtio_blk_handles_port(u16_t port)
{
	u16_t base;

	if (!vblk_present) return 0;
	base = vblk_iobase();

	return base != 0 && port >= base && port < base + VIRTIO_BLK_IO_SIZE;
}

static u32_t
vblk_reg_read(u16_t off, int bytes)
{
	u8_t  regs[VPCI_CONFIG];
	u8_t *src;
	u32_t v = 0;

	if (off >= VPCI_CONFIG) {
		u16_t coff = off - VPCI_CONFIG;

		if ((size_t)coff + (size_t)bytes > sizeof(vblk.config)) return 0; /* reads past the config are zero */
		src = (u8_t *)&vblk.config + coff;
	} else {
		if (off + bytes > VPCI_CONFIG) return 0;
		memset(regs, 0, sizeof(regs));
		memcpy(&regs[VPCI_HOST_FEATURES], &vblk.host_features, 4);
		memcpy(&regs[VPCI_GUEST_FEATURES], &vblk.guest_features, 4);
		memcpy(&regs[VPCI_QUEUE_PFN], &vblk.queue_pfn, 4);
		if (vblk.queue_sel == 0) {
			u16_t qnum = VIRTIO_BLK_QUEUE_SIZE;
			memcpy(&regs[VPCI_QUEUE_NUM], &qnum, 2);
		}
		memcpy(&regs[VPCI_QUEUE_SEL], &vblk.queue_sel, 2);
		regs[VPCI_STATUS] = vblk.status;
		regs[VPCI_ISR]    = vblk.isr;
		src = &regs[off];
		/* Reading the ISR acknowledges (and deasserts) the interrupt */
		if (off <= VPCI_ISR && VPCI_ISR < off + bytes) vblk.isr = 0;
	}
	memcpy(&v, src, bytes);

	return v;
}

static void
vblk_reg_write(u16_t off, int bytes, u32_t v)
{
	switch (off) {
	case VPCI_GUEST_FEATURES:
		if (bytes != 4) goto bad;
		/* Only features we offered can be acknowledged */
		vblk.guest_features = v & vblk.host_features;
		break;
	case VPCI_QUEUE_PFN:
		if (bytes != 4) goto bad;
		vblk_queue_setup(v);
		break;
	case VPCI_QUEUE_SEL:
		if (bytes != 2) goto bad;
		vblk.queue_sel = (u16_t)v;
		break;
	case VPCI_QUEUE_NOTIFY:
		if (bytes != 2) goto bad;
		if ((u16_t)v == 0) vblk_notify();
		break;
	case VPCI_STATUS:
		if (bytes != 1) goto bad;
		if ((u8_t)v == 0) {
			vblk_reset();
		} else {
			vblk.status = (u8_t)v;
			if (vblk.status & VIRTIO_CONFIG_S_DRIVER_OK) VMRT_INFO("virtio-blk: driver ready\n");
		}
		break;
	default:
		goto bad;
	}
	return;
bad:
	VMRT_INFO("virtio-blk: ignoring %d-byte write of 0x%x to register 0x%x\n", bytes, v, off);
}

void
virtio_blk_handler(u16_t port, int dir, int sz, struct vmrt_vm_vcpu *vcpu)
{
	volatile struct vm_vcpu_shared_region *regs = vcpu->shared_region;
	u16_t off   = port - vblk_iobase();
	int   bytes = (sz == IO_BYTE) ? 1 : (sz == IO_WORD) ? 2 : 4;

	assert(vblk_present && off < VIRTIO_BLK_IO_SIZE);

	if (dir == IO_IN) {
		u64_t ax = regs->ax;
		u32_t v  = vblk_reg_read(off, bytes);

		/* Only the accessed width of RAX changes */
		if (bytes == 1)      ax = (ax & ~0xFFULL) | v;
		else if (bytes == 2) ax = (ax & ~0xFFFFULL) | v;
		else                 ax = v;
		regs->ax = ax;
	} else {
		u32_t v = (u32_t)regs->ax;

		if (bytes == 1)      v &= 0xFF;
		else if (bytes == 2) v &= 0xFFFF;
		vblk_reg_write(off, bytes, v);
	}
}

void
virtio_blk_init(struct vmrt_vm_comp *vm)
{
	u64_t size = (u64_t)VM_BLK_DISK_MB * 1024 * 1024;
	int   ret;

	if (size == 0) {
		printc("VMM: virtio-blk disabled (VM_BLK_DISK_MB = 0)\n");
		return;
	}

	memset(&vblk, 0, sizeof(vblk));
	vblk.vm = vm;

	ret = vblk_ramdisk_create(&vblk.be, size);
	if (ret) {
		printc("VMM: virtio-blk disabled: cannot allocate a %lluMB RAM disk (%d)\n",
		       (unsigned long long)VM_BLK_DISK_MB, ret);
		return;
	}

	vblk.host_features = (1U << VIRTIO_BLK_F_SEG_MAX) | (1U << VIRTIO_BLK_F_BLK_SIZE) | (1U << VIRTIO_BLK_F_FLUSH);
	if (vblk.be.readonly) vblk.host_features |= 1U << VIRTIO_BLK_F_RO;

	vblk.config.capacity = vblk.be.size / VBLK_SECTOR_SIZE;
	vblk.config.seg_max  = VBLK_SEG_MAX;
	vblk.config.blk_size = VBLK_SECTOR_SIZE;

	vblk_reset();
	vblk.pci     = vpci_regist((struct vpci_config_space *)&virtio_blk_pci, sizeof(virtio_blk_pci));
	vblk_present = 1;

	printc("VMM: virtio-blk: %llu sectors (%lluMB, %s) at I/O 0x%x, IRQ %u\n",
	       (unsigned long long)vblk.config.capacity, (unsigned long long)VM_BLK_DISK_MB, vblk.be.name,
	       vblk_iobase(), VIRQ_GSI_VIRTIO_BLK);
}
