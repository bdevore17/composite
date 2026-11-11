/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * vfs_core implementation; see vfs_core.h.
 */

#include <string.h>
#include "vfs_core.h"

void
vfs_core_init(struct vfs_core *v)
{
	memset(v, 0, sizeof(*v));
}

void
vfs_core_client_init(struct vfs_client *c)
{
	memset(c, 0, sizeof(*c));
}

/*
 * Paths
 */

/* Lexically normalize in (absolute or relative to "/") into out[VFS_PATH_MAX]. */
static int
path_norm(const char *in, char *out)
{
	size_t len = fs_strnlen(in, VFS_PATH_MAX), o = 0, i = 0;

	if (len == VFS_PATH_MAX) return -ENAMETOOLONG;
	if (len == 0) return -ENOENT;

	while (i < len) {
		size_t s, clen;

		while (i < len && in[i] == '/') i++;
		s = i;
		while (i < len && in[i] != '/') i++;
		clen = i - s;
		if (clen == 0 || (clen == 1 && in[s] == '.')) continue;
		if (clen == 2 && in[s] == '.' && in[s + 1] == '.') {
			/* drop the last component */
			while (o > 0 && out[o - 1] != '/') o--;
			if (o > 0) o--;
			continue;
		}
		if (o + 1 + clen + 1 > VFS_PATH_MAX) return -ENAMETOOLONG;
		out[o++] = '/';
		memcpy(out + o, in + s, clen);
		o += clen;
	}
	if (o == 0) out[o++] = '/';
	out[o] = '\0';

	return 0;
}

/* Find the mount covering the normalized path; *rel is the path inside it (no leading '/'). */
static struct vfs_mount *
mount_find(struct vfs_core *v, const char *path, const char **rel)
{
	struct vfs_mount *best = NULL;
	int               i;

	for (i = 0; i < v->nmounts; i++) {
		struct vfs_mount *m = &v->mounts[i];

		if (m->plen == 1) { /* "/" */
			if (!best) best = m;
			continue;
		}
		if (strncmp(path, m->path, m->plen) || (path[m->plen] != '/' && path[m->plen] != '\0')) continue;
		if (!best || m->plen > best->plen) best = m;
	}
	if (!best) return NULL;
	*rel = path + (best->plen == 1 ? 1 : best->plen);
	if (**rel == '/') (*rel)++;

	return best;
}

/* Copy the next component of *p into name, advancing *p. Returns its length, or -errno. */
static int
next_comp(struct vfs_mount *m, const char **p, char *name)
{
	const char *s = *p, *e = s;

	while (*e && *e != '/') e++;
	if ((int)(e - s) > m->ops->name_max) return -ENAMETOOLONG;
	memcpy(name, s, e - s);
	name[e - s] = '\0';
	*p          = *e ? e + 1 : e;

	return e - s;
}

/* Resolve rel (inside m) to an inode. */
static int
walk(struct vfs_core *v, struct vfs_mount *m, const char *rel, fs_ino_t *ino)
{
	fs_ino_t cur = m->root;
	int      ret;

	while (*rel) {
		if ((ret = next_comp(m, &rel, v->name[0])) < 0) return ret;
		if ((ret = m->ops->lookup(m->fs, cur, v->name[0], &cur))) return ret;
	}
	*ino = cur;

	return 0;
}

/* Resolve all but the last component of rel (non-empty); the last one is copied to name. */
static int
walk_parent(struct vfs_core *v, struct vfs_mount *m, const char *rel, fs_ino_t *dir, char *name)
{
	fs_ino_t cur = m->root;
	int      ret;

	for (;;) {
		if ((ret = next_comp(m, &rel, name)) < 0) return ret;
		if (!*rel) break;
		if ((ret = m->ops->lookup(m->fs, cur, name, &cur))) return ret;
	}
	*dir = cur;

	return 0;
}

/* Resolve path (into scratch path buffer idx) to its mount and the path inside it. */
static int
resolve(struct vfs_core *v, const char *path, int idx, struct vfs_mount **m, const char **rel)
{
	int ret;

	if ((ret = path_norm(path, v->path[idx]))) return ret;
	*m = mount_find(v, v->path[idx], rel);
	if (!*m) return -ENOENT;

	return 0;
}

