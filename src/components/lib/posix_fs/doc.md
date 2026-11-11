## posix_fs

POSIX file API for components: routes musl's file system calls to the
`vfs` interface, so ordinary C code (`open`/`read`/`write`/`lseek`/
`stat`/`unlink`/`mkdir`/`rename`/`opendir`/`readdir`, and stdio's
`fopen`/`fprintf`/`fgets`/`fread`/`fseek`/...) works in a component.

### Description

On initialization (`libc_posixfs_initialization_handler`, called by
`lib/component` after the other posix libraries) it installs handlers
for: `open`, `openat`, `close`, `read`, `readv`, `pread64`, `write`,
`writev`, `pwrite64`, `lseek`, `fstat`, `stat`, `lstat`, `newfstatat`,
`access`, `ftruncate`, `truncate`, `unlink`, `unlinkat`, `rmdir`,
`mkdir`, `mkdirat`, `rename`, `renameat`, `getdents64`, `fsync`,
`fdatasync`, `sync`, `fcntl` (`F_GETFD/SETFD/GETFL/SETFL`) and `ioctl`.
Each maps onto the `vfsc_*` client library.

fds 0-2 are not VFS files: `read`/`write`/`writev`/`ioctl` on them chain
to the handler installed before (e.g. `posix_cap`'s stdout writer);
without one, stdout/stderr go to `printc`.

### Usage and Assumptions

- Add `posix_fs` to `LIBRARY_DEPENDENCIES` (next to `posix`, and
  usually `posix_cap`/`posix_sched` for `malloc` and threads) and `vfs
  memmgr` to `INTERFACE_DEPENDENCIES`; compose with a `vfs` server.
- `*at()` calls only accept `AT_FDCWD` or absolute paths (`-ENOTSUP`
  otherwise). There is no `chdir`: the working directory is `/`.
- No `dup`/`dup2`, `mmap` of files, file locks, permissions, symlinks or
  timestamps (`st_*tim` are 0).
- Test: `composition_scripts/unit_vfs_posix.toml`.
