/*
 * cosfs: host-side tool to create, inspect and modify cosfs disk
 * images (the on-disk file system of the Composite VFS). It uses the
 * same file system code as the VFS component (src/components/lib/fs).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_env.h"
#include "vfs_core.h"

static struct host_dev   hdev;
static struct vfs_core   vfs;
static struct vfs_client cli;

static void
usage(void)
{
	fprintf(stderr,
	        "usage: cosfs <command> <image> [args]\n"
	        "  mkfs   IMG [SIZE_MB]      create a file system (creating/resizing IMG if SIZE_MB given)\n"
	        "  fsck   IMG [-v]           check consistency (-v: also list every file)\n"
	        "  ls     IMG [PATH]         list a directory\n"
	        "  tree   IMG                list every file and directory\n"
	        "  cat    IMG PATH           print a file\n"
	        "  get    IMG PATH HOSTFILE  copy a file out of the image\n"
	        "  put    IMG HOSTFILE PATH  copy a host file into the image\n"
	        "  mkdir  IMG PATH\n"
	        "  rm     IMG PATH\n"
	        "  rmdir  IMG PATH\n"
	        "  verify IMG PATH SIZE SEED check that PATH holds SIZE bytes of the test pattern SEED\n");
	exit(2);
}

static void
die(const char *what, int err)
{
	fprintf(stderr, "cosfs: %s: %s\n", what, strerror(-err));
	exit(1);
}

static void
mount_image(const char *img)
{
	struct cosfs *fs;
	int           err;

	if ((err = host_dev_open(&hdev, img, 0, 0))) die(img, err);
	fs = cosfs_mount(&hdev.dev, &err);
	if (!fs) die("mount (not a cosfs image?)", err);
	vfs_core_init(&vfs);
	if ((err = vfs_core_mount(&vfs, "/", &cosfs_ops, fs))) die("mount", err);
	vfs_core_client_init(&cli);
}

static int
do_ls(const char *path, int recursive)
{
	static char buf[8192];
	long        n;
	int         fd = vfs_core_open(&vfs, &cli, path, VFS_O_RDONLY | VFS_O_DIRECTORY, 0);

	if (fd < 0) die(path, fd);
	while ((n = vfs_core_getdents(&vfs, &cli, fd, buf, sizeof(buf))) > 0) {
		long off;

		for (off = 0; off < n;) {
			struct vfs_dirent *d = (struct vfs_dirent *)(buf + off);
			struct vfs_stat    st;
			char               child[VFS_PATH_MAX];
			int                ret;

			snprintf(child, sizeof(child), "%s/%s", strcmp(path, "/") ? path : "", d->name);
			if ((ret = vfs_core_stat(&vfs, child, &st))) die(child, ret);
			printf("%c %10llu %s\n", d->type == VFS_DT_DIR ? 'd' : '-', (unsigned long long)st.size,
			       recursive ? child : d->name);
			off += d->reclen;
			if (recursive && d->type == VFS_DT_DIR) {
				/* recursion reuses buf: remember where we were */
				static char saved[8192];
				long        rest = n - off;

				memcpy(saved, buf + off, rest);
				do_ls(child, 1);
				memcpy(buf, saved, rest);
				n   = rest;
				off = 0;
			}
		}
	}
	if (n < 0) die(path, n);
	vfs_core_close(&vfs, &cli, fd);

	return 0;
}

