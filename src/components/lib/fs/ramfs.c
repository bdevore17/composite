/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * ramfs: see ramfs.h. Inode handles (fs_ino_t) are the addresses of
 * the in-memory inodes.
 */

#include <string.h>
#include "ramfs.h"

#define RAMFS_NDIRECT 16
#define RAMFS_PPP     (FS_PAGE_SIZE / sizeof(void *)) /* pointers per page */
#define RAMFS_MAX_PAGES ((uint64_t)RAMFS_NDIRECT + RAMFS_PPP + (uint64_t)RAMFS_PPP * RAMFS_PPP)
#define RAMFS_DENT_SZ 32 /* bytes reported per entry in a directory's size */

struct ramfs_inode;

struct ramfs_dent {
	struct ramfs_dent  *next;
	struct ramfs_inode *inode;
	uint16_t            namelen;
	char                name[]; /* NUL-terminated */
};

struct ramfs_inode {
	uint64_t num;
	uint32_t mode;
	uint32_t nlink;
	uint64_t size;
	uint64_t npages; /* data pages allocated to this file */
	/* regular files */
	void    *direct[RAMFS_NDIRECT];
	void   **ind;
	void  ***dind;
	/* directories */
	struct ramfs_dent *dents, *dents_tail;
	uint64_t           nents;
};

struct ramfs {
	struct ramfs_inode *root;
	uint64_t            next_num;
	uint64_t            pages_used;
	uint64_t            max_pages;
};

#define RFS(f)   ((struct ramfs *)(f))
#define RINO(i)  ((struct ramfs_inode *)(uintptr_t)(i))
#define HANDLE(i) ((fs_ino_t)(uintptr_t)(i))

static size_t
dent_sz(size_t namelen)
{
	return sizeof(struct ramfs_dent) + namelen + 1;
}

static struct ramfs_inode *
inode_alloc(struct ramfs *fs, uint32_t mode)
{
	struct ramfs_inode *in = fs_zalloc(sizeof(struct ramfs_inode));

	if (!in) return NULL;
	in->num   = fs->next_num++;
	in->mode  = mode;
	in->nlink = 1;

	return in;
}

struct ramfs *
ramfs_create(uint64_t max_pages)
{
	struct ramfs *fs = fs_zalloc(sizeof(struct ramfs));

	if (!fs) return NULL;
	fs->next_num  = 1;
	fs->max_pages = max_pages;
	fs->root      = inode_alloc(fs, FS_S_IFDIR | 0755);
	if (!fs->root) {
		fs_free(fs, sizeof(struct ramfs));
		return NULL;
	}

	return fs;
}

uint64_t
ramfs_pages_used(struct ramfs *fs)
{
	return fs->pages_used;
}

/*
 * Find the slot holding the pointer to data page idx. If alloc, the
 * intermediate (indirect) pages are allocated as needed. Returns NULL
 * if the slot doesn't exist (and !alloc), or on allocation failure.
 */
static void **
page_slot(struct ramfs_inode *in, uint64_t idx, int alloc)
{
	uint64_t l1;

	if (idx < RAMFS_NDIRECT) return &in->direct[idx];
	idx -= RAMFS_NDIRECT;
	if (idx < RAMFS_PPP) {
		if (!in->ind) {
			if (!alloc || !(in->ind = fs_page_alloc())) return NULL;
		}
		return &in->ind[idx];
	}
	idx -= RAMFS_PPP;
	if (idx >= (uint64_t)RAMFS_PPP * RAMFS_PPP) return NULL;
	if (!in->dind) {
		if (!alloc || !(in->dind = fs_page_alloc())) return NULL;
	}
	l1 = idx / RAMFS_PPP;
	if (!in->dind[l1]) {
		if (!alloc || !(in->dind[l1] = fs_page_alloc())) return NULL;
	}

	return &in->dind[l1][idx % RAMFS_PPP];
}

static void *
page_get(struct ramfs_inode *in, uint64_t idx)
{
	void **slot = page_slot(in, idx, 0);

	return slot ? *slot : NULL;
}

