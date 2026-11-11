# Host-side unit tests for parts of the VMM that can be checked without VT-x.
# Not part of the Composite build (the component Makefile only compiles the
# top-level *.c of vmm/ and the CFILES it lists). Run on the build host:
#
#     make -C src/components/implementation/simple_vmm/vmm/host_test
#
# The code under test is compiled unmodified against stub headers (stub/),
# under ASan/UBSan. Each test prints PASS/FAIL lines and TEST PASSED/FAILED.
#
#   virtio_blk_test: drives devices/vpci/virtio_blk.c (+ the RAM disk) the way
#                    Linux's legacy virtio-pci and virtio_blk drivers do.
#   vlapic_test:     ICR emulation in vlapic.c: INIT/SIPI, IPI destinations and
#                    shorthands, vIOAPIC message delivery.
VMM    := ..
CFLAGS := -O1 -g -Wall -Wno-unused -Wno-sign-compare -fsanitize=address,undefined -Istub/common
TESTS  := virtio_blk_test vlapic_test

all: run

virtio_blk_test: virtio_blk_test.c $(VMM)/devices/vpci/virtio_blk.c $(VMM)/devices/vblk/vblk_ramdisk.c
	$(CC) $(CFLAGS) -Istub/vblk -I$(VMM)/devices/vpci -o $@ $^

# vlapic.c relies on printc being declared by other headers in the real build
vlapic_test: vlapic_test.c $(VMM)/vlapic.c
	$(CC) $(CFLAGS) -w -Istub/vlapic -I$(VMM) -include stub/common/llprint.h -o $@ $^

run: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t | grep -E '^(PASS|FAIL|TEST)' || exit 1; done

clean:
	rm -f $(TESTS)

.PHONY: all run clean
