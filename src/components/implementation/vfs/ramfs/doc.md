## vfs.ramfs

The VFS server (implements `vfs`) with a RAM file system mounted at
`/`. It needs no devices; contents are lost when the system stops.

### Description

All of the VFS logic lives in `lib/vfs_server` (request handling,
per-client shared buffers and fd tables, locking) and `lib/fs` (the VFS
core and ramfs); this component only mounts the file system. File data
and metadata are allocated from `memmgr` heap pages on demand (freed
pages are reused), so the file system grows until memory runs out.

### Usage and Assumptions

- Dependencies: `init` and `memmgr` (heap pages, mapping client
  buffers). Clients depend on it via the `vfs` interface and also need
  `memmgr` (their library allocates the shared buffer).
- See `interface/vfs/doc.md` for the API, and `lib/fs/doc.md` for the
  semantics and limitations.
- Test: `composition_scripts/unit_vfs_ramfs.toml`.
