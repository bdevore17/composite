/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * vfs client library: owns the shared buffer used to pass paths and
 * data to the VFS server, and serializes its use.
 */

#include <errno.h>
#include <string.h>
#include <cos_component.h>
#include <memmgr.h>
#include <vfs.h>

static struct ps_lock vfsc_lock;
static char          *vfsc_shm;
static int            vfsc_ready;

static int
vfsc_init_locked(void)
{
	vaddr_t addr = 0;
	cbuf_t  id;
	long    ret;

	if (vfsc_ready) return 0;
	id = memmgr_shared_page_allocn(VFS_SHM_PAGES, &addr);
	if (!id || !addr) return -ENOMEM;
	ret = vfs_shm_attach(id, VFS_SHM_PAGES);
	if (ret < 0) return ret;
	vfsc_shm   = (char *)addr;
	vfsc_ready = 1;

	return 0;
}

int
vfsc_init(void)
{
	int ret;

	ps_lock_take(&vfsc_lock);
	ret = vfsc_init_locked();
	ps_lock_release(&vfsc_lock);

	return ret;
}

/* Take the lock, making sure the buffer exists. Returns -errno (lock not held) on failure. */
static int
vfsc_enter(void)
{
	int ret;

	ps_lock_take(&vfsc_lock);
	if ((ret = vfsc_init_locked())) ps_lock_release(&vfsc_lock);

	return ret;
}

static void
vfsc_exit(void)
{
	ps_lock_release(&vfsc_lock);
}

/* Copy a path into the buffer at off. */
static int
vfsc_path(const char *path, size_t off)
{
	size_t len = strlen(path);

	if (len >= VFS_PATH_MAX) return -ENAMETOOLONG;
	memcpy(vfsc_shm + off, path, len + 1);

	return 0;
}

int
vfsc_open(const char *path, int flags, int mode)
{
	long ret;

	if ((ret = vfsc_enter())) return ret;
	if (!(ret = vfsc_path(path, 0))) ret = vfs_open(flags, mode);
	vfsc_exit();

	return ret;
}

int
vfsc_close(int fd)
{
	return vfs_close(fd);
}

/*
 * Reads and writes are split into buffer-sized chunks. A short chunk
 * ends the transfer (like a short read/write), and an error after some
 * progress returns the progress.
 */
static long
vfsc_rw(int fd, void *rbuf, const void *wbuf, size_t len, long off, int use_off)
{
	size_t done = 0;
	long   ret  = 0;

	if ((ret = vfsc_enter())) return ret;
	while (done < len) {
		size_t chunk = len - done > VFS_SHM_SIZE ? VFS_SHM_SIZE : len - done;

		if (wbuf) {
			memcpy(vfsc_shm, (const char *)wbuf + done, chunk);
			ret = use_off ? vfs_pwrite(fd, chunk, off + done) : vfs_write(fd, chunk);
		} else {
			ret = use_off ? vfs_pread(fd, chunk, off + done) : vfs_read(fd, chunk);
			if (ret > 0) memcpy((char *)rbuf + done, vfsc_shm, ret);
		}
		if (ret <= 0) break;
		done += ret;
		if ((size_t)ret < chunk) break;
	}
	vfsc_exit();

	return done ? (long)done : ret;
}

long
vfsc_read(int fd, void *buf, size_t len)
{
	return vfsc_rw(fd, buf, NULL, len, 0, 0);
}

long
vfsc_write(int fd, const void *buf, size_t len)
{
	return vfsc_rw(fd, NULL, buf, len, 0, 0);
}

long
vfsc_pread(int fd, void *buf, size_t len, long off)
{
	return vfsc_rw(fd, buf, NULL, len, off, 1);
}

long
vfsc_pwrite(int fd, const void *buf, size_t len, long off)
{
	return vfsc_rw(fd, NULL, buf, len, off, 1);
}

long
vfsc_lseek(int fd, long off, int whence)
{
	return vfs_lseek(fd, off, whence);
}

int
vfsc_fstat(int fd, struct vfs_stat *st)
{
	long ret;

	if ((ret = vfsc_enter())) return ret;
	ret = vfs_fstat(fd);
	if (!ret) memcpy(st, vfsc_shm, sizeof(*st));
	vfsc_exit();

	return ret;
}

int
vfsc_stat(const char *path, struct vfs_stat *st)
{
	long ret;

	if ((ret = vfsc_enter())) return ret;
	if (!(ret = vfsc_path(path, 0))) ret = vfs_stat();
	if (!ret) memcpy(st, vfsc_shm, sizeof(*st));
	vfsc_exit();

	return ret;
}

int
vfsc_ftruncate(int fd, long len)
{
	return vfs_ftruncate(fd, len);
}

/* Path-only operations */
static int
vfsc_pathop(const char *path, long (*fn)(void))
{
	long ret;

	if ((ret = vfsc_enter())) return ret;
	if (!(ret = vfsc_path(path, 0))) ret = fn();
	vfsc_exit();

	return ret;
}

int
vfsc_truncate(const char *path, long len)
{
	long ret;

	if ((ret = vfsc_enter())) return ret;
	if (!(ret = vfsc_path(path, 0))) ret = vfs_truncate(len);
	vfsc_exit();

	return ret;
}

int
vfsc_unlink(const char *path)
{
	return vfsc_pathop(path, vfs_unlink);
}

int
vfsc_rmdir(const char *path)
{
	return vfsc_pathop(path, vfs_rmdir);
}

int
vfsc_mkdir(const char *path, int mode)
{
	long ret;

	if ((ret = vfsc_enter())) return ret;
	if (!(ret = vfsc_path(path, 0))) ret = vfs_mkdir(mode);
	vfsc_exit();

	return ret;
}

int
vfsc_rename(const char *oldpath, const char *newpath)
{
	long ret;

	if ((ret = vfsc_enter())) return ret;
	if (!(ret = vfsc_path(oldpath, 0)) && !(ret = vfsc_path(newpath, VFS_PATH_MAX))) ret = vfs_rename();
	vfsc_exit();

	return ret;
}

long
vfsc_getdents(int fd, void *buf, size_t len)
{
	long ret;

	if (len > VFS_SHM_SIZE) len = VFS_SHM_SIZE;
	if ((ret = vfsc_enter())) return ret;
	ret = vfs_getdents(fd, len);
	if (ret > 0) memcpy(buf, vfsc_shm, ret);
	vfsc_exit();

	return ret;
}

int
vfsc_fsync(int fd)
{
	return vfs_fsync(fd);
}

int
vfsc_fcntl(int fd, int cmd, long arg)
{
	return vfs_fcntl(fd, cmd, arg);
}

int
vfsc_sync(void)
{
	return vfs_sync();
}
