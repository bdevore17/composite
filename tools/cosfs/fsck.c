/*
 * cosfs consistency checker. Independent of cosfs.c (it only uses the
 * on-disk format), so it can catch bugs in the file system code.
 *
 * Checks: superblock layout; every allocated inode's block pointers
 * are in the data area and no block is referenced twice; block counts;
 * no blocks mapped beyond the file size; the directory tree from the
 * root reaches every allocated inode exactly nlink times, with valid
 * names and types and no directory reachable twice; the bitmap equals
 * the set of referenced blocks (no leaks, no used-but-free blocks).
 */

#include <stdlib.h>
#include <string.h>
#include "host_env.h"

struct fsck {
	struct fs_blkdev   *dev;
	struct cosfs_super  sb;
	struct cosfs_dinode *inodes;
	uint8_t            *bused;  /* blocks referenced */
	uint32_t           *refs;   /* directory references per inode */
	uint8_t            *dirseen;
	FILE               *out;
	int                 verbose;
	int                 errors;
	char                buf[COSFS_BLOCK_SIZE];
};

#define ERR(f, ...)                                         \
	do {                                                \
		(f)->errors++;                              \
		if ((f)->out) fprintf((f)->out, "fsck: " __VA_ARGS__); \
	} while (0)

static int
rd(struct fsck *f, uint64_t blk, void *buf)
{
	if (blk >= f->sb.nblocks || f->dev->read(f->dev->ctx, blk, buf)) {
		ERR(f, "cannot read block %llu\n", (unsigned long long)blk);
		return -1;
	}
	return 0;
}

/* Claim a block for inode ino. Returns 0 if ok, -1 if invalid. */
static int
claim(struct fsck *f, uint32_t ino, uint32_t blk, uint32_t *count)
{
	if (blk < f->sb.data_start || blk >= f->sb.nblocks) {
		ERR(f, "inode %u: block %u out of the data area\n", ino, blk);
		return -1;
	}
	if (f->bused[blk]) {
		ERR(f, "inode %u: block %u referenced twice\n", ino, blk);
		return -1;
	}
	f->bused[blk] = 1;
	(*count)++;
	return 0;
}

/* Check the inode's block tree; *maxfblk gets 1 + the highest mapped file block. */
static void
check_blocks(struct fsck *f, uint32_t ino, struct cosfs_dinode *di)
{
	uint32_t count = 0, i, j;
	uint64_t maxf = 0;
	uint32_t ind[COSFS_PTRS_PER_BLOCK], l2[COSFS_PTRS_PER_BLOCK];

	for (i = 0; i < COSFS_NDIRECT; i++) {
		if (di->direct[i] && !claim(f, ino, di->direct[i], &count)) maxf = i + 1;
	}
	if (di->ind && !claim(f, ino, di->ind, &count) && !rd(f, di->ind, ind)) {
		for (i = 0; i < COSFS_PTRS_PER_BLOCK; i++) {
			if (ind[i] && !claim(f, ino, ind[i], &count)) maxf = COSFS_NDIRECT + i + 1;
		}
	}
	if (di->dind && !claim(f, ino, di->dind, &count) && !rd(f, di->dind, ind)) {
		for (i = 0; i < COSFS_PTRS_PER_BLOCK; i++) {
			if (!ind[i] || claim(f, ino, ind[i], &count) || rd(f, ind[i], l2)) continue;
			for (j = 0; j < COSFS_PTRS_PER_BLOCK; j++) {
				if (l2[j] && !claim(f, ino, l2[j], &count)) {
					maxf = COSFS_NDIRECT + COSFS_PTRS_PER_BLOCK + (uint64_t)i * COSFS_PTRS_PER_BLOCK + j + 1;
				}
			}
		}
	}
	if (count != di->nblocks) ERR(f, "inode %u: nblocks %u but %u blocks referenced\n", ino, di->nblocks, count);
	if (maxf * COSFS_BLOCK_SIZE > ((di->size + COSFS_BLOCK_SIZE - 1) / COSFS_BLOCK_SIZE) * COSFS_BLOCK_SIZE) {
		ERR(f, "inode %u: blocks mapped beyond size %llu\n", ino, (unsigned long long)di->size);
	}
}

