# Required variables used to drive the compilation process. It is OK
# for many of these to be empty.
#
# The library names associated with .a files output that are linked
# (via, for example, -lmemmgr_peer) into dependents.
LIBRARY_OUTPUT =
# The .o files that are mandatorily linked into dependents (rare).
OBJECT_OUTPUT =
# The path within this directory that holds the .h files for
# dependents to compile with (./ by default).
INCLUDE_PATHS = .
# The interfaces this component is dependent on for compilation (this
# is a list of directory names in interface/)
INTERFACE_DEPENDENCIES =
# The library dependencies this component is reliant on for
# compilation/linking (this is a list of directory names in lib/)
LIBRARY_DEPENDENCIES = stubs
# Note: Both the interface and library dependencies should be
# *minimal*. That is to say that removing a dependency should cause
# the build to fail. The build system does not validate this
# minimality; that's on you!

include Makefile.subdir
