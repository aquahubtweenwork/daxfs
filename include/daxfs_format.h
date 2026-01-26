/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * daxfs on-disk format definitions
 *
 * Shared between kernel module and user-space tools (e.g., mkfs.daxfs).
 *
 * Copyright (C) 2026 Multikernel Technologies, Inc. All rights reserved.
 */
#ifndef _DAXFS_FORMAT_H
#define _DAXFS_FORMAT_H

#include <linux/types.h>

#define DAXFS2_MAGIC		0x64617832	/* "dax2" */
#define DAXFS_VERSION		2
#define DAXFS_BLOCK_SIZE	4096
#define DAXFS_INODE_SIZE	64
#define DAXFS_ROOT_INO		1

#define DAXFS_BRANCH_NAME_MAX	31
#define DAXFS_MAX_BRANCHES	256

/* Branch states */
#define DAXFS_BRANCH_FREE	0
#define DAXFS_BRANCH_ACTIVE	1
#define DAXFS_BRANCH_COMMITTED	2
#define DAXFS_BRANCH_ABORTED	3

/* Delta log entry types */
#define DAXFS_DELTA_WRITE	1	/* File data write */
#define DAXFS_DELTA_CREATE	2	/* Create file */
#define DAXFS_DELTA_DELETE	3	/* Delete (tombstone) */
#define DAXFS_DELTA_TRUNCATE	4	/* Truncate file */
#define DAXFS_DELTA_MKDIR	5	/* Create directory */
#define DAXFS_DELTA_RENAME	6	/* Rename */
#define DAXFS_DELTA_SETATTR	7	/* Inode metadata update */

/*
 * Superblock - at offset 0, 4KB
 *
 * On-DAX Layout:
 * [ Superblock (4KB) | Branch Table | Base Image (optional) | Delta Region ]
 */
struct daxfs_super {
	__le32 magic;			/* DAXFS_MAGIC */
	__le32 version;			/* DAXFS_VERSION */
	__le32 flags;
	__le32 block_size;		/* 4096 */
	__le64 total_size;

	/* Base image (optional embedded read-only image) */
	__le64 base_offset;		/* Offset to base image (0 if none) */
	__le64 base_size;

	/* Branch management */
	__le64 branch_table_offset;
	__le32 branch_table_entries;	/* Max branches (DAXFS_MAX_BRANCHES) */
	__le32 active_branches;
	__le64 next_branch_id;
	__le64 next_inode_id;		/* Global inode counter */

	/* Delta region */
	__le64 delta_region_offset;
	__le64 delta_region_size;	/* Total size of delta region */
	__le64 delta_alloc_offset;	/* Next free byte in delta region */

	__u8   reserved[3944];		/* Pad to 4KB */
};

/*
 * Branch record - 128 bytes
 */
struct daxfs_branch {
	__le64 branch_id;
	__le64 parent_id;		/* 0 = no parent (main branch) */
	__le64 delta_log_offset;	/* Start of this branch's delta log */
	__le64 delta_log_size;		/* Bytes used */
	__le64 delta_log_capacity;	/* Bytes allocated */
	__le32 state;			/* FREE, ACTIVE, COMMITTED, ABORTED */
	__le32 refcount;		/* Child branches + active mounts */
	__le64 next_local_ino;		/* Branch-local inode counter */
	char   name[32];		/* Branch name (null-terminated) */
	__u8   reserved[40];		/* Pad to 128 bytes */
};

/*
 * Delta log entry header - variable size entries
 */
struct daxfs_delta_hdr {
	__le32 type;
	__le32 total_size;		/* Size of this entry including header */
	__le64 ino;
	__le64 timestamp;		/* For ordering */
};

/*
 * WRITE entry: header + this + data
 */
struct daxfs_delta_write {
	__le64 offset;			/* File offset */
	__le32 len;			/* Data length */
	__le32 flags;
	/* Data follows immediately */
};

/*
 * CREATE entry: header + this + name
 */
struct daxfs_delta_create {
	__le64 parent_ino;
	__le64 new_ino;
	__le32 mode;
	__le16 name_len;
	__le16 flags;
	/* Name follows immediately */
};

/*
 * DELETE entry (tombstone): header + this + name
 */
struct daxfs_delta_delete {
	__le64 parent_ino;
	__le16 name_len;
	__le16 flags;
	__le32 reserved;
	/* Name follows immediately */
};

/*
 * TRUNCATE entry: header + this
 */
struct daxfs_delta_truncate {
	__le64 new_size;
};

/*
 * RENAME entry: header + this + old_name + new_name
 */
struct daxfs_delta_rename {
	__le64 old_parent_ino;
	__le64 new_parent_ino;
	__le64 ino;
	__le16 old_name_len;
	__le16 new_name_len;
	__le32 reserved;
	/* old_name then new_name follow */
};

/*
 * SETATTR entry: header + this
 */
struct daxfs_delta_setattr {
	__le32 mode;
	__le32 uid;
	__le32 gid;
	__le32 valid;			/* Bitmask of which fields are valid */
	__le64 size;			/* For truncate via setattr */
};

/* Flags for daxfs_delta_setattr.valid */
#define DAXFS_ATTR_MODE		(1 << 0)
#define DAXFS_ATTR_UID		(1 << 1)
#define DAXFS_ATTR_GID		(1 << 2)
#define DAXFS_ATTR_SIZE		(1 << 3)

/*
 * ============================================================================
 * Base Image Format (embedded read-only snapshot)
 * ============================================================================
 *
 * The base image is an optional embedded read-only filesystem image
 * that provides the initial state. New changes are stored in deltas.
 */

#define DAXFS_BASE_MAGIC	0x64646178	/* "ddax" */

/*
 * Base image superblock - always at base_offset, padded to DAXFS_BLOCK_SIZE
 */
struct daxfs_base_super {
	__le32 magic;		/* DAXFS_BASE_MAGIC */
	__le32 version;		/* 1 */
	__le32 flags;
	__le32 block_size;	/* Always DAXFS_BLOCK_SIZE */
	__le64 total_size;	/* Total base image size in bytes */
	__le64 inode_offset;	/* Offset to inode table (relative to base) */
	__le32 inode_count;	/* Number of inodes */
	__le32 root_inode;	/* Root directory inode number */
	__le64 strtab_offset;	/* Offset to string table (relative to base) */
	__le64 strtab_size;	/* Size of string table */
	__le64 data_offset;	/* Offset to file data area (relative to base) */
	__u8   reserved[4032];	/* Pad to 4KB */
};

/*
 * Base image inode - fixed size for simple indexing
 *
 * Directories use first_child/next_sibling for a linked list structure.
 * Regular files store data at data_offset.
 * Symlinks store target path at data_offset.
 */
struct daxfs_base_inode {
	__le32 ino;		/* Inode number (1-based) */
	__le32 mode;		/* File type and permissions */
	__le32 uid;		/* Owner UID */
	__le32 gid;		/* Owner GID */
	__le64 size;		/* File size in bytes */
	__le64 data_offset;	/* Offset to file data (relative to base) */
	__le32 name_offset;	/* Offset into string table for filename */
	__le32 name_len;	/* Length of filename */
	__le32 parent_ino;	/* Parent directory inode number */
	__le32 nlink;		/* Link count */
	__le32 first_child;	/* For dirs: first child inode (0 if empty) */
	__le32 next_sibling;	/* Next entry in same directory (0 if last) */
	__u8   reserved[8];	/* Pad to DAXFS_INODE_SIZE (64 bytes) */
};

#endif /* _DAXFS_FORMAT_H */
