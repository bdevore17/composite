# Server side of the vfs interface: per-client state, shared-memory
# argument passing and locking around lib/fs's vfs_core. Used by the
# implementations in implementation/vfs/*, which only set up mounts.
LIBRARY_OUTPUT = vfs_server
OBJECT_OUTPUT =
INCLUDE_PATHS = .
INTERFACE_DEPENDENCIES = vfs memmgr
LIBRARY_DEPENDENCIES = component fs

include Makefile.lib