/* Free all data pages at index >= first, and indirect pages no longer needed. */
static void
pages_free_from(struct ramfs *fs, struct ramfs_inode *in, uint64_t first)
{
	uint64_t i, j;
	void   **slot;

	for (i = first; i < RAMFS_NDIRECT; i++) {
		if (in->direct[i]) {
			fs_page_free(in->direct[i]);
			in->direct[i] = NULL;
			in->npages--;
			fs->pages_used--;
		}
	}
	if (in->ind) {
		i = first > RAMFS_NDIRECT ? first - RAMFS_NDIRECT : 0;
		for (; i < RAMFS_PPP; i++) {
			slot = &in->ind[i];
			if (*slot) {
				fs_page_free(*slot);
				*slot = NULL;
				in->npages--;
				fs->pages_used--;
			}
		}
		if (first <= RAMFS_NDIRECT) {
			fs_page_free(in->ind);
			in->ind = NULL;
		}
	}
	if (in->dind) {
		uint64_t base = RAMFS_NDIRECT + RAMFS_PPP;
		uint64_t rel  = first > base ? first - base : 0;

		for (i = rel / RAMFS_PPP; i < RAMFS_PPP; i++) {
			void **l2 = in->dind[i];

			if (!l2) continue;
			j = (i == rel / RAMFS_PPP) ? rel % RAMFS_PPP : 0;
			for (; j < RAMFS_PPP; j++) {
				if (l2[j]) {
					fs_page_free(l2[j]);
					l2[j] = NULL;
					in->npages--;
					fs->pages_used--;
				}
			}
			/* free the second-level page if nothing below it can be in use anymore */
			if (i * RAMFS_PPP >= rel) {
				fs_page_free(l2);
				in->dind[i] = NULL;
			}
		}
		if (first <= base) {
			fs_page_free(in->dind);
			in->dind = NULL;
		}
	}
}

static struct ramfs_dent *
dent_find(struct ramfs_inode *dir, const char *name, struct ramfs_dent **prev)
{
	struct ramfs_dent *d, *p = NULL;

	for (d = dir->dents; d; p = d, d = d->next) {
		if (!strcmp(d->name, name)) {
			if (prev) *prev = p;
			return d;
		}
	}

	return NULL;
}

static void
dent_insert(struct ramfs_inode *dir, struct ramfs_dent *d)
{
	d->next = NULL;
	if (dir->dents_tail) {
		dir->dents_tail->next = d;
	} else {
		dir->dents = d;
	}
	dir->dents_tail = d;
	dir->nents++;
	dir->size = dir->nents * RAMFS_DENT_SZ;
}

static void
dent_remove(struct ramfs_inode *dir, struct ramfs_dent *d, struct ramfs_dent *prev)
{
	if (prev) {
		prev->next = d->next;
	} else {
		dir->dents = d->next;
	}
	if (dir->dents_tail == d) dir->dents_tail = prev;
	dir->nents--;
	dir->size = dir->nents * RAMFS_DENT_SZ;
}

static struct ramfs_dent *
dent_alloc(const char *name, struct ramfs_inode *in)
{
	size_t             len = strlen(name);
	struct ramfs_dent *d;

	if (len > FS_NAME_MAX) return NULL;
	d = fs_zalloc(dent_sz(len));
	if (!d) return NULL;
	d->inode   = in;
	d->namelen = len;
	memcpy(d->name, name, len + 1);

	return d;
}

static void
dent_free(struct ramfs_dent *d)
{
	fs_free(d, dent_sz(d->namelen));
}

static int
ramfs_root(void *fs, fs_ino_t *ino)
{
	*ino = HANDLE(RFS(fs)->root);

	return 0;
}

static int
ramfs_lookup(void *fs, fs_ino_t dir, const char *name, fs_ino_t *ino)
{
	struct ramfs_dent *d;

	if (!FS_ISDIR(RINO(dir)->mode)) return -ENOTDIR;
	d = dent_find(RINO(dir), name, NULL);
	if (!d) return -ENOENT;
	*ino = HANDLE(d->inode);

	return 0;
}

