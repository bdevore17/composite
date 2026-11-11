# POSIX file API (open/read/write/stat/readdir/... and thus stdio) for
# components, implemented with the vfs interface. Linked as an object
# so its initialization hook always overrides the weak default.
LIBRARY_OUTPUT =
OBJECT_OUTPUT = posix_fs
INCLUDE_PATHS = .
INTERFACE_DEPENDENCIES = vfs memmgr
LIBRARY_DEPENDENCIES = component posix

include Makefile.lib
