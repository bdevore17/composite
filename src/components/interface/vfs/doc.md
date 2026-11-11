## vfs

A file system service: open/close/read/write/pread/pwrite/lseek,
fstat/stat, ftruncate/truncate, unlink/rmdir/mkdir/rename, getdents,
fsync/fcntl and a global sync, over synchronous invocations.

### Description

Each client registers one shared-memory buffer with the server
(`vfs_shm_attach`, a `memmgr` cbuf). Paths, file data, `struct
vfs_stat` and `struct vfs_dirent` records are passed through that
buffer; registers only carry fds, flags, lengths and offsets. The
formats and flag values are in `lib/fs/vfs_abi.h` (the flag values are
the Linux ones).

Two layers are exported:

- `vfs_*`: the raw invocation functions (see `vfs.h` for where each one
  expects/puts its data in the buffer). All return `>= 0` on success
  and `-errno` on failure.
- `vfsc_*` (`lib.c`): the client library. It allocates the
  `VFS_SHM_PAGES` (64 KiB) buffer from `memmgr` on first use, copies
  arguments/results, and splits larger reads and writes into
  buffer-sized chunks. A lock serializes the client's threads on the
  buffer.

POSIX programs should not call either directly: link the `posix_fs`
library and use `open`/`read`/`fopen`/`fprintf`/`readdir`/... instead.

Protection: the server identifies the client by its component id
(`cos_inv_token`), keeps an fd table per client (fds start at
`VFS_FD_BASE` = 3 for every client), and bounds every buffer access by
the size the client registered. Paths are copied out of the buffer
before use.

### Usage and Assumptions

- A client component depends on `vfs` and on `memmgr` (for the buffer).
- Implementations: `vfs.ramfs` (RAM file system at `/`) and
  `vfs.diskfs` (RAM file system at `/`, cosfs on a block device at
  `/disk`).
- Any component that knows a cbuf id can map it through `memmgr`
  (a `memmgr` limitation), so the buffer protects against other
  clients only as far as cbuf ids are not leaked.
- No permissions/ownership: every client can access every file.
- There is no per-client current directory: relative paths are
  relative to `/`.
