/* Host-side unit test for the vlapic IPI emulation (see Makefile): INIT/SIPI
 * bring-up, fixed IPIs with every destination mode and shorthand, and vIOAPIC
 * delivery, against stubbed vmrt delivery functions that record calls. */
#include <cos_types.h>
#include <vmrt.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "vlapic.h"
#include "vlapic_priv.h"
#include "apicreg.h"

static int npost[16][256], ninit[16], nsipi[16], sipivec[16];
void vmrt_vcpu_post_intr(struct vmrt_vm_vcpu *v, u8_t vec) { npost[v->cpuid][vec]++; }
void vmrt_vcpu_init_ipi(struct vmrt_vm_vcpu *v) { ninit[v->cpuid]++; }
void vmrt_vcpu_sipi(struct vmrt_vm_vcpu *v, u8_t vec) { nsipi[v->cpuid]++; sipivec[v->cpuid] = vec; }
void vmrt_dump_vcpu(struct vmrt_vm_vcpu *v) {}
int32_t apic_write_vmexit_handler(struct vmrt_vm_vcpu *vcpu);

#define N 4
static struct vmrt_vm_comp vm; static struct acrn_vlapic vl[N]; static struct vm_vcpu_shared_region sr[N];
static int fails;
#define CHECK(c, m) do { if (c) printf("PASS: %s\n", m); else { printf("FAIL: %s (line %d)\n", m, __LINE__); fails++; } } while (0)
static void clr(void) { memset(npost, 0, sizeof npost); memset(ninit, 0, sizeof ninit); memset(nsipi, 0, sizeof nsipi); }
static void wr(int c, u32_t off, u32_t v) { *(u32_t *)((char *)vl[c].apic_page + off) = v; sr[c].qualification = off; apic_write_vmexit_handler(&vm.vcpus[c]); }
static void ipi(int c, u32_t hi, u32_t lo) { wr(c, 0x310, hi); wr(c, 0x300, lo); }
static int posted(int vec, int mask) { for (int i = 0; i < N; i++) if (!!npost[i][vec] != !!(mask & (1 << i))) return 0; return 1; }

int main(void)
{
	vm.num_vcpu = N;
	for (int i = 0; i < N; i++) {
		struct vmrt_vm_vcpu *v = &vm.vcpus[i];
		v->cpuid = i; v->vm = &vm; v->shared_region = &sr[i]; v->lapic_page = aligned_alloc(4096, 4096); v->vlapic = &vl[i];
		vl[i].apic_page = v->lapic_page; vl[i].vcpu = v;
		vlapic_reset(&vl[i]);
	}
	CHECK(vl[0].msr_apicbase & 0x100 && !(vl[1].msr_apicbase & 0x100), "BSP flag only on vCPU 0");
	CHECK(*(u32_t *)((char *)vl[2].apic_page + 0x20) == (2u << 24), "APIC ID register = vCPU index");

	/* Linux AP bring-up of CPU 2 (physical dest) */
	clr();
	ipi(0, 2 << 24, APIC_TRIGMOD_LEVEL | APIC_LEVEL_ASSERT | APIC_DELMODE_INIT);
	ipi(0, 2 << 24, APIC_TRIGMOD_LEVEL | APIC_DELMODE_INIT);
	ipi(0, 2 << 24, APIC_DELMODE_STARTUP | 0x9d);
	ipi(0, 2 << 24, APIC_DELMODE_STARTUP | 0x9d);
	CHECK(ninit[2] == 1 && ninit[0] + ninit[1] + ninit[3] == 0, "INIT assert delivered once, de-assert ignored");
	CHECK(nsipi[2] == 2 && sipivec[2] == 0x9d, "both SIPIs reach the target (vmrt ignores the 2nd)");
	CHECK(!(*(u32_t *)((char *)vl[0].apic_page + 0x300) & APIC_DELSTAT_PEND), "ICR delivery status idle");

	/* enable all APICs, flat logical model */
	for (int i = 0; i < N; i++) { wr(i, 0xe0, 0xffffffff); wr(i, 0xd0, (1u << i) << 24); wr(i, 0xf0, 0x1ff); }

	clr(); ipi(1, 3 << 24, 0xfd); CHECK(posted(0xfd, 1 << 3), "fixed physical to vCPU 3");
	clr(); ipi(1, 0x5 << 24, APIC_DESTMODE_LOG | 0xfb); CHECK(posted(0xfb, 0x5), "fixed logical flat mask 0x5");
	clr(); ipi(1, 0, APIC_DEST_ALLESELF | 0xfc); CHECK(posted(0xfc, 0xf & ~2), "all-excluding-self");
	clr(); ipi(1, 0, APIC_DEST_ALLISELF | 0xfc); CHECK(posted(0xfc, 0xf), "all-including-self");
	clr(); ipi(2, 0, APIC_DEST_SELF | 0xf6); CHECK(posted(0xf6, 1 << 2), "self");
	clr(); ipi(0, 0xffu << 24, 0xf2); CHECK(posted(0xf2, 0xf), "physical broadcast 0xff");
	clr(); ipi(0, 9 << 24, 0xf2); CHECK(posted(0xf2, 0), "absent APIC ID dropped");
	clr(); ipi(0, 1 << 24, 0x05); CHECK(posted(0x05, 0), "illegal vector not delivered");
	clr(); ipi(0, 1 << 24, APIC_DELMODE_NMI); CHECK(posted(2, 0), "NMI dropped (unsupported)");
	clr(); ipi(0, 0, APIC_DEST_SELF | APIC_DELMODE_INIT); CHECK(ninit[0] == 0, "self INIT ignored");
	/* software-disabled target */
	wr(3, 0xf0, 0xff); clr(); ipi(0, 3 << 24, 0xfd); CHECK(posted(0xfd, 0), "disabled APIC drops fixed IPI");
	/* IOAPIC-style delivery */
	wr(3, 0xf0, 0x1ff); clr(); vlapic_receive_intr(&vm, false, 0x8, false, IOAPIC_RTE_DELMODE_FIXED, 0x22, false); CHECK(posted(0x22, 1 << 3), "ioapic logical dest 0x8 -> vCPU 3");
	clr(); vlapic_receive_intr(&vm, false, 1, true, IOAPIC_RTE_DELMODE_FIXED, 0x23, false); CHECK(posted(0x23, 1 << 1), "ioapic physical dest 1");
	printf(fails ? "TEST FAILED (%d)\n" : "TEST PASSED\n", fails);
	return !!fails;
}
