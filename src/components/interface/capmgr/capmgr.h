/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Copyright 2018, The George Washington University
 * Author: Phani Gadepalli, phanikishoreg@gwu.edu
 *
 * Copyright 2020, The George Washington University
 * Author: Gabe Parmer, gparmer@gwu.edu
 */

#ifndef CAPMGR_H
#define CAPMGR_H

/***
 * The capability manager is in charge of the controlling and
 * allocating resource tables (capability and page tables). It
 * controls creating all kernel resources, and the protocols for
 * delegation and revocation.
 */

#include <cos_kernel_api.h>
#include <cos_defkernel_api.h>
#include <cos_stubs.h>

void capmgr_set_tls(thdcap_t cap, void* tls_addr);

thdcap_t  capmgr_initthd_create(spdid_t child, thdid_t *tid);
thdcap_t  COS_STUB_DECL(capmgr_initthd_create)(spdid_t child, thdid_t *tid);

thdcap_t  capmgr_initaep_create(spdid_t child, struct cos_aep_info *aep, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax, asndcap_t *sndret);
thdcap_t  COS_STUB_DECL(capmgr_initaep_create)(spdid_t child, struct cos_aep_info *aep, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax, asndcap_t *sndret);

thdcap_t  capmgr_thd_create(cos_thd_fn_t fn, void *data, thdid_t *tid);
thdcap_t  capmgr_aep_create(struct cos_aep_info *a, cos_aepthd_fn_t fn, void *data, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax);
arcvcap_t capmgr_rcv_alloc(cos_thd_fn_t fn, void *data, int flags, asndcap_t *asnd, thdcap_t *thdcap, thdid_t *tid);

thdcap_t capmgr_thd_create_thunk(thdclosure_index_t idx, thdid_t *tid);
thdcap_t  COS_STUB_DECL(capmgr_thd_create_thunk)(thdclosure_index_t idx, thdid_t *tid);

thdcap_t  capmgr_aep_create_thunk(struct cos_aep_info *a, thdclosure_index_t idx, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax);
thdcap_t  COS_STUB_DECL(capmgr_aep_create_thunk)(struct cos_aep_info *a, thdclosure_index_t idx, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax);

thdcap_t  capmgr_thd_create_ext(spdid_t child, thdclosure_index_t idx, thdid_t *tid);
thdcap_t  COS_STUB_DECL(capmgr_thd_create_ext)(spdid_t child, thdclosure_index_t idx, thdid_t *tid);

thdcap_t  capmgr_aep_create_ext(spdid_t child, struct cos_aep_info *a, thdclosure_index_t idx, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax, arcvcap_t *extrcv);
thdcap_t  COS_STUB_DECL(capmgr_aep_create_ext)(spdid_t child, struct cos_aep_info *a, thdclosure_index_t idx, int owntc, cos_channelkey_t key, microsec_t ipiwin, u32_t ipimax, arcvcap_t *extrcv);

arcvcap_t capmgr_rcv_create(thdclosure_index_t idx, int flags, asndcap_t *asnd, thdcap_t *thdcap, thdid_t *tid);
arcvcap_t COS_STUB_DECL(capmgr_rcv_create)(thdclosure_index_t idx, int flags, asndcap_t *asnd, thdcap_t *thdcap, thdid_t *tid);

/*
 * Release a thread created by capmgr_thd_create* (in any component)
 * on behalf of the calling component, which must be the component the
 * thread capability was given to (the scheduler, or the component
 * itself for capmgr_thd_create). The thread, its capabilities and
 * kernel memory are deallocated, and its thread id can be reused. The
 * thread must not be executing: call this on the core the thread was
 * created on, and not on the thread itself. Returns 0 on success,
 * -EINVAL if tid is unknown or on another core, -EPERM if the caller
 * doesn't have the thread, -EBUSY if it is the calling thread, or the
 * kernel's error.
 */