/*
 * Mounts
 */

int
vfs_core_mount(struct vfs_core *v, const char *path, const struct fs_ops *ops, void *fs)
{
	struct vfs_mount *m, *parent;
	const char       *rel;
	fs_ino_t          dir, ino;
	int               ret;

	if (v->nmounts == VFS_MAX_MOUNTS) return -ENOSPC;
	if ((ret = path_norm(path, v->path[0]))) return ret;
	if (v->nmounts == 0 && strcmp(v->path[0], "/")) return -EINVAL;

	if (v->nmounts > 0) {
		/* make sure the mount point exists as a directory in the parent */
		parent = mount_find(v, v->path[0], &rel);
		if (!*rel) return -EBUSY; /* already a mount point */
		if ((ret = walk_parent(v, parent, rel, &dir, v->name[1]))) return ret;
		ret = parent->ops->lookup(parent->fs, dir, v->name[1], &ino);
		if (ret == -ENOENT) ret = parent->ops->create(parent->fs, dir, v->name[1], FS_S_IFDIR | 0755, &ino);
		if (ret) return ret;
	}

	m = &v->mounts[v->nmounts];
	strcpy(m->path, v->path[0]);
	m->plen = strlen(m->path);
	m->ops  = ops;
	m->fs   = fs;
	if ((ret = ops->root(fs, &m->root))) return ret;
	v->nmounts++;

	return 0;
}

/*
 * Open files and fds
 */

static int
file_is_open(struct vfs_core *v, struct vfs_mount *m, fs_ino_t ino)
{
	int i;

	for (i = 0; i < VFS_MAX_FILES; i++) {
		if (v->files[i].refcnt && v->files[i].m == m && v->files[i].ino == ino) return 1;
	}

	return 0;
}

/* Free the inode if it has no links and is not open. */
static void
maybe_evict(struct vfs_core *v, struct vfs_mount *m, fs_ino_t ino)
{
	if (!ino || file_is_open(v, m, ino)) return;
	/* evict returns -EBUSY (and does nothing) if the inode is still linked */
	m->ops->evict(m->fs, ino);
}

static struct vfs_file *
fd_get(struct vfs_client *c, int fd)
{
	if (fd < VFS_FD_BASE || fd >= VFS_FD_BASE + VFS_MAX_FDS) return NULL;

	return c->fds[fd - VFS_FD_BASE];
}

static int
fd_free_slot(struct vfs_client *c)
{
	int i;

	for (i = 0; i < VFS_MAX_FDS; i++) {
		if (!c->fds[i]) return i;
	}

	return -EMFILE;
}

static struct vfs_file *
file_free_slot(struct vfs_core *v)
{
	int i;

	for (i = 0; i < VFS_MAX_FILES; i++) {
		if (!v->files[i].refcnt) return &v->files[i];
	}

	return NULL;
}

static void
stat_conv(struct vfs_core *v, struct vfs_mount *m, const struct fs_stat *fst, struct vfs_stat *st)
{
	st->ino     = fst->ino;
	st->mode    = fst->mode;
	st->nlink   = fst->nlink;
	st->size    = fst->size;
	st->blocks  = fst->blocks;
	st->blksize = fst->blksize;
	st->dev     = m - v->mounts;
}

