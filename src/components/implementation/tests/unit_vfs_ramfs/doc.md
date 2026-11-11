## unit_vfs_ramfs

Tests the VFS (`vfs.ramfs`) through the `vfsc_*` client library:
directory trees and error cases, create/write/read/seek, holes,
truncate, O_APPEND/O_TRUNC/access modes, unlink (including of an open
file), rename, 200 files in a directory with getdents, a 5 MiB file
(larger than the shared buffer), and per-client fd isolation.

`composition_scripts/unit_vfs_ramfs.toml` runs two instances; each
works in `/t<compid>` and prints `SUCCESS`/`FAILURE` per case and a
final `[<compid>] TEST PASSED` / `TEST FAILED`. Both must pass:
`QEMU_TEST_PASS_COUNT=2 tools/qemu_test.sh composition_scripts/unit_vfs_ramfs.toml`.
`vfs_test.h` is the small harness shared with the other vfs tests.