int capmgr_thd_release(thdid_t tid);
int COS_STUB_DECL(capmgr_thd_release)(thdid_t tid);

/*
 * Release a rcv end-point created by capmgr_rcv_create (identified by
 * its thread's tid), along with its asnd, thread, and capabilities.
 * Same constraints as capmgr_thd_release. The kernel cannot free
 * tcaps, so the rcv's tcap is kept by the capmgr and reused for this
 * component's next rcv on the same core.
 */
int capmgr_rcv_release(thdid_t tid);
int COS_STUB_DECL(capmgr_rcv_release)(thdid_t tid);

/*
 * Component recovery: the number of times component `id` was reset
 * after a fault, -ECOMPRESET if it faulted and could not be recovered
 * (it remains unavailable), or -EINVAL if it isn't recoverable.
 */
int capmgr_comp_resets(compid_t id);

asndcap_t capmgr_asnd_create(spdid_t child, thdid_t t);
asndcap_t COS_STUB_DECL(capmgr_asnd_create)(spdid_t child, thdid_t t);

asndcap_t capmgr_asnd_rcv_create(arcvcap_t rcv);
asndcap_t COS_STUB_DECL(capmgr_asnd_rcv_create)(arcvcap_t rcv);

asndcap_t capmgr_asnd_key_create(cos_channelkey_t key);
asndcap_t COS_STUB_DECL(capmgr_asnd_key_create)(cos_channelkey_t key);

vaddr_t capmgr_shared_kernel_page_create(vaddr_t *resource);
vaddr_t COS_STUB_DECL(capmgr_shared_kernel_page_create)(vaddr_t *resource);

vaddr_t capmgr_vm_shared_kernel_page_create_at(compid_t comp_id, vaddr_t addr);
vaddr_t COS_STUB_DECL(capmgr_vm_shared_kernel_page_create_at)(compid_t comp_id, vaddr_t addr);

compid_t capmgr_vm_comp_create(u64_t mem_sz);
compid_t COS_STUB_DECL(capmgr_vm_comp_create)(u64_t mem_sz);

capid_t capmgr_vm_vmcs_create(void);
capid_t COS_STUB_DECL(capmgr_vm_vmcs_create)(void);

capid_t capmgr_vm_msr_bitmap_create(void);
capid_t COS_STUB_DECL(capmgr_vm_msr_bitmap_create)(void);

capid_t capmgr_vm_lapic_create(vaddr_t *page);
capid_t COS_STUB_DECL(capmgr_vm_lapic_create)(vaddr_t *page);

capid_t capmgr_vm_shared_region_create(vaddr_t *page);
capid_t COS_STUB_DECL(capmgr_vm_shared_region_create)(vaddr_t *page);

capid_t capmgr_vm_lapic_access_create(vaddr_t mem);
compid_t COS_STUB_DECL(capmgr_vm_lapic_access_create)(vaddr_t mem);

capid_t capmgr_vm_vmcb_create(vm_vmcscap_t vmcs_cap, vm_msrbitmapcap_t msr_bitmap_cap, vm_lapicaccesscap_t lapic_access_cap, vm_lapiccap_t lapic_cap, vm_shared_mem_t shared_mem_cap, thdid_t handler_thd_id, word_t vpid);
capid_t COS_STUB_DECL(capmgr_vm_vmcb_create)(vm_vmcscap_t vmcs_cap, vm_msrbitmapcap_t msr_bitmap_cap, vm_lapicaccesscap_t lapic_access_cap, vm_lapiccap_t lapic_cap, vm_shared_mem_t shared_mem_cap, thdid_t handler_thd_id, word_t vpid);

thdcap_t capmgr_vm_vcpu_create(compid_t vm_comp, vm_vmcb_t vmcb_cap, thdid_t *tid);
thdcap_t COS_STUB_DECL(capmgr_vm_vcpu_create)(compid_t vm_comp, vm_vmcb_t vmcb_cap, thdid_t *tid);

#endif /* CAPMGR_H */
