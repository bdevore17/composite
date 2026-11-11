#ifndef COS_RECOVERY_H
#define COS_RECOVERY_H

/***
 * Client-side convention for component recovery (see the capmgr's
 * documentation): when a server component faults, the invocations
 * that were executing in it, and the invocations made while it is
 * being reset, return -ECOMPRESET (an int/long return value; other
 * return types get it cast). The server's state is lost: it was
 * reset to its initial state. The invocation can be retried, after
 * re-creating any server state the client depends on.
 */

#include <cos_types.h>
#include <consts.h>

/* Did an invocation fail because the server faulted, and was (or is being) reset? */
static inline int
cos_inv_reset(long ret)
{
	return ret == -ECOMPRESET;
}

/*
 * Make an invocation, `call` (an expression returning a status), and
 * retry it while it fails because the server is being reset, at most
 * `tries` times in total. Evaluates to the last return value. The
 * server is unavailable for the duration of its reset: a client with
 * a higher priority than the thread recovering the server, on the
 * same core, can spin here until it gives up.
 */
#define COS_INV_RETRY(tries, call)                                           \
	({                                                                   \
		long __cos_ret;                                              \
		int  __cos_try = 0;                                          \
		do {                                                         \
			__cos_ret = (long)(call);                            \
		} while (cos_inv_reset(__cos_ret) && ++__cos_try < (tries)); \
		__cos_ret;                                                   \
	})

#endif /* COS_RECOVERY_H */
