/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Server side of the vfs interface; see vfs_server.h.
 *
 * Protection: the caller is identified by cos_inv_token() (its
 * component id), which the kernel provides and clients cannot forge.
 * Each client has its own fd table and shared buffer; all offsets and
 * lengths into the buffer are bounded by the buffer's size, and paths
 * are copied out of it (and NUL-checked) before use, so a client
 * scribbling on its own buffer concurrently can only hurt itself.
 *
 * Concurrency: one lock around every request. File system operations
 * are short (RAM) or bounded by a single block device request at a
 * time, so this keeps the code simple without much loss.
 */

#include <string.h>
#include <cos_component.h>
#include <memmgr.h>
#include <vfs.h>
#include "vfs_server.h"

struct vfs_srv_client {
	struct vfs_client fds;
	char             *shm;
	size_t            shmsz;
};

static struct vfs_core       vfs_core;
static struct vfs_srv_client vfs_clients[MAX_NUM_COMPS];
static struct ps_lock        vfs_lock;
static char                  vfs_path[2][VFS_PATH_MAX];

/*
 * Page allocator for lib/fs. memmgr has no free, so freed pages are
 * kept on a free list. Called with vfs_lock held (or during init).
 */
struct free_page {
	struct free_page *next;
};
static struct free_page *free_pages;

void *
fs_page_alloc(void)
{
	void *p;

	if (free_pages) {
		p          = free_pages;
		free_pages = free_pages->next;
	} else {
		p = (void *)memmgr_heap_page_alloc();
		if (!p) return NULL;
	}
	memset(p, 0, PAGE_SIZE);

	return p;
}

void
fs_page_free(void *p)
{
	struct free_page *f = p;

	f->next    = free_pages;
	free_pages = f;
}

void
vfs_server_lock(void)
{
	ps_lock_take(&vfs_lock);
}

void
vfs_server_unlock(void)
{
	ps_lock_release(&vfs_lock);
}

struct vfs_core *
vfs_server_init(void)
{
	int i;

	vfs_core_init(&vfs_core);
	for (i = 0; i < MAX_NUM_COMPS; i++) vfs_core_client_init(&vfs_clients[i].fds);

	return &vfs_core;
}

static struct vfs_srv_client *
caller(void)
{
	compid_t id = (compid_t)cos_inv_token();

	if (id >= MAX_NUM_COMPS) return NULL;

	return &vfs_clients[id];
}

/*
 * Request prologue: lock, and find the caller. If need_shm, the caller
 * must have a shared buffer. Returns NULL (with *err set, lock not
 * held) on failure.
 */
static struct vfs_srv_client *
enter(int need_shm, long *err)
{
	struct vfs_srv_client *c = caller();

	if (!c) {
		*err = -EINVAL;
		return NULL;
	}
	vfs_server_lock();
	if (need_shm && !c->shm) {
		vfs_server_unlock();
		*err = -ENOBUFS;
		return NULL;
	}

	return c;
}

#define EXIT(ret)                     \
	do {                          \
		vfs_server_unlock();  \
		return (ret);         \
	} while (0)

/* Copy the NUL-terminated path at shm+off into vfs_path[idx]. */
static int
copy_path(struct vfs_srv_client *c, size_t off, int idx)
{
	size_t len = fs_strnlen(c->shm + off, VFS_PATH_MAX);

	if (len == VFS_PATH_MAX) return -ENAMETOOLONG;
	memcpy(vfs_path[idx], c->shm + off, len + 1);

	return 0;
}

static size_t
clamp(struct vfs_srv_client *c, unsigned long len)
{
	return len > c->shmsz ? c->shmsz : len;
}

long
vfs_shm_attach(cbuf_t shm, unsigned long npages)
{
	struct vfs_srv_client *c;
	vaddr_t                addr = 0;
	unsigned long          n;
	long                   ret;

	if (!(c = enter(0, &ret))) return ret;
	if (c->shm) EXIT(-EEXIST);
	n = memmgr_shared_page_map(shm, &addr);
	if (!n || !addr) EXIT(-EINVAL);
	/* we need room for two paths (rename) and some data */
	if (n * PAGE_SIZE < 2 * VFS_PATH_MAX) EXIT(-EINVAL);
	c->shm   = (char *)addr;
	c->shmsz = n * PAGE_SIZE;
	EXIT(0);
}

