#include <loader.h>
#include <initargs.h>
#include <string.h>
#include <stdlib.h>

compid_t
loader_comp_id(const char *name)
{
	struct initargs      entries, curr;
	struct initargs_iter i;
	int                  cont, keylen;

	/* The composer lists the deferred components we can start in "loadable" */
	if (args_get_entry("loadable", &entries)) return 0;
	for (cont = args_iter(&entries, &i, &curr); cont; cont = args_iter_next(&i, &curr)) {
		if (!strcmp(args_value(&curr), name)) return atoi(args_key(&curr, &keylen));
	}

	return 0;
}
