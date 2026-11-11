/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * ramfs: a hierarchical, growable in-memory file system. Files are
 * stored in 4 KiB pages indexed by a direct/indirect/double-indirect
 * radix (max file size ~1 GiB), directories are lists of entries.
 */

#ifndef RAMFS_H
#define RAMFS_H

#include "fs.h"

struct ramfs;

extern const struct fs_ops ramfs_ops;

/*
 * Create an empty file system (just the root directory). max_pages
 * bounds the number of data pages files may use (0 = unbounded, i.e.
 * until the page allocator fails). Returns NULL on allocation failure.
 */
struct ramfs *ramfs_create(uint64_t max_pages);

/* Number of data pages currently used by file contents. */
uint64_t ramfs_pages_used(struct ramfs *fs);

#endif /* RAMFS_H */
