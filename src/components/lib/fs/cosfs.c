/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * cosfs implementation. See cosfs_format.h for the on-disk layout.
 *
 * Caching: a small LRU block cache, write-through (every modification
 * is written to the device before the operation returns), so the
 * image is always up to date and `sync` only has to flush the
 * device's own write cache.
 *
 * Buffers returned by bget() are only valid until the next bget(), so
 * code never holds two of them at a time: inodes are copied out
 * (iget) and back (iput), and indirect blocks are copied into
 * per-file-system scratch pages when they must be walked.
 *
 * Ordering (no journal): allocations mark the bitmap before the new
 * block/inode is referenced, frees drop the reference before clearing
 * the bitmap. A crash can therefore leak blocks or inodes, but never
 * make two files share a block.
 */

#include <string.h>
#include "cosfs.h"

#define COSFS_CACHE_N 32
#define PPB           COSFS_PTRS_PER_BLOCK

struct cosfs_cache_ent {
	uint64_t blk;
	uint64_t lru;
	int      valid;
	char    *data;
};

struct cosfs {
	struct fs_blkdev      *dev;
	struct cosfs_super     sb;
	struct cosfs_cache_ent cache[COSFS_CACHE_N];
	uint64_t               tick;
	uint64_t               free_blocks;
	uint64_t               bmap_hint; /* block number to start the next allocation search at */
	uint32_t               ino_hint;
	uint32_t              *scratch1, *scratch2; /* pages for walking indirect blocks */
};

#define CFS(f) ((struct cosfs *)(f))

/*
 * Block cache
 */

/* Get a cached copy of blk. If !load the contents are undefined (the caller overwrites it all). */
static char *
bget(struct cosfs *fs, uint64_t blk, int load)
{
	struct cosfs_cache_ent *e, *victim = NULL;
	int                     i;

	if (blk >= fs->sb.nblocks) return NULL;
	for (i = 0; i < COSFS_CACHE_N; i++) {
		e = &fs->cache[i];
		if (e->valid && e->blk == blk) {
			e->lru = ++fs->tick;
			return e->data;
		}
		/* victim: an invalid entry if any, else the least recently used */
		if (!victim || (victim->valid && (!e->valid || e->lru < victim->lru))) victim = e;
	}
	e        = victim;
	e->valid = 0;
	if (load && fs->dev->read(fs->dev->ctx, blk, e->data)) return NULL;
	e->blk   = blk;
	e->valid = 1;
	e->lru   = ++fs->tick;

	return e->data;
}

/* Write a block previously returned by bget (and modified) through to the device. */
static int
bwrite(struct cosfs *fs, uint64_t blk, const char *data)
{
	return fs->dev->write(fs->dev->ctx, blk, data) ? -EIO : 0;
}

/*
 * Inodes
 */

static int
iget(struct cosfs *fs, uint32_t ino, struct cosfs_dinode *di)
{
	char *b;

	if (ino == 0 || ino >= fs->sb.ninodes) return -EINVAL;
	b = bget(fs, fs->sb.itab_start + ino / COSFS_INODES_PER_BLOCK, 1);
	if (!b) return -EIO;
	memcpy(di, b + (ino % COSFS_INODES_PER_BLOCK) * COSFS_INODE_SIZE, sizeof(*di));

	return 0;
}

static int
iput(struct cosfs *fs, uint32_t ino, const struct cosfs_dinode *di)
{
	uint64_t blk = fs->sb.itab_start + ino / COSFS_INODES_PER_BLOCK;
	char    *b   = bget(fs, blk, 1);

	if (!b) return -EIO;
	memcpy(b + (ino % COSFS_INODES_PER_BLOCK) * COSFS_INODE_SIZE, di, sizeof(*di));

	return bwrite(fs, blk, b);
}

/* Allocate an inode and write it initialized with mode. */
static int
ialloc(struct cosfs *fs, uint32_t mode, uint32_t *ino)
{
	struct cosfs_dinode di;
	uint32_t            n, cand;
	int                 ret;

	for (n = 0; n < fs->sb.ninodes; n++) {
		cand = (fs->ino_hint + n) % fs->sb.ninodes;
		if (cand < COSFS_ROOT_INO) continue;
		if ((ret = iget(fs, cand, &di))) return ret;
		if (di.mode) continue;
		memset(&di, 0, sizeof(di));
		di.mode  = mode;
		di.nlink = 1;
		if ((ret = iput(fs, cand, &di))) return ret;
		fs->ino_hint = cand + 1;
		*ino         = cand;
		return 0;
	}

	return -ENOSPC;
}

/*
 * Block allocation bitmap
 */

