/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * unit_vfs_ramfs: exercises the VFS (with ramfs) through the vfs client
 * library: directory trees, create/write/read/seek, holes, truncate,
 * unlink (also of open files), rename, many files, a large file and fd
 * isolation between clients.
 *
 * The composition runs two instances of this component; each works in
 * its own directory (/t<compid>). Isolation check: each instance's
 * first fd must be VFS_FD_BASE and refer to its own file, which can
 * only hold if fd tables are per client.
 */

#include <cos_component.h>
#include <llprint.h>
#include <string.h>
#include <stdio.h>
#include <vfs.h>
#include "vfs_test.h"

static char pre[32];
static char pbuf[4][VFS_PATH_MAX];
static char buf[16384];

/* "<pre>/<rel>" in one of four rotating buffers */
static const char *
P(const char *rel)
{
	static int i;

	i = (i + 1) % 4;
	snprintf(pbuf[i], VFS_PATH_MAX, "%s/%s", pre, rel);
	return pbuf[i];
}

static long
put(const char *path, const char *s, int flags)
{
	int  fd = vfsc_open(path, VFS_O_WRONLY | VFS_O_CREAT | flags, 0644);
	long r;

	if (fd < 0) return fd;
	r = vfsc_write(fd, s, strlen(s));
	vfsc_close(fd);
	return r;
}

/* Read the whole file into buf (NUL-terminated); returns its length or -errno. */
static long
get(const char *path)
{
	int  fd = vfsc_open(path, VFS_O_RDONLY, 0);
	long r, tot = 0;

	if (fd < 0) return fd;
	while ((r = vfsc_read(fd, buf + tot, sizeof(buf) - 1 - tot)) > 0) tot += r;
	vfsc_close(fd);
	if (r < 0) return r;
	buf[tot] = '\0';
	return tot;
}

static int
count_entries(const char *path, const char *must_have)
{
	static char dbuf[1024];
	int         fd = vfsc_open(path, VFS_O_RDONLY | VFS_O_DIRECTORY, 0), n = 0, found = 0;
	long        r;

	if (fd < 0) return fd;
	while ((r = vfsc_getdents(fd, dbuf, sizeof(dbuf))) > 0) {
		long off;

		for (off = 0; off < r;) {
			struct vfs_dirent *d = (struct vfs_dirent *)(dbuf + off);

			if (must_have && !strcmp(d->name, must_have)) found = 1;
			n++;
			off += d->reclen;
		}
	}
	vfsc_close(fd);
	if (r < 0) return r;
	return (must_have && !found) ? -1000 : n;
}

static void
test_isolation(void)
{
	char mine[32];
	int  fd;

	snprintf(mine, sizeof(mine), "owned by %ld", cos_compid());
	VT_CHECK(vfsc_mkdir(pre, 0755) == 0);
	VT_CHECK(put(P("mine"), mine, 0) == (long)strlen(mine));
	/* our first fd is VFS_FD_BASE even though the other instance has fds open too */
	fd = vfsc_open(P("mine"), VFS_O_RDONLY, 0);
	VT_CHECK(fd == VFS_FD_BASE);
	VT_CHECK(vfsc_read(fd, buf, sizeof(buf)) == (long)strlen(mine) && !memcmp(buf, mine, strlen(mine)));
	VT_CHECK(vfsc_close(fd) == 0);
	VT_CHECK(vfsc_close(fd) == -EBADF);
	/* fds we never opened are invalid for us */
	VT_CHECK(vfsc_read(VFS_FD_BASE + 1, buf, 1) == -EBADF);
	VT_CHECK(vfsc_lseek(VFS_FD_BASE + 5, 0, VFS_SEEK_SET) == -EBADF);
	VT_CHECK(vfsc_read(0, buf, 1) == -EBADF);
	VT_CHECK(vfsc_read(100000, buf, 1) == -EBADF);
}

