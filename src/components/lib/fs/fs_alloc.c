/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * A trivial power-of-two segregated free-list allocator on top of the
 * environment's page allocator. Pages carved into objects are never
 * returned to the environment, but freed objects are reused.
 */

#include <string.h>
#include "fs.h"

#define FS_ALLOC_MIN_SHIFT 4  /* 16 bytes */
#define FS_ALLOC_MAX_SHIFT 11 /* 2048 bytes; larger requests get whole pages */
#define FS_ALLOC_NCLASSES  (FS_ALLOC_MAX_SHIFT - FS_ALLOC_MIN_SHIFT + 1)

struct fs_free_obj {
	struct fs_free_obj *next;
};

static struct fs_free_obj *fs_freelists[FS_ALLOC_NCLASSES];

static int
fs_size_class(size_t sz)
{
	int c = 0;

	while (((size_t)1 << (c + FS_ALLOC_MIN_SHIFT)) < sz) c++;

	return c;
}

size_t
fs_strnlen(const char *s, size_t max)
{
	size_t i;

	for (i = 0; i < max && s[i]; i++)
		;

	return i;
}

void *
fs_zalloc(size_t sz)
{
	struct fs_free_obj *o;
	size_t              objsz;
	char               *page;
	int                 c, i;

	if (sz == 0 || sz > FS_PAGE_SIZE) return NULL;
	if (sz > ((size_t)1 << FS_ALLOC_MAX_SHIFT)) return fs_page_alloc();

	c     = fs_size_class(sz);
	objsz = (size_t)1 << (c + FS_ALLOC_MIN_SHIFT);
	if (!fs_freelists[c]) {
		page = fs_page_alloc();
		if (!page) return NULL;
		for (i = FS_PAGE_SIZE / objsz - 1; i >= 0; i--) {
			o              = (struct fs_free_obj *)(page + i * objsz);
			o->next        = fs_freelists[c];
			fs_freelists[c] = o;
		}
	}
	o               = fs_freelists[c];
	fs_freelists[c] = o->next;
	memset(o, 0, objsz);

	return o;
}

void
fs_free(void *p, size_t sz)
{
	struct fs_free_obj *o = p;
	int                 c;

	if (!p) return;
	if (sz > ((size_t)1 << FS_ALLOC_MAX_SHIFT)) {
		fs_page_free(p);
		return;
	}
	c               = fs_size_class(sz);
	o->next         = fs_freelists[c];
	fs_freelists[c] = o;
}
