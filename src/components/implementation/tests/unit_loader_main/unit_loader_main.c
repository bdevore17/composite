/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * A component with a main, started at runtime (`boot = "deferred"`)
 * by the unit_loader test. Its main reports its progress through the
 * (also deferred) test server, waits for the test to let it finish,
 * and returns.
 */

#include <cos_component.h>
#include <llprint.h>
#include <sched.h>
#include <loadtest.h>

static long runs; /* reset to 0 each time the component is started */

int
main(void)
{
	runs++;
	loadtest_report(LOADTEST_MAIN_STARTED);
	while (loadtest_reported() != LOADTEST_MAIN_GO) sched_thd_yield();
	loadtest_report(LOADTEST_MAIN_DONE + runs);

	return 0;
}