static void
test_dirtree(void)
{
	struct vfs_stat st;

	VT_CHECK(vfsc_mkdir(P("a"), 0755) == 0);
	VT_CHECK(vfsc_mkdir(P("a/b"), 0755) == 0);
	VT_CHECK(vfsc_mkdir(P("a/b/c"), 0700) == 0);
	VT_CHECK(vfsc_mkdir(P("a/b"), 0755) == -EEXIST);
	VT_CHECK(vfsc_mkdir(P("x/y"), 0755) == -ENOENT);
	VT_CHECK(vfsc_stat(P("a/b/c"), &st) == 0 && (st.mode & VFS_S_IFMT) == VFS_S_IFDIR && (st.mode & 0777) == 0700);
	VT_CHECK(vfsc_stat(P("a/./b/../b//c/"), &st) == 0);
	VT_CHECK(put(P("a/b/f"), "hello", 0) == 5);
	VT_CHECK(vfsc_stat(P("a/b/f"), &st) == 0 && (st.mode & VFS_S_IFMT) == VFS_S_IFREG && st.size == 5);
	VT_CHECK(vfsc_mkdir(P("a/b/f/g"), 0755) == -ENOTDIR);
	VT_CHECK(vfsc_open(P("a/b"), VFS_O_RDWR, 0) == -EISDIR);
	VT_CHECK(vfsc_open(P("a/b/f"), VFS_O_RDONLY | VFS_O_DIRECTORY, 0) == -ENOTDIR);
	VT_CHECK(vfsc_open(P("a/b/f"), VFS_O_RDWR | VFS_O_CREAT | VFS_O_EXCL, 0644) == -EEXIST);
	VT_CHECK(vfsc_open(P("nope"), VFS_O_RDONLY, 0) == -ENOENT);
	VT_CHECK(count_entries(P("a/b"), "f") == 2);
	VT_CHECK(vfsc_rmdir(P("a/b")) == -ENOTEMPTY);
	VT_CHECK(vfsc_rmdir(P("a/b/f")) == -ENOTDIR);
	VT_CHECK(vfsc_unlink(P("a/b/c")) == -EISDIR);
	VT_CHECK(vfsc_rmdir(P("a/b/c")) == 0);
	VT_CHECK(vfsc_stat(P("a/b/c"), &st) == -ENOENT);
}

