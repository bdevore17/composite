# VM Development Guide

### The guest Linux image

The guest is an **unmodified upstream Linux kernel**. It is built by the
[cos-vmimg](https://github.com/esmakokten/cos-vmimg) submodule at
`src/components/implementation/simple_vmm/vmimg`, which downloads a stock kernel
tarball, verifies its checksum, and builds it with a BusyBox initramfs linked in.

## How to build the system

### Build the guest Linux VM

```shell
git submodule update --init --recursive
./cos build
```

That is the whole procedure. The guest image is built during `./cos compose` and
installed into `guest/` as `vmlinux-<recipe>-<kver>.img`; see "Choosing which
guest to boot" below.

### Choosing which guest to boot

The composition script names the image, so switching guests is a `./cos compose`
rather than a rebuild:

```toml
[[components]]
name = "vmm"
img  = "simple_vmm.vmm"
constants = [{variable = "VM_GUEST_IMAGE",
              value = "\"guest/vmlinux-vmexit-bench-5.15.107.img\""}]
```

The escaped quotes matter: the composer substitutes the value verbatim into a
`#define`, so without them it is not a string literal and the component will not
compile. If the named image is not in `guest/` yet, it is built on demand — and
only it, not the other recipes.

The available images are the recipes in `vmimg/recipes/`, each a small TOML
naming the programs, kernel modules and init script to include. Adding one means
adding a recipe and a program; see `vmimg/README.md`.

`./cos build` on its own does not build a guest kernel — there is no composition
at that point, so it embeds a placeholder. `./cos compose` recompiles the
component against the image its script names, building that image if needed. So
a Composite build for a system with no VM in it costs nothing extra.

A `vmlinux-<recipe>-<kver>.manifest` is installed beside each image recording the
kernel version, the config and initramfs hashes, and the program list, so an
image in a build tree can always be identified.

**Kernel version.** It comes from whichever cos-vmimg commit the submodule is
pinned to; `.gitmodules` records the tag. cos-vmimg's `main` tracks the current
kernel and older lines are frozen as tags, so moving the guest forward is a
deliberate submodule bump rather than something that changes underneath this
tree.

**Testing the guest without Composite.** The same image boots under plain QEMU,
which is much faster to iterate on:

```shell
cd src/components/implementation/simple_vmm/vmimg
make run RECIPE=shell
```

### Build the Composite

- **Where is the hypervisor code**?

	There are three components to support vmx in Composite:

	1. `src/platform/x86_64/vmx`: this is the kernel support for vmx.
	2. `src/components/lib/vmrt`: this is the vm lib for user level manipulating vm operation in Composite.
	3. `src/components/implementation/simple_vmm`: this is a simple hypervisor implementation based on the vmrt.

- **Where is the VM (guest) image and how does Composite hypervisor load it**?

	The guest image consists of two parts: the guest bootloader and the guest Linux. Thus the hypervisor needs to load both of them and let the guest bootloader to find guest Linux and load it.

	The guest Linux image is built by the `vmimg` submodule during
	`./cos compose` and installed into
	`src/components/implementation/simple_vmm/vmm/guest/` as
	`vmlinux-<recipe>-<kver>.img`. It is a build output and is not tracked in
	git.

	The guest bootloader is here: `src/components/implementation/simple_vmm/vmm/guest/guest_realmode.S`. It will then be compiled to this binary file: `src/components/implementation/simple_vmm/vmm/guest/guest.img`.

	Now we have both the `guest.img` (the guest bootloader) and the `vmlinux-<recipe>-<kver>.img` (the stock Linux kernel image).

	The two guest images will then be included into the simple vmm component. 

	The hypervisor will then read these two images and load them into VM's virtual physical address page by page.

- **How to build the Composite hypervisor?**
	```shell
	git submodule update --init --recursive
	./cos init x86_64
	./cos build
	./cos compose composition_scripts/vmm_simple_test.toml vm
	```
	Note that `./cos build` exits 0 even when a component fails to compile, so
	check its output for `error:` rather than relying on the exit status.
- **How to run the system?**
	To run the system on Qemu:
	```shell
	./cos run vm
	```

## Debugging

### Debugging the guest Linux in Composite

You can use the kernel's `printk` function to print information you want to know what is happening in the VM, for example:
```c
	printk(KERN_INFO "%s at %u in ()\n", __FILE__, __LINE__);
	pr_info("%s at %u in (%s)\n", __FILE__, __LINE__, __func__);
```

You can also use `vmcall` inside the linux kernel to force it out the VM:
```c
	asm volatile("vmcall");
```
This will cause VM exit and then you can do some hacking.

### Debugging the guest Linux in Qemu
Since it is in the Qemu, you can use either the print functions within the Linux kernel or GDB support by Qemu to hack the kernel.
## Multi-vCPU guests and virtio-blk

Three composition constants on the `vmm` component control them:

| Constant | Default | Meaning |
|---|---|---|
| `VM_NUM_VCPUS` | `1` | vCPUs; vCPU *i* runs on core *i*, so at most `NUM_CPU`. vCPU 0 boots, the guest brings up the others with INIT/SIPI. |
| `VM_BLK_DISK_MB` | `16` | Size of the RAM disk behind the virtio-blk device (`/dev/vda` in the guest); `0` removes the device. |
| `VMRT_LOG_LEVEL` | `1` | VMM log verbosity: 0 errors, 1 events (vCPU start, INIT/SIPI, device setup), 2 every IPI and block request. |

The guest finds its CPUs in the MADT that `vmimg`'s `vmxbooter` writes; it
lists as many CPUs as the VMM reports in CPUID leaf 0BH (core level). Guest
images built before that change always list one CPU. The `smp-blk` recipe
(`vmimg/recipes/smp-blk.toml`) runs a self-test at boot.

**The guest-side changes live in the cos-vmimg submodule** and are carried
here as `vmm/vmimg-patches/0001-*.patch` until they are merged into
cos-vmimg and the submodule pointer is bumped. Apply them with
```shell
git -C src/components/implementation/simple_vmm/vmimg am \
    ../vmm/vmimg-patches/0001-*.patch
```
(they enable `CONFIG_VIRTIO_BLK`/`CONFIG_EXT2_FS` in the shared defconfig,
make `vmxbooter` list the VMM's CPU count, and add the `smp-blk` recipe).
Without them the VMM still runs, but the guest sees one CPU and no
virtio-blk driver.

The VMM-side logic that can be checked without VT-x has host unit tests:
`make -C src/components/implementation/simple_vmm/vmm/host_test` (virtio-blk
request handling and the vlapic's IPI/INIT/SIPI emulation, under ASan/UBSan).

`composition_scripts/vmm_smp_blk_test.toml` composes a 4-vCPU guest with a
32MB disk running that recipe. Kernel-side VMX logging is controlled by
`VMX_LOG_GLOBAL_LEVEL` in `src/platform/x86_64/vmx/include/vmx_logging.h`
(set it to `VMX_LOG_DEBUG` to see every VM exit, `VMX_LOG_INFO` to see the
EPT capabilities and each AP's start address).

### Testing SMP and virtio-blk (needs VT-x)

None of this can run under QEMU TCG, which does not emulate VMX. Use a Linux
host with VT-x and nested virtualization enabled
(`cat /sys/module/kvm_intel/parameters/nested` prints `Y`), or boot the ISO on
bare metal.

1. Build and compose (`NUM_CPU` is 8 in the default configuration):
   ```shell
   ./cos build
   ./cos compose composition_scripts/vmm_smp_blk_test.toml smpblk
   ```
   The first compose builds the `smp-blk` guest image (a full kernel build).
   Check the output for `error:` (`./cos build` exits 0 on component errors).
2. Run, with the same number of CPUs as `NUM_CPU`:
   ```shell
   qemu-system-x86_64 -enable-kvm -cpu host -smp 8 -m 8192 \
       -cdrom system_binaries/cos_build-smpblk/cos.iso \
       -no-reboot -nographic -nic none | tee smpblk.log
   ```
   (`./cos run smpblk` does the same, but with `-smp 16`; use it only if
   `NUM_CPU` is 16.)
3. Expected VMM output before the guest starts:
   ```
   VMM: virtio-blk: 65536 sectors (32MB, cos-vmm-ramdisk) at I/O 0x8000, IRQ 10
   [vmm] vCPU 0 created on core 0 ...
   [vmm] vCPU 1 created on core 1 ...      (... up to vCPU 3)
   VMM: Starting vCPU 0 (BSP) on core 0 ..., 4 vCPU(s) total
   VMM: vCPU 1 (AP) on core 1 waiting for INIT/SIPI      (likewise 2, 3)
   ```
4. Expected during guest boot, once per AP:
   ```
   [vmm] vCPU 1: INIT, waiting for SIPI
   [vmm] vCPU 1: SIPI vector 0x9X, starting at 0x9X000
   [vmm] vCPU 1: starting on core 1 at 9X00:0000
   ```
   (the vector is wherever Linux put its real-mode trampoline, normally just
   below 0x9e000), then in the guest log:
   ```
   smp: Bringing up secondary CPUs ...
   x86: Booting SMP configuration:
   .... node  #0, CPUs:      #1 #2 #3
   smp: Brought up 1 node, 4 CPUs
   ```
   and for the disk:
   ```
   virtio_blk virtio1: [vda] 65536 512-byte logical blocks (33.6 MB/32.0 MiB)
   ```
   (`virtio1` because virtio-net is `virtio0`). A warning like
   `pci 0000:00:02.0: can't find IRQ for PCI INT A` or `PCI INT A: no GSI` is
   expected: there is no interrupt routing table, so Linux uses the IRQ in the
   device's interrupt-line register (10).
5. The self-test prints `SMPBLK PASS:` / `SMPBLK FAIL:` lines and ends with
   ```
   SMPBLK DONE: 0 failure(s)
   ```
   then gives a shell. In it, check by hand:
   ```shell
   nproc                                   # 4
   grep -c ^processor /proc/cpuinfo        # 4
   grep -E 'apicid|core id' /proc/cpuinfo  # apicid 0..3, initial apicid = apicid
   cat /proc/interrupts                    # LOC counts rising on every CPU;
                                           # RES/CAL/TLB non-zero on several CPUs;
                                           # an IO-APIC 10-edge line for virtio1
   taskset -c 3 sh -c 'echo on cpu 3'      # if taskset is in the BusyBox build
   dd if=/dev/zero of=/dev/vda bs=1M count=8 conv=fsync
   mkfs.ext2 /dev/vda && mount /dev/vda /mnt && echo hi > /mnt/f && sync && umount /mnt
   mount /dev/vda /mnt && cat /mnt/f       # hi
   dd if=/dev/vda of=/dev/null bs=1M       # 32+0 records in
   ```
6. Regression, one vCPU: compose `composition_scripts/vmm_simple_test.toml`
   (one vCPU, the `shell` image, a 16MB disk) and check the guest reaches the
   shell exactly as before, and that `/dev/vda` exists if the image was
   rebuilt with the new `vmxbooter_defconfig` (`CONFIG_VIRTIO_BLK=y`).
   `composition_scripts/vmm_simple_ping_test.toml` (one vCPU, virtio-net
   through the DPDK NIC manager) must still ping.
7. Variations worth running: `VM_NUM_VCPUS` = 2 and 8 (all cores; vCPU 0
   then shares core 0 with the virtio-net threads in the ping composition),
   `VMRT_LOG_LEVEL` = 2 for one boot (very verbose: every IPI), and a guest
   stress test such as running the self-test loop with `dd` on `/dev/vda` while
   all CPUs are busy.

What failures would look like:
- Guest stops after `smp: Bringing up secondary CPUs` and reports
  `CPU1 failed to report alive state` / `do_boot_cpu failed`: the AP did not
  start. Look for the VMM's `SIPI vector` / `starting on core` lines and the
  kernel's `vCPU thd ... starts at CS` line (`VMX_LOG_INFO`); a VM-entry
  failure shows up as `VMLAUNCH FAILED` (`VMX_LOG_DEBUG`).
- Guest hangs later with all CPUs up: IPIs are not getting through (watch
  `RES`/`CAL` in `/proc/interrupts`, VMM log level 2).
- `virtio_blk` probe errors or I/O errors: VMM log level 2 shows each request
  and its status.
