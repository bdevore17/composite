/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * The data formats shared by VFS clients and the VFS server: open
 * flags, seek whences, fcntl commands, and the layouts of the stat and
 * directory-entry records the server writes into the client's shared
 * memory. Included by the `vfs` interface header.
 *
 * Flag/whence/command values equal Linux's (and musl's on x86), so
 * POSIX code can pass them through, but clients should use these
 * names.
 */

#ifndef VFS_ABI_H
#define VFS_ABI_H

#include <stdint.h>

#define VFS_PATH_MAX 1024 /* including the terminating NUL */
#define VFS_FD_BASE  3    /* first fd the VFS hands out (0-2 are stdio) */
#define VFS_MAX_FDS  64   /* open fds per client */

#define VFS_O_RDONLY    00
#define VFS_O_WRONLY    01
#define VFS_O_RDWR      02
#define VFS_O_ACCMODE   03
#define VFS_O_CREAT     0100
#define VFS_O_EXCL      0200
#define VFS_O_TRUNC     01000
#define VFS_O_APPEND    02000
#define VFS_O_DIRECTORY 0200000
#define VFS_O_SUPPORTED                                                                                     \
	(VFS_O_ACCMODE | VFS_O_CREAT | VFS_O_EXCL | VFS_O_TRUNC | VFS_O_APPEND | VFS_O_DIRECTORY)

#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

#define VFS_F_GETFD 1
#define VFS_F_SETFD 2
#define VFS_F_GETFL 3
#define VFS_F_SETFL 4

#define VFS_S_IFMT  0170000
#define VFS_S_IFDIR 0040000
#define VFS_S_IFREG 0100000

#define VFS_DT_DIR 4
#define VFS_DT_REG 8

struct vfs_stat {
	uint64_t ino;
	uint32_t mode; /* VFS_S_IF* | permission bits */
	uint32_t nlink;
	uint64_t size;
	uint64_t blocks; /* 512-byte units */
	uint32_t blksize;
	uint32_t dev;    /* index of the mount the file lives on */
};

/*
 * Directory entries returned by getdents. Records are packed back to
 * back; each is reclen bytes long (8-byte aligned) and name is
 * NUL-terminated. off is the directory offset of the *next* entry
 * (usable with lseek(SEEK_SET)). This is the same layout as Linux's
 * struct linux_dirent64 / musl's struct dirent.
 */
struct vfs_dirent {
	uint64_t ino;
	int64_t  off;
	uint16_t reclen;
	uint8_t  type;
	char     name[];
};

#endif /* VFS_ABI_H */