int
vfs_core_open(struct vfs_core *v, struct vfs_client *c, const char *path, uint32_t flags, uint32_t mode)
{
	struct vfs_mount *m;
	struct vfs_file  *f;
	struct fs_stat    st;
	const char       *rel;
	fs_ino_t          dir, ino;
	uint32_t          acc = flags & VFS_O_ACCMODE;
	int               fd, ret;

	if (acc == VFS_O_ACCMODE) return -EINVAL;
	if ((fd = fd_free_slot(c)) < 0) return fd;
	if (!(f = file_free_slot(v))) return -ENFILE;
	if ((ret = resolve(v, path, 0, &m, &rel))) return ret;

	if (!*rel) {
		/* the root of a mount */
		if ((flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)) return -EEXIST;
		ino = m->root;
	} else {
		if ((ret = walk_parent(v, m, rel, &dir, v->name[1]))) return ret;
		ret = m->ops->lookup(m->fs, dir, v->name[1], &ino);
		if (ret == -ENOENT && (flags & VFS_O_CREAT)) {
			if (flags & VFS_O_DIRECTORY) return -EINVAL;
			ret = m->ops->create(m->fs, dir, v->name[1], FS_S_IFREG | (mode & FS_S_IPERM), &ino);
		} else if (ret == 0 && (flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)) {
			return -EEXIST;
		}
		if (ret) return ret;
	}
	if ((ret = m->ops->getattr(m->fs, ino, &st))) return ret;
	if (FS_ISDIR(st.mode)) {
		if (acc != VFS_O_RDONLY || (flags & VFS_O_TRUNC)) return -EISDIR;
	} else if (flags & VFS_O_DIRECTORY) {
		return -ENOTDIR;
	}
	if ((flags & VFS_O_TRUNC) && acc != VFS_O_RDONLY && st.size) {
		if ((ret = m->ops->truncate(m->fs, ino, 0))) return ret;
	}

	*f            = (struct vfs_file){.m = m, .ino = ino, .off = 0, .refcnt = 1};
	f->flags      = flags & (VFS_O_ACCMODE | VFS_O_APPEND);
	f->isdir      = FS_ISDIR(st.mode);
	c->fds[fd]    = f;

	return fd + VFS_FD_BASE;
}

int
vfs_core_close(struct vfs_core *v, struct vfs_client *c, int fd)
{
	struct vfs_file *f = fd_get(c, fd);

	if (!f) return -EBADF;
	c->fds[fd - VFS_FD_BASE] = NULL;
	if (--f->refcnt == 0) maybe_evict(v, f->m, f->ino);

	return 0;
}

void
vfs_core_client_teardown(struct vfs_core *v, struct vfs_client *c)
{
	int i;

	for (i = 0; i < VFS_MAX_FDS; i++) {
		if (c->fds[i]) vfs_core_close(v, c, i + VFS_FD_BASE);
	}
}

static long
do_read(struct vfs_client *c, int fd, void *buf, size_t len, int64_t off, int use_off)
{
	struct vfs_file *f = fd_get(c, fd);
	long             r;

	if (!f || (f->flags & VFS_O_ACCMODE) == VFS_O_WRONLY) return -EBADF;
	if (f->isdir) return -EISDIR;
	if (use_off && off < 0) return -EINVAL;
	r = f->m->ops->read(f->m->fs, f->ino, use_off ? (uint64_t)off : f->off, buf, len);
	if (r > 0 && !use_off) f->off += r;

	return r;
}

static long
do_write(struct vfs_client *c, int fd, const void *buf, size_t len, int64_t off, int use_off)
{
	struct vfs_file *f = fd_get(c, fd);
	struct fs_stat   st;
	uint64_t         pos;
	long             r;
	int              ret;

	if (!f || (f->flags & VFS_O_ACCMODE) == VFS_O_RDONLY) return -EBADF;
	if (f->isdir) return -EISDIR;
	if (use_off && off < 0) return -EINVAL;
	if (len == 0) return 0;
	pos = use_off ? (uint64_t)off : f->off;
	if (!use_off && (f->flags & VFS_O_APPEND)) {
		if ((ret = f->m->ops->getattr(f->m->fs, f->ino, &st))) return ret;
		pos = st.size;
	}
	r = f->m->ops->write(f->m->fs, f->ino, pos, buf, len);
	if (r > 0 && !use_off) f->off = pos + r;

	return r;
}

long
vfs_core_read(struct vfs_core *v, struct vfs_client *c, int fd, void *buf, size_t len)
{
	return do_read(c, fd, buf, len, 0, 0);
}

long
vfs_core_write(struct vfs_core *v, struct vfs_client *c, int fd, const void *buf, size_t len)
{
	return do_write(c, fd, buf, len, 0, 0);
}

long
vfs_core_pread(struct vfs_core *v, struct vfs_client *c, int fd, void *buf, size_t len, int64_t off)
{
	return do_read(c, fd, buf, len, off, 1);
}

