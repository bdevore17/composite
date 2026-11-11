/* Host-side unit test for simple_vmm's virtio-blk device model (see Makefile):
 * drives it the way Linux's legacy virtio-pci + virtio_blk drivers do, over a
 * fake guest RAM. Prints PASS/FAIL per check and TEST PASSED/TEST FAILED. */
#include <cos_types.h>
#include <vmrt.h>
#include <string.h>
#include <stdlib.h>
#include "vpci.h"
#include "virtio_blk.h"

static struct vpci_config_space slot;
struct vpci_config_space *vpci_regist(struct vpci_config_space *v, u32_t sz) { memcpy(&slot, v, sz); return &slot; }
static int pulses; static u32_t last_gsi;
void virq_pulse(struct vmrt_vm_comp *vm, u32_t gsi) { pulses++; last_gsi = gsi; }

#define RAM (4 << 20)
static char *ram; static struct vmrt_vm_comp vm; static struct vm_vcpu_shared_region sr; static struct vmrt_vm_vcpu vcpu;
static u16_t base;
static int fails;
#define CHECK(c, msg) do { if (c) printf("PASS: %s\n", msg); else { printf("FAIL: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)
static void *G(u64_t gpa) { return ram + gpa - 4096; }
static void out(u16_t off, int sz, u32_t v) { sr.ax = v; virtio_blk_handler(base + off, IO_OUT, sz, &vcpu); }
static u32_t in(u16_t off, int sz) { sr.ax = 0xdeadbeefcafef00dULL; virtio_blk_handler(base + off, IO_IN, sz, &vcpu); return sz == IO_BYTE ? (u8_t)sr.ax : sz == IO_WORD ? (u16_t)sr.ax : (u32_t)sr.ax; }

struct desc { u64_t addr; u32_t len; u16_t flags, next; } __attribute__((packed));
#define QSZ 128
#define RING 0x10000
static struct desc *D; static u16_t *avail; static u16_t *used; static u16_t aidx;
static void ring_init(void) { D = G(RING); avail = (u16_t *)(D + QSZ); used = G(RING + 4096 + 4096 * 0 + ((QSZ * 16 + (3 + QSZ) * 2 + 4095) & ~4095) - 4096); }
static void submit(u16_t head) { avail[2 + (aidx % QSZ)] = head; aidx++; __sync_synchronize(); avail[1] = aidx; out(0x10, IO_WORD, 0); }
static u16_t used_idx(void) { return used[1]; }
static u32_t used_len(u16_t i) { u32_t *e = (u32_t *)(used + 2); return e[2 * (i % QSZ) + 1]; }
static u32_t used_id(u16_t i) { u32_t *e = (u32_t *)(used + 2); return e[2 * (i % QSZ)]; }
struct hdr { u32_t type, ioprio; u64_t sector; } __attribute__((packed));
#define HDR 0x30000
#define DATA 0x40000
#define ST 0x31000
static void setd(int i, u64_t a, u32_t l, u16_t f, u16_t n) { D[i].addr = a; D[i].len = l; D[i].flags = f; D[i].next = n; }

