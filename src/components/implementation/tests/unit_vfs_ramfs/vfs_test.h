/*
 * Tiny test harness shared by the vfs unit tests: individual check
 * failures print "FAILURE: ..." lines, each test case prints
 * "SUCCESS: <case>" or "FAILURE: <case>", and the final line is
 * "TEST PASSED" or "TEST FAILED".
 */

#ifndef VFS_TEST_H
#define VFS_TEST_H

#include <llprint.h>

static int vt_case_failures, vt_failures;

#define VT_CHECK(cond)                                                                                 \
	do {                                                                                           \
		if (!(cond)) {                                                                         \
			vt_case_failures++;                                                            \
			printc("FAILURE: [%ld] %s:%d: %s\n", cos_compid(), __func__, __LINE__, #cond); \
		}                                                                                      \
	} while (0)

#define VT_CASE(fn)                                                                                    \
	do {                                                                                           \
		vt_case_failures = 0;                                                                  \
		fn();                                                                                  \
		printc("%s: [%ld] %s\n", vt_case_failures ? "FAILURE" : "SUCCESS", cos_compid(), #fn); \
		vt_failures += vt_case_failures;                                                       \
	} while (0)

#define VT_DONE() printc("[%ld] %s\n", cos_compid(), vt_failures ? "TEST FAILED" : "TEST PASSED")

/* Deterministic data pattern; identical to cosfs_pattern() in tools/cosfs/host_env.h. */
static inline unsigned char
vt_pattern(unsigned long i, unsigned seed)
{
	return (unsigned char)((i * 131 + (i >> 9) * 7 + seed * 17 + 3) & 0xff);
}

#endif