static void
test_rw_seek(void)
{
	struct vfs_stat st;
	int             fd, i, nz = 0;

	fd = vfsc_open(P("rw"), VFS_O_RDWR | VFS_O_CREAT, 0644);
	VT_CHECK(fd >= VFS_FD_BASE);
	VT_CHECK(vfsc_write(fd, "0123456789", 10) == 10);
	VT_CHECK(vfsc_lseek(fd, 0, VFS_SEEK_CUR) == 10);
	VT_CHECK(vfsc_lseek(fd, 2, VFS_SEEK_SET) == 2);
	VT_CHECK(vfsc_read(fd, buf, 3) == 3 && !memcmp(buf, "234", 3));
	VT_CHECK(vfsc_lseek(fd, -2, VFS_SEEK_END) == 8);
	VT_CHECK(vfsc_read(fd, buf, 100) == 2 && !memcmp(buf, "89", 2));
	VT_CHECK(vfsc_read(fd, buf, 100) == 0);
	VT_CHECK(vfsc_lseek(fd, -100, VFS_SEEK_CUR) == -EINVAL);
	/* writing past the end leaves a zero-filled hole */
	VT_CHECK(vfsc_lseek(fd, 10000, VFS_SEEK_SET) == 10000);
	VT_CHECK(vfsc_write(fd, "Z", 1) == 1);
	VT_CHECK(vfsc_pread(fd, buf, 9991, 10) == 9991);
	for (i = 0; i < 9990; i++) nz |= buf[i];
	VT_CHECK(nz == 0 && buf[9990] == 'Z');
	VT_CHECK(vfsc_pwrite(fd, "ab", 2, 0) == 2);
	VT_CHECK(vfsc_lseek(fd, 0, VFS_SEEK_CUR) == 10001);
	VT_CHECK(vfsc_ftruncate(fd, 4) == 0);
	VT_CHECK(vfsc_ftruncate(fd, 6000) == 0);
	VT_CHECK(vfsc_pread(fd, buf, 6000, 0) == 6000 && !memcmp(buf, "ab23", 4) && buf[4] == 0 && buf[5999] == 0);
	VT_CHECK(vfsc_fstat(fd, &st) == 0 && st.size == 6000);
	VT_CHECK(vfsc_fsync(fd) == 0);
	VT_CHECK(vfsc_close(fd) == 0);
	VT_CHECK(vfsc_read(fd, buf, 1) == -EBADF);

	/* append, truncate and access modes */
	VT_CHECK(put(P("app"), "one", 0) == 3);
	VT_CHECK(put(P("app"), "two", VFS_O_APPEND) == 3);
	VT_CHECK(get(P("app")) == 6 && !strcmp(buf, "onetwo"));
	VT_CHECK(put(P("app"), "x", VFS_O_TRUNC) == 1);
	VT_CHECK(get(P("app")) == 1 && !strcmp(buf, "x"));
	VT_CHECK(vfsc_truncate(P("app"), 0) == 0 && get(P("app")) == 0);
	fd = vfsc_open(P("app"), VFS_O_RDONLY, 0);
	VT_CHECK(vfsc_write(fd, "y", 1) == -EBADF);
	VT_CHECK(vfsc_fcntl(fd, VFS_F_GETFL, 0) == VFS_O_RDONLY);
	vfsc_close(fd);
	fd = vfsc_open(P("app"), VFS_O_WRONLY, 0);
	VT_CHECK(vfsc_read(fd, buf, 1) == -EBADF);
	vfsc_close(fd);
}

static void
test_unlink_rename(void)
{
	struct vfs_stat st;
	int             fd;

	VT_CHECK(put(P("tmp"), "still here", 0) == 10);
	fd = vfsc_open(P("tmp"), VFS_O_RDONLY, 0);
	VT_CHECK(vfsc_unlink(P("tmp")) == 0);
	VT_CHECK(vfsc_stat(P("tmp"), &st) == -ENOENT);
	VT_CHECK(vfsc_read(fd, buf, 100) == 10 && !memcmp(buf, "still here", 10));
	VT_CHECK(vfsc_close(fd) == 0);
	VT_CHECK(vfsc_unlink(P("tmp")) == -ENOENT);

	VT_CHECK(put(P("r1"), "R1", 0) == 2 && put(P("r2"), "R2", 0) == 2);
	VT_CHECK(vfsc_rename(P("r1"), P("a/r1")) == 0);
	VT_CHECK(get(P("a/r1")) == 2 && !strcmp(buf, "R1"));
	VT_CHECK(vfsc_rename(P("a/r1"), P("r2")) == 0);
	VT_CHECK(get(P("r2")) == 2 && !strcmp(buf, "R1"));
	VT_CHECK(vfsc_stat(P("a/r1"), &st) == -ENOENT);
	VT_CHECK(vfsc_rename(P("a"), P("a/b/d")) == -EINVAL);
	VT_CHECK(vfsc_rename(P("a/b"), P("b2")) == 0);
	VT_CHECK(vfsc_rename(P("r2"), P("b2")) == -EISDIR);
	VT_CHECK(vfsc_stat(P("b2/f"), &st) == 0 && st.size == 5);
}

#define NFILES 200

