/*
 * Host unit tests for src/components/lib/fs: the VFS core, ramfs and
 * cosfs (on an in-memory disk), checked with the independent fsck.
 * The same scenarios run inside Composite in the unit_vfs_* tests;
 * these additionally run a randomized differential test (ramfs vs.
 * cosfs) and check for leaks.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_env.h"
#include "ramfs.h"
#include "vfs_core.h"

extern long host_pages_outstanding;

static int failures, checks;

#define CHECK(cond, ...)                                                       \
	do {                                                                   \
		checks++;                                                      \
		if (!(cond)) {                                                 \
			failures++;                                            \
			printf("FAILURE %s:%d: %s: ", __FILE__, __LINE__, #cond); \
			printf(__VA_ARGS__);                                   \
			printf("\n");                                          \
		}                                                              \
	} while (0)

static struct vfs_core   vfs;
static struct vfs_client cli, cli2;

static char *
P(const char *prefix, const char *rel)
{
	static char buf[4][VFS_PATH_MAX];
	static int  i;

	i = (i + 1) % 4;
	snprintf(buf[i], sizeof(buf[i]), "%s/%s", prefix, rel);
	return buf[i];
}

static long
write_str(const char *path, const char *s, uint32_t flags)
{
	int  fd = vfs_core_open(&vfs, &cli, path, VFS_O_WRONLY | VFS_O_CREAT | flags, 0644);
	long r;

	if (fd < 0) return fd;
	r = vfs_core_write(&vfs, &cli, fd, s, strlen(s));
	vfs_core_close(&vfs, &cli, fd);
	return r;
}

static int
read_all(const char *path, char *buf, size_t len)
{
	int  fd = vfs_core_open(&vfs, &cli, path, VFS_O_RDONLY, 0);
	long r, tot = 0;

	if (fd < 0) return fd;
	while ((r = vfs_core_read(&vfs, &cli, fd, buf + tot, len - tot)) > 0) tot += r;
	vfs_core_close(&vfs, &cli, fd);
	if (r < 0) return r;
	buf[tot] = '\0';
	return tot;
}

/* Count entries of a directory with getdents, using a small buffer to exercise continuation. */
static int
count_entries(const char *path, const char *must_have)
{
	char buf[200];
	int  fd = vfs_core_open(&vfs, &cli, path, VFS_O_RDONLY | VFS_O_DIRECTORY, 0), n = 0, found = 0;
	long r;

	if (fd < 0) return fd;
	while ((r = vfs_core_getdents(&vfs, &cli, fd, buf, sizeof(buf))) > 0) {
		long off = 0;

		while (off < r) {
			struct vfs_dirent *d = (struct vfs_dirent *)(buf + off);

			if (must_have && !strcmp(d->name, must_have)) found = 1;
			n++;
			off += d->reclen;
		}
	}
	vfs_core_close(&vfs, &cli, fd);
	if (r < 0) return r;
	return must_have && !found ? -1000 : n;
}

