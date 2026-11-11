/*
 * The interface of the test server (tests.unit_loader_server) that is
 * started and stopped at runtime by tests.unit_loader. While the
 * server isn't running, all of these functions return -ENOENT.
 */

#ifndef LOADTEST_H
#define LOADTEST_H

#include <cos_component.h>

/* Values reported by the test component with a main (tests.unit_loader_main) */
#define LOADTEST_MAIN_STARTED 1
#define LOADTEST_MAIN_GO 2
#define LOADTEST_SHARED_MAGIC 0x5a5a5a5a
#define LOADTEST_MAIN_DONE 100 /* + the number of times its main executed since it was started */

/* The number of calls to this function since the server was started */
long loadtest_calls(void);
long COS_STUB_DECL(loadtest_calls)(void);
/* 1 if the server's data and bss were in their initial state when it was initialized */
long loadtest_fresh(void);
long COS_STUB_DECL(loadtest_fresh)(void);
/* The thread that initialized the server */
long loadtest_init_tid(void);
long COS_STUB_DECL(loadtest_init_tid)(void);
/* The heap memory allocated when the server was initialized (0 if the allocation failed) */
long loadtest_heap(void);
long COS_STUB_DECL(loadtest_heap)(void);
/* The shared memory (cbuf id) allocated when the server was initialized, with LOADTEST_SHARED_MAGIC written in its
 * first word */
long loadtest_shared(void);
long COS_STUB_DECL(loadtest_shared)(void);
/* Store and retrieve a value */
long loadtest_report(long v);
long COS_STUB_DECL(loadtest_report)(long v);
long loadtest_reported(void);
long COS_STUB_DECL(loadtest_reported)(void);

#endif /* LOADTEST_H */