long
vfs_core_pwrite(struct vfs_core *v, struct vfs_client *c, int fd, const void *buf, size_t len, int64_t off)
{
	return do_write(c, fd, buf, len, off, 1);
}

long
vfs_core_lseek(struct vfs_core *v, struct vfs_client *c, int fd, int64_t off, int whence)
{
	struct vfs_file *f = fd_get(c, fd);
	struct fs_stat   st;
	int64_t          base;
	int              ret;

	if (!f) return -EBADF;
	if (f->isdir) {
		/* directory offsets are opaque cookies: only absolute positioning */
		if (whence != VFS_SEEK_SET || off < 0) return -EINVAL;
		f->off = off;
		return off;
	}
	switch (whence) {
	case VFS_SEEK_SET:
		base = 0;
		break;
	case VFS_SEEK_CUR:
		base = f->off;
		break;
	case VFS_SEEK_END:
		if ((ret = f->m->ops->getattr(f->m->fs, f->ino, &st))) return ret;
		base = st.size;
		break;
	default:
		return -EINVAL;
	}
	if (base + off < 0) return -EINVAL;
	f->off = base + off;

	return f->off;
}

int
vfs_core_fstat(struct vfs_core *v, struct vfs_client *c, int fd, struct vfs_stat *st)
{
	struct vfs_file *f = fd_get(c, fd);
	struct fs_stat   fst;
	int              ret;

	if (!f) return -EBADF;
	if ((ret = f->m->ops->getattr(f->m->fs, f->ino, &fst))) return ret;
	stat_conv(v, f->m, &fst, st);

	return 0;
}

int
vfs_core_ftruncate(struct vfs_core *v, struct vfs_client *c, int fd, int64_t len)
{
	struct vfs_file *f = fd_get(c, fd);

	if (!f) return -EBADF;
	if (f->isdir) return -EISDIR;
	if ((f->flags & VFS_O_ACCMODE) == VFS_O_RDONLY || len < 0) return -EINVAL;

	return f->m->ops->truncate(f->m->fs, f->ino, len);
}

int
vfs_core_fsync(struct vfs_core *v, struct vfs_client *c, int fd)
{
	struct vfs_file *f = fd_get(c, fd);

	if (!f) return -EBADF;

	return f->m->ops->sync(f->m->fs);
}

int
vfs_core_fcntl(struct vfs_core *v, struct vfs_client *c, int fd, int cmd, long arg)
{
	struct vfs_file *f = fd_get(c, fd);

	if (!f) return -EBADF;
	switch (cmd) {
	case VFS_F_GETFD:
		return 0;
	case VFS_F_SETFD:
		return 0; /* no exec, so close-on-exec is meaningless */
	case VFS_F_GETFL:
		return f->flags;
	case VFS_F_SETFL:
		/* only O_APPEND can be changed */
		f->flags = (f->flags & ~VFS_O_APPEND) | (arg & VFS_O_APPEND);
		return 0;
	default:
		return -EINVAL;
	}
}

long
vfs_core_getdents(struct vfs_core *v, struct vfs_client *c, int fd, void *buf, size_t len)
{
	struct vfs_file  *f = fd_get(c, fd);
	struct fs_dirent *de = &v->de;
	size_t            used = 0;
	int               ret;

	if (!f) return -EBADF;
	if (!f->isdir) return -ENOTDIR;

	for (;;) {
		struct vfs_dirent *out;
		size_t             nlen, reclen;

		ret = f->m->ops->readdir(f->m->fs, f->ino, f->off, de);
		if (ret < 0) return used ? (long)used : ret;
		if (ret == 0) break;
		nlen   = strlen(de->name);
		reclen = (sizeof(struct vfs_dirent) + nlen + 1 + 7) & ~(size_t)7;
		if (used + reclen > len) {
			if (!used) return -EINVAL;
			break;
		}
		out         = (struct vfs_dirent *)((char *)buf + used);
		out->ino    = de->ino;
		out->off    = de->next;
		out->reclen = reclen;
		out->type   = de->type;
		memcpy(out->name, de->name, nlen + 1);
		used  += reclen;
		f->off = de->next;
	}

	return used;
}