/* Read file block fblk of a directory (0 if hole). */
static uint32_t
fmap(struct fsck *f, struct cosfs_dinode *di, uint64_t fblk)
{
	uint32_t ind[COSFS_PTRS_PER_BLOCK];

	if (fblk < COSFS_NDIRECT) return di->direct[fblk];
	fblk -= COSFS_NDIRECT;
	if (fblk < COSFS_PTRS_PER_BLOCK) {
		if (!di->ind || rd(f, di->ind, ind)) return 0;
		return ind[fblk];
	}
	fblk -= COSFS_PTRS_PER_BLOCK;
	if (!di->dind || rd(f, di->dind, ind)) return 0;
	if (!ind[fblk / COSFS_PTRS_PER_BLOCK] || rd(f, ind[fblk / COSFS_PTRS_PER_BLOCK], ind)) return 0;
	return ind[fblk % COSFS_PTRS_PER_BLOCK];
}

static void
walk_dir(struct fsck *f, uint32_t dino, const char *path, int depth)
{
	struct cosfs_dinode *di = &f->inodes[dino];
	uint64_t             nslots, s;

	if (depth > 64) {
		ERR(f, "%s: directory tree too deep (loop?)\n", path);
		return;
	}
	if (di->size % COSFS_DIRENT_SIZE) ERR(f, "%s: directory size %llu not a multiple of the entry size\n", path,
	                                      (unsigned long long)di->size);
	nslots = di->size / COSFS_DIRENT_SIZE;
	for (s = 0; s < nslots; s++) {
		struct cosfs_ddirent d;
		struct cosfs_dinode *ci;
		uint32_t             blk = fmap(f, di, s / COSFS_DIRENTS_PER_BLOCK);
		char                 name[COSFS_NAME_MAX + 1], cpath[4096];
		int                  i;

		if (!blk) continue;
		/* (re-)read the block for every entry: the recursion below clobbers f->buf */
		if (rd(f, blk, f->buf)) continue;
		memcpy(&d, f->buf + (s % COSFS_DIRENTS_PER_BLOCK) * COSFS_DIRENT_SIZE, sizeof(d));
		if (!d.ino) continue;
		if (d.namelen == 0 || d.namelen > COSFS_NAME_MAX) {
			ERR(f, "%s: entry %llu has bad name length %u\n", path, (unsigned long long)s, d.namelen);
			continue;
		}
		memcpy(name, d.name, d.namelen);
		name[d.namelen] = '\0';
		for (i = 0; i < d.namelen; i++) {
			if (name[i] == '/' || name[i] == '\0') ERR(f, "%s: entry %llu has an invalid name\n", path, (unsigned long long)s);
		}
		if (!strcmp(name, ".") || !strcmp(name, "..")) ERR(f, "%s: entry named %s\n", path, name);
		snprintf(cpath, sizeof(cpath), "%s/%s", strcmp(path, "/") ? path : "", name);
		if (d.ino <= COSFS_ROOT_INO || d.ino >= f->sb.ninodes) {
			ERR(f, "%s: bad inode number %u\n", cpath, d.ino);
			continue;
		}
		ci = &f->inodes[d.ino];
		if (!ci->mode) {
			ERR(f, "%s: refers to free inode %u\n", cpath, d.ino);
			continue;
		}
		f->refs[d.ino]++;
		if ((FS_ISDIR(ci->mode) ? FS_DT_DIR : FS_DT_REG) != d.type) ERR(f, "%s: entry type mismatch\n", cpath);
		if (f->verbose && f->out) {
			fprintf(f->out, "%c %10llu %s\n", FS_ISDIR(ci->mode) ? 'd' : '-', (unsigned long long)ci->size, cpath);
		}
		if (FS_ISDIR(ci->mode)) {
			if (f->dirseen[d.ino]) {
				ERR(f, "%s: directory reachable twice\n", cpath);
				continue;
			}
			f->dirseen[d.ino] = 1;
			walk_dir(f, d.ino, cpath, depth + 1);
		}
	}
}