static int
bmap_set(struct cosfs *fs, uint64_t blk, int used)
{
	uint64_t bb = fs->sb.bmap_start + blk / COSFS_BITS_PER_BLOCK;
	uint64_t bit = blk % COSFS_BITS_PER_BLOCK;
	char    *b  = bget(fs, bb, 1);

	if (!b) return -EIO;
	if (used) {
		b[bit / 8] |= (1 << (bit % 8));
	} else {
		b[bit / 8] &= ~(1 << (bit % 8));
	}

	return bwrite(fs, bb, b);
}

/* Allocate a data block. If zero, its contents are zeroed on disk (and in the cache). */
static int
balloc(struct cosfs *fs, int zero, uint32_t *out)
{
	uint64_t n, blk;
	char    *b;
	int      ret;

	if (fs->free_blocks == 0) return -ENOSPC;
	for (n = 0; n < fs->sb.nblocks;) {
		uint64_t bit, bb;

		blk = (fs->bmap_hint + n) % fs->sb.nblocks;
		if (blk < fs->sb.data_start) {
			n += fs->sb.data_start - blk;
			continue;
		}
		bb  = fs->sb.bmap_start + blk / COSFS_BITS_PER_BLOCK;
		bit = blk % COSFS_BITS_PER_BLOCK;
		b   = bget(fs, bb, 1);
		if (!b) return -EIO;
		/* skip whole bytes that are full */
		if (bit % 8 == 0 && blk + 8 <= fs->sb.nblocks && (unsigned char)b[bit / 8] == 0xff) {
			n += 8;
			continue;
		}
		if (b[bit / 8] & (1 << (bit % 8))) {
			n++;
			continue;
		}
		b[bit / 8] |= (1 << (bit % 8));
		if ((ret = bwrite(fs, bb, b))) return ret;
		fs->free_blocks--;
		fs->bmap_hint = blk + 1;
		if (zero) {
			b = bget(fs, blk, 0);
			if (!b) return -EIO;
			memset(b, 0, COSFS_BLOCK_SIZE);
			if ((ret = bwrite(fs, blk, b))) return ret;
		}
		*out = blk;
		return 0;
	}

	return -ENOSPC;
}

static int
bfree(struct cosfs *fs, uint32_t blk)
{
	int ret;

	if (blk < fs->sb.data_start || blk >= fs->sb.nblocks) return -EIO;
	if ((ret = bmap_set(fs, blk, 0))) return ret;
	fs->free_blocks++;

	return 0;
}

/*
 * File block mapping
 */

/* Read entry idx of the indirect block ind. */
static int
ind_get(struct cosfs *fs, uint32_t ind, uint64_t idx, uint32_t *out)
{
	uint32_t *b = (uint32_t *)bget(fs, ind, 1);

	if (!b) return -EIO;
	*out = b[idx];

	return 0;
}

static int
ind_set(struct cosfs *fs, uint32_t ind, uint64_t idx, uint32_t val)
{
	uint32_t *b = (uint32_t *)bget(fs, ind, 1);

	if (!b) return -EIO;
	b[idx] = val;

	return bwrite(fs, ind, (char *)b);
}

/*
 * Make *slot point to an allocated block (allocating, zeroed if zero,
 * when it is 0 and alloc is set). Used for both inode-embedded
 * pointers and pointers inside indirect blocks (in which case ind is
 * the containing block and idx the index in it).
 */
static int
slot_fill(struct cosfs *fs, struct cosfs_dinode *di, uint32_t *slot, uint32_t ind, uint64_t idx, int alloc, int zero)
{
	int ret;

	if (*slot || !alloc) return 0;
	if ((ret = balloc(fs, zero, slot))) return ret;
	di->nblocks++;
	if (ind) {
		if ((ret = ind_set(fs, ind, idx, *slot))) return ret;
	}

	return 0;
}

/*
 * Map file block fblk of the inode to a disk block (*out, 0 = hole).
 * If alloc, missing blocks are allocated (data blocks zeroed if
 * zero_new). The caller must write the (possibly modified) inode back.
 */
