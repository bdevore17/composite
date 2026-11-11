## vfs_server

The server half of the `vfs` interface, shared by the VFS components
(`implementation/vfs/*`).

### Description

Implements every `vfs_*` invocation function on top of `lib/fs`'s
`vfs_core`: maps each client's shared buffer (`memmgr_shared_page_map`),
keeps a `struct vfs_client` (fd table) per component id, copies and
NUL-checks paths, clamps lengths to the buffer size, and takes one lock
around each request. It also provides the page allocator `lib/fs`
expects (`fs_page_alloc`/`fs_page_free`), backed by `memmgr` heap pages
plus a free list (memmgr cannot free pages).

### Usage and Assumptions

A VFS component calls `vfs_server_init()` from `cos_init` and mounts
its file systems on the returned `struct vfs_core` with
`vfs_core_mount()`; see `implementation/vfs/ramfs`. Use
`vfs_server_lock()` around any later direct use of the core.
