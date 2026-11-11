/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * blkdev.virtio: a virtio-blk driver for the legacy (transitional) PCI
 * interface, as provided by QEMU's default virtio-blk-pci device
 * (`-drive file=disk.img,if=virtio,format=raw`).
 *
 * Design:
 * - The device is found by scanning PCI for vendor 0x1af4, device
 *   0x1001, and driven through its I/O-port BAR (the kernel runs
 *   components with IOPL 3).
 * - One virtqueue (queue 0) lives in physically contiguous memory from
 *   contigmem. Requests are submitted one at a time under a lock and
 *   completion is detected by polling the used ring (interrupts are
 *   disabled with VRING_AVAIL_F_NO_INTERRUPT).
 * - Zero copy: each client registers a shared buffer; its pages are
 *   translated to physical addresses once (memmgr_virt_to_phys) and
 *   requests use one descriptor per page-sized piece of the transfer,
 *   so the device DMAs directly to/from the client's buffer. Large
 *   transfers are split to respect the device's segment limit.
 */

#include <cos_component.h>
#include <cos_types.h>
#include <llprint.h>
#include <string.h>
#include <memmgr.h>
#include <contigmem.h>
#include <pci.h>
#include <io.h>
#include <blkdev.h>
#include "virtio_blk.h"

#define VBLK_MAX_PCI_DEVS 64
#define VBLK_MAX_SEGS 64           /* data segments per request we use at most */
#define VBLK_MAX_CLIENT_PAGES 1024 /* largest client buffer: 4 MiB */
#define PTE_ADDR_MASK 0x000ffffffffff000UL

#define mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define cbar() __asm__ __volatile__("" ::: "memory")

struct vblk_client {
	char    *shm;
	size_t   npages;
	paddr_t *paddrs; /* physical address of each page of shm */
};

static struct {
	int                         ready;
	u16_t                       io;       /* I/O port base */
	u32_t                       features; /* negotiated */
	u64_t                       capacity; /* sectors */
	int                         readonly;
	unsigned int                max_segs; /* data segments per request */
	u16_t                       qsz;
	struct vring_desc          *desc;
	struct vring_avail         *avail;
	volatile struct vring_used *used;
	u16_t                       last_used;
	/* request header and status byte, in DMA-able memory */
	struct virtio_blk_req_hdr *hdr;
	volatile u8_t             *status;
	paddr_t                    hdr_pa, status_pa;
	unsigned long              nreqs;
} vblk;

static struct vblk_client vblk_clients[MAX_NUM_COMPS];
static struct ps_lock     vblk_lock;
static struct pci_dev     pci_devs[VBLK_MAX_PCI_DEVS];

static paddr_t
virt_to_phys(void *va)
{
	vaddr_t page = round_to_page((vaddr_t)va);
	paddr_t pa   = memmgr_virt_to_phys(page) & PTE_ADDR_MASK;

	return pa ? pa + ((vaddr_t)va - page) : 0;
}

/* Bytes needed for a legacy split virtqueue of qsz entries. */
static size_t
vring_size(u16_t qsz)
{
	size_t avail_end = sizeof(struct vring_desc) * qsz + sizeof(u16_t) * (3 + qsz);

	return round_up_to_page(avail_end) + round_up_to_page(sizeof(u16_t) * 3 + sizeof(struct vring_used_elem) * qsz);
}

static int
vblk_find(struct pci_dev *out)
{
	int n = pci_dev_count(), i;

	if (n > VBLK_MAX_PCI_DEVS) n = VBLK_MAX_PCI_DEVS;
	if (pci_scan(pci_devs, n)) return -ENODEV;
	for (i = 0; i < n; i++) {
		if (pci_devs[i].vendor != VIRTIO_PCI_VENDOR) continue;
		if (pci_devs[i].device == VIRTIO_PCI_DEV_BLK_LEGACY) {
			*out = pci_devs[i];
			return 0;
		}
		if (pci_devs[i].device == VIRTIO_PCI_DEV_BLK_MODERN) {
			printc("blkdev.virtio: found a modern-only virtio-blk device; only the legacy interface "
			       "is supported (use QEMU's default, or disable-legacy=off)\n");
		}
	}

	return -ENODEV;
}

