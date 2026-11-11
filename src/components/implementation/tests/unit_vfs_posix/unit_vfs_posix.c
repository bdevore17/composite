/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * unit_vfs_posix: the VFS through ordinary POSIX and stdio calls
 * (posix_fs routes musl's system calls to the vfs interface).
 *
 * The tests run in /disk/posix_test if /disk exists (vfs.diskfs) and
 * in /posix_test otherwise (vfs.ramfs), so the same component tests
 * both file systems.
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

static char base[64];
static char pbuf[4][256];

static const char *
P(const char *rel)
{
	static int i;

	i = (i + 1) % 4;
	snprintf(pbuf[i], sizeof(pbuf[i]), "%s/%s", base, rel);
	return pbuf[i];
}

static void
test_stdio_text(void)
{
	char  line[128];
	FILE *f;
	int   i, n, bad = 0;

	f = fopen(P("text.txt"), "w");
	VT_CHECK(f != NULL);
	if (!f) return;
	for (i = 0; i < 500; i++) VT_CHECK(fprintf(f, "line %d: %s\n", i, i % 2 ? "odd" : "even") > 0);
	VT_CHECK(fclose(f) == 0);

	f = fopen(P("text.txt"), "r");
	VT_CHECK(f != NULL);
	if (!f) return;
	for (i = 0; fgets(line, sizeof(line), f); i++) {
		char expect[128];

		snprintf(expect, sizeof(expect), "line %d: %s\n", i, i % 2 ? "odd" : "even");
		if (strcmp(line, expect)) bad++;
	}
	VT_CHECK(i == 500 && !bad);
	VT_CHECK(feof(f) && !ferror(f));
	/* seek back and parse with fscanf */
	VT_CHECK(fseek(f, 0, SEEK_SET) == 0);
	VT_CHECK(fscanf(f, "line %d", &n) == 1 && n == 0);
	VT_CHECK(fseek(f, -9, SEEK_END) == 0);
	VT_CHECK(fgets(line, sizeof(line), f) && !strcmp(line, "499: odd\n"));
	VT_CHECK(ftell(f) > 0);
	VT_CHECK(fclose(f) == 0);

	/* append mode */
	f = fopen(P("text.txt"), "a");
	VT_CHECK(f && fputs("appended\n", f) >= 0 && fclose(f) == 0);
	f = fopen(P("text.txt"), "r");
	VT_CHECK(f && fseek(f, -9, SEEK_END) == 0 && fgets(line, sizeof(line), f) && !strcmp(line, "appended\n"));
	if (f) fclose(f);

	/* "w" truncates */
	f = fopen(P("text.txt"), "w");
	VT_CHECK(f && fputs("short", f) >= 0 && fclose(f) == 0);
	f = fopen(P("text.txt"), "r+");
	VT_CHECK(f && fgets(line, sizeof(line), f) && !strcmp(line, "short"));
	if (f) fclose(f);
}

#define BIN_SZ (1024 * 1024 + 77)

static void
test_stdio_binary(void)
{
	static unsigned char data[BIN_SZ], back[BIN_SZ];
	struct stat          st;
	FILE                *f;
	size_t               i;

	for (i = 0; i < BIN_SZ; i++) data[i] = vt_pattern(i, 3);
	f = fopen(P("data.bin"), "wb");
	VT_CHECK(f && fwrite(data, 1, BIN_SZ, f) == BIN_SZ && fclose(f) == 0);
	VT_CHECK(stat(P("data.bin"), &st) == 0 && st.st_size == BIN_SZ && S_ISREG(st.st_mode));

	f = fopen(P("data.bin"), "rb");
	VT_CHECK(f != NULL);
	if (!f) return;
	VT_CHECK(fread(back, 1, BIN_SZ, f) == BIN_SZ && !memcmp(data, back, BIN_SZ));
	VT_CHECK(fread(back, 1, 1, f) == 0 && feof(f));
	/* random access with small reads through the stdio buffer */
	for (i = 0; i < 50; i++) {
		long          off = (i * 104729) % BIN_SZ;
		unsigned char c;

		VT_CHECK(fseek(f, off, SEEK_SET) == 0 && fread(&c, 1, 1, f) == 1 && c == data[off]);
	}
	VT_CHECK(fclose(f) == 0);
}

static void
test_posix_io(void)
{
	struct stat st;
	char        b[64];
	int         fd;

	fd = open(P("raw"), O_RDWR | O_CREAT | O_TRUNC, 0644);
	VT_CHECK(fd >= 3);
	VT_CHECK(write(fd, "abcdefghij", 10) == 10);
	VT_CHECK(lseek(fd, 3, SEEK_SET) == 3);
	VT_CHECK(read(fd, b, 4) == 4 && !memcmp(b, "defg", 4));
	VT_CHECK(pwrite(fd, "XY", 2, 0) == 2 && pread(fd, b, 3, 0) == 3 && !memcmp(b, "XYc", 3));
	VT_CHECK(ftruncate(fd, 5) == 0 && fstat(fd, &st) == 0 && st.st_size == 5);
	VT_CHECK(fsync(fd) == 0);
	VT_CHECK(close(fd) == 0);
	errno = 0;
	VT_CHECK(close(fd) == -1 && errno == EBADF);

	errno = 0;
	VT_CHECK(open(P("raw"), O_RDWR | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST);
	errno = 0;
	VT_CHECK(open(P("missing/x"), O_RDONLY) == -1 && errno == ENOENT);
	errno = 0;
	VT_CHECK(fopen(P("missing"), "r") == NULL && errno == ENOENT);
	VT_CHECK(access(P("raw"), F_OK) == 0);
	errno = 0;
	VT_CHECK(access(P("nothere"), F_OK) == -1 && errno == ENOENT);
	VT_CHECK(truncate(P("raw"), 2) == 0 && stat(P("raw"), &st) == 0 && st.st_size == 2);
}

#define NFILES 100

static void
test_dirs(void)
{
	char           name[64];
	struct stat    st;
	struct dirent *de;
	DIR           *d;
	int            i, n = 0, bad = 0;
	char           seen[NFILES] = {0};

	VT_CHECK(mkdir(P("dir"), 0755) == 0);
	errno = 0;
	VT_CHECK(mkdir(P("dir"), 0755) == -1 && errno == EEXIST);
	VT_CHECK(mkdir(P("dir/sub"), 0755) == 0);
	VT_CHECK(stat(P("dir/sub"), &st) == 0 && S_ISDIR(st.st_mode));
	for (i = 0; i < NFILES; i++) {
		FILE *f;

		snprintf(name, sizeof(name), "dir/f%d", i);
		f = fopen(P(name), "w");
		if (!f || fprintf(f, "%d", i) <= 0 || fclose(f)) bad++;
	}
	VT_CHECK(bad == 0);

	d = opendir(P("dir"));
	VT_CHECK(d != NULL);
	if (!d) return;
	while ((de = readdir(d))) {
		n++;
		if (de->d_name[0] == 'f') {
			i = atoi(de->d_name + 1);
			if (i < 0 || i >= NFILES || seen[i] || de->d_type != DT_REG)
				bad++;
			else
				seen[i] = 1;
		} else if (strcmp(de->d_name, "sub") || de->d_type != DT_DIR) {
			bad++;
		}
	}
	VT_CHECK(n == NFILES + 1 && !bad);
	/* rewinddir restarts the listing */
	rewinddir(d);
	VT_CHECK(readdir(d) != NULL);
	VT_CHECK(closedir(d) == 0);

	/* rename and remove */
	VT_CHECK(rename(P("dir/f0"), P("dir/sub/moved")) == 0);
	VT_CHECK(stat(P("dir/sub/moved"), &st) == 0 && st.st_size == 1);
	errno = 0;
	VT_CHECK(rmdir(P("dir/sub")) == -1 && errno == ENOTEMPTY);
	VT_CHECK(remove(P("dir/sub/moved")) == 0);
	VT_CHECK(remove(P("dir/sub")) == 0);
	for (i = 1, bad = 0; i < NFILES; i++) {
		snprintf(name, sizeof(name), "dir/f%d", i);
		if (unlink(P(name))) bad++;
	}
	VT_CHECK(bad == 0);
	VT_CHECK(rmdir(P("dir")) == 0);
	errno = 0;
	VT_CHECK(stat(P("dir"), &st) == -1 && errno == ENOENT);
}

static void
test_cleanup(void)
{
	DIR *d;

	VT_CHECK(unlink(P("text.txt")) == 0);
	VT_CHECK(unlink(P("data.bin")) == 0);
	VT_CHECK(unlink(P("raw")) == 0);
	d = opendir(base);
	VT_CHECK(d && readdir(d) == NULL);
	if (d) closedir(d);
	VT_CHECK(rmdir(base) == 0);
}

int
main(void)
{
	struct stat st;

	snprintf(base, sizeof(base), "%s/posix_test", stat("/disk", &st) == 0 && S_ISDIR(st.st_mode) ? "/disk" : "");
	printc("unit_vfs_posix: testing in %s\n", base);
	if (mkdir(base, 0755)) {
		printc("FAILURE: cannot create %s (errno %d)\n", base, errno);
		VT_DONE();
		return 0;
	}

	VT_CASE(test_stdio_text);
	VT_CASE(test_stdio_binary);
	VT_CASE(test_posix_io);
	VT_CASE(test_dirs);
	VT_CASE(test_cleanup);
	VT_DONE();

	return 0;
}
