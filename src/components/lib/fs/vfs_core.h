/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * vfs_core: the file-system-independent part of the VFS: a mount
 * table, path resolution, open file objects and per-client
 * file-descriptor tables. It is a plain library (no IPC, no locking)
 * so that the VFS component and host tests share it.
 *
 * - Paths are normalized lexically ("//", ".", ".." are resolved
 *   without consulting the file systems; there are no symlinks, so
 *   this is exact except that "missing/.." resolves). Relative paths
 *   are relative to "/".
 * - A mount covers every path at or below its mount point; the
 *   longest matching mount point wins.
 * - fds are per client (VFS_FD_BASE ... VFS_FD_BASE+VFS_MAX_FDS-1)
 *   and refer to open file objects in a global table; a client can
 *   only name file objects through its own fd table.
 *
 * All functions return >= 0 on success and a negative errno on failure.
 */

#ifndef VFS_CORE_H
#define VFS_CORE_H

#include "fs.h"
#include "vfs_abi.h"

#define VFS_MAX_MOUNTS 8
#define VFS_MAX_FILES  512 /* open file objects, system wide */

struct vfs_mount {
	char                 path[VFS_PATH_MAX];
	size_t               plen;
	const struct fs_ops *ops;
	void                *fs;
	fs_ino_t             root;
};

struct vfs_file {
	struct vfs_mount *m;
	fs_ino_t          ino;
	uint64_t          off;
	uint32_t          flags;
	int               isdir;
	uint32_t          refcnt; /* 0 = free slot */
};

struct vfs_client {
	struct vfs_file *fds[VFS_MAX_FDS];
};

struct vfs_core {
	struct vfs_mount mounts[VFS_MAX_MOUNTS];
	int              nmounts;
	struct vfs_file  files[VFS_MAX_FILES];
	/* scratch space (calls are serialized; keeps big buffers off small stacks) */
	char             path[2][VFS_PATH_MAX];
	char             name[2][FS_NAME_MAX + 1];
	struct fs_dirent de;
};

void vfs_core_init(struct vfs_core *v);

/*
 * Mount fs at path. The root mount must be "/" and come first. For
 * other mounts, a directory named like the mount point is created in
 * the parent file system if missing so the mount shows up in readdir.
 */
int vfs_core_mount(struct vfs_core *v, const char *path, const struct fs_ops *ops, void *fs);

void vfs_core_client_init(struct vfs_client *c);
/* Close every fd of the client. */
void vfs_core_client_teardown(struct vfs_core *v, struct vfs_client *c);

int  vfs_core_open(struct vfs_core *v, struct vfs_client *c, const char *path, uint32_t flags, uint32_t mode);
int  vfs_core_close(struct vfs_core *v, struct vfs_client *c, int fd);
long vfs_core_read(struct vfs_core *v, struct vfs_client *c, int fd, void *buf, size_t len);
long vfs_core_write(struct vfs_core *v, struct vfs_client *c, int fd, const void *buf, size_t len);
long vfs_core_pread(struct vfs_core *v, struct vfs_client *c, int fd, void *buf, size_t len, int64_t off);
long vfs_core_pwrite(struct vfs_core *v, struct vfs_client *c, int fd, const void *buf, size_t len, int64_t off);
long vfs_core_lseek(struct vfs_core *v, struct vfs_client *c, int fd, int64_t off, int whence);
int  vfs_core_fstat(struct vfs_core *v, struct vfs_client *c, int fd, struct vfs_stat *st);
int  vfs_core_ftruncate(struct vfs_core *v, struct vfs_client *c, int fd, int64_t len);
int  vfs_core_fsync(struct vfs_core *v, struct vfs_client *c, int fd);
int  vfs_core_fcntl(struct vfs_core *v, struct vfs_client *c, int fd, int cmd, long arg);
/* Fill buf with packed struct vfs_dirent records; returns bytes used, 0 at the end. */
long vfs_core_getdents(struct vfs_core *v, struct vfs_client *c, int fd, void *buf, size_t len);

int vfs_core_stat(struct vfs_core *v, const char *path, struct vfs_stat *st);
int vfs_core_truncate(struct vfs_core *v, const char *path, int64_t len);
int vfs_core_unlink(struct vfs_core *v, const char *path);
int vfs_core_rmdir(struct vfs_core *v, const char *path);
int vfs_core_mkdir(struct vfs_core *v, const char *path, uint32_t mode);
int vfs_core_rename(struct vfs_core *v, const char *oldpath, const char *newpath);
/* Sync every mounted file system. */
int vfs_core_sync(struct vfs_core *v);

/* Number of open file objects (for tests). */
int vfs_core_nfiles_open(struct vfs_core *v);

#endif /* VFS_CORE_H */