static int
vblk_init(void)
{
	struct pci_dev dev;
	u32_t          bar0, cmd, host_features;
	size_t         qbytes;
	char          *ring, *req;

	if (vblk_find(&dev)) {
		printc("blkdev.virtio: no virtio-blk device found\n");
		return -ENODEV;
	}
	bar0 = pci_config_read(dev.bus, dev.dev, dev.func, 0x10);
	if (!(bar0 & 1)) {
		printc("blkdev.virtio: BAR0 is not an I/O BAR\n");
		return -ENODEV;
	}
	vblk.io = bar0 & ~0x3;
	/* enable I/O space decoding and bus mastering (DMA) */
	cmd = pci_config_read(dev.bus, dev.dev, dev.func, 0x04);
	pci_config_write(dev.bus, dev.dev, dev.func, 0x04, (cmd & 0xffff) | 0x1 | 0x4);

	/* reset, then acknowledge the device */
	outb(vblk.io + VIRTIO_PCI_STATUS, 0);
	outb(vblk.io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK);
	outb(vblk.io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

	host_features = inl(vblk.io + VIRTIO_PCI_HOST_FEATURES);
	vblk.features = host_features
	                & ((1 << VIRTIO_BLK_F_RO) | (1 << VIRTIO_BLK_F_FLUSH) | (1 << VIRTIO_BLK_F_SEG_MAX));
	outl(vblk.io + VIRTIO_PCI_GUEST_FEATURES, vblk.features);
	vblk.readonly = !!(vblk.features & (1 << VIRTIO_BLK_F_RO));

	vblk.capacity = (u64_t)inl(vblk.io + VIRTIO_PCI_CONFIG + VIRTIO_BLK_CFG_CAPACITY)
	                | ((u64_t)inl(vblk.io + VIRTIO_PCI_CONFIG + VIRTIO_BLK_CFG_CAPACITY + 4) << 32);

	/* the request queue */
	outw(vblk.io + VIRTIO_PCI_QUEUE_SEL, 0);
	vblk.qsz = inw(vblk.io + VIRTIO_PCI_QUEUE_NUM);
	if (vblk.qsz < 4) {
		printc("blkdev.virtio: unusable queue size %u\n", vblk.qsz);
		goto fail;
	}
	vblk.max_segs = VBLK_MAX_SEGS;
	if (vblk.max_segs > vblk.qsz - 2u) vblk.max_segs = vblk.qsz - 2;
	if (vblk.features & (1 << VIRTIO_BLK_F_SEG_MAX)) {
		u32_t seg_max = inl(vblk.io + VIRTIO_PCI_CONFIG + VIRTIO_BLK_CFG_SEG_MAX);

		if (seg_max && seg_max < vblk.max_segs) vblk.max_segs = seg_max;
	}

	qbytes = vring_size(vblk.qsz);
	ring   = (char *)contigmem_alloc(qbytes / PAGE_SIZE);
	req    = (char *)contigmem_alloc(1);
	if (!ring || !req) {
		printc("blkdev.virtio: cannot allocate DMA memory\n");
		goto fail;
	}
	memset(ring, 0, qbytes);
	memset(req, 0, PAGE_SIZE);
	vblk.desc         = (struct vring_desc *)ring;
	vblk.avail        = (struct vring_avail *)(ring + sizeof(struct vring_desc) * vblk.qsz);
	vblk.used         = (struct vring_used *)(ring
                                          + round_up_to_page(sizeof(struct vring_desc) * vblk.qsz
	                                                             + sizeof(u16_t) * (3 + vblk.qsz)));
	vblk.avail->flags = VRING_AVAIL_F_NO_INTERRUPT; /* we poll */
	vblk.hdr          = (struct virtio_blk_req_hdr *)req;
	vblk.status       = (volatile u8_t *)(req + 64);
	vblk.hdr_pa       = virt_to_phys(vblk.hdr);
	vblk.status_pa    = virt_to_phys((void *)vblk.status);
	if (!vblk.hdr_pa || !virt_to_phys(ring)) {
		printc("blkdev.virtio: cannot translate DMA addresses\n");
		goto fail;
	}
	outl(vblk.io + VIRTIO_PCI_QUEUE_PFN, virt_to_phys(ring) >> VIRTIO_PCI_QUEUE_ADDR_SHIFT);

	outb(vblk.io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
	vblk.ready = 1;
	printc("blkdev.virtio: %02x:%02x.%x io 0x%x, %llu sectors (%llu MiB)%s, queue %u, max %u segments%s\n", dev.bus,
	       dev.dev, dev.func, vblk.io, (unsigned long long)vblk.capacity, (unsigned long long)(vblk.capacity >> 11),
	       vblk.readonly ? ", read-only" : "", vblk.qsz, vblk.max_segs,
	       (vblk.features & (1 << VIRTIO_BLK_F_FLUSH)) ? ", flush" : "");

	return 0;
fail:
	outb(vblk.io + VIRTIO_PCI_STATUS, VIRTIO_STATUS_FAILED);
	return -EIO;
}

/*
 * Submit one request (descriptors 0..ndesc-1 are filled in) and poll
 * for its completion. Returns 0 or -EIO.
 */
static int
vblk_submit(int ndesc)
{
	u16_t idx;

	/* descriptor chain 0 -> 1 -> ... -> ndesc-1 */
	vblk.desc[ndesc - 1].flags &= ~VRING_DESC_F_NEXT;
	*vblk.status = 0xff;

	idx                              = vblk.avail->idx;
	vblk.avail->ring[idx % vblk.qsz] = 0;
	mb(); /* descriptors and ring entry before the index */
	vblk.avail->idx = idx + 1;
	mb(); /* index before the notification */
	outw(vblk.io + VIRTIO_PCI_QUEUE_NOTIFY, 0);

	while (vblk.used->idx == vblk.last_used) { __asm__ __volatile__("pause" ::: "memory"); }
	mb(); /* read the status after seeing the completion */
	vblk.last_used++;
	vblk.nreqs++;

	return *vblk.status == VIRTIO_BLK_S_OK ? 0 : -EIO;
}

static void
desc_set(int i, paddr_t pa, u32_t len, u16_t flags)
{
	vblk.desc[i] = (struct vring_desc){.addr = pa, .len = len, .flags = flags | VRING_DESC_F_NEXT, .next = i + 1};
}

/*
 * Transfer nsect sectors between the device and the client buffer at
 * off, using at most max_segs data descriptors per request.
 */
static long
vblk_rw(struct vblk_client *c, u64_t sector, unsigned long nsect, unsigned long off, int write)
{
	u64_t bytes = (u64_t)nsect * BLKDEV_SECTOR_SIZE;
	u64_t done  = 0;
	int   ret;

	while (done < bytes) {
		int nd = 1;

		vblk.hdr->type     = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
		vblk.hdr->reserved = 0;
		vblk.hdr->sector   = sector + done / BLKDEV_SECTOR_SIZE;
		desc_set(0, vblk.hdr_pa, sizeof(struct virtio_blk_req_hdr), 0);

		/* data segments: page-sized pieces, ending on a sector boundary */
		{
			u64_t start = done;

			while (done < bytes && (unsigned int)(nd - 1) < vblk.max_segs) {
				unsigned long pos = off + done;
				unsigned long pg  = pos / PAGE_SIZE;
				u64_t         len = PAGE_SIZE - pos % PAGE_SIZE;

				if (len > bytes - done) len = bytes - done;
				desc_set(nd++, c->paddrs[pg] + pos % PAGE_SIZE, len, write ? 0 : VRING_DESC_F_WRITE);
				done += len;
			}
			/* a request must cover whole sectors: back off a partial last sector */
			if (done % BLKDEV_SECTOR_SIZE) {
				u64_t extra = done % BLKDEV_SECTOR_SIZE;

				if (vblk.desc[nd - 1].len == extra) {
					nd--;
				} else {
					vblk.desc[nd - 1].len -= extra;
				}
				done -= extra;
			}
			if (done == start) return -EIO; /* cannot happen with max_segs >= 2 */
		}
		desc_set(nd++, vblk.status_pa, 1, VRING_DESC_F_WRITE);
		if ((ret = vblk_submit(nd))) return ret;
	}

	return nsect;
}

static struct vblk_client *
caller(void)
{
	compid_t id = (compid_t)cos_inv_token();

	return id < MAX_NUM_COMPS ? &vblk_clients[id] : NULL;
}

long
blkdev_shm_attach(cbuf_t shm, unsigned long npages)
{
	struct vblk_client *c    = caller();
	vaddr_t             addr = 0;
	unsigned long       n, i;
	long                ret = 0;

	if (!c) return -EINVAL;
	ps_lock_take(&vblk_lock);
	if (c->shm) {
		ret = -EEXIST;
		goto done;
	}
	n = memmgr_shared_page_map(shm, &addr);
	if (!n || !addr || n > VBLK_MAX_CLIENT_PAGES) {
		ret = -EINVAL;
		goto done;
	}
	c->paddrs = (paddr_t *)memmgr_heap_page_allocn(round_up_to_page(n * sizeof(paddr_t)) / PAGE_SIZE);
	if (!c->paddrs) {
		ret = -ENOMEM;
		goto done;
	}
	for (i = 0; i < n; i++) {
		c->paddrs[i] = virt_to_phys((char *)addr + i * PAGE_SIZE);
		if (!c->paddrs[i]) {
			ret = -EFAULT;
			goto done;
		}
	}
	c->shm    = (char *)addr;
	c->npages = n;
done:
	ps_lock_release(&vblk_lock);
	return ret;
}

long
blkdev_capacity(void)
{
	if (!vblk.ready) return -ENODEV;

	return vblk.capacity;
}

static long
blkdev_rw(unsigned long sector, unsigned long nsect, unsigned long off, int write)
{
	struct vblk_client *c = caller();
	long                ret;

	if (!vblk.ready) return -ENODEV;
	if (!c) return -EINVAL;
	if (!c->shm) return -ENOBUFS;
	if (write && vblk.readonly) return -EROFS;
	if (nsect == 0 || sector >= vblk.capacity || nsect > vblk.capacity - sector) return -EINVAL;
	if (off >= c->npages * PAGE_SIZE || nsect > (c->npages * PAGE_SIZE - off) / BLKDEV_SECTOR_SIZE) return -EINVAL;

	ps_lock_take(&vblk_lock);
	ret = vblk_rw(c, sector, nsect, off, write);
	ps_lock_release(&vblk_lock);

	return ret;
}

long
blkdev_read(unsigned long sector, unsigned long nsect, unsigned long shm_off)
{
	return blkdev_rw(sector, nsect, shm_off, 0);
}

long
blkdev_write(unsigned long sector, unsigned long nsect, unsigned long shm_off)
{
	return blkdev_rw(sector, nsect, shm_off, 1);
}

long
blkdev_flush(void)
{
	long ret;

	if (!vblk.ready) return -ENODEV;
	if (!(vblk.features & (1 << VIRTIO_BLK_F_FLUSH))) return -ENOTSUP;

	ps_lock_take(&vblk_lock);
	vblk.hdr->type     = VIRTIO_BLK_T_FLUSH;
	vblk.hdr->reserved = 0;
	vblk.hdr->sector   = 0;
	desc_set(0, vblk.hdr_pa, sizeof(struct virtio_blk_req_hdr), 0);
	desc_set(1, vblk.status_pa, 1, VRING_DESC_F_WRITE);
	ret = vblk_submit(2);
	ps_lock_release(&vblk_lock);

	return ret;
}

void
cos_init(void)
{
	vblk_init();
}

/*
 * A passive server has no main. Defining a (trivial) parallel init
 * tells the init protocol that, so the initialization threads exit
 * after it instead of looking for a main to run.
 */
void
cos_parallel_init(coreid_t cid, int init_core, int ncores)
{
}
