## blkdev

A block device interface: capacity, read, write and flush of 512-byte
sectors over synchronous invocations.

### Description

Data never goes through registers: each client registers one
shared-memory buffer (`blkdev_shm_attach`, a `memmgr` cbuf; the helper
`blkdev_client_shm(npages)` allocates and registers it), and
`blkdev_read`/`blkdev_write` take a sector number, a sector count and a
byte offset into that buffer. A DMA-capable driver (such as
`blkdev.virtio`) transfers directly between the device and the buffer.

All functions return `>= 0` on success and `-errno` on failure
(`-EINVAL` for out-of-range sectors or buffer offsets, `-EIO` for
device errors, `-EROFS` for writes to read-only devices, `-ENOBUFS` if
no buffer is registered).

### Usage and Assumptions

- Clients depend on `blkdev` and `memmgr`.
- There is no access control between clients: every client can read and
  write the whole device. Compose it only with trusted clients (e.g. the
  VFS).
- Implementation: `blkdev.virtio` (virtio-blk on PCI).
