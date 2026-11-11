/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * All vfs functions take at most four word arguments and return a
 * single word, so the default stubs suffice.
 */

#include <cos_asm_stubs.h>

cos_asm_stub(vfs_shm_attach)
cos_asm_stub(vfs_open)
cos_asm_stub(vfs_close)
cos_asm_stub(vfs_read)
cos_asm_stub(vfs_write)
cos_asm_stub(vfs_pread)
cos_asm_stub(vfs_pwrite)
cos_asm_stub(vfs_lseek)
cos_asm_stub(vfs_fstat)
cos_asm_stub(vfs_stat)
cos_asm_stub(vfs_ftruncate)
cos_asm_stub(vfs_truncate)
cos_asm_stub(vfs_unlink)
cos_asm_stub(vfs_rmdir)
cos_asm_stub(vfs_mkdir)
cos_asm_stub(vfs_rename)
cos_asm_stub(vfs_getdents)
cos_asm_stub(vfs_fsync)
cos_asm_stub(vfs_fcntl)
cos_asm_stub(vfs_sync)