static int
bmap(struct cosfs *fs, struct cosfs_dinode *di, uint64_t fblk, int alloc, int zero_new, uint32_t *out)
{
	uint32_t l1, l2;
	int      ret;

	*out = 0;
	if (fblk < COSFS_NDIRECT) {
		if ((ret = slot_fill(fs, di, &di->direct[fblk], 0, 0, alloc, zero_new))) return ret;
		*out = di->direct[fblk];
		return 0;
	}
	fblk -= COSFS_NDIRECT;
	if (fblk < PPB) {
		if ((ret = slot_fill(fs, di, &di->ind, 0, 0, alloc, 1))) return ret;
		if (!di->ind) return 0;
		if ((ret = ind_get(fs, di->ind, fblk, &l1))) return ret;
		if ((ret = slot_fill(fs, di, &l1, di->ind, fblk, alloc, zero_new))) return ret;
		*out = l1;
		return 0;
	}
	fblk -= PPB;
	if (fblk >= (uint64_t)PPB * PPB) return -EFBIG;
	if ((ret = slot_fill(fs, di, &di->dind, 0, 0, alloc, 1))) return ret;
	if (!di->dind) return 0;
	if ((ret = ind_get(fs, di->dind, fblk / PPB, &l1))) return ret;
	if ((ret = slot_fill(fs, di, &l1, di->dind, fblk / PPB, alloc, 1))) return ret;
	if (!l1) return 0;
	if ((ret = ind_get(fs, l1, fblk % PPB, &l2))) return ret;
	if ((ret = slot_fill(fs, di, &l2, l1, fblk % PPB, alloc, zero_new))) return ret;
	*out = l2;

	return 0;
}

/* Free the pointers ptrs[from..PPB) (each a data block). Returns -errno or 0. */
static int
free_ptrs(struct cosfs *fs, struct cosfs_dinode *di, uint32_t *ptrs, uint64_t from)
{
	uint64_t i;
	int      ret;

	for (i = from; i < PPB; i++) {
		if (!ptrs[i]) continue;
		if ((ret = bfree(fs, ptrs[i]))) return ret;
		ptrs[i] = 0;
		di->nblocks--;
	}

	return 0;
}

static int
load_scratch(struct cosfs *fs, uint32_t blk, uint32_t *scratch)
{
	char *b = bget(fs, blk, 1);

	if (!b) return -EIO;
	memcpy(scratch, b, COSFS_BLOCK_SIZE);

	return 0;
}

/* Free every block of the file at file-block index >= first (including unneeded indirect blocks). */
static int
blocks_free_from(struct cosfs *fs, struct cosfs_dinode *di, uint64_t first)
{
	uint64_t i, start;
	int      ret;

	for (i = first; i < COSFS_NDIRECT; i++) {
		if (!di->direct[i]) continue;
		if ((ret = bfree(fs, di->direct[i]))) return ret;
		di->direct[i] = 0;
		di->nblocks--;
	}
	if (di->ind) {
		start = first > COSFS_NDIRECT ? first - COSFS_NDIRECT : 0;
		if (start < PPB) {
			if ((ret = load_scratch(fs, di->ind, fs->scratch1))) return ret;
			if (start == 0) {
				/* drop the reference first, then free (see ordering note above) */
				uint32_t ind = di->ind;

				di->ind = 0;
				di->nblocks--;
				if ((ret = free_ptrs(fs, di, fs->scratch1, 0))) return ret;
				if ((ret = bfree(fs, ind))) return ret;
			} else {
				uint32_t *b;

				/* clear the pointers on disk before freeing their blocks */
				b = (uint32_t *)bget(fs, di->ind, 1);
				if (!b) return -EIO;
				memset(b + start, 0, (PPB - start) * sizeof(uint32_t));
				if ((ret = bwrite(fs, di->ind, (char *)b))) return ret;
				if ((ret = free_ptrs(fs, di, fs->scratch1, start))) return ret;
			}
		}
	}
	if (di->dind) {
		uint64_t base = COSFS_NDIRECT + PPB;

		start = first > base ? first - base : 0;
		if ((ret = load_scratch(fs, di->dind, fs->scratch1))) return ret;
		for (i = start / PPB; i < PPB; i++) {
			uint64_t sub = (i == start / PPB) ? start % PPB : 0;
			uint32_t l1  = fs->scratch1[i];

			if (!l1) continue;
			if ((ret = load_scratch(fs, l1, fs->scratch2))) return ret;
			if (sub == 0) {
				if ((ret = ind_set(fs, di->dind, i, 0))) return ret;
				if ((ret = free_ptrs(fs, di, fs->scratch2, 0))) return ret;
				if ((ret = bfree(fs, l1))) return ret;
				di->nblocks--;
			} else {
				uint32_t *b = (uint32_t *)bget(fs, l1, 1);

				if (!b) return -EIO;
				memset(b + sub, 0, (PPB - sub) * sizeof(uint32_t));
				if ((ret = bwrite(fs, l1, (char *)b))) return ret;
				if ((ret = free_ptrs(fs, di, fs->scratch2, sub))) return ret;
			}
		}
		if (start == 0) {
			uint32_t dind = di->dind;

			di->dind = 0;
			di->nblocks--;
			if ((ret = bfree(fs, dind))) return ret;
		}
	}

	return 0;
}

/*
 * File data
 */

