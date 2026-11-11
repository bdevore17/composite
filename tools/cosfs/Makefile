# Host-side tools for cosfs and host tests of the file system library
# (src/components/lib/fs). No Composite toolchain needed:
#
#   make            build ./cosfs and ./test_fs
#   make test       run the host unit tests
#
# See README.md (or `./cosfs help`).

FSLIB  = ../../src/components/lib/fs
CC    ?= gcc
CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -I$(FSLIB)
LIBSRC = $(FSLIB)/fs_alloc.c $(FSLIB)/ramfs.c $(FSLIB)/cosfs.c $(FSLIB)/vfs_core.c host_env.c
HDRS   = $(wildcard $(FSLIB)/*.h) host_env.h

all: cosfs test_fs

cosfs: cosfs_tool.c fsck.c $(LIBSRC) $(HDRS)
	$(CC) $(CFLAGS) -o $@ cosfs_tool.c fsck.c $(LIBSRC)

test_fs: test_fs.c fsck.c $(LIBSRC) $(HDRS)
	$(CC) $(CFLAGS) -o $@ test_fs.c fsck.c $(LIBSRC)

test: test_fs
	./test_fs

clean:
	rm -f cosfs test_fs

.PHONY: all test clean