static void
test_many_files(void)
{
	char name[64], content[64];
	int  i, bad = 0;

	VT_CHECK(vfsc_mkdir(P("many"), 0755) == 0);
	for (i = 0; i < NFILES; i++) {
		snprintf(name, sizeof(name), "many/file_%03d", i);
		snprintf(content, sizeof(content), "content of %d", i);
		if (put(P(name), content, 0) != (long)strlen(content)) bad++;
	}
	VT_CHECK(bad == 0);
	VT_CHECK(count_entries(P("many"), "file_199") == NFILES);
	for (i = 0, bad = 0; i < NFILES; i++) {
		snprintf(name, sizeof(name), "many/file_%03d", i);
		snprintf(content, sizeof(content), "content of %d", i);
		if (get(P(name)) < 0 || strcmp(buf, content)) bad++;
		if (i % 2 == 0 && vfsc_unlink(P(name))) bad++;
	}
	VT_CHECK(bad == 0);
	VT_CHECK(count_entries(P("many"), "file_001") == NFILES / 2);
	for (i = 1, bad = 0; i < NFILES; i += 2) {
		snprintf(name, sizeof(name), "many/file_%03d", i);
		if (vfsc_unlink(P(name))) bad++;
	}
	VT_CHECK(bad == 0);
	VT_CHECK(vfsc_rmdir(P("many")) == 0);
}

/* 5 MiB: larger than the shared buffer and beyond ramfs' direct and single-indirect ranges */
#define BIG (5 * 1024 * 1024 + 321)

static void
test_large_file(void)
{
	struct vfs_stat st;
	unsigned long   off, k;
	long            r = 0;
	int             fd, bad = 0;

	fd = vfsc_open(P("big"), VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, 0644);
	VT_CHECK(fd >= 0);
	for (off = 0; off < BIG; off += r) {
		unsigned long n = BIG - off < sizeof(buf) ? BIG - off : sizeof(buf);

		for (k = 0; k < n; k++) buf[k] = vt_pattern(off + k, 9);
		r = vfsc_write(fd, buf, n);
		if (r <= 0) break;
	}
	VT_CHECK(off == BIG);
	VT_CHECK(vfsc_fstat(fd, &st) == 0 && st.size == BIG);
	VT_CHECK(vfsc_lseek(fd, 0, VFS_SEEK_SET) == 0);
	for (off = 0; off < BIG; off += r) {
		r = vfsc_read(fd, buf, sizeof(buf));
		if (r <= 0) break;
		for (k = 0; k < (unsigned long)r; k++) bad |= (unsigned char)buf[k] != vt_pattern(off + k, 9);
	}
	VT_CHECK(off == BIG && !bad);
	/* one big read through the client library's chunking */
	{
		static char big[200 * 1024];

		VT_CHECK(vfsc_pread(fd, big, sizeof(big), 1000) == sizeof(big));
		for (k = 0, bad = 0; k < sizeof(big); k++) bad |= (unsigned char)big[k] != vt_pattern(1000 + k, 9);
		VT_CHECK(!bad);
	}
	VT_CHECK(vfsc_close(fd) == 0);
	VT_CHECK(vfsc_unlink(P("big")) == 0);
}

static void
test_cleanup(void)
{
	VT_CHECK(vfsc_unlink(P("b2/f")) == 0);
	VT_CHECK(vfsc_rmdir(P("b2")) == 0);
	VT_CHECK(vfsc_rmdir(P("a")) == 0);
	VT_CHECK(vfsc_unlink(P("rw")) == 0 && vfsc_unlink(P("app")) == 0 && vfsc_unlink(P("r2")) == 0);
	VT_CHECK(vfsc_unlink(P("mine")) == 0);
	VT_CHECK(count_entries(pre, NULL) == 0);
	VT_CHECK(vfsc_rmdir(pre) == 0);
}

int
main(void)
{
	snprintf(pre, sizeof(pre), "/t%ld", cos_compid());
	printc("unit_vfs_ramfs [%ld]: testing in %s\n", cos_compid(), pre);

	VT_CASE(test_isolation);
	VT_CASE(test_dirtree);
	VT_CASE(test_rw_seek);
	VT_CASE(test_unlink_rename);
	VT_CASE(test_many_files);
	VT_CASE(test_large_file);
	VT_CASE(test_cleanup);
	VT_DONE();

	return 0;
}