static long
file_read(struct cosfs *fs, struct cosfs_dinode *di, uint64_t off, void *buf, size_t len)
{
	size_t done = 0;
	int    ret;

	if (off >= di->size) return 0;
	if (len > di->size - off) len = di->size - off;
	while (done < len) {
		uint64_t pos   = off + done;
		size_t   boff  = pos % COSFS_BLOCK_SIZE;
		size_t   chunk = COSFS_BLOCK_SIZE - boff;
		uint32_t blk;
		char    *b;

		if (chunk > len - done) chunk = len - done;
		if ((ret = bmap(fs, di, pos / COSFS_BLOCK_SIZE, 0, 0, &blk))) return done ? (long)done : ret;
		if (blk) {
			b = bget(fs, blk, 1);
			if (!b) return done ? (long)done : -EIO;
			memcpy((char *)buf + done, b + boff, chunk);
		} else {
			memset((char *)buf + done, 0, chunk);
		}
		done += chunk;
	}

	return done;
}

/* Write into the file described by di (caller writes the inode back). */
static long
file_write(struct cosfs *fs, struct cosfs_dinode *di, uint64_t off, const void *buf, size_t len)
{
	size_t done = 0;
	int    ret  = 0;

	if (off + len < off || (off + len + COSFS_BLOCK_SIZE - 1) / COSFS_BLOCK_SIZE > COSFS_MAX_FILE_BLOCKS) {
		return -EFBIG;
	}
	while (done < len) {
		uint64_t pos   = off + done;
		size_t   boff  = pos % COSFS_BLOCK_SIZE;
		size_t   chunk = COSFS_BLOCK_SIZE - boff;
		int      full;
		uint32_t blk;
		char    *b;

		if (chunk > len - done) chunk = len - done;
		full = (chunk == COSFS_BLOCK_SIZE);
		if ((ret = bmap(fs, di, pos / COSFS_BLOCK_SIZE, 1, !full, &blk))) break;
		b = bget(fs, blk, !full);
		if (!b) {
			ret = -EIO;
			break;
		}
		memcpy(b + boff, (const char *)buf + done, chunk);
		if ((ret = bwrite(fs, blk, b))) break;
		done += chunk;
		if (off + done > di->size) di->size = off + done;
	}
	if (done == 0 && ret) return ret;

	return done;
}

/*
 * Directories
 */

/* Find name in the directory. *slot gets its index, *ent a copy. Returns 0, -ENOENT or -errno. */
static int
dir_find(struct cosfs *fs, struct cosfs_dinode *dir, const char *name, uint64_t *slot, struct cosfs_ddirent *ent)
{
	uint64_t nslots = dir->size / COSFS_DIRENT_SIZE, s;
	size_t   nlen   = strlen(name);
	int      ret;

	for (s = 0; s < nslots; s++) {
		struct cosfs_ddirent *d;
		uint32_t              blk;
		char                 *b;

		if ((ret = bmap(fs, dir, s / COSFS_DIRENTS_PER_BLOCK, 0, 0, &blk))) return ret;
		if (!blk) {
			/* a hole: skip the whole block */
			s += COSFS_DIRENTS_PER_BLOCK - 1 - s % COSFS_DIRENTS_PER_BLOCK;
			continue;
		}
		b = bget(fs, blk, 1);
		if (!b) return -EIO;
		d = (struct cosfs_ddirent *)(b + (s % COSFS_DIRENTS_PER_BLOCK) * COSFS_DIRENT_SIZE);
		if (d->ino && d->namelen == nlen && !memcmp(d->name, name, nlen)) {
			if (slot) *slot = s;
			if (ent) *ent = *d;
			return 0;
		}
	}

	return -ENOENT;
}

static int
dir_empty(struct cosfs *fs, struct cosfs_dinode *dir)
{
	uint64_t nslots = dir->size / COSFS_DIRENT_SIZE, s;
	struct cosfs_ddirent d;

	for (s = 0; s < nslots; s++) {
		long r = file_read(fs, dir, s * COSFS_DIRENT_SIZE, &d, sizeof(d));

		if (r < 0) return r;
		if (r == sizeof(d) && d.ino) return 0;
	}

	return 1;
}

static int
dir_write_slot(struct cosfs *fs, uint32_t dirino, struct cosfs_dinode *dir, uint64_t slot,
               const struct cosfs_ddirent *d)
{
	long r = file_write(fs, dir, slot * COSFS_DIRENT_SIZE, d, sizeof(*d));
	int  ret;

	if ((ret = iput(fs, dirino, dir))) return ret;
	if (r < 0) return r;

	return r == sizeof(*d) ? 0 : -ENOSPC;
}

