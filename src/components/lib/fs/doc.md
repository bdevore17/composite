## fs

File system building blocks used by the `vfs` components: the VFS core,
an in-memory file system (ramfs) and a small on-disk file system
(cosfs). Plain C with no Composite dependencies, so the same code is
built on the host by `tools/cosfs` (image tool and host unit tests).

### Description

- `fs.h`: the backend interface (`struct fs_ops`: lookup, create,
  unlink, rename, getattr, read, write, truncate, readdir, evict, sync),
  inode handles (`fs_ino_t`), and a small object allocator (`fs_zalloc`,
  `fs_free`) on top of the page allocator the embedding environment
  provides (`fs_page_alloc`, `fs_page_free`; zeroed 4 KiB pages).
- `vfs_core.[ch]`: mount table (longest-prefix match), lexical path
  normalization, open file objects (offset, flags, reference count) and
  per-client fd tables. It implements POSIX-like semantics: access
  modes, `O_CREAT/O_EXCL/O_TRUNC/O_APPEND/O_DIRECTORY`, unlink of open
  files (the inode is evicted on last close), rename over existing
  entries, `EXDEV` across mounts, `EBUSY` on mount points.
- `vfs_abi.h`: the client/server data formats (flags, `struct
  vfs_stat`, `struct vfs_dirent`), included by the `vfs` interface.
- `ramfs.[ch]`: hierarchical, growable RAM file system. Files are 4 KiB
  pages in a direct/indirect/double-indirect radix (max ~1 GiB, sparse
  files supported), directories are entry lists. Names up to 255 bytes.
- `cosfs.[ch]`, `cosfs_format.h`: on-disk file system over an abstract
  block device (`struct fs_blkdev`, 4 KiB blocks). Superblock, block
  bitmap, inode table (128-byte inodes with 12 direct, 1 indirect and 1
  double-indirect pointer, max ~4 GiB per file), directories of 64-byte
  entries (names up to 58 bytes). Write-through LRU block cache; no
  journal, but writes are ordered so that a crash can only leak blocks
  or inodes. `cosfs_mkfs` formats a device.

### Usage and Assumptions

- Nothing here is thread-safe; callers serialize (the VFS server takes
  one lock per request).
- No timestamps (there is no clock source in the library), no
  ownership/permission checks, no symlinks/hard links, no `.`/`..`
  entries in `readdir`.
- readdir offsets are entry indices: entries removed concurrently with
  a directory scan can make the scan skip an entry.
- Directories never shrink on disk (freed entries are reused).
- Host tests: `make -C tools/cosfs test`.