int main(void)
{
	ram = calloc(1, RAM); vm.guest_addr = ram; vm.guest_mem_sz = RAM; vm.ioapic = (void *)1;
	vcpu.shared_region = &sr; vcpu.vm = &vm;
	virtio_blk_init(&vm);
	base = ((u32_t *)&slot)[4] & 0xFFFC;
	CHECK(base == 0x8000, "BAR0 at 0x8000");
	CHECK(slot.header.vendor_id == 0x1af4 && slot.header.device_id == 0x1001, "PCI ids");
	CHECK(((u8_t *)&slot)[0x3d] == 1 && ((u8_t *)&slot)[0x3c] == 10, "INTx pin A, line 10");
	CHECK(virtio_blk_handles_port(0x8000) && virtio_blk_handles_port(0x803f) && !virtio_blk_handles_port(0x8040) && !virtio_blk_handles_port(0x4000), "port decode");

	/* Linux virtio_pci_legacy probe + virtio_blk probe */
	out(0x12, IO_BYTE, 0); out(0x12, IO_BYTE, 1); out(0x12, IO_BYTE, 3);
	u32_t f = in(0x00, IO_LONG);
	CHECK(f == ((1 << 2) | (1 << 6) | (1 << 9)), "host features SEG_MAX|BLK_SIZE|FLUSH");
	out(0x04, IO_LONG, f | (1u << 28));
	CHECK(in(0x04, IO_LONG) == f, "guest features masked to offered");
	u64_t cap = 0; for (int i = 0; i < 8; i++) cap |= (u64_t)in(0x14 + i, IO_BYTE) << (8 * i);
	CHECK(cap == 32768, "capacity 16MB = 32768 sectors (byte reads)");
	CHECK(in(0x20, IO_LONG) == 126, "seg_max");
	CHECK(in(0x28, IO_LONG) == 512, "blk_size");
	CHECK(in(0x3c, IO_LONG) == 0, "read past config is 0");
	out(0x0e, IO_WORD, 1); CHECK(in(0x0c, IO_WORD) == 0, "queue 1 absent");
	out(0x0e, IO_WORD, 0); CHECK(in(0x0c, IO_WORD) == QSZ, "queue 0 size 128");
	out(0x08, IO_LONG, (RAM + 0x100000) >> 12); CHECK(in(0x08, IO_LONG) == 0, "pfn outside RAM refused");
	out(0x08, IO_LONG, RING >> 12); CHECK(in(0x08, IO_LONG) == RING >> 12, "pfn accepted");
	ring_init();
	out(0x12, IO_BYTE, 7);

	/* write 2 sectors at sector 8, split data */
	struct hdr *h = G(HDR); u8_t *st = G(ST); char *data = G(DATA);
	for (int i = 0; i < 1024; i++) data[i] = (char)(i * 7 + 1);
	h->type = 1; h->sector = 8;
	setd(0, HDR, 16, 1, 1); setd(1, DATA, 512, 1, 2); setd(2, DATA + 512, 512, 1, 3); setd(3, ST, 1, 2 | 0, 0);
	*st = 0xff; pulses = 0; submit(0);
	CHECK(used_idx() == 1 && used_id(0) == 0 && used_len(0) == 1 && *st == 0, "write OK, used len 1");
	CHECK(pulses == 1 && last_gsi == 10, "irq pulsed on GSI 10");
	CHECK(in(0x13, IO_BYTE) == 1 && in(0x13, IO_BYTE) == 0, "ISR read-to-clear");

	/* read it back into one buffer */
	char *rb = G(DATA + 0x2000); memset(rb, 0, 1024);
	h->type = 0; setd(0, HDR, 16, 1, 1); setd(1, DATA + 0x2000, 1024, 3, 2); setd(2, ST, 1, 2, 0);
	*st = 0xff; submit(0);
	CHECK(used_idx() == 2 && used_len(1) == 1025 && *st == 0 && !memcmp(rb, data, 1024), "read back matches, used len 1025");

	/* header and data in one descriptor (out) */
	char *cmb = G(0x50000); struct hdr *h2 = (void *)cmb; h2->type = 1; h2->ioprio = 0; h2->sector = 100;
	memset(cmb + 16, 0x5a, 512);
	setd(5, 0x50000, 16 + 512, 1, 6); setd(6, ST, 1, 2, 0); *st = 0xff; submit(5);
	h->type = 0; h->sector = 100; setd(0, HDR, 16, 1, 1); setd(1, DATA + 0x3000, 512, 3, 2); setd(2, ST, 1, 2, 0); submit(0);
	char *c3 = G(DATA + 0x3000); int ok = 1; for (int i = 0; i < 512; i++) ok &= c3[i] == 0x5a;
	CHECK(ok && *st == 0, "header+data combined descriptor");

	/* errors */
	h->type = 0; h->sector = 32767; setd(1, DATA, 1024, 3, 2); *st = 0xff; submit(0);
	CHECK(*st == 1, "read beyond end -> IOERR");
	h->sector = 0; setd(1, DATA, 100, 3, 2); *st = 0xff; submit(0);
	CHECK(*st == 1, "partial sector -> IOERR");
	h->type = 1; setd(1, DATA, 512, 3, 2); *st = 0xff; submit(0);
	CHECK(*st == 1, "write from device-writable buffer -> IOERR");
	h->type = 4; setd(0, HDR, 16, 1, 1); setd(1, ST, 1, 2, 0); *st = 0xff; submit(0);
	CHECK(*st == 0, "flush OK");
	h->type = 8; setd(0, HDR, 16, 1, 1); setd(1, DATA, 20, 3, 2); setd(2, ST, 1, 2, 0); *st = 0xff; submit(0);
	CHECK(*st == 0 && !strncmp(G(DATA), "cos-vmm-ramdisk", 15), "GET_ID");
	h->type = 99; setd(0, HDR, 16, 1, 1); setd(1, ST, 1, 2, 0); *st = 0xff; submit(0);
	CHECK(*st == 2, "unknown type -> UNSUPP");
	u16_t before = used_idx();
	setd(0, HDR, 16, 1, 1); setd(1, ST, 1, 3, 200); submit(0);
	CHECK(used_idx() == before + 1 && used_len(before) == 0, "bad next index: dropped, chain returned with len 0");
	setd(0, 0x7fffffff, 16, 1, 1); setd(1, ST, 1, 2, 0); before = used_idx(); submit(0);
	CHECK(used_idx() == before + 1, "descriptor outside RAM handled");
	setd(0, HDR, 16, 1, 0); before = used_idx(); submit(0);
	CHECK(used_idx() == before + 1, "chain without status handled");
	/* loop: 0 -> 1 -> 0 */
	setd(0, HDR, 16, 1, 1); setd(1, DATA, 512, 1, 0); before = used_idx(); submit(0);
	CHECK(used_idx() == before + 1, "descriptor loop handled");
	/* NO_INTERRUPT */
	avail[0] = 1; pulses = 0; h->type = 4; setd(0, HDR, 16, 1, 1); setd(1, ST, 1, 2, 0); submit(0);
	CHECK(pulses == 0, "VRING_AVAIL_F_NO_INTERRUPT honoured");
	avail[0] = 0;
	/* bogus avail index */
	avail[1] = aidx + 500; out(0x10, IO_WORD, 0);
	CHECK(in(0x08, IO_LONG) == 0, "insane avail idx stops the queue");
	/* reset */
	out(0x12, IO_BYTE, 0);
	CHECK(in(0x12, IO_BYTE) == 0 && in(0x04, IO_LONG) == 0, "reset");
	printf(fails ? "TEST FAILED (%d)\n" : "TEST PASSED\n", fails);
	return !!fails;
}