static int
dir_add(struct cosfs *fs, uint32_t dirino, struct cosfs_dinode *dir, const char *name, uint32_t ino, uint8_t type)
{
	uint64_t             nslots = dir->size / COSFS_DIRENT_SIZE, s;
	struct cosfs_ddirent d;
	size_t               nlen = strlen(name);

	/* reuse a free slot if there is one, else append */
	for (s = 0; s < nslots; s++) {
		long r = file_read(fs, dir, s * COSFS_DIRENT_SIZE, &d, sizeof(d));

		if (r < 0) return r;
		if (!d.ino) break;
	}
	memset(&d, 0, sizeof(d));
	d.ino     = ino;
	d.type    = type;
	d.namelen = nlen;
	memcpy(d.name, name, nlen);

	return dir_write_slot(fs, dirino, dir, s, &d);
}

static int
dir_clear_slot(struct cosfs *fs, uint32_t dirino, struct cosfs_dinode *dir, uint64_t slot)
{
	struct cosfs_ddirent d;

	memset(&d, 0, sizeof(d));

	return dir_write_slot(fs, dirino, dir, slot, &d);
}

static int
name_ok(const char *name)
{
	size_t l = strlen(name);

	if (l == 0) return -ENOENT;
	if (l > COSFS_NAME_MAX) return -ENAMETOOLONG;

	return 0;
}

/*
 * The fs_ops
 */

static int
cosfs_root(void *fs, fs_ino_t *ino)
{
	*ino = CFS(fs)->sb.root_ino;

	return 0;
}

static int
cosfs_lookup(void *f, fs_ino_t dirino, const char *name, fs_ino_t *ino)
{
	struct cosfs        *fs = CFS(f);
	struct cosfs_dinode  dir;
	struct cosfs_ddirent d;
	int                  ret;

	if ((ret = name_ok(name))) return ret;
	if ((ret = iget(fs, dirino, &dir))) return ret;
	if (!FS_ISDIR(dir.mode)) return -ENOTDIR;
	if ((ret = dir_find(fs, &dir, name, NULL, &d))) return ret;
	*ino = d.ino;

	return 0;
}

static int
cosfs_create(void *f, fs_ino_t dirino, const char *name, uint32_t mode, fs_ino_t *ino)
{
	struct cosfs        *fs = CFS(f);
	struct cosfs_dinode  dir, di;
	uint32_t             new;
	int                  ret;

	if ((ret = name_ok(name))) return ret;
	if (!FS_ISDIR(mode) && !FS_ISREG(mode)) return -EINVAL;
	if ((ret = iget(fs, dirino, &dir))) return ret;
	if (!FS_ISDIR(dir.mode)) return -ENOTDIR;
	if (!dir.nlink) return -ENOENT;
	ret = dir_find(fs, &dir, name, NULL, NULL);
	if (ret == 0) return -EEXIST;
	if (ret != -ENOENT) return ret;

	if ((ret = ialloc(fs, mode, &new))) return ret;
	if ((ret = dir_add(fs, dirino, &dir, name, new, FS_ISDIR(mode) ? FS_DT_DIR : FS_DT_REG))) {
		/* undo the inode allocation */
		memset(&di, 0, sizeof(di));
		iput(fs, new, &di);
		return ret;
	}
	*ino = new;

	return 0;
}

static int
cosfs_unlink(void *f, fs_ino_t dirino, const char *name, int is_dir, fs_ino_t *victim)
{
	struct cosfs        *fs = CFS(f);
	struct cosfs_dinode  dir, di;
	struct cosfs_ddirent d;
	uint64_t             slot;
	int                  ret;

	if ((ret = name_ok(name))) return ret;
	if ((ret = iget(fs, dirino, &dir))) return ret;
	if (!FS_ISDIR(dir.mode)) return -ENOTDIR;
	if ((ret = dir_find(fs, &dir, name, &slot, &d))) return ret;
	if ((ret = iget(fs, d.ino, &di))) return ret;
	if (is_dir) {
		if (!FS_ISDIR(di.mode)) return -ENOTDIR;
		ret = dir_empty(fs, &di);
		if (ret < 0) return ret;
		if (!ret) return -ENOTEMPTY;
	} else if (FS_ISDIR(di.mode)) {
		return -EISDIR;
	}
	if ((ret = dir_clear_slot(fs, dirino, &dir, slot))) return ret;
	di.nlink--;
	if ((ret = iput(fs, d.ino, &di))) return ret;
	*victim = d.ino;

	return 0;
}


