/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * posix_fs: route musl's file system calls to the VFS (vfs interface),
 * so that open/read/write/lseek/stat/unlink/mkdir/readdir and the
 * stdio functions built on them (fopen, fprintf, fread, ...) work in
 * any component that depends on vfs.
 *
 * fds 0-2 are not VFS files: calls on them go to whichever handler was
 * installed before (e.g. posix_cap's stdout writer). Handlers return
 * -errno like the Linux kernel; musl's wrappers set errno.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <syscall.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <cos_component.h>
#include <llprint.h>
#include <posix.h>
#include <vfs.h>

/* We pass open flags and fcntl commands through unchanged: make sure that's valid. */
_Static_assert(O_WRONLY == VFS_O_WRONLY && O_RDWR == VFS_O_RDWR && O_CREAT == VFS_O_CREAT, "open flags");
_Static_assert(O_EXCL == VFS_O_EXCL && O_TRUNC == VFS_O_TRUNC && O_APPEND == VFS_O_APPEND, "open flags");
_Static_assert(O_DIRECTORY == VFS_O_DIRECTORY, "open flags");
_Static_assert(SEEK_SET == VFS_SEEK_SET && SEEK_CUR == VFS_SEEK_CUR && SEEK_END == VFS_SEEK_END, "whence");
_Static_assert(F_GETFD == VFS_F_GETFD && F_SETFD == VFS_F_SETFD && F_GETFL == VFS_F_GETFL
               && F_SETFL == VFS_F_SETFL, "fcntl");

/* Handlers for fds 0-2 installed before us */
static cos_syscall_t prev_read, prev_write, prev_writev, prev_ioctl;

static int
is_stdio(long fd)
{
	return fd >= 0 && fd < VFS_FD_BASE;
}

static long
chain(cos_syscall_t fn, long a, long b, long c)
{
	if (!fn) return -EBADF;

	return fn(a, b, c, 0, 0, 0);
}

/* stdout/stderr when no other library (e.g. posix_cap) handles them */
static long
stdout_write(long fd, long buf, long len, long d, long e, long f)
{
	if (fd != 1 && fd != 2) return -EBADF;
	printc("%.*s", (int)len, (const char *)buf);

	return len;
}

static long
stdout_writev(long fd, long iov, long cnt, long d, long e, long f)
{
	const struct iovec *v = (const struct iovec *)iov;
	long                i, tot = 0;

	if (fd != 1 && fd != 2) return -EBADF;
	for (i = 0; i < cnt; i++) tot += stdout_write(fd, (long)v[i].iov_base, v[i].iov_len, 0, 0, 0);

	return tot;
}

static void
stat_conv(const struct vfs_stat *vs, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_dev     = vs->dev;
	st->st_ino     = vs->ino;
	st->st_mode    = vs->mode;
	st->st_nlink   = vs->nlink;
	st->st_size    = vs->size;
	st->st_blksize = vs->blksize;
	st->st_blocks  = vs->blocks;
}

/* Only paths relative to the current directory ("/") or absolute ones are supported for *at() calls. */
static int
at_ok(long dirfd, const char *path)
{
	return dirfd == AT_FDCWD || path[0] == '/';
}

static long
posixfs_open(long path, long flags, long mode)
{
	return vfsc_open((const char *)path, flags & VFS_O_SUPPORTED, mode);
}

static long
posixfs_openat(long dirfd, long path, long flags, long mode)
{
	if (!at_ok(dirfd, (const char *)path)) return -ENOTSUP;

	return posixfs_open(path, flags, mode);
}

static long
posixfs_close(long fd)
{
	if (is_stdio(fd)) return 0;

	return vfsc_close(fd);
}

static long
posixfs_read(long fd, long buf, long len)
{
	if (is_stdio(fd)) return chain(prev_read, fd, buf, len);

	return vfsc_read(fd, (void *)buf, len);
}

static long
posixfs_write(long fd, long buf, long len)
{
	if (is_stdio(fd)) return chain(prev_write, fd, buf, len);

	return vfsc_write(fd, (const void *)buf, len);
}

/* Scatter/gather as a sequence of reads or writes, stopping at the first short one. */
static long
posixfs_rwv(long fd, const struct iovec *iov, long cnt, int wr)
{
	long tot = 0, r = 0, i;

	if (cnt < 0) return -EINVAL;
	for (i = 0; i < cnt; i++) {
		if (!iov[i].iov_len) continue;
		r = wr ? vfsc_write(fd, iov[i].iov_base, iov[i].iov_len) : vfsc_read(fd, iov[i].iov_base, iov[i].iov_len);
		if (r < 0) return tot ? tot : r;
		tot += r;
		if ((size_t)r < iov[i].iov_len) break;
	}

	return tot;
}

static long
posixfs_readv(long fd, long iov, long cnt)
{
	if (is_stdio(fd)) return -EBADF;

	return posixfs_rwv(fd, (const struct iovec *)iov, cnt, 0);
}

static long
posixfs_writev(long fd, long iov, long cnt)
{
	if (is_stdio(fd)) return chain(prev_writev, fd, iov, cnt);

	return posixfs_rwv(fd, (const struct iovec *)iov, cnt, 1);
}

static long
posixfs_pread(long fd, long buf, long len, long off)
{
	if (is_stdio(fd)) return -ESPIPE;

	return vfsc_pread(fd, (void *)buf, len, off);
}

static long
posixfs_pwrite(long fd, long buf, long len, long off)
{
	if (is_stdio(fd)) return -ESPIPE;

	return vfsc_pwrite(fd, (const void *)buf, len, off);
}

static long
posixfs_lseek(long fd, long off, long whence)
{
	if (is_stdio(fd)) return -ESPIPE;

	return vfsc_lseek(fd, off, whence);
}

static long
posixfs_fstat(long fd, long st)
{
	struct vfs_stat vs;
	long            ret;

	if (is_stdio(fd)) {
		/* a character device, so stdio treats it as a terminal-like stream */
		memset((void *)st, 0, sizeof(struct stat));
		((struct stat *)st)->st_mode = S_IFCHR | 0620;
		return 0;
	}
	if ((ret = vfsc_fstat(fd, &vs))) return ret;
	stat_conv(&vs, (struct stat *)st);

	return 0;
}

static long
posixfs_stat(long path, long st)
{
	struct vfs_stat vs;
	long            ret;

	if ((ret = vfsc_stat((const char *)path, &vs))) return ret;
	stat_conv(&vs, (struct stat *)st);

	return 0;
}

static long
posixfs_fstatat(long dirfd, long path, long st, long flag)
{
	if ((flag & AT_EMPTY_PATH) && !((const char *)path)[0]) return posixfs_fstat(dirfd, st);
	if (!at_ok(dirfd, (const char *)path)) return -ENOTSUP;

	/* no symlinks, so AT_SYMLINK_NOFOLLOW makes no difference */
	return posixfs_stat(path, st);
}

static long
posixfs_access(long path, long mode)
{
	struct vfs_stat vs;

	return vfsc_stat((const char *)path, &vs);
}

static long
posixfs_ftruncate(long fd, long len)
{
	if (is_stdio(fd)) return -EINVAL;

	return vfsc_ftruncate(fd, len);
}

static long
posixfs_truncate(long path, long len)
{
	return vfsc_truncate((const char *)path, len);
}

static long
posixfs_unlink(long path)
{
	return vfsc_unlink((const char *)path);
}

static long
posixfs_rmdir(long path)
{
	return vfsc_rmdir((const char *)path);
}

static long
posixfs_unlinkat(long dirfd, long path, long flag)
{
	if (!at_ok(dirfd, (const char *)path)) return -ENOTSUP;

	return (flag & AT_REMOVEDIR) ? posixfs_rmdir(path) : posixfs_unlink(path);
}

static long
posixfs_mkdir(long path, long mode)
{
	return vfsc_mkdir((const char *)path, mode);
}

static long
posixfs_mkdirat(long dirfd, long path, long mode)
{
	if (!at_ok(dirfd, (const char *)path)) return -ENOTSUP;

	return posixfs_mkdir(path, mode);
}

static long
posixfs_rename(long oldpath, long newpath)
{
	return vfsc_rename((const char *)oldpath, (const char *)newpath);
}

static long
posixfs_renameat(long olddirfd, long oldpath, long newdirfd, long newpath)
{
	if (!at_ok(olddirfd, (const char *)oldpath) || !at_ok(newdirfd, (const char *)newpath)) return -ENOTSUP;

	return posixfs_rename(oldpath, newpath);
}

/* struct vfs_dirent has the layout of struct linux_dirent64 (musl's struct dirent) */
static long
posixfs_getdents64(long fd, long buf, long len)
{
	if (is_stdio(fd)) return -ENOTDIR;

	return vfsc_getdents(fd, (void *)buf, len);
}

static long
posixfs_fsync(long fd)
{
	if (is_stdio(fd)) return 0;

	return vfsc_fsync(fd);
}

static long
posixfs_sync(void)
{
	vfsc_sync();

	return 0;
}

static long
posixfs_fcntl(long fd, long cmd, long arg)
{
	if (is_stdio(fd)) {
		if (cmd == F_GETFL) return O_RDWR;
		return 0;
	}
	if (cmd != F_GETFD && cmd != F_SETFD && cmd != F_GETFL && cmd != F_SETFL) return -EINVAL;

	return vfsc_fcntl(fd, cmd, arg);
}

static long
posixfs_ioctl(long fd, long req, long arg)
{
	if (is_stdio(fd)) return prev_ioctl ? chain(prev_ioctl, fd, req, arg) : -ENOTTY;

	return -ENOTTY;
}

#define OVERRIDE(fn, nr) libc_syscall_override((cos_syscall_t)(void *)(fn), (nr))

void
libc_posixfs_initialization_handler(void)
{
	prev_read   = libc_syscall_get(__NR_read);
	prev_write  = libc_syscall_get(__NR_write);
	prev_writev = libc_syscall_get(__NR_writev);
	prev_ioctl  = libc_syscall_get(__NR_ioctl);
	if (!prev_write) prev_write = stdout_write;
	if (!prev_writev) prev_writev = stdout_writev;

	OVERRIDE(posixfs_open, __NR_open);
	OVERRIDE(posixfs_openat, __NR_openat);
	OVERRIDE(posixfs_close, __NR_close);
	OVERRIDE(posixfs_read, __NR_read);
	OVERRIDE(posixfs_readv, __NR_readv);
	OVERRIDE(posixfs_pread, __NR_pread64);
	OVERRIDE(posixfs_write, __NR_write);
	OVERRIDE(posixfs_writev, __NR_writev);
	OVERRIDE(posixfs_pwrite, __NR_pwrite64);
	OVERRIDE(posixfs_lseek, __NR_lseek);
	OVERRIDE(posixfs_fstat, __NR_fstat);
	OVERRIDE(posixfs_stat, __NR_stat);
	OVERRIDE(posixfs_stat, __NR_lstat);
	OVERRIDE(posixfs_fstatat, __NR_newfstatat);
	OVERRIDE(posixfs_access, __NR_access);
	OVERRIDE(posixfs_ftruncate, __NR_ftruncate);
	OVERRIDE(posixfs_truncate, __NR_truncate);
	OVERRIDE(posixfs_unlink, __NR_unlink);
	OVERRIDE(posixfs_unlinkat, __NR_unlinkat);
	OVERRIDE(posixfs_rmdir, __NR_rmdir);
	OVERRIDE(posixfs_mkdir, __NR_mkdir);
	OVERRIDE(posixfs_mkdirat, __NR_mkdirat);
	OVERRIDE(posixfs_rename, __NR_rename);
	OVERRIDE(posixfs_renameat, __NR_renameat);
	OVERRIDE(posixfs_getdents64, __NR_getdents64);
	OVERRIDE(posixfs_fsync, __NR_fsync);
	OVERRIDE(posixfs_fsync, __NR_fdatasync);
	OVERRIDE(posixfs_sync, __NR_sync);
	OVERRIDE(posixfs_fcntl, __NR_fcntl);
	OVERRIDE(posixfs_ioctl, __NR_ioctl);
}
