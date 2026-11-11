## blkdev.virtio

A virtio-blk driver implementing the `blkdev` interface, for the legacy
(transitional) virtio PCI interface that QEMU's `virtio-blk-pci`
provides by default, e.g. `-drive file=disk.img,if=virtio,format=raw`.

### Description

- Finds the device with `lib/pci` (vendor `0x1af4`, device `0x1001`),
  enables I/O decoding and bus mastering, and drives it through its
  I/O-port BAR (components run with IOPL 3).
- Negotiates only `VIRTIO_BLK_F_RO`, `VIRTIO_BLK_F_FLUSH` and
  `VIRTIO_BLK_F_SEG_MAX`; sets up queue 0 in physically contiguous
  memory from `contigmem`.
- Zero copy: each client's shared buffer is mapped once and its pages
  translated to physical addresses (`memmgr_virt_to_phys`); requests
  are descriptor chains of the request header, one descriptor per
  page-sized piece of the client's buffer, and the status byte. Large
  transfers are split to stay within the device's segment limit (at
  most 64 data segments per request).
- One request in flight at a time (a lock serializes clients);
  completion is detected by polling the used ring, with device
  interrupts suppressed (`VRING_AVAIL_F_NO_INTERRUPT`).

### Usage and Assumptions

- Dependencies: `init`, `memmgr`, `contigmem` (so compose it with
  `capmgr` providing `contigmem`).
- Only the first virtio-blk device is used. Modern-only devices
  (`0x1042`, e.g. `disable-legacy=on` or a PCIe machine) are detected
  and reported but not supported.
- Polling only: interrupt-driven completion (and multiple outstanding
  requests) would need IRQ-to-thread delivery, which no Composite
  driver uses yet.
- Client buffers are limited to 4 MiB.
- Test: `composition_scripts/unit_virtio_blk.toml` with a scratch disk
  image of at least 16 MiB.