static int
cosfs_rename(void *f, fs_ino_t odirino, const char *oname, fs_ino_t ndirino, const char *nname, fs_ino_t *replaced)
{
	struct cosfs        *fs = CFS(f);
	struct cosfs_dinode  odir, ndir, src, dst;
	struct cosfs_ddirent od, nd;
	uint64_t             oslot, nslot;
	uint32_t             dst_ino = 0;
	int                  ret;

	*replaced = 0;
	if ((ret = name_ok(oname)) || (ret = name_ok(nname))) return ret;
	if ((ret = iget(fs, odirino, &odir)) || (ret = iget(fs, ndirino, &ndir))) return ret;
	if (!FS_ISDIR(odir.mode) || !FS_ISDIR(ndir.mode)) return -ENOTDIR;
	if ((ret = dir_find(fs, &odir, oname, &oslot, &od))) return ret;
	if ((ret = iget(fs, od.ino, &src))) return ret;
	ret = dir_find(fs, &ndir, nname, &nslot, &nd);
	if (ret && ret != -ENOENT) return ret;

	if (ret == 0) {
		dst_ino = nd.ino;
		if (dst_ino == od.ino) return 0;
		if ((ret = iget(fs, dst_ino, &dst))) return ret;
		if (FS_ISDIR(dst.mode)) {
			if (!FS_ISDIR(src.mode)) return -EISDIR;
			ret = dir_empty(fs, &dst);
			if (ret < 0) return ret;
			if (!ret) return -ENOTEMPTY;
		} else if (FS_ISDIR(src.mode)) {
			return -ENOTDIR;
		}
		/* atomically point the existing name at the source inode */
		nd.ino  = od.ino;
		nd.type = od.type;
		if ((ret = dir_write_slot(fs, ndirino, &ndir, nslot, &nd))) return ret;
		dst.nlink--;
		if ((ret = iput(fs, dst_ino, &dst))) return ret;
		*replaced = dst_ino;
	} else {
		if ((ret = dir_add(fs, ndirino, &ndir, nname, od.ino, od.type))) return ret;
	}
	/* the old directory may be the one we just modified: reload it */
	if ((ret = iget(fs, odirino, &odir))) return ret;

	return dir_clear_slot(fs, odirino, &odir, oslot);
}

static int
cosfs_getattr(void *f, fs_ino_t ino, struct fs_stat *st)
{
	struct cosfs_dinode di;
	int                 ret;

	if ((ret = iget(CFS(f), ino, &di))) return ret;
	if (!di.mode) return -ENOENT;
	st->ino     = ino;
	st->mode    = di.mode;
	st->nlink   = FS_ISDIR(di.mode) ? di.nlink + 1 : di.nlink;
	st->size    = di.size;
	st->blocks  = (uint64_t)di.nblocks * COSFS_SECTORS_PER_BLOCK;
	st->blksize = COSFS_BLOCK_SIZE;

	return 0;
}

static long
cosfs_read(void *f, fs_ino_t ino, uint64_t off, void *buf, size_t len)
{
	struct cosfs_dinode di;
	int                 ret;

	if ((ret = iget(CFS(f), ino, &di))) return ret;
	if (FS_ISDIR(di.mode)) return -EISDIR;

	return file_read(CFS(f), &di, off, buf, len);
}

static long
cosfs_write(void *f, fs_ino_t ino, uint64_t off, const void *buf, size_t len)
{
	struct cosfs_dinode di;
	long                r;
	int                 ret;

	if ((ret = iget(CFS(f), ino, &di))) return ret;
	if (FS_ISDIR(di.mode)) return -EISDIR;
	r = file_write(CFS(f), &di, off, buf, len);
	/* write the inode back even on partial failure: blocks may have been allocated */
	if ((ret = iput(CFS(f), ino, &di))) return ret;

	return r;
}

static int
cosfs_truncate(void *f, fs_ino_t ino, uint64_t size)
{
	struct cosfs       *fs = CFS(f);
	struct cosfs_dinode di;
	int                 ret, ret2;

	if ((ret = iget(fs, ino, &di))) return ret;
	if (FS_ISDIR(di.mode)) return -EISDIR;
	if ((size + COSFS_BLOCK_SIZE - 1) / COSFS_BLOCK_SIZE > COSFS_MAX_FILE_BLOCKS) return -EFBIG;
	if (size < di.size) {
		ret = blocks_free_from(fs, &di, (size + COSFS_BLOCK_SIZE - 1) / COSFS_BLOCK_SIZE);
		/* zero the tail of the last partial block so a later extension reads zeros */
		if (!ret && size % COSFS_BLOCK_SIZE) {
			uint32_t blk;

			ret = bmap(fs, &di, size / COSFS_BLOCK_SIZE, 0, 0, &blk);
			if (!ret && blk) {
				char *b = bget(fs, blk, 1);

				if (!b) {
					ret = -EIO;
				} else {
					memset(b + size % COSFS_BLOCK_SIZE, 0, COSFS_BLOCK_SIZE - size % COSFS_BLOCK_SIZE);
					ret = bwrite(fs, blk, b);
				}
			}
		}
	}
	if (!ret) di.size = size;
	ret2 = iput(fs, ino, &di);

	return ret ? ret : ret2;
}

