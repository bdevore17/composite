/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * File system building blocks shared by the VFS server, the RAM file
 * system, the on-disk file system (cosfs), and the host-side cosfs
 * tool.
 *
 * Everything in this library is plain C with no Composite
 * dependencies so that it can be compiled and tested on the host
 * (see tools/cosfs). The embedding environment provides the page
 * allocator (fs_page_alloc/fs_page_free); all other memory is carved
 * out of those pages by fs_alloc.c.
 *
 * Nothing in this library is thread-safe: the caller (e.g. the VFS
 * server) serializes all calls.
 */

#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>
#include <errno.h>

#define FS_PAGE_SIZE 4096

/* File type and permission bits (same values as Linux/musl). */
#define FS_S_IFMT   0170000
#define FS_S_IFDIR  0040000
#define FS_S_IFREG  0100000
#define FS_S_IPERM  07777
#define FS_ISDIR(m) (((m) & FS_S_IFMT) == FS_S_IFDIR)
#define FS_ISREG(m) (((m) & FS_S_IFMT) == FS_S_IFREG)

/* d_type values for directory entries (same values as Linux/musl). */
#define FS_DT_UNKNOWN 0
#define FS_DT_DIR     4
#define FS_DT_REG     8

/* Longest name any backend supports (backends may support less). */
#define FS_NAME_MAX 255

/*
 * Opaque inode handle. Each backend picks its own encoding. 0 is never a
 * valid inode.
 */
typedef uint64_t fs_ino_t;

struct fs_stat {
	uint64_t ino;     /* inode number reported to the user */
	uint32_t mode;    /* FS_S_IF* | permissions */
	uint32_t nlink;
	uint64_t size;    /* bytes */
	uint64_t blocks;  /* 512-byte blocks allocated */
	uint32_t blksize; /* preferred I/O size */
};

struct fs_dirent {
	fs_ino_t ino;
	uint64_t next;       /* cookie of the next entry */
	uint8_t  type;       /* FS_DT_* */
	char     name[FS_NAME_MAX + 1];
};

/*
 * The operations a file system backend provides to the VFS. All return
 * 0 (or a byte count) on success and a negative errno on failure.
 *
 * Link counting: unlink/rmdir/rename only remove the directory entry
 * and decrement the inode's link count. The inode itself is freed by
 * `evict`, which the VFS calls once the link count is 0 *and* no open
 * file refers to it (POSIX unlink-while-open semantics).
 */
struct fs_ops {
	const char *name;
	int  name_max;
	int  (*root)(void *fs, fs_ino_t *ino);
	int  (*lookup)(void *fs, fs_ino_t dir, const char *name, fs_ino_t *ino);
	/* mode includes the type (FS_S_IFREG or FS_S_IFDIR) */
	int  (*create)(void *fs, fs_ino_t dir, const char *name, uint32_t mode, fs_ino_t *ino);
	/* is_dir: rmdir semantics (target must be an empty directory), else target must not be a directory */
	int  (*unlink)(void *fs, fs_ino_t dir, const char *name, int is_dir, fs_ino_t *victim);
	/* *replaced is set to the inode the new name used to point to (or 0) */
	int  (*rename)(void *fs, fs_ino_t odir, const char *oname, fs_ino_t ndir, const char *nname, fs_ino_t *replaced);
	int  (*getattr)(void *fs, fs_ino_t ino, struct fs_stat *st);
	long (*read)(void *fs, fs_ino_t ino, uint64_t off, void *buf, size_t len);
	long (*write)(void *fs, fs_ino_t ino, uint64_t off, const void *buf, size_t len);
	int  (*truncate)(void *fs, fs_ino_t ino, uint64_t size);
	/* returns 1 and fills *de if there is an entry at or after cookie, 0 at the end */
	int  (*readdir)(void *fs, fs_ino_t dir, uint64_t cookie, struct fs_dirent *de);
	int  (*evict)(void *fs, fs_ino_t ino);
	int  (*sync)(void *fs);
};

/* Provided by the environment: zeroed 4 KiB pages. */
void *fs_page_alloc(void);
void  fs_page_free(void *p);

/*
 * Small object allocator on top of fs_page_alloc. Objects of up to
 * FS_PAGE_SIZE bytes; the size must be passed to fs_free. Returned
 * memory is zeroed.
 */
void *fs_zalloc(size_t sz);
void  fs_free(void *p, size_t sz);

/* Minimal string helpers so the library does not depend on libc details. */
size_t fs_strnlen(const char *s, size_t max);

#endif /* FS_H */