static void
suite(const char *pre)
{
	struct vfs_stat st;
	static char     buf[16384];
	int             fd, i, ret;
	long            r;

	printf("-- suite on %s\n", pre);
	/* directory tree */
	CHECK(vfs_core_mkdir(&vfs, P(pre, "a"), 0755) == 0, "mkdir a");
	CHECK(vfs_core_mkdir(&vfs, P(pre, "a/b"), 0755) == 0, "mkdir a/b");
	CHECK(vfs_core_mkdir(&vfs, P(pre, "a/b/c"), 0755) == 0, "mkdir a/b/c");
	CHECK(vfs_core_mkdir(&vfs, P(pre, "a/b"), 0755) == -EEXIST, "mkdir exists");
	CHECK(vfs_core_mkdir(&vfs, P(pre, "x/y"), 0755) == -ENOENT, "mkdir missing parent");
	CHECK(vfs_core_stat(&vfs, P(pre, "a/b/c"), &st) == 0 && (st.mode & VFS_S_IFMT) == VFS_S_IFDIR, "stat dir");
	CHECK(vfs_core_stat(&vfs, P(pre, "a/./b/../b//c/"), &st) == 0, "path normalization");
	CHECK(write_str(P(pre, "a/b/f"), "hello", 0) == 5, "create file");
	CHECK(vfs_core_stat(&vfs, P(pre, "a/b/f"), &st) == 0 && (st.mode & VFS_S_IFMT) == VFS_S_IFREG && st.size == 5,
	      "stat file");
	CHECK(vfs_core_mkdir(&vfs, P(pre, "a/b/f/g"), 0755) == -ENOTDIR, "ENOTDIR");
	CHECK(vfs_core_open(&vfs, &cli, P(pre, "a/b"), VFS_O_RDWR, 0) == -EISDIR, "open dir rw");
	CHECK(vfs_core_open(&vfs, &cli, P(pre, "a/b/f"), VFS_O_RDONLY | VFS_O_DIRECTORY, 0) == -ENOTDIR, "O_DIRECTORY");
	CHECK(vfs_core_open(&vfs, &cli, P(pre, "a/b/f"), VFS_O_RDONLY | VFS_O_CREAT | VFS_O_EXCL, 0) == -EEXIST, "EXCL");
	CHECK(vfs_core_open(&vfs, &cli, P(pre, "nope"), VFS_O_RDONLY, 0) == -ENOENT, "ENOENT");
	CHECK(count_entries(P(pre, "a/b"), "f") == 2, "readdir a/b");
	CHECK(vfs_core_rmdir(&vfs, P(pre, "a/b")) == -ENOTEMPTY, "rmdir non-empty");
	CHECK(vfs_core_rmdir(&vfs, P(pre, "a/b/f")) == -ENOTDIR, "rmdir file");
	CHECK(vfs_core_unlink(&vfs, P(pre, "a/b/c")) == -EISDIR, "unlink dir");

	/* read/write/seek */
	fd = vfs_core_open(&vfs, &cli, P(pre, "rw"), VFS_O_RDWR | VFS_O_CREAT, 0644);
	CHECK(fd >= VFS_FD_BASE, "open rw %d", fd);
	CHECK(vfs_core_write(&vfs, &cli, fd, "0123456789", 10) == 10, "write");
	CHECK(vfs_core_lseek(&vfs, &cli, fd, 0, VFS_SEEK_CUR) == 10, "tell");
	CHECK(vfs_core_lseek(&vfs, &cli, fd, 2, VFS_SEEK_SET) == 2, "seek set");
	CHECK(vfs_core_read(&vfs, &cli, fd, buf, 3) == 3 && !memcmp(buf, "234", 3), "read after seek");
	CHECK(vfs_core_lseek(&vfs, &cli, fd, -2, VFS_SEEK_END) == 8, "seek end");
	CHECK(vfs_core_read(&vfs, &cli, fd, buf, 100) == 2 && !memcmp(buf, "89", 2), "short read at end");
	CHECK(vfs_core_read(&vfs, &cli, fd, buf, 100) == 0, "EOF");
	CHECK(vfs_core_lseek(&vfs, &cli, fd, -100, VFS_SEEK_CUR) == -EINVAL, "negative seek");
	/* write past EOF leaves a zero-filled hole */
	CHECK(vfs_core_lseek(&vfs, &cli, fd, 10000, VFS_SEEK_SET) == 10000, "seek past eof");
	CHECK(vfs_core_write(&vfs, &cli, fd, "Z", 1) == 1, "write past eof");
	CHECK(vfs_core_pread(&vfs, &cli, fd, buf, 9991, 10) == 9991, "pread hole");
	for (i = 0, ret = 0; i < 9990; i++) ret |= buf[i];
	CHECK(ret == 0 && buf[9990] == 'Z', "hole is zero");
	CHECK(vfs_core_pwrite(&vfs, &cli, fd, "ab", 2, 0) == 2, "pwrite");
	CHECK(vfs_core_lseek(&vfs, &cli, fd, 0, VFS_SEEK_CUR) == 10001, "pwrite doesn't move offset");
	CHECK(vfs_core_ftruncate(&vfs, &cli, fd, 4) == 0, "ftruncate shrink");
	CHECK(vfs_core_ftruncate(&vfs, &cli, fd, 6000) == 0, "ftruncate grow");
	CHECK(vfs_core_pread(&vfs, &cli, fd, buf, 6000, 0) == 6000 && !memcmp(buf, "ab23", 4) && buf[4] == 0
	          && buf[5999] == 0,
	      "truncate zeroes");
	CHECK(vfs_core_fstat(&vfs, &cli, fd, &st) == 0 && st.size == 6000, "fstat");
	vfs_core_close(&vfs, &cli, fd);
	CHECK(vfs_core_read(&vfs, &cli, fd, buf, 1) == -EBADF, "closed fd");

	/* O_APPEND, O_TRUNC, access modes */
	CHECK(write_str(P(pre, "app"), "one", 0) == 3, "w1");
	CHECK(write_str(P(pre, "app"), "two", VFS_O_APPEND) == 3, "append");
	CHECK(read_all(P(pre, "app"), buf, sizeof(buf)) == 6 && !strcmp(buf, "onetwo"), "append content");
	CHECK(write_str(P(pre, "app"), "x", VFS_O_TRUNC) == 1, "trunc");
	CHECK(read_all(P(pre, "app"), buf, sizeof(buf)) == 1 && !strcmp(buf, "x"), "trunc content");
	fd = vfs_core_open(&vfs, &cli, P(pre, "app"), VFS_O_RDONLY, 0);
	CHECK(vfs_core_write(&vfs, &cli, fd, "y", 1) == -EBADF, "write on rdonly");
	vfs_core_close(&vfs, &cli, fd);
	fd = vfs_core_open(&vfs, &cli, P(pre, "app"), VFS_O_WRONLY, 0);
	CHECK(vfs_core_read(&vfs, &cli, fd, buf, 1) == -EBADF, "read on wronly");
	CHECK(vfs_core_fcntl(&vfs, &cli, fd, VFS_F_GETFL, 0) == VFS_O_WRONLY, "getfl");
	vfs_core_close(&vfs, &cli, fd);

	/* unlink while open */
	CHECK(write_str(P(pre, "tmp"), "still here", 0) == 10, "w tmp");
	fd = vfs_core_open(&vfs, &cli, P(pre, "tmp"), VFS_O_RDONLY, 0);
	CHECK(vfs_core_unlink(&vfs, P(pre, "tmp")) == 0, "unlink open file");
	CHECK(vfs_core_stat(&vfs, P(pre, "tmp"), &st) == -ENOENT, "gone from namespace");
	CHECK(vfs_core_read(&vfs, &cli, fd, buf, 100) == 10 && !memcmp(buf, "still here", 10), "still readable");
	vfs_core_close(&vfs, &cli, fd);
	CHECK(vfs_core_unlink(&vfs, P(pre, "tmp")) == -ENOENT, "unlink twice");

	/* rename */
	CHECK(write_str(P(pre, "r1"), "R1", 0) == 2 && write_str(P(pre, "r2"), "R2", 0) == 2, "w r1 r2");
	CHECK(vfs_core_rename(&vfs, P(pre, "r1"), P(pre, "a/r1")) == 0, "rename into dir");
	CHECK(read_all(P(pre, "a/r1"), buf, sizeof(buf)) == 2 && !strcmp(buf, "R1"), "renamed content");
	CHECK(vfs_core_rename(&vfs, P(pre, "a/r1"), P(pre, "r2")) == 0, "rename over existing");
	CHECK(read_all(P(pre, "r2"), buf, sizeof(buf)) == 2 && !strcmp(buf, "R1"), "replaced content");
	CHECK(vfs_core_stat(&vfs, P(pre, "a/r1"), &st) == -ENOENT, "old name gone");
	CHECK(vfs_core_rename(&vfs, P(pre, "a"), P(pre, "a/b/c/d")) == -EINVAL, "rename into itself");
	CHECK(vfs_core_rename(&vfs, P(pre, "a/b/c"), P(pre, "c2")) == 0, "rename dir");
	CHECK(vfs_core_rename(&vfs, P(pre, "r2"), P(pre, "c2")) == -EISDIR, "file over dir");
	CHECK(vfs_core_rmdir(&vfs, P(pre, "c2")) == 0, "rmdir");

	/* many files */
	CHECK(vfs_core_mkdir(&vfs, P(pre, "many"), 0755) == 0, "mkdir many");
	for (i = 0, ret = 0; i < 300; i++) {
		char name[64], content[64];

		snprintf(name, sizeof(name), "many/file_%03d", i);
		snprintf(content, sizeof(content), "content of %d", i);
		if (write_str(P(pre, name), content, 0) != (long)strlen(content)) ret++;
	}
	CHECK(ret == 0, "create 300 files");
	CHECK(count_entries(P(pre, "many"), "file_299") == 300, "300 entries");
	for (i = 0, ret = 0; i < 300; i++) {
		char name[64], content[64];

		snprintf(name, sizeof(name), "many/file_%03d", i);
		snprintf(content, sizeof(content), "content of %d", i);
		if (read_all(P(pre, name), buf, sizeof(buf)) < 0 || strcmp(buf, content)) ret++;
		if (i % 2 == 0 && vfs_core_unlink(&vfs, P(pre, name))) ret++;
	}
	CHECK(ret == 0, "verify + delete half");
	CHECK(count_entries(P(pre, "many"), "file_001") == 150, "150 entries left");

	/* large file: crosses the direct, single- and double-indirect ranges of both file systems */
	{
		const size_t big = 9 * 1024 * 1024 + 123;
		size_t       off;
		int          bad = 0;

		fd = vfs_core_open(&vfs, &cli, P(pre, "big"), VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644);
		for (off = 0; off < big; off += r) {
			size_t n = big - off < 7000 ? big - off : 7000;
			size_t k;

			for (k = 0; k < n; k++) buf[k] = cosfs_pattern(off + k, 5);
			r = vfs_core_write(&vfs, &cli, fd, buf, n);
			if (r <= 0) break;
		}
		CHECK(off == big, "write big file (%zu)", off);
		CHECK(vfs_core_fstat(&vfs, &cli, fd, &st) == 0 && st.size == big, "big size");
		vfs_core_lseek(&vfs, &cli, fd, 0, VFS_SEEK_SET);
		for (off = 0; off < big; off += r) {
			size_t k;

			r = vfs_core_read(&vfs, &cli, fd, buf, 8192);
			if (r <= 0) break;
			for (k = 0; k < (size_t)r; k++) bad |= (unsigned char)buf[k] != cosfs_pattern(off + k, 5);
		}
		CHECK(off == big && !bad, "read big file back");
		CHECK(vfs_core_ftruncate(&vfs, &cli, fd, 3 * 1024 * 1024 + 5) == 0, "truncate big");
		CHECK(vfs_core_pread(&vfs, &cli, fd, buf, 10, 3 * 1024 * 1024 - 5) == 10, "read near new end");
		for (i = 0, bad = 0; i < 10; i++) bad |= (unsigned char)buf[i] != cosfs_pattern(3 * 1024 * 1024 - 5 + i, 5);
		CHECK(!bad, "content after truncate");
		vfs_core_close(&vfs, &cli, fd);
		CHECK(vfs_core_unlink(&vfs, P(pre, "big")) == 0, "unlink big");
	}

	/* clean up everything so leak checks work */
	for (i = 1; i < 300; i += 2) {
		char name[64];

		snprintf(name, sizeof(name), "many/file_%03d", i);
		vfs_core_unlink(&vfs, P(pre, name));
	}
	CHECK(vfs_core_rmdir(&vfs, P(pre, "many")) == 0, "rmdir many");
	CHECK(vfs_core_unlink(&vfs, P(pre, "a/b/f")) == 0 && vfs_core_rmdir(&vfs, P(pre, "a/b")) == 0
	          && vfs_core_rmdir(&vfs, P(pre, "a")) == 0,
	      "rm tree");
	CHECK(vfs_core_unlink(&vfs, P(pre, "rw")) == 0 && vfs_core_unlink(&vfs, P(pre, "app")) == 0
	          && vfs_core_unlink(&vfs, P(pre, "r2")) == 0,
	      "rm files");
	CHECK(count_entries(pre[0] ? pre : "/", NULL) >= 0, "readdir prefix");
	CHECK(vfs_core_nfiles_open(&vfs) == 0, "no open files left (%d)", vfs_core_nfiles_open(&vfs));
}

