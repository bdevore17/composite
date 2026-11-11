/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * Server side of the vfs interface. The library implements every vfs_*
 * invocation function; a VFS component only has to initialize it and
 * mount file systems from its cos_init:
 *
 *     void cos_init(void) {
 *             struct vfs_core *v = vfs_server_init();
 *             vfs_core_mount(v, "/", &ramfs_ops, ramfs_create(0));
 *     }
 *
 * It also provides the page allocator lib/fs needs (fs_page_alloc /
 * fs_page_free), backed by memmgr heap pages and a free list.
 */

#ifndef VFS_SERVER_H
#define VFS_SERVER_H

#include <vfs_core.h>

/* Initialize the server state; returns the core to mount file systems on. */
struct vfs_core *vfs_server_init(void);

/* Serialize with the request path (e.g. for mount-time work after init). */
void vfs_server_lock(void);
void vfs_server_unlock(void);

#endif /* VFS_SERVER_H */