static int
cosfs_readdir(void *f, fs_ino_t dirino, uint64_t cookie, struct fs_dirent *de)
{
	struct cosfs        *fs = CFS(f);
	struct cosfs_dinode  dir;
	struct cosfs_ddirent d;
	uint64_t             s;
	int                  ret;

	if ((ret = iget(fs, dirino, &dir))) return ret;
	if (!FS_ISDIR(dir.mode)) return -ENOTDIR;
	for (s = cookie; s < dir.size / COSFS_DIRENT_SIZE; s++) {
		long r = file_read(fs, &dir, s * COSFS_DIRENT_SIZE, &d, sizeof(d));

		if (r < 0) return r;
		if (r != sizeof(d) || !d.ino) continue;
		de->ino  = d.ino;
		de->next = s + 1;
		de->type = d.type;
		memcpy(de->name, d.name, d.namelen);
		de->name[d.namelen] = '\0';
		return 1;
	}

	return 0;
}

static int
cosfs_evict(void *f, fs_ino_t ino)
{
	struct cosfs       *fs = CFS(f);
	struct cosfs_dinode di;
	int                 ret;

	if (ino == fs->sb.root_ino) return -EBUSY;
	if ((ret = iget(fs, ino, &di))) return ret;
	if (di.nlink) return -EBUSY;
	ret = blocks_free_from(fs, &di, 0);
	if (ret) {
		iput(fs, ino, &di);
		return ret;
	}
	memset(&di, 0, sizeof(di));
	if (ino < fs->ino_hint) fs->ino_hint = ino;

	return iput(fs, ino, &di);
}

static int
cosfs_sync(void *f)
{
	struct cosfs *fs = CFS(f);

	if (!fs->dev->flush) return 0;

	return fs->dev->flush(fs->dev->ctx) ? -EIO : 0;
}

const struct fs_ops cosfs_ops = {
	.name     = "cosfs",
	.name_max = COSFS_NAME_MAX,
	.root     = cosfs_root,
	.lookup   = cosfs_lookup,
	.create   = cosfs_create,
	.unlink   = cosfs_unlink,
	.rename   = cosfs_rename,
	.getattr  = cosfs_getattr,
	.read     = cosfs_read,
	.write    = cosfs_write,
	.truncate = cosfs_truncate,
	.readdir  = cosfs_readdir,
	.evict    = cosfs_evict,
	.sync     = cosfs_sync,
};

/*
 * mkfs and mount
 */

int
cosfs_dev_blank(struct fs_blkdev *dev)
{
	uint64_t *b = fs_page_alloc();
	size_t    i;
	int       blank = 1;

	if (!b) return -ENOMEM;
	if (dev->read(dev->ctx, 0, b)) {
		fs_page_free(b);
		return -EIO;
	}
	for (i = 0; i < COSFS_BLOCK_SIZE / sizeof(uint64_t); i++) {
		if (b[i]) blank = 0;
	}
	fs_page_free(b);

	return blank;
}

int
cosfs_mkfs(struct fs_blkdev *dev, uint32_t ninodes)
{
	struct cosfs_super   sb;
	struct cosfs_dinode *root;
	uint64_t             i;
	char                *b;
	int                  ret = 0;

	if (dev->nblocks > UINT32_MAX) return -EFBIG; /* block numbers are 32 bits */
	if (ninodes == 0) ninodes = dev->nblocks / 4;
	if (ninodes < 2 * COSFS_INODES_PER_BLOCK) ninodes = 2 * COSFS_INODES_PER_BLOCK;
	ninodes = (ninodes + COSFS_INODES_PER_BLOCK - 1) / COSFS_INODES_PER_BLOCK * COSFS_INODES_PER_BLOCK;

	memset(&sb, 0, sizeof(sb));
	sb.magic       = COSFS_MAGIC;
	sb.version     = COSFS_VERSION;
	sb.block_size  = COSFS_BLOCK_SIZE;
	sb.nblocks     = dev->nblocks;
	sb.ninodes     = ninodes;
	sb.bmap_start  = 1;
	sb.bmap_blocks = (dev->nblocks + COSFS_BITS_PER_BLOCK - 1) / COSFS_BITS_PER_BLOCK;
	sb.itab_start  = sb.bmap_start + sb.bmap_blocks;
	sb.itab_blocks = ninodes / COSFS_INODES_PER_BLOCK;
	sb.data_start  = sb.itab_start + sb.itab_blocks;
	sb.root_ino    = COSFS_ROOT_INO;
	if (sb.data_start + 1 >= dev->nblocks) return -ENOSPC;

	b = fs_page_alloc();
	if (!b) return -ENOMEM;

	/* invalidate any old superblock first, so a failed mkfs doesn't look valid */
	memset(b, 0, COSFS_BLOCK_SIZE);
	if (dev->write(dev->ctx, 0, b)) goto ioerr;

	/* bitmap: the metadata blocks [0, data_start) are in use */
	for (i = 0; i < sb.bmap_blocks; i++) {
		uint64_t first = i * COSFS_BITS_PER_BLOCK, bit;

		memset(b, 0, COSFS_BLOCK_SIZE);
		for (bit = first; bit < sb.data_start && bit < first + COSFS_BITS_PER_BLOCK; bit++) {
			b[(bit - first) / 8] |= 1 << ((bit - first) % 8);
		}
		if (dev->write(dev->ctx, sb.bmap_start + i, b)) goto ioerr;
	}
	/* inode table, with the root directory */
	for (i = 0; i < sb.itab_blocks; i++) {
		memset(b, 0, COSFS_BLOCK_SIZE);
		if (i == COSFS_ROOT_INO / COSFS_INODES_PER_BLOCK) {
			root        = (struct cosfs_dinode *)(b + (COSFS_ROOT_INO % COSFS_INODES_PER_BLOCK) * COSFS_INODE_SIZE);
			root->mode  = FS_S_IFDIR | 0755;
			root->nlink = 1;
		}
		if (dev->write(dev->ctx, sb.itab_start + i, b)) goto ioerr;
	}
	/* the superblock last */
	memset(b, 0, COSFS_BLOCK_SIZE);
	memcpy(b, &sb, sizeof(sb));
	if (dev->write(dev->ctx, 0, b)) goto ioerr;
	if (dev->flush && dev->flush(dev->ctx)) goto ioerr;
done:
	fs_page_free(b);
	return ret;
ioerr:
	ret = -EIO;
	goto done;
}

