/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * The vfs interface: a file system service.
 *
 * There are two layers:
 *
 * 1. The invocation functions (vfs_*) implemented by the VFS server.
 *    Each client first registers a shared-memory buffer with
 *    vfs_shm_attach(); afterwards paths, data, stat records and
 *    directory entries are passed through that buffer (offset 0)
 *    rather than through registers. All return a non-negative value
 *    on success and a negative errno on failure. File descriptors are
 *    per client: the server identifies the caller by its component id
 *    and only consults that client's fd table.
 *
 * 2. The client library (vfsc_*, lib.c) which manages the buffer
 *    (allocated lazily from memmgr on first use), copies arguments in
 *    and results out, and splits large reads/writes into buffer-sized
 *    chunks. Applications should use this layer, or plain POSIX/stdio
 *    via the posix_fs library.
 */

#ifndef VFS_H
#define VFS_H

#include <cos_component.h>
#include <cos_stubs.h>
#include <errno.h>
#include <vfs_abi.h>

/* Size of the per-client shared buffer the client library uses. */
#define VFS_SHM_PAGES 16
#define VFS_SHM_SIZE  (VFS_SHM_PAGES * PAGE_SIZE)

/*
 * Invocation functions. "shm" is the caller's registered buffer.
 */

/* Register the client's shared buffer (memmgr cbuf of npages pages). */
long vfs_shm_attach(cbuf_t shm, unsigned long npages);
/* Path at shm. Returns an fd. */
long vfs_open(unsigned long flags, unsigned long mode);
long vfs_close(long fd);
/* Read up to len bytes into shm; returns bytes read (0 at EOF). */
long vfs_read(long fd, unsigned long len);
/* Write len bytes from shm; returns bytes written. */
long vfs_write(long fd, unsigned long len);
long vfs_pread(long fd, unsigned long len, long off);
long vfs_pwrite(long fd, unsigned long len, long off);
long vfs_lseek(long fd, long off, long whence);
/* Writes a struct vfs_stat to shm. */
long vfs_fstat(long fd);
/* Path at shm; writes a struct vfs_stat to shm (replacing the path). */
long vfs_stat(void);
long vfs_ftruncate(long fd, long len);
/* Path at shm. */
long vfs_truncate(long len);
long vfs_unlink(void);
long vfs_rmdir(void);
long vfs_mkdir(unsigned long mode);
/* Old path at shm, new path at shm + VFS_PATH_MAX. */
long vfs_rename(void);
/* Fills up to len bytes of shm with struct vfs_dirent records; returns bytes used, 0 at the end. */
long vfs_getdents(long fd, unsigned long len);
long vfs_fsync(long fd);
long vfs_fcntl(long fd, long cmd, long arg);
/* Flush every mounted file system. */
long vfs_sync(void);

/*
 * Client library. Same conventions: >= 0 on success, -errno on failure.
 * Thread safe (the shared buffer is protected by a lock).
 */
int  vfsc_init(void); /* optional: done on first use */
int  vfsc_open(const char *path, int flags, int mode);
int  vfsc_close(int fd);
long vfsc_read(int fd, void *buf, size_t len);
long vfsc_write(int fd, const void *buf, size_t len);
long vfsc_pread(int fd, void *buf, size_t len, long off);
long vfsc_pwrite(int fd, const void *buf, size_t len, long off);
long vfsc_lseek(int fd, long off, int whence);
int  vfsc_fstat(int fd, struct vfs_stat *st);
int  vfsc_stat(const char *path, struct vfs_stat *st);
int  vfsc_ftruncate(int fd, long len);
int  vfsc_truncate(const char *path, long len);
int  vfsc_unlink(const char *path);
int  vfsc_rmdir(const char *path);
int  vfsc_mkdir(const char *path, int mode);
int  vfsc_rename(const char *oldpath, const char *newpath);
/* Fills buf with struct vfs_dirent records (len must be at least 512 bytes to fit any name). */
long vfsc_getdents(int fd, void *buf, size_t len);
int  vfsc_fsync(int fd);
int  vfsc_fcntl(int fd, int cmd, long arg);
int  vfsc_sync(void);

#endif /* VFS_H */
