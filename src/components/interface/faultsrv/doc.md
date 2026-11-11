## faultsrv

A test interface for component recovery (see the capmgr's documentation).

### Description

The component implementing this interface faults on demand (`faultsrv_fault`, and every `FAULTSRV_PERIOD`-th `faultsrv_call`), has state that its reset must clear (the call counter, a heap allocation made by its `cos_init`), and can make concurrent invocations last long (`faultsrv_spin`).

### Usage and Assumptions

Only for tests (see `tests.unit_recovery`). All functions can return `-ECOMPRESET` when the server faulted, or is being reset.
