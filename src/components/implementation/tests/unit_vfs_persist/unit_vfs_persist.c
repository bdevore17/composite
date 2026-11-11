/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * unit_vfs_persist: persistence of cosfs on a virtio-blk disk, across
 * reboots and between Composite and the host. Driven by
 * tools/cosfs/persist_test.sh, which formats an image, puts
 * /from_host.txt into it, boots twice and checks the image with the
 * host tool after each boot.
 *
 * Phase 1 (no /disk/persist/phase1.done): check the host's file, write
 * a tree of files, mark phase 1 done.
 * Phase 2 (phase1.done, no phase2.done): verify everything phase 1
 * wrote (it survived the reboot), then modify it (append, truncate,
 * delete), mark phase 2 done.
 * Later boots: verify the phase 2 state.
 *
 * The expected contents are mirrored in persist_test.sh: keep them in sync.
 */

#include <cos_component.h>
#include <llprint.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <unit_vfs_ramfs/vfs_test.h>

#define PDIR "/disk/persist"
#define BIG PDIR "/big.bin"
#define BIG_SZ (5 * 1024 * 1024 + 1234) /* reaches cosfs' double-indirect blocks */
#define BIG_SZ2 (1024 * 1024)           /* after phase 2's truncation */
#define BIG_SEED 7
#define HELLO PDIR "/dir1/dir2/hello.txt"
#define NLINES 50
#define NFILES 20
#define HOST_FILE "/disk/from_host.txt"
#define HOST_TEXT "written by the host\n"

static char buf[65536];

static long
slurp(const char *path)
{
	FILE  *f = fopen(path, "r");
	size_t n;

	if (!f) return -errno;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return n;
}

static int
exists(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0;
}

static int
count_dir(const char *path)
{
	DIR           *d = opendir(path);
	struct dirent *de;
	int            n = 0;

	if (!d) return -1;
	while ((de = readdir(d))) n++;
	closedir(d);
	return n;
}

static int
check_big(size_t size)
{
	struct stat st;
	FILE       *f;
	size_t      off = 0, n, i;
	int         ok  = 1;

	if (stat(BIG, &st) || (size_t)st.st_size != size) return 0;
	f = fopen(BIG, "rb");
	if (!f) return 0;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		for (i = 0; i < n; i++, off++) ok &= (unsigned char)buf[i] == vt_pattern(off, BIG_SEED);
	}
	fclose(f);
	return ok && off == size;
}

static int
check_hello(int phase2)
{
	char  expect[NLINES * 48 + 32];
	char *p = expect;
	int   i;

	for (i = 0; i < NLINES; i++) p += sprintf(p, "line %d of the persistence test\n", i);
	if (phase2) sprintf(p, "phase 2\n");
	return slurp(HELLO) == (long)strlen(expect) && !strcmp(buf, expect);
}

static int
mark(const char *path)
{
	FILE *f = fopen(path, "w");

	return f && fputs("done\n", f) >= 0 && fclose(f) == 0;
}

static void
phase1_write(void)
{
	FILE  *f;
	size_t off, i, n;
	int    fd;
	char   name[64];

	VT_CHECK(slurp(HOST_FILE) == (long)strlen(HOST_TEXT) && !strcmp(buf, HOST_TEXT));

	VT_CHECK(mkdir(PDIR, 0755) == 0);
	VT_CHECK(mkdir(PDIR "/dir1", 0755) == 0);
	VT_CHECK(mkdir(PDIR "/dir1/dir2", 0755) == 0);
	VT_CHECK(mkdir(PDIR "/files", 0755) == 0);

	fd = open(BIG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	VT_CHECK(fd >= 0);
	for (off = 0; fd >= 0 && off < BIG_SZ; off += n) {
		n = BIG_SZ - off < sizeof(buf) ? BIG_SZ - off : sizeof(buf);
		for (i = 0; i < n; i++) buf[i] = vt_pattern(off + i, BIG_SEED);
		if (write(fd, buf, n) != (ssize_t)n) break;
	}
	VT_CHECK(off == BIG_SZ);
	VT_CHECK(fsync(fd) == 0 && close(fd) == 0);

	f = fopen(HELLO, "w");
	VT_CHECK(f != NULL);
	for (i = 0; f && i < NLINES; i++) fprintf(f, "line %d of the persistence test\n", (int)i);
	VT_CHECK(f && fclose(f) == 0);

	for (i = 0; i < NFILES; i++) {
		snprintf(name, sizeof(name), PDIR "/files/f%02d", (int)i);
		f = fopen(name, "w");
		VT_CHECK(f && fprintf(f, "file %02d\n", (int)i) > 0 && fclose(f) == 0);
	}
	VT_CHECK(mark(PDIR "/tmp") && rename(PDIR "/tmp", PDIR "/renamed") == 0);
	VT_CHECK(mark(PDIR "/deleted") && unlink(PDIR "/deleted") == 0);

	/* read back before declaring success */
	VT_CHECK(check_big(BIG_SZ));
	VT_CHECK(check_hello(0));
	VT_CHECK(count_dir(PDIR "/files") == NFILES);
	VT_CHECK(mark(PDIR "/phase1.done"));
	sync();
}

static void
phase2_verify_and_modify(void)
{
	char  name[64];
	FILE *f;
	int   i;

	/* everything from phase 1 survived the reboot */
	VT_CHECK(check_big(BIG_SZ));
	VT_CHECK(check_hello(0));
	VT_CHECK(count_dir(PDIR "/files") == NFILES);
	VT_CHECK(slurp(PDIR "/files/f13") == 8 && !strcmp(buf, "file 13\n"));
	VT_CHECK(exists(PDIR "/renamed") && !exists(PDIR "/tmp") && !exists(PDIR "/deleted"));
	VT_CHECK(exists(HOST_FILE));

	/* modify */
	f = fopen(HELLO, "a");
	VT_CHECK(f && fputs("phase 2\n", f) >= 0 && fclose(f) == 0);
	VT_CHECK(truncate(BIG, BIG_SZ2) == 0);
	for (i = 0; i < NFILES / 2; i++) {
		snprintf(name, sizeof(name), PDIR "/files/f%02d", i);
		VT_CHECK(unlink(name) == 0);
	}
	VT_CHECK(check_hello(1));
	VT_CHECK(check_big(BIG_SZ2));
	VT_CHECK(mark(PDIR "/phase2.done"));
	sync();
}

static void
phase2_state_verify(void)
{
	VT_CHECK(check_hello(1));
	VT_CHECK(check_big(BIG_SZ2));
	VT_CHECK(count_dir(PDIR "/files") == NFILES / 2);
}

int
main(void)
{
	struct stat st, root;

	if (stat("/disk", &st) || stat("/", &root) || st.st_dev == root.st_dev) {
		printc("FAILURE: /disk is not mounted (no virtio-blk disk?)\n");
		VT_DONE();
		return 0;
	}
	if (!exists(PDIR "/phase1.done")) {
		printc("unit_vfs_persist: phase 1 (write)\n");
		VT_CASE(phase1_write);
		printc("unit_vfs_persist: phase 1 %s\n", vt_failures ? "FAILED" : "done");
	} else if (!exists(PDIR "/phase2.done")) {
		printc("unit_vfs_persist: phase 2 (verify after reboot, modify)\n");
		VT_CASE(phase2_verify_and_modify);
		printc("unit_vfs_persist: phase 2 %s\n", vt_failures ? "FAILED" : "done");
	} else {
		printc("unit_vfs_persist: both phases done; verifying the final state\n");
		VT_CASE(phase2_state_verify);
	}
	VT_DONE();

	return 0;
}
