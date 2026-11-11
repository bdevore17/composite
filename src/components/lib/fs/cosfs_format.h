/*
 * Redistribution of this file is permitted under the BSD two clause license.
 *
 * On-disk format of cosfs, a deliberately small block file system.
 * All integers are little-endian (we only target x86). Layout, in
 * 4 KiB blocks:
 *
 *   block 0                      superblock
 *   [bmap_start, +bmap_blocks)   block allocation bitmap, 1 bit per block
 *                                (bit set = in use; covers every block,
 *                                metadata included)
 *   [itab_start, +itab_blocks)   inode table, COSFS_INODES_PER_BLOCK per block
 *   [data_start, nblocks)        data blocks (file contents, directory
 *                                contents, indirect blocks)
 *
 * Inode 0 is never used; inode 1 is the root directory. An inode is
 * free iff its mode is 0. Files map their blocks through 12 direct
 * pointers, one single-indirect and one double-indirect block of
 * 32-bit block numbers (block number 0 = hole). Directories are files
 * containing an array of 64-byte entries; an entry with ino 0 is free.
 * There is no journal: cosfs writes through to the device, metadata
 * last-ordered as noted in cosfs.c, and `cosfs fsck` checks
 * consistency.
 */

#ifndef COSFS_FORMAT_H
#define COSFS_FORMAT_H

#include <stdint.h>

#define COSFS_MAGIC            0x31534653534f43ULL /* "COSSFS1" */
#define COSFS_VERSION          1
#define COSFS_BLOCK_SIZE       4096
#define COSFS_SECTORS_PER_BLOCK (COSFS_BLOCK_SIZE / 512)
#define COSFS_ROOT_INO         1
#define COSFS_NDIRECT          12
#define COSFS_PTRS_PER_BLOCK   (COSFS_BLOCK_SIZE / sizeof(uint32_t))
#define COSFS_INODE_SIZE       128
#define COSFS_INODES_PER_BLOCK (COSFS_BLOCK_SIZE / COSFS_INODE_SIZE)
#define COSFS_DIRENT_SIZE      64
#define COSFS_DIRENTS_PER_BLOCK (COSFS_BLOCK_SIZE / COSFS_DIRENT_SIZE)
#define COSFS_NAME_MAX         58
#define COSFS_BITS_PER_BLOCK   (COSFS_BLOCK_SIZE * 8)
/* maximum file size in blocks */
#define COSFS_MAX_FILE_BLOCKS                                                                 \
	((uint64_t)COSFS_NDIRECT + COSFS_PTRS_PER_BLOCK + (uint64_t)COSFS_PTRS_PER_BLOCK * COSFS_PTRS_PER_BLOCK)

struct cosfs_super {
	uint64_t magic;
	uint32_t version;
	uint32_t block_size;
	uint64_t nblocks;
	uint32_t ninodes;
	uint32_t bmap_start;
	uint32_t bmap_blocks;
	uint32_t itab_start;
	uint32_t itab_blocks;
	uint32_t data_start;
	uint32_t root_ino;
	uint32_t pad;
};

struct cosfs_dinode {
	uint32_t mode;  /* FS_S_IF* | perms; 0 = free */
	uint32_t nlink;
	uint64_t size;
	uint64_t mtime; /* unused for now (no clock), kept for the format */
	uint32_t direct[COSFS_NDIRECT];
	uint32_t ind;
	uint32_t dind;
	uint32_t nblocks; /* data + indirect blocks owned */
	uint8_t  pad[COSFS_INODE_SIZE - 84];
};

struct cosfs_ddirent {
	uint32_t ino; /* 0 = free slot */
	uint8_t  type;
	uint8_t  namelen;
	char     name[COSFS_NAME_MAX]; /* not NUL-terminated */
};

_Static_assert(sizeof(struct cosfs_dinode) == COSFS_INODE_SIZE, "cosfs inode size");
_Static_assert(sizeof(struct cosfs_ddirent) == COSFS_DIRENT_SIZE, "cosfs dirent size");

#endif /* COSFS_FORMAT_H */
