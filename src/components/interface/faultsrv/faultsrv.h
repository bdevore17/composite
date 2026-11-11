#ifndef FAULTSRV_H
#define FAULTSRV_H

/***
 * A test interface for component recovery: a server that faults on
 * demand. Every function can return -ECOMPRESET if the server faulted
 * (and was reset) during the invocation, or is being reset.
 */

#include <cos_component.h>
#include <cos_stubs.h>

/* The server faults on every FAULTSRV_PERIOD-th faultsrv_call since its (re)initialization */
#define FAULTSRV_PERIOD 5

/* Fault types for faultsrv_fault */
typedef enum
{
	FAULTSRV_NULL_READ,
	FAULTSRV_NULL_WRITE,
	FAULTSRV_DIV_ZERO,
	FAULTSRV_INV_OPCODE,
	FAULTSRV_GEN_PROT,
	FAULTSRV_NTYPES,
} faultsrv_fault_t;

/*
 * Increment the server's call counter, and return it, except for
 * every FAULTSRV_PERIOD-th call, which faults (null dereference).
 */
long faultsrv_call(void);
/* Fault with the given type (faultsrv_fault_t). */
long faultsrv_fault(long type);
/* Spin for `iters` iterations inside the server, then return 2 * arg + 1. */
long faultsrv_spin(long arg, long iters);
/* The number of invocations of faultsrv_spin currently inside the server. */
long faultsrv_inside(void);
/*
 * The address of the heap memory that the server allocated in its
 * initialization, or -1 if that memory was not zeroed.
 */
long faultsrv_heap(void);
/*
 * Allocate shared memory: the server can't be reset anymore (its next
 * fault leaves it unavailable). Returns 0 on success.
 */
long faultsrv_unrecoverable(void);
/* The server's component id */
long faultsrv_id(void);

#endif /* FAULTSRV_H */