static void
cosfs_free(struct cosfs *fs)
{
	int i;

	for (i = 0; i < COSFS_CACHE_N; i++) {
		if (fs->cache[i].data) fs_page_free(fs->cache[i].data);
	}
	if (fs->scratch1) fs_page_free(fs->scratch1);
	if (fs->scratch2) fs_page_free(fs->scratch2);
	fs_free(fs, sizeof(struct cosfs));
}

struct cosfs *
cosfs_mount(struct fs_blkdev *dev, int *err)
{
	struct cosfs *fs;
	uint64_t      blk;
	int           i;
	char         *b;

	*err = -ENOMEM;
	fs   = fs_zalloc(sizeof(struct cosfs));
	if (!fs) return NULL;
	fs->dev = dev;
	for (i = 0; i < COSFS_CACHE_N; i++) {
		if (!(fs->cache[i].data = fs_page_alloc())) goto err;
	}
	if (!(fs->scratch1 = fs_page_alloc()) || !(fs->scratch2 = fs_page_alloc())) goto err;

	*err = -EIO;
	if (dev->read(dev->ctx, 0, fs->cache[0].data)) goto err;
	memcpy(&fs->sb, fs->cache[0].data, sizeof(fs->sb));

	*err = -EINVAL;
	if (fs->sb.magic != COSFS_MAGIC || fs->sb.version != COSFS_VERSION || fs->sb.block_size != COSFS_BLOCK_SIZE) {
		goto err;
	}
	if (fs->sb.nblocks > dev->nblocks || fs->sb.bmap_start != 1
	    || fs->sb.bmap_blocks != (fs->sb.nblocks + COSFS_BITS_PER_BLOCK - 1) / COSFS_BITS_PER_BLOCK
	    || fs->sb.itab_start != fs->sb.bmap_start + fs->sb.bmap_blocks
	    || fs->sb.ninodes != fs->sb.itab_blocks * COSFS_INODES_PER_BLOCK
	    || fs->sb.data_start != fs->sb.itab_start + fs->sb.itab_blocks || fs->sb.data_start >= fs->sb.nblocks
	    || fs->sb.root_ino != COSFS_ROOT_INO) {
		goto err;
	}

	/* count free blocks */
	*err = -EIO;
	for (blk = fs->sb.data_start; blk < fs->sb.nblocks; blk++) {
		uint64_t bit = blk % COSFS_BITS_PER_BLOCK;

		b = bget(fs, fs->sb.bmap_start + blk / COSFS_BITS_PER_BLOCK, 1);
		if (!b) goto err;
		if (!(b[bit / 8] & (1 << (bit % 8)))) fs->free_blocks++;
	}
	fs->bmap_hint = fs->sb.data_start;
	fs->ino_hint  = COSFS_ROOT_INO + 1;
	*err          = 0;

	return fs;
err:
	cosfs_free(fs);
	return NULL;
}

uint64_t
cosfs_free_blocks(struct cosfs *fs)
{
	return fs->free_blocks;
}