/*
 * Path-based operations
 */

int
vfs_core_stat(struct vfs_core *v, const char *path, struct vfs_stat *st)
{
	struct vfs_mount *m;
	struct fs_stat    fst;
	const char       *rel;
	fs_ino_t          ino;
	int               ret;

	if ((ret = resolve(v, path, 0, &m, &rel))) return ret;
	if ((ret = walk(v, m, rel, &ino))) return ret;
	if ((ret = m->ops->getattr(m->fs, ino, &fst))) return ret;
	stat_conv(v, m, &fst, st);

	return 0;
}

int
vfs_core_truncate(struct vfs_core *v, const char *path, int64_t len)
{
	struct vfs_mount *m;
	const char       *rel;
	fs_ino_t          ino;
	int               ret;

	if (len < 0) return -EINVAL;
	if ((ret = resolve(v, path, 0, &m, &rel))) return ret;
	if ((ret = walk(v, m, rel, &ino))) return ret;

	return m->ops->truncate(m->fs, ino, len);
}

static int
do_unlink(struct vfs_core *v, const char *path, int is_dir)
{
	struct vfs_mount *m;
	const char       *rel;
	fs_ino_t          dir, victim = 0;
	int               ret;

	if ((ret = resolve(v, path, 0, &m, &rel))) return ret;
	if (!*rel) return -EBUSY; /* root of a mount */
	if ((ret = walk_parent(v, m, rel, &dir, v->name[1]))) return ret;
	if ((ret = m->ops->unlink(m->fs, dir, v->name[1], is_dir, &victim))) return ret;
	maybe_evict(v, m, victim);

	return 0;
}

int
vfs_core_unlink(struct vfs_core *v, const char *path)
{
	return do_unlink(v, path, 0);
}

int
vfs_core_rmdir(struct vfs_core *v, const char *path)
{
	return do_unlink(v, path, 1);
}

int
vfs_core_mkdir(struct vfs_core *v, const char *path, uint32_t mode)
{
	struct vfs_mount *m;
	const char       *rel;
	fs_ino_t          dir, ino;
	int               ret;

	if ((ret = resolve(v, path, 0, &m, &rel))) return ret;
	if (!*rel) return -EEXIST;
	if ((ret = walk_parent(v, m, rel, &dir, v->name[1]))) return ret;

	return m->ops->create(m->fs, dir, v->name[1], FS_S_IFDIR | (mode & FS_S_IPERM), &ino);
}

int
vfs_core_rename(struct vfs_core *v, const char *oldpath, const char *newpath)
{
	struct vfs_mount *om, *nm;
	const char       *orel, *nrel;
	fs_ino_t          odir, ndir, replaced = 0;
	size_t            olen;
	int               ret;

	if ((ret = resolve(v, oldpath, 0, &om, &orel))) return ret;
	if ((ret = resolve(v, newpath, 1, &nm, &nrel))) return ret;
	if (om != nm) return -EXDEV;
	if (!*orel || !*nrel) return -EBUSY;
	/* a directory can't be moved below itself */
	olen = strlen(v->path[0]);
	if (!strncmp(v->path[1], v->path[0], olen) && v->path[1][olen] == '/') return -EINVAL;

	if ((ret = walk_parent(v, om, orel, &odir, v->name[0]))) return ret;
	if ((ret = walk_parent(v, nm, nrel, &ndir, v->name[1]))) return ret;
	if ((ret = om->ops->rename(om->fs, odir, v->name[0], ndir, v->name[1], &replaced))) return ret;
	maybe_evict(v, om, replaced);

	return 0;
}

int
vfs_core_sync(struct vfs_core *v)
{
	int i, ret, err = 0;

	for (i = 0; i < v->nmounts; i++) {
		ret = v->mounts[i].ops->sync(v->mounts[i].fs);
		if (ret && !err) err = ret;
	}

	return err;
}

int
vfs_core_nfiles_open(struct vfs_core *v)
{
	int i, n = 0;

	for (i = 0; i < VFS_MAX_FILES; i++) {
		if (v->files[i].refcnt) n++;
	}

	return n;
}
