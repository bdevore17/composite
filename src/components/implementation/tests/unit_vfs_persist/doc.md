## unit_vfs_persist

Persistence of the on-disk file system (cosfs on `blkdev.virtio`, via
`vfs.diskfs`) across reboots and between Composite and the host,
driven by `tools/cosfs/persist_test.sh`:

1. the host formats an image and stores `/from_host.txt` in it;
2. boot 1: the test checks that file, writes a tree under
   `/disk/persist` (a 5 MiB file, a text file written with stdio, 20
   small files, a rename and an unlink);
3. the host runs `cosfs fsck` and checks every file's contents;
4. boot 2: the test verifies all of it survived the reboot, appends to,
   truncates and deletes files;
5. the host checks the image again.

    ./cos compose composition_scripts/unit_vfs_persist.toml vfs_persist
    tools/cosfs/persist_test.sh vfs_persist