static int
ramfs_create_node(void *f, fs_ino_t dirh, const char *name, uint32_t mode, fs_ino_t *ino)
{
	struct ramfs       *fs  = RFS(f);
	struct ramfs_inode *dir = RINO(dirh), *in;
	struct ramfs_dent  *d;

	if (!FS_ISDIR(dir->mode)) return -ENOTDIR;
	if (!dir->nlink) return -ENOENT; /* directory was removed while we held it */
	if (strlen(name) > FS_NAME_MAX) return -ENAMETOOLONG;
	if (dent_find(dir, name, NULL)) return -EEXIST;
	if (!FS_ISDIR(mode) && !FS_ISREG(mode)) return -EINVAL;

	in = inode_alloc(fs, mode);
	if (!in) return -ENOMEM;
	d = dent_alloc(name, in);
	if (!d) {
		fs_free(in, sizeof(struct ramfs_inode));
		return -ENOMEM;
	}
	dent_insert(dir, d);
	*ino = HANDLE(in);

	return 0;
}

static int
ramfs_unlink(void *fs, fs_ino_t dirh, const char *name, int is_dir, fs_ino_t *victim)
{
	struct ramfs_inode *dir = RINO(dirh), *in;
	struct ramfs_dent  *d, *prev = NULL;

	if (!FS_ISDIR(dir->mode)) return -ENOTDIR;
	d = dent_find(dir, name, &prev);
	if (!d) return -ENOENT;
	in = d->inode;
	if (is_dir) {
		if (!FS_ISDIR(in->mode)) return -ENOTDIR;
		if (in->nents) return -ENOTEMPTY;
	} else if (FS_ISDIR(in->mode)) {
		return -EISDIR;
	}
	dent_remove(dir, d, prev);
	dent_free(d);
	in->nlink--;
	*victim = HANDLE(in);

	return 0;
}

static int
ramfs_rename(void *fs, fs_ino_t odirh, const char *oname, fs_ino_t ndirh, const char *nname, fs_ino_t *replaced)
{
	struct ramfs_inode *odir = RINO(odirh), *ndir = RINO(ndirh), *src, *dst;
	struct ramfs_dent  *od, *oprev = NULL, *nd, *nprev = NULL, *newd;

	*replaced = 0;
	if (!FS_ISDIR(odir->mode) || !FS_ISDIR(ndir->mode)) return -ENOTDIR;
	if (strlen(nname) > FS_NAME_MAX) return -ENAMETOOLONG;
	od = dent_find(odir, oname, &oprev);
	if (!od) return -ENOENT;
	src = od->inode;
	nd  = dent_find(ndir, nname, &nprev);
	if (nd) {
		dst = nd->inode;
		if (dst == src) return 0;
		if (FS_ISDIR(dst->mode)) {
			if (!FS_ISDIR(src->mode)) return -EISDIR;
			if (dst->nents) return -ENOTEMPTY;
		} else if (FS_ISDIR(src->mode)) {
			return -ENOTDIR;
		}
	}

	/* allocate first so that failure leaves everything untouched */
	newd = dent_alloc(nname, src);
	if (!newd) return -ENOMEM;

	if (nd) {
		dent_remove(ndir, nd, nprev);
		dent_free(nd);
		dst->nlink--;
		*replaced = HANDLE(dst);
		/* the removal may have changed oprev if both live in the same directory */
		od = dent_find(odir, oname, &oprev);
	}
	dent_remove(odir, od, oprev);
	dent_free(od);
	dent_insert(ndir, newd);

	return 0;
}

static int
ramfs_getattr(void *fs, fs_ino_t ino, struct fs_stat *st)
{
	struct ramfs_inode *in = RINO(ino);

	st->ino     = in->num;
	st->mode    = in->mode;
	st->nlink   = FS_ISDIR(in->mode) ? in->nlink + 1 : in->nlink;
	st->size    = in->size;
	st->blocks  = in->npages * (FS_PAGE_SIZE / 512);
	st->blksize = FS_PAGE_SIZE;

	return 0;
}

static long
ramfs_read(void *fs, fs_ino_t ino, uint64_t off, void *buf, size_t len)
{
	struct ramfs_inode *in = RINO(ino);
	size_t              done = 0;

	if (FS_ISDIR(in->mode)) return -EISDIR;
	if (off >= in->size) return 0;
	if (len > in->size - off) len = in->size - off;

	while (done < len) {
		uint64_t pos   = off + done;
		size_t   poff  = pos % FS_PAGE_SIZE;
		size_t   chunk = FS_PAGE_SIZE - poff;
		char    *page  = page_get(in, pos / FS_PAGE_SIZE);

		if (chunk > len - done) chunk = len - done;
		if (page) {
			memcpy((char *)buf + done, page + poff, chunk);
		} else {
			memset((char *)buf + done, 0, chunk); /* hole */
		}
		done += chunk;
	}

	return done;
}