static void
test_clients(void)
{
	char buf[16];
	int  fa, fb;

	printf("-- client isolation\n");
	CHECK(write_str("/r/iso", "AAAA", 0) == 4, "w");
	fa = vfs_core_open(&vfs, &cli, "/r/iso", VFS_O_RDONLY, 0);
	CHECK(fa == VFS_FD_BASE, "first fd %d", fa);
	CHECK(vfs_core_read(&vfs, &cli2, fa, buf, 4) == -EBADF, "other client can't use the fd");
	CHECK(vfs_core_close(&vfs, &cli2, fa) == -EBADF, "nor close it");
	fb = vfs_core_open(&vfs, &cli2, "/r/iso", VFS_O_RDONLY, 0);
	CHECK(fb == VFS_FD_BASE, "same fd number, different file object");
	CHECK(vfs_core_read(&vfs, &cli, fa, buf, 2) == 2, "A reads 2");
	CHECK(vfs_core_lseek(&vfs, &cli2, fb, 0, VFS_SEEK_CUR) == 0, "B's offset unaffected");
	CHECK(vfs_core_read(&vfs, &cli, -1, buf, 1) == -EBADF && vfs_core_read(&vfs, &cli, 1000, buf, 1) == -EBADF,
	      "out of range fds");
	vfs_core_client_teardown(&vfs, &cli2);
	CHECK(vfs_core_lseek(&vfs, &cli2, fb, 0, VFS_SEEK_CUR) == -EBADF, "teardown closed B's fds");
	vfs_core_close(&vfs, &cli, fa);
	{
		int fds[VFS_MAX_FDS + 1], i, last = 0;

		for (i = 0; i <= VFS_MAX_FDS; i++) last = fds[i] = vfs_core_open(&vfs, &cli, "/r/iso", VFS_O_RDONLY, 0);
		CHECK(last == -EMFILE, "EMFILE");
		for (i = 0; i < VFS_MAX_FDS; i++) vfs_core_close(&vfs, &cli, fds[i]);
	}
	vfs_core_unlink(&vfs, "/r/iso");
}

