# tools/cosfs

Host-side tools for the Composite file system code in
`src/components/lib/fs` (VFS core, ramfs, cosfs). Plain C, no Composite
toolchain needed: `make -C tools/cosfs`.

- `cosfs`: create and inspect cosfs disk images, using the same file
  system code as the `vfs.diskfs` component:
  `cosfs mkfs IMG [SIZE_MB]`, `fsck IMG [-v]`, `ls IMG [PATH]`,
  `tree IMG`, `cat IMG PATH`, `get IMG PATH HOSTFILE`,
  `put IMG HOSTFILE PATH`, `mkdir|rm|rmdir IMG PATH`,
  `verify IMG PATH SIZE SEED`, `verify-raw IMG OFFSET SIZE SEED`.
  `fsck` is an independent consistency checker (it only uses the
  on-disk format definitions).
- `test_fs` (`make -C tools/cosfs test`): host unit tests of the VFS
  core, ramfs and cosfs (same scenarios as the in-Composite tests, plus
  a randomized differential test of ramfs against cosfs, leak checks,
  ENOSPC handling and fsck after every phase).
- `run_qemu_test.sh BUILD LOG [-n N] [-t SECS] [-d DISK]`: boot a
  composed system, wait for N `TEST PASSED/FAILED` lines, stop QEMU.
- `persist_test.sh [BUILD]`: the two-boot persistence test for
  `unit_vfs_persist` with host-side checks of the disk image.

The test data pattern (`cosfs_pattern` in `host_env.h`) is the same
function as `vt_pattern` in the in-Composite tests.
