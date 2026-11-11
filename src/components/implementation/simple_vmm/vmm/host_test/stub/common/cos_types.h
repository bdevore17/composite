#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8_t; typedef uint16_t u16_t; typedef uint32_t u32_t; typedef uint64_t u64_t;
typedef unsigned long word_t; typedef unsigned long vaddr_t; typedef unsigned long paddr_t;
typedef unsigned long thdid_t, thdcap_t, compid_t, capid_t, cbuf_t;
typedef capid_t vm_vmcscap_t, vm_msrbitmapcap_t, vm_lapiccap_t, vm_shared_mem_t, vm_lapicaccesscap_t, vm_vmcb_t;
#define PAGE_SIZE_4K 4096
#define likely(x) (x)
#define unlikely(x) (x)
