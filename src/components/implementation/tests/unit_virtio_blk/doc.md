## unit_virtio_blk

Raw sector I/O through `blkdev.virtio`: capacity, single- and
multi-sector round trips, a ~256 KiB transfer at a non-aligned buffer
offset (many DMA segments, split into several requests), overlapping
writes, bounds checking (last sector, past the end, zero sectors,
buffer overflow), and flush.

Run with a scratch image of at least 16 MiB:

    truncate -s 64M blk.img
    QEMU_TEST_EXTRA="-drive file=$PWD/blk.img,if=virtio,format=raw" \
        tools/qemu_test.sh composition_scripts/unit_virtio_blk.toml
    tools/cosfs/cosfs verify-raw blk.img $((2048*512)) $((256*512)) 11

The last command checks, from the host, the marker the test leaves in
sectors 2048-2303.