int
main(int argc, char **argv)
{
	const char *cmd, *img;
	int         ret, fd = -1;

	if (argc < 3) usage();
	cmd = argv[1];
	img = argv[2];

	if (!strcmp(cmd, "mkfs")) {
		uint64_t sz = argc > 3 ? strtoull(argv[3], NULL, 0) * 1024 * 1024 : 0;

		if ((ret = host_dev_open(&hdev, img, sz != 0, sz))) die(img, ret);
		if ((ret = cosfs_mkfs(&hdev.dev, 0))) die("mkfs", ret);
		printf("cosfs: %s: %llu blocks of %d bytes\n", img, (unsigned long long)hdev.dev.nblocks, COSFS_BLOCK_SIZE);
		return 0;
	}
	if (!strcmp(cmd, "fsck")) {
		int errors;

		if ((ret = host_dev_open(&hdev, img, 0, 0))) die(img, ret);
		errors = cosfs_fsck(&hdev.dev, stdout, argc > 3 && !strcmp(argv[3], "-v"));
		printf("cosfs fsck: %s: %d error(s)\n", img, errors);
		return errors ? 1 : 0;
	}

	mount_image(img);
	if (!strcmp(cmd, "ls")) {
		do_ls(argc > 3 ? argv[3] : "/", 0);
	} else if (!strcmp(cmd, "tree")) {
		do_ls("/", 1);
	} else if (!strcmp(cmd, "cat") || !strcmp(cmd, "get")) {
		static char buf[65536];
		FILE       *out = stdout;
		long        n;

		if (argc < 4 || (!strcmp(cmd, "get") && argc < 5)) usage();
		if (!strcmp(cmd, "get") && !(out = fopen(argv[4], "wb"))) die(argv[4], -errno);
		fd = vfs_core_open(&vfs, &cli, argv[3], VFS_O_RDONLY, 0);
		if (fd < 0) die(argv[3], fd);
		while ((n = vfs_core_read(&vfs, &cli, fd, buf, sizeof(buf))) > 0) fwrite(buf, 1, n, out);
		if (n < 0) die(argv[3], n);
		if (out != stdout) fclose(out);
	} else if (!strcmp(cmd, "put")) {
		static char buf[65536];
		FILE       *in;
		size_t      n;

		if (argc < 5) usage();
		if (!(in = fopen(argv[3], "rb"))) die(argv[3], -errno);
		fd = vfs_core_open(&vfs, &cli, argv[4], VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, 0644);
		if (fd < 0) die(argv[4], fd);
		while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
			long w = vfs_core_write(&vfs, &cli, fd, buf, n);

			if (w < 0) die(argv[4], w);
			if ((size_t)w != n) die(argv[4], -ENOSPC);
		}
		fclose(in);
	} else if (!strcmp(cmd, "mkdir")) {
		if (argc < 4) usage();
		if ((ret = vfs_core_mkdir(&vfs, argv[3], 0755))) die(argv[3], ret);
	} else if (!strcmp(cmd, "rm")) {
		if (argc < 4) usage();
		if ((ret = vfs_core_unlink(&vfs, argv[3]))) die(argv[3], ret);
	} else if (!strcmp(cmd, "rmdir")) {
		if (argc < 4) usage();
		if ((ret = vfs_core_rmdir(&vfs, argv[3]))) die(argv[3], ret);
	} else if (!strcmp(cmd, "verify")) {
		static unsigned char buf[65536];
		uint64_t             size, seed, off = 0;
		struct vfs_stat      st;
		long                 n, i;

		if (argc < 6) usage();
		size = strtoull(argv[4], NULL, 0);
		seed = strtoull(argv[5], NULL, 0);
		if ((ret = vfs_core_stat(&vfs, argv[3], &st))) die(argv[3], ret);
		if (st.size != size) {
			printf("verify %s: FAILED: size %llu, expected %llu\n", argv[3], (unsigned long long)st.size,
			       (unsigned long long)size);
			return 1;
		}
		fd = vfs_core_open(&vfs, &cli, argv[3], VFS_O_RDONLY, 0);
		if (fd < 0) die(argv[3], fd);
		while ((n = vfs_core_read(&vfs, &cli, fd, buf, sizeof(buf))) > 0) {
			for (i = 0; i < n; i++, off++) {
				if (buf[i] != cosfs_pattern(off, seed)) {
					printf("verify %s: FAILED: byte %llu differs\n", argv[3], (unsigned long long)off);
					return 1;
				}
			}
		}
		if (n < 0) die(argv[3], n);
		printf("verify %s: OK (%llu bytes)\n", argv[3], (unsigned long long)off);
	} else {
		usage();
	}
	vfs_core_close(&vfs, &cli, fd); /* harmless if fd isn't open */
	if ((ret = vfs_core_sync(&vfs))) die("sync", ret);
	host_dev_close(&hdev);

	return 0;
}