long
vfs_open(unsigned long flags, unsigned long mode)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	if ((ret = copy_path(c, 0, 0))) EXIT(ret);
	EXIT(vfs_core_open(&vfs_core, &c->fds, vfs_path[0], flags, mode));
}

long
vfs_close(long fd)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(0, &ret))) return ret;
	EXIT(vfs_core_close(&vfs_core, &c->fds, fd));
}

long
vfs_read(long fd, unsigned long len)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	EXIT(vfs_core_read(&vfs_core, &c->fds, fd, c->shm, clamp(c, len)));
}

long
vfs_write(long fd, unsigned long len)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	EXIT(vfs_core_write(&vfs_core, &c->fds, fd, c->shm, clamp(c, len)));
}

long
vfs_pread(long fd, unsigned long len, long off)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	EXIT(vfs_core_pread(&vfs_core, &c->fds, fd, c->shm, clamp(c, len), off));
}

long
vfs_pwrite(long fd, unsigned long len, long off)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	EXIT(vfs_core_pwrite(&vfs_core, &c->fds, fd, c->shm, clamp(c, len), off));
}

long
vfs_lseek(long fd, long off, long whence)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(0, &ret))) return ret;
	EXIT(vfs_core_lseek(&vfs_core, &c->fds, fd, off, whence));
}

long
vfs_fstat(long fd)
{
	struct vfs_srv_client *c;
	struct vfs_stat        st;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	ret = vfs_core_fstat(&vfs_core, &c->fds, fd, &st);
	if (!ret) memcpy(c->shm, &st, sizeof(st));
	EXIT(ret);
}

long
vfs_stat(void)
{
	struct vfs_srv_client *c;
	struct vfs_stat        st;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	if ((ret = copy_path(c, 0, 0))) EXIT(ret);
	ret = vfs_core_stat(&vfs_core, vfs_path[0], &st);
	if (!ret) memcpy(c->shm, &st, sizeof(st));
	EXIT(ret);
}

long
vfs_ftruncate(long fd, long len)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(0, &ret))) return ret;
	EXIT(vfs_core_ftruncate(&vfs_core, &c->fds, fd, len));
}

long
vfs_truncate(long len)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	if ((ret = copy_path(c, 0, 0))) EXIT(ret);
	EXIT(vfs_core_truncate(&vfs_core, vfs_path[0], len));
}

long
vfs_unlink(void)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	if ((ret = copy_path(c, 0, 0))) EXIT(ret);
	EXIT(vfs_core_unlink(&vfs_core, vfs_path[0]));
}

long
vfs_rmdir(void)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	if ((ret = copy_path(c, 0, 0))) EXIT(ret);
	EXIT(vfs_core_rmdir(&vfs_core, vfs_path[0]));
}

long
vfs_mkdir(unsigned long mode)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	if ((ret = copy_path(c, 0, 0))) EXIT(ret);
	EXIT(vfs_core_mkdir(&vfs_core, vfs_path[0], mode));
}

long
vfs_rename(void)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	if ((ret = copy_path(c, 0, 0)) || (ret = copy_path(c, VFS_PATH_MAX, 1))) EXIT(ret);
	EXIT(vfs_core_rename(&vfs_core, vfs_path[0], vfs_path[1]));
}

long
vfs_getdents(long fd, unsigned long len)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(1, &ret))) return ret;
	EXIT(vfs_core_getdents(&vfs_core, &c->fds, fd, c->shm, clamp(c, len)));
}

long
vfs_fsync(long fd)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(0, &ret))) return ret;
	EXIT(vfs_core_fsync(&vfs_core, &c->fds, fd));
}

long
vfs_fcntl(long fd, long cmd, long arg)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(0, &ret))) return ret;
	EXIT(vfs_core_fcntl(&vfs_core, &c->fds, fd, cmd, arg));
}

long
vfs_sync(void)
{
	struct vfs_srv_client *c;
	long                   ret;

	if (!(c = enter(0, &ret))) return ret;
	EXIT(vfs_core_sync(&vfs_core));
}