static long
ramfs_write(void *f, fs_ino_t ino, uint64_t off, const void *buf, size_t len)
{
	struct ramfs       *fs = RFS(f);
	struct ramfs_inode *in = RINO(ino);
	size_t              done = 0;

	if (FS_ISDIR(in->mode)) return -EISDIR;
	if (off + len < off || (off + len + FS_PAGE_SIZE - 1) / FS_PAGE_SIZE > RAMFS_MAX_PAGES) return -EFBIG;

	while (done < len) {
		uint64_t pos   = off + done;
		size_t   poff  = pos % FS_PAGE_SIZE;
		size_t   chunk = FS_PAGE_SIZE - poff;
		void   **slot;

		if (chunk > len - done) chunk = len - done;
		slot = page_slot(in, pos / FS_PAGE_SIZE, 1);
		if (!slot) break;
		if (!*slot) {
			if (fs->max_pages && fs->pages_used >= fs->max_pages) break;
			*slot = fs_page_alloc();
			if (!*slot) break;
			in->npages++;
			fs->pages_used++;
		}
		memcpy((char *)*slot + poff, (const char *)buf + done, chunk);
		done += chunk;
		if (off + done > in->size) in->size = off + done;
	}
	if (done == 0 && len > 0) return -ENOSPC;

	return done;
}

static int
ramfs_truncate(void *f, fs_ino_t ino, uint64_t size)
{
	struct ramfs       *fs = RFS(f);
	struct ramfs_inode *in = RINO(ino);
	char               *page;

	if (FS_ISDIR(in->mode)) return -EISDIR;
	if ((size + FS_PAGE_SIZE - 1) / FS_PAGE_SIZE > RAMFS_MAX_PAGES) return -EFBIG;
	if (size < in->size) {
		pages_free_from(fs, in, (size + FS_PAGE_SIZE - 1) / FS_PAGE_SIZE);
		/* zero the tail of the last partial page so a later extension reads zeros */
		if (size % FS_PAGE_SIZE) {
			page = page_get(in, size / FS_PAGE_SIZE);
			if (page) memset(page + size % FS_PAGE_SIZE, 0, FS_PAGE_SIZE - size % FS_PAGE_SIZE);
		}
	}
	in->size = size;

	return 0;
}

static int
ramfs_readdir(void *fs, fs_ino_t dirh, uint64_t cookie, struct fs_dirent *de)
{
	struct ramfs_inode *dir = RINO(dirh);
	struct ramfs_dent  *d;
	uint64_t            i;

	if (!FS_ISDIR(dir->mode)) return -ENOTDIR;
	for (d = dir->dents, i = 0; d && i < cookie; d = d->next, i++)
		;
	if (!d) return 0;
	de->ino  = d->inode->num;
	de->next = cookie + 1;
	de->type = FS_ISDIR(d->inode->mode) ? FS_DT_DIR : FS_DT_REG;
	memcpy(de->name, d->name, d->namelen + 1);

	return 1;
}

static int
ramfs_evict(void *f, fs_ino_t ino)
{
	struct ramfs       *fs = RFS(f);
	struct ramfs_inode *in = RINO(ino);

	if (in->nlink || in == fs->root) return -EBUSY;
	pages_free_from(fs, in, 0);
	fs_free(in, sizeof(struct ramfs_inode));

	return 0;
}

static int
ramfs_sync(void *fs)
{
	return 0;
}

const struct fs_ops ramfs_ops = {
	.name     = "ramfs",
	.name_max = FS_NAME_MAX,
	.root     = ramfs_root,
	.lookup   = ramfs_lookup,
	.create   = ramfs_create_node,
	.unlink   = ramfs_unlink,
	.rename   = ramfs_rename,
	.getattr  = ramfs_getattr,
	.read     = ramfs_read,
	.write    = ramfs_write,
	.truncate = ramfs_truncate,
	.readdir  = ramfs_readdir,
	.evict    = ramfs_evict,
	.sync     = ramfs_sync,
};
