## vfs.diskfs

The VFS server (implements `vfs`) with a RAM file system at `/` and the
on-disk file system cosfs at `/disk`, stored on a `blkdev` (for example
`blkdev.virtio`).

### Description

At initialization it reads the device size, registers a small shared
buffer with the block device, and mounts cosfs at `/disk`:

- if the first 4 KiB block of the device is all zeros, it formats the
  device first (`cosfs_mkfs`);
- if the device holds something that is not a valid cosfs, `/disk` is
  not mounted (nothing is overwritten) and only the ramfs root is
  served;
- without a device, only the ramfs root is served.

cosfs writes through to the device (every VFS call that modifies the
disk has reached the device when it returns); `fsync`/`sync` also
issue a device cache flush. Images can be created, inspected and
checked on the host with `tools/cosfs` (`cosfs mkfs|ls|tree|cat|put|
get|fsck|verify`).

### Usage and Assumptions

- Dependencies: `blkdev`, `memmgr`, `init`.
- One block request is issued per 4 KiB block (cosfs' block cache holds
  32 blocks); throughput is bounded by the polling virtio driver.
- See `lib/fs/doc.md` for cosfs' format and limits (names up to 58
  bytes, files up to ~4 GiB, no journal).
- Tests: `composition_scripts/unit_vfs_posix_disk.toml`,
  `composition_scripts/unit_vfs_persist.toml` with
  `tools/cosfs/persist_test.sh`.