/* Randomized differential test: identical operations on ramfs and cosfs must give identical results. */
static void
test_differential(unsigned seed, int nops)
{
	static char a[70000], b[70000], c[70000];
	const char *roots[2] = {"/r/d", "/disk/d"};
	int         fds[2][8], i, k, mism = 0, ok = 0;

	printf("-- differential test seed %u, %d ops\n", seed, nops);
	srand(seed);
	for (k = 0; k < 2; k++) {
		vfs_core_mkdir(&vfs, roots[k], 0755);
		for (i = 0; i < 8; i++) fds[k][i] = -1;
	}
	for (i = 0; i < nops; i++) {
		int  op = rand() % 10, f = rand() % 8, dirn = rand() % 3, j;
		long off = rand() % 40000, len = rand() % 20000, tlen = off * (rand() % 3);
		char rel[64], rel2[64];
		long res[2];

		snprintf(rel, sizeof(rel), "%s%d/f%d", dirn ? "s" : "", dirn, f);
		snprintf(rel2, sizeof(rel2), "%s%d/f%d", "s", rand() % 3, rand() % 8);
		if (op == 2 || op == 3) {
			for (j = 0; j < len; j++) b[j] = (char)(rand() % 256);
		}
		for (k = 0; k < 2; k++) {
			char p[VFS_PATH_MAX], p2[VFS_PATH_MAX], dp[VFS_PATH_MAX];
			struct vfs_stat st;

			snprintf(p, sizeof(p), "%s/%s", roots[k], rel);
			snprintf(p2, sizeof(p2), "%s/%s", roots[k], rel2);
			snprintf(dp, sizeof(dp), "%s/s%d", roots[k], dirn);
			switch (op) {
			case 0:
				res[k] = vfs_core_mkdir(&vfs, dp, 0755);
				break;
			case 1:
				if (fds[k][f] >= 0) vfs_core_close(&vfs, &cli, fds[k][f]);
				res[k] = fds[k][f] = vfs_core_open(&vfs, &cli, p, VFS_O_RDWR | VFS_O_CREAT, 0644);
				if (res[k] >= 0) res[k] = 0;
				break;
			case 2:
			case 3:
				res[k] = fds[k][f] < 0 ? -EBADF : vfs_core_pwrite(&vfs, &cli, fds[k][f], b, len, off);
				break;
			case 4:
			case 5:
				res[k] = fds[k][f] < 0 ? -EBADF : vfs_core_pread(&vfs, &cli, fds[k][f], k ? c : a, len, off);
				if (k == 1 && res[0] == res[1] && res[1] > 0 && memcmp(a, c, res[1])) res[1] = -9999;
				break;
			case 6:
				res[k] = fds[k][f] < 0 ? -EBADF : vfs_core_ftruncate(&vfs, &cli, fds[k][f], tlen);
				break;
			case 7:
				res[k] = vfs_core_unlink(&vfs, p);
				break;
			case 8:
				res[k] = vfs_core_rename(&vfs, p, p2);
				break;
			case 9:
				res[k] = vfs_core_stat(&vfs, p, &st);
				if (!res[k]) res[k] = st.size;
				break;
			}
		}
		if (res[0] >= 0) ok++;
		if (res[0] != res[1]) {
			mism++;
			if (mism < 10) printf("   op %d (%d) on %s: ramfs %ld cosfs %ld\n", i, op, rel, res[0], res[1]);
		}
	}
	printf("   %d of %d operations succeeded\n", ok, nops);
	CHECK(mism == 0, "differential mismatches: %d", mism);
	for (k = 0; k < 2; k++) {
		for (i = 0; i < 8; i++) {
			if (fds[k][i] >= 0) vfs_core_close(&vfs, &cli, fds[k][i]);
		}
	}
}

