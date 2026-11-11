/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * vfs.ramfs: the VFS server with an in-memory file system at "/".
 * Needs no devices. Contents are lost at reboot.
 */

#include <cos_component.h>
#include <llprint.h>
#include <vfs_server.h>
#include <ramfs.h>

void
cos_init(void)
{
	struct vfs_core *v = vfs_server_init();
	struct ramfs    *fs;

	fs = ramfs_create(0);
	if (!fs || vfs_core_mount(v, "/", &ramfs_ops, fs)) {
		printc("vfs.ramfs: cannot create the root file system\n");
		BUG();
	}
	printc("vfs.ramfs: ramfs mounted at /\n");
}

/*
 * A passive server has no main. Defining a (trivial) parallel init
 * tells the init protocol that, so the initialization threads exit
 * after it instead of looking for a main to run.
 */
void
cos_parallel_init(coreid_t cid, int init_core, int ncores)
{
}
