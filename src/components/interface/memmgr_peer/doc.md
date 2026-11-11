## memmgr_peer

A test interface for shared-memory reclamation across components.

### Description

The component implementing this interface maps shared memory allocated by its client (`memmgr_peer_map`), checks and modifies its contents, and unmaps it (`memmgr_peer_unmap`).
`memmgr_peer_map_released` checks that a released shared memory id can no longer be mapped.

### Usage and Assumptions

Only for tests (see `tests.unit_memmgr_free`). The peer tracks a single mapping per shared memory id, and a bounded number of ids.