int
main(void)
{
	static char      disk[64 * 1024 * 1024];
	struct host_dev  hd;
	struct ramfs    *rfs;
	struct cosfs    *cfs;
	uint64_t         free0;
	long             pages0;
	int              err;

	host_dev_mem(&hd, disk, sizeof(disk) / COSFS_BLOCK_SIZE);
	CHECK(cosfs_dev_blank(&hd.dev) == 1, "blank disk");
	CHECK(cosfs_mkfs(&hd.dev, 0) == 0, "mkfs");
	CHECK(cosfs_dev_blank(&hd.dev) == 0, "formatted disk is not blank");
	CHECK(cosfs_fsck(&hd.dev, stdout, 0) == 0, "fsck fresh");

	vfs_core_init(&vfs);
	vfs_core_client_init(&cli);
	vfs_core_client_init(&cli2);
	rfs = ramfs_create(0);
	CHECK(vfs_core_mount(&vfs, "/", &ramfs_ops, rfs) == 0, "mount ramfs");
	cfs = cosfs_mount(&hd.dev, &err);
	CHECK(cfs != NULL, "mount cosfs %d", err);
	CHECK(vfs_core_mount(&vfs, "/disk", &cosfs_ops, cfs) == 0, "mount cosfs at /disk");
	CHECK(vfs_core_mkdir(&vfs, "/r", 0755) == 0, "mkdir /r");
	CHECK(count_entries("/", "disk") == 2, "mount point visible in /");
	CHECK(vfs_core_rmdir(&vfs, "/disk") == -EBUSY, "can't remove mount point");
	CHECK(write_str("/r/x", "x", 0) == 1 && vfs_core_rename(&vfs, "/r/x", "/disk/x") == -EXDEV, "EXDEV");
	vfs_core_unlink(&vfs, "/r/x");
	CHECK(vfs_core_open(&vfs, &cli, "/disk/../r/../disk", VFS_O_RDONLY, 0) == VFS_FD_BASE, "open mount root");
	vfs_core_close(&vfs, &cli, VFS_FD_BASE);
	{
		char longname[80];

		memset(longname, 'n', 70);
		longname[70] = '\0';
		CHECK(write_str(P("/disk", longname), "x", 0) == -ENAMETOOLONG, "cosfs name too long");
		CHECK(write_str(P("/r", longname), "x", 0) == 1 && vfs_core_unlink(&vfs, P("/r", longname)) == 0,
		      "ramfs long name");
	}

	free0  = cosfs_free_blocks(cfs);
	pages0 = ramfs_pages_used(rfs);
	suite("/r");
	CHECK(ramfs_pages_used(rfs) == (uint64_t)pages0, "ramfs data pages all freed (%llu)",
	      (unsigned long long)ramfs_pages_used(rfs));
	suite("/disk");
	{
		/* directories never shrink, so only the root directory's blocks remain in use */
		struct vfs_stat rst;

		vfs_core_stat(&vfs, "/disk", &rst);
		CHECK(cosfs_free_blocks(cfs) == free0 - rst.blocks / COSFS_SECTORS_PER_BLOCK,
		      "cosfs blocks all freed (%llu vs %llu)", (unsigned long long)cosfs_free_blocks(cfs),
		      (unsigned long long)free0);
	}
	CHECK(cosfs_fsck(&hd.dev, stdout, 0) == 0, "fsck after suite");

	test_clients();
	test_differential(1, 3000);
	test_differential(42, 3000);
	CHECK(cosfs_fsck(&hd.dev, stdout, 0) == 0, "fsck after differential test");

	/* persistence: remount and read back */
	CHECK(write_str("/disk/persist", "survives remount", 0) == 16, "w persist");
	cfs = cosfs_mount(&hd.dev, &err);
	CHECK(cfs != NULL, "remount");
	{
		struct vfs_core   v2;
		static char       b[64];
		int               fd;

		vfs_core_init(&v2);
		vfs_core_mount(&v2, "/", &cosfs_ops, cfs);
		fd = vfs_core_open(&v2, &cli2, "/persist", VFS_O_RDONLY, 0);
		CHECK(fd >= 0 && vfs_core_read(&v2, &cli2, fd, b, sizeof(b)) == 16 && !memcmp(b, "survives remount", 16),
		      "read after remount");
		vfs_core_close(&v2, &cli2, fd);
	}
	CHECK(cosfs_fsck(&hd.dev, stdout, 0) == 0, "final fsck");

	/* ENOSPC on a tiny disk, and the file system stays consistent */
	{
		static char      small[600 * 1024];
		struct host_dev  sd;
		struct cosfs    *s;
		struct vfs_core  v3;
		static char      blob[1 << 20];
		int              fd;
		long             r;

		host_dev_mem(&sd, small, sizeof(small) / COSFS_BLOCK_SIZE);
		CHECK(cosfs_mkfs(&sd.dev, 0) == 0, "mkfs small");
		s = cosfs_mount(&sd.dev, &err);
		vfs_core_init(&v3);
		vfs_core_mount(&v3, "/", &cosfs_ops, s);
		fd = vfs_core_open(&v3, &cli2, "/fill", VFS_O_WRONLY | VFS_O_CREAT, 0644);
		r  = vfs_core_write(&v3, &cli2, fd, blob, sizeof(blob));
		CHECK(r > 0 && r < (long)sizeof(blob), "partial write when full (%ld)", r);
		CHECK(vfs_core_write(&v3, &cli2, fd, blob, 4096) == -ENOSPC, "ENOSPC");
		vfs_core_close(&v3, &cli2, fd);
		CHECK(cosfs_fsck(&sd.dev, stdout, 0) == 0, "fsck full disk");
		CHECK(vfs_core_unlink(&v3, "/fill") == 0, "unlink fill");
		CHECK(cosfs_fsck(&sd.dev, stdout, 0) == 0, "fsck after freeing");
	}

	printf("%d checks, %d failures\n", checks, failures);
	printf("%s\n", failures ? "TEST FAILED" : "TEST PASSED");
	return failures ? 1 : 0;
}
