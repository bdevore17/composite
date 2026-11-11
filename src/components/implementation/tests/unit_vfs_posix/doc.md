## unit_vfs_posix

Tests the POSIX/stdio file API (`posix_fs`) end to end: `fopen`/
`fprintf`/`fgets`/`fscanf`/`fseek` on a text file, append and truncate
modes, a 1 MiB binary file with `fwrite`/`fread` and random access,
`open`/`read`/`write`/`pread`/`pwrite`/`lseek`/`ftruncate`/`fstat`/
`fsync`, errno values, `access`, `truncate`, `mkdir`/`opendir`/
`readdir`/`rewinddir` over 100 files, `rename`/`remove`/`rmdir`.

It works in `/disk/posix_test` when `/disk` exists (`vfs.diskfs`) and in
`/posix_test` otherwise, so it runs in both
`composition_scripts/unit_vfs_posix.toml` (ramfs) and
`composition_scripts/unit_vfs_posix_disk.toml` (cosfs on virtio-blk).
Prints `SUCCESS`/`FAILURE` per case and `TEST PASSED`/`TEST FAILED`.