int
cosfs_fsck(struct fs_blkdev *dev, FILE *out, int verbose)
{
	struct fsck *f = calloc(1, sizeof(*f));
	uint64_t     i;
	int          errors;

	f->dev     = dev;
	f->out     = out;
	f->verbose = verbose;
	if (dev->read(dev->ctx, 0, f->buf)) {
		ERR(f, "cannot read the superblock\n");
		goto done;
	}
	memcpy(&f->sb, f->buf, sizeof(f->sb));
	if (f->sb.magic != COSFS_MAGIC || f->sb.version != COSFS_VERSION || f->sb.block_size != COSFS_BLOCK_SIZE) {
		ERR(f, "bad superblock (magic/version/block size)\n");
		goto done;
	}
	if (f->sb.nblocks > dev->nblocks || f->sb.bmap_start != 1
	    || f->sb.bmap_blocks != (f->sb.nblocks + COSFS_BITS_PER_BLOCK - 1) / COSFS_BITS_PER_BLOCK
	    || f->sb.itab_start != f->sb.bmap_start + f->sb.bmap_blocks
	    || f->sb.ninodes != f->sb.itab_blocks * COSFS_INODES_PER_BLOCK
	    || f->sb.data_start != f->sb.itab_start + f->sb.itab_blocks || f->sb.root_ino != COSFS_ROOT_INO) {
		ERR(f, "bad superblock layout\n");
		goto done;
	}

	f->inodes  = calloc(f->sb.ninodes, sizeof(struct cosfs_dinode));
	f->refs    = calloc(f->sb.ninodes, sizeof(uint32_t));
	f->dirseen = calloc(f->sb.ninodes, 1);
	f->bused   = calloc(f->sb.nblocks, 1);
	for (i = 0; i < f->sb.itab_blocks; i++) {
		if (rd(f, f->sb.itab_start + i, f->buf)) goto done;
		memcpy(&f->inodes[i * COSFS_INODES_PER_BLOCK], f->buf, COSFS_BLOCK_SIZE);
	}
	for (i = 0; i < f->sb.data_start; i++) f->bused[i] = 1;

	if (!FS_ISDIR(f->inodes[COSFS_ROOT_INO].mode)) {
		ERR(f, "root inode is not a directory\n");
		goto done;
	}
	if (f->inodes[0].mode) ERR(f, "inode 0 is in use\n");
	for (i = 1; i < f->sb.ninodes; i++) {
		struct cosfs_dinode *di = &f->inodes[i];

		if (!di->mode) continue;
		if (!FS_ISDIR(di->mode) && !FS_ISREG(di->mode)) ERR(f, "inode %llu: bad mode %o\n", (unsigned long long)i, di->mode);
		check_blocks(f, i, di);
	}
	f->dirseen[COSFS_ROOT_INO] = 1;
	walk_dir(f, COSFS_ROOT_INO, "/", 0);

	for (i = 1; i < f->sb.ninodes; i++) {
		struct cosfs_dinode *di   = &f->inodes[i];
		uint32_t             want = (i == COSFS_ROOT_INO) ? 1 : f->refs[i];

		if (!di->mode) continue;
		if (i != COSFS_ROOT_INO && f->refs[i] == 0) {
			ERR(f, "inode %llu: allocated but unreachable (orphan)\n", (unsigned long long)i);
		} else if (di->nlink != want) {
			ERR(f, "inode %llu: nlink %u but %u references\n", (unsigned long long)i, di->nlink, want);
		}
	}
	for (i = 0; i < f->sb.nblocks; i++) {
		uint64_t bit = i % COSFS_BITS_PER_BLOCK;
		int      set;

		if (bit == 0 && rd(f, f->sb.bmap_start + i / COSFS_BITS_PER_BLOCK, f->buf)) goto done;
		set = !!(f->buf[bit / 8] & (1 << (bit % 8)));
		if (set && !f->bused[i]) ERR(f, "block %llu marked used but unreferenced (leak)\n", (unsigned long long)i);
		if (!set && f->bused[i]) ERR(f, "block %llu referenced but marked free\n", (unsigned long long)i);
	}
done:
	errors = f->errors;
	free(f->inodes);
	free(f->refs);
	free(f->dirseen);
	free(f->bused);
	free(f);
	return errors;
}
