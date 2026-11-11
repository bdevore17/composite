## no_interface.llbooter

A *constructor* implementation that is as simple as possible, and pushes most complexity to shared (and better tested) libraries such as `kernel` and `crt`.

### Description

Creates a set of components as provided by the `composer`.
Takes a tarball of these components (accessed through `initargs`), and an `initargs` specification of those components to be booted.
This will FIFO schedule initialization through the components that depend on it for `init`.

First draft of checkpointing, such that a component can create a checkpoint of itself once initialized (in `init_done`). Then a new component can be created and executed from the checkpoint. 

### Usage and Assumptions

- Strongly assumes that the `composer` is used to provide all necessary metadata to construct the rest of the system.
- We currently define the `addr` interface that is a hack to provide frontier (heap pointer) information.
	This should be replaced with additional `composer` information being passed by `initargs` eventually.

### Components started at runtime

Components marked `boot = "deferred"` (listed in the `deferred` initargs) are created like all others (resource tables, memory image, synchronous invocations), but their clients' invocations to them are redirected (`crt_sinv_retarget`) to `addr_comp_absent`, a booter function that returns `-ENOENT`.
The capmgr, on behalf of the component's scheduler (see the `loader` interface), loads them with `addr_comp_load`, which resets their memory image from the elf object (`crt_comp_image_reset`), rewrites the ucaps of their own invocations (that are in their image), and points their clients' invocations back to them; `addr_comp_unload` redirects the invocations again.
The booter keeps the images (the tarball) and the `crt_sinv` records of all invocations for this.
The booter must implement the `addr` interface in systems with deferred components (the composer checks this).
