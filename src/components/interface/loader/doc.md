## loader

Start and stop components at runtime.

### Description

Components marked `boot = "deferred"` in the composition script are created in the system image like all others (including the synchronous invocations to and from them) but are not initialized at boot.
Their scheduler implements this interface to start them (`loader_comp_start`), which runs their initialization and `main`, and to stop them (`loader_comp_stop`), which deletes their threads and releases their memory so that they can be started again.
`loader_comp_id` finds a deferred component's id from its name in the composition script.

### Usage and Assumptions

- A component can start and stop the deferred components of a scheduler if it depends on that scheduler's `loader` interface: the dependency is the authorization.
- While a deferred component is not running, its clients' invocations of its functions return `-ENOENT`.
- See `loader.h` for the conditions under which a component can be stopped, and sched.pfprr_quantum_static's documentation for the design and its limitations.
