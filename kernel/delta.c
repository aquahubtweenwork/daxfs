// SPDX-License-Identifier: GPL-2.0
/*
 * daxfs delta log operations
 *
 * Copyright (C) 2026 Multikernel Technologies, Inc. All rights reserved.
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/rbtree.h>
#include <linux/jhash.h>
#include "daxfs.h"

/*
 * Simple hash for directory entry lookup
 */
static u32 dirent_hash(u64 parent_ino, const char *name, int namelen)
{
	u32 hash = jhash(name, namelen, (u32)parent_ino);
	return hash ^ (parent_ino >> 32);
}

/*
 * Initialize delta log for a branch
 */
int daxfs_delta_init_branch(struct daxfs_info *info,
			    struct daxfs_branch_ctx *branch)
{
	/* Nothing special needed - rb_trees already initialized */
	(void)info;
	(void)branch;
	return 0;
}

/*
 * Free all index entries for a branch
 */
static void free_inode_index(struct daxfs_branch_ctx *branch)
{
	struct rb_node *node;
	struct daxfs_delta_inode_entry *entry;

	while ((node = rb_first(&branch->inode_index)) != NULL) {
		entry = rb_entry(node, struct daxfs_delta_inode_entry, rb_node);
		rb_erase(node, &branch->inode_index);
		kfree(entry);
	}
}

static void free_dirent_index(struct daxfs_branch_ctx *branch)
{
	struct rb_node *node;
	struct daxfs_delta_dirent_entry *entry;

	while ((node = rb_first(&branch->dirent_index)) != NULL) {
		entry = rb_entry(node, struct daxfs_delta_dirent_entry, rb_node);
		rb_erase(node, &branch->dirent_index);
		kfree(entry->name);
		kfree(entry);
	}
}

/*
 * Destroy delta log state for a branch
 */
void daxfs_delta_destroy_branch(struct daxfs_branch_ctx *branch)
{
	unsigned long flags;

	spin_lock_irqsave(&branch->index_lock, flags);
	free_inode_index(branch);
	free_dirent_index(branch);
	spin_unlock_irqrestore(&branch->index_lock, flags);
}

/*
 * Allocate space in branch's delta log
 */
void *daxfs_delta_alloc(struct daxfs_info *info,
			struct daxfs_branch_ctx *branch, size_t size)
{
	void *ptr;
	u64 new_size;

	spin_lock(&info->alloc_lock);

	new_size = branch->delta_size + size;
	if (new_size > branch->delta_capacity) {
		spin_unlock(&info->alloc_lock);
		pr_err("daxfs: delta log full for branch '%s'\n", branch->name);
		return NULL;
	}

	ptr = branch->delta_log + branch->delta_size;
	branch->delta_size = new_size;
	branch->on_dax->delta_log_size = cpu_to_le64(new_size);

	spin_unlock(&info->alloc_lock);

	return ptr;
}

/*
 * Insert or update inode index entry
 */
static int index_add_inode(struct daxfs_branch_ctx *branch, u64 ino,
			   struct daxfs_delta_hdr *hdr, bool deleted,
			   u64 size, u32 mode)
{
	struct rb_node **link = &branch->inode_index.rb_node;
	struct rb_node *parent = NULL;
	struct daxfs_delta_inode_entry *entry;
	unsigned long flags;

	spin_lock_irqsave(&branch->index_lock, flags);

	/* Search for existing entry */
	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct daxfs_delta_inode_entry, rb_node);

		if (ino < entry->ino) {
			link = &parent->rb_left;
		} else if (ino > entry->ino) {
			link = &parent->rb_right;
		} else {
			/* Update existing entry */
			entry->hdr = hdr;
			entry->deleted = deleted;
			if (size != (u64)-1)
				entry->size = size;
			if (mode != (u32)-1)
				entry->mode = mode;
			spin_unlock_irqrestore(&branch->index_lock, flags);
			return 0;
		}
	}

	/* Create new entry */
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		spin_unlock_irqrestore(&branch->index_lock, flags);
		return -ENOMEM;
	}

	entry->ino = ino;
	entry->hdr = hdr;
	entry->deleted = deleted;
	entry->size = (size != (u64)-1) ? size : 0;
	entry->mode = (mode != (u32)-1) ? mode : 0;

	rb_link_node(&entry->rb_node, parent, link);
	rb_insert_color(&entry->rb_node, &branch->inode_index);

	spin_unlock_irqrestore(&branch->index_lock, flags);
	return 0;
}

/*
 * Insert or update dirent index entry
 */
static int index_add_dirent(struct daxfs_branch_ctx *branch, u64 parent_ino,
			    const char *name, int namelen,
			    struct daxfs_delta_hdr *hdr, bool deleted)
{
	struct rb_node **link = &branch->dirent_index.rb_node;
	struct rb_node *parent = NULL;
	struct daxfs_delta_dirent_entry *entry;
	u32 hash = dirent_hash(parent_ino, name, namelen);
	u64 key;
	unsigned long flags;

	/* Combined key: parent_ino in upper bits, hash in lower */
	key = (parent_ino << 32) | hash;

	spin_lock_irqsave(&branch->index_lock, flags);

	while (*link) {
		u64 entry_key;

		parent = *link;
		entry = rb_entry(parent, struct daxfs_delta_dirent_entry, rb_node);
		entry_key = ((u64)entry->parent_ino << 32) | entry->name_hash;

		if (key < entry_key) {
			link = &parent->rb_left;
		} else if (key > entry_key) {
			link = &parent->rb_right;
		} else {
			/* Same hash - check actual name */
			if (namelen == entry->name_len &&
			    memcmp(name, entry->name, namelen) == 0) {
				/* Update existing entry */
				entry->hdr = hdr;
				entry->deleted = deleted;
				spin_unlock_irqrestore(&branch->index_lock, flags);
				return 0;
			}
			/* Hash collision - use right subtree */
			link = &parent->rb_right;
		}
	}

	/* Create new entry */
	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		spin_unlock_irqrestore(&branch->index_lock, flags);
		return -ENOMEM;
	}

	entry->parent_ino = parent_ino;
	entry->name_hash = hash;
	entry->name = kmemdup(name, namelen, GFP_ATOMIC);
	if (!entry->name) {
		kfree(entry);
		spin_unlock_irqrestore(&branch->index_lock, flags);
		return -ENOMEM;
	}
	entry->name_len = namelen;
	entry->hdr = hdr;
	entry->deleted = deleted;

	rb_link_node(&entry->rb_node, parent, link);
	rb_insert_color(&entry->rb_node, &branch->dirent_index);

	spin_unlock_irqrestore(&branch->index_lock, flags);
	return 0;
}

/*
 * Append entry to branch's delta log
 */
int daxfs_delta_append(struct daxfs_branch_ctx *branch, u32 type,
		       u64 ino, void *data, size_t data_len)
{
	struct daxfs_info *info = branch->info;
	struct daxfs_delta_hdr *hdr;
	size_t total_size;
	void *entry;

	total_size = sizeof(*hdr) + data_len;
	entry = daxfs_delta_alloc(info, branch, total_size);
	if (!entry)
		return -ENOSPC;

	hdr = entry;
	hdr->type = cpu_to_le32(type);
	hdr->total_size = cpu_to_le32(total_size);
	hdr->ino = cpu_to_le64(ino);
	hdr->timestamp = cpu_to_le64(ktime_get_real_ns());

	if (data && data_len)
		memcpy(entry + sizeof(*hdr), data, data_len);

	/* Update index based on entry type */
	switch (type) {
	case DAXFS_DELTA_CREATE:
	case DAXFS_DELTA_MKDIR: {
		struct daxfs_delta_create *cr = entry + sizeof(*hdr);
		char *name = (char *)(cr + 1);
		u64 new_ino = le64_to_cpu(cr->new_ino);
		u64 parent_ino = le64_to_cpu(cr->parent_ino);
		u32 mode = le32_to_cpu(cr->mode);
		u16 name_len = le16_to_cpu(cr->name_len);

		index_add_inode(branch, new_ino, hdr, false, 0, mode);
		index_add_dirent(branch, parent_ino, name, name_len, hdr, false);
		break;
	}
	case DAXFS_DELTA_DELETE: {
		struct daxfs_delta_delete *del = entry + sizeof(*hdr);
		char *name = (char *)(del + 1);
		u64 parent_ino = le64_to_cpu(del->parent_ino);
		u16 name_len = le16_to_cpu(del->name_len);

		index_add_inode(branch, ino, hdr, true, -1, -1);
		index_add_dirent(branch, parent_ino, name, name_len, hdr, true);
		break;
	}
	case DAXFS_DELTA_TRUNCATE: {
		struct daxfs_delta_truncate *tr = entry + sizeof(*hdr);
		u64 new_size = le64_to_cpu(tr->new_size);

		index_add_inode(branch, ino, hdr, false, new_size, -1);
		break;
	}
	case DAXFS_DELTA_WRITE: {
		struct daxfs_delta_write *wr = entry + sizeof(*hdr);
		u64 offset = le64_to_cpu(wr->offset);
		u32 len = le32_to_cpu(wr->len);
		u64 end = offset + len;

		/* Update size if write extends file */
		index_add_inode(branch, ino, hdr, false, end, -1);
		break;
	}
	case DAXFS_DELTA_SETATTR: {
		struct daxfs_delta_setattr *sa = entry + sizeof(*hdr);
		u32 valid = le32_to_cpu(sa->valid);
		u64 size = (valid & DAXFS_ATTR_SIZE) ?
			   le64_to_cpu(sa->size) : (u64)-1;
		u32 mode = (valid & DAXFS_ATTR_MODE) ?
			   le32_to_cpu(sa->mode) : (u32)-1;

		index_add_inode(branch, ino, hdr, false, size, mode);
		break;
	}
	case DAXFS_DELTA_RENAME: {
		struct daxfs_delta_rename *rn = entry + sizeof(*hdr);
		char *old_name = (char *)(rn + 1);
		char *new_name = old_name + le16_to_cpu(rn->old_name_len);
		u64 old_parent = le64_to_cpu(rn->old_parent_ino);
		u64 new_parent = le64_to_cpu(rn->new_parent_ino);

		/* Delete from old location */
		index_add_dirent(branch, old_parent, old_name,
				 le16_to_cpu(rn->old_name_len), hdr, true);
		/* Add at new location */
		index_add_dirent(branch, new_parent, new_name,
				 le16_to_cpu(rn->new_name_len), hdr, false);
		break;
	}
	}

	return 0;
}

/*
 * Scan delta log and build in-memory index
 */
int daxfs_delta_build_index(struct daxfs_branch_ctx *branch)
{
	struct daxfs_delta_hdr *hdr;
	u64 offset = 0;

	/* First, build parent's index if needed */
	if (branch->parent && rb_first(&branch->parent->inode_index) == NULL) {
		int ret = daxfs_delta_build_index(branch->parent);
		if (ret)
			return ret;
	}

	/* Scan this branch's delta log */
	while (offset < branch->delta_size) {
		u32 type, total_size;

		hdr = branch->delta_log + offset;
		type = le32_to_cpu(hdr->type);
		total_size = le32_to_cpu(hdr->total_size);

		if (total_size == 0 || offset + total_size > branch->delta_size)
			break;

		/* Index this entry based on type */
		switch (type) {
		case DAXFS_DELTA_CREATE:
		case DAXFS_DELTA_MKDIR: {
			struct daxfs_delta_create *cr =
				(void *)hdr + sizeof(*hdr);
			char *name = (char *)(cr + 1);

			index_add_inode(branch, le64_to_cpu(cr->new_ino), hdr,
					false, 0, le32_to_cpu(cr->mode));
			index_add_dirent(branch, le64_to_cpu(cr->parent_ino),
					 name, le16_to_cpu(cr->name_len),
					 hdr, false);
			break;
		}
		case DAXFS_DELTA_DELETE: {
			struct daxfs_delta_delete *del =
				(void *)hdr + sizeof(*hdr);
			char *name = (char *)(del + 1);

			index_add_inode(branch, le64_to_cpu(hdr->ino), hdr,
					true, -1, -1);
			index_add_dirent(branch, le64_to_cpu(del->parent_ino),
					 name, le16_to_cpu(del->name_len),
					 hdr, true);
			break;
		}
		case DAXFS_DELTA_TRUNCATE: {
			struct daxfs_delta_truncate *tr =
				(void *)hdr + sizeof(*hdr);

			index_add_inode(branch, le64_to_cpu(hdr->ino), hdr,
					false, le64_to_cpu(tr->new_size), -1);
			break;
		}
		case DAXFS_DELTA_WRITE: {
			struct daxfs_delta_write *wr =
				(void *)hdr + sizeof(*hdr);
			u64 end = le64_to_cpu(wr->offset) + le32_to_cpu(wr->len);

			index_add_inode(branch, le64_to_cpu(hdr->ino), hdr,
					false, end, -1);
			break;
		}
		case DAXFS_DELTA_SETATTR: {
			struct daxfs_delta_setattr *sa =
				(void *)hdr + sizeof(*hdr);
			u32 valid = le32_to_cpu(sa->valid);

			index_add_inode(branch, le64_to_cpu(hdr->ino), hdr,
					false,
					(valid & DAXFS_ATTR_SIZE) ?
						le64_to_cpu(sa->size) : (u64)-1,
					(valid & DAXFS_ATTR_MODE) ?
						le32_to_cpu(sa->mode) : (u32)-1);
			break;
		}
		case DAXFS_DELTA_RENAME: {
			struct daxfs_delta_rename *rn =
				(void *)hdr + sizeof(*hdr);
			char *old_name = (char *)(rn + 1);
			char *new_name = old_name +
					 le16_to_cpu(rn->old_name_len);

			index_add_dirent(branch,
					 le64_to_cpu(rn->old_parent_ino),
					 old_name,
					 le16_to_cpu(rn->old_name_len),
					 hdr, true);
			index_add_dirent(branch,
					 le64_to_cpu(rn->new_parent_ino),
					 new_name,
					 le16_to_cpu(rn->new_name_len),
					 hdr, false);
			break;
		}
		}

		offset += total_size;
	}

	return 0;
}

/*
 * Lookup inode in delta log index
 */
struct daxfs_delta_hdr *daxfs_delta_lookup_inode(struct daxfs_branch_ctx *branch,
						 u64 ino)
{
	struct rb_node *node;
	struct daxfs_delta_inode_entry *entry;
	unsigned long flags;

	spin_lock_irqsave(&branch->index_lock, flags);

	node = branch->inode_index.rb_node;
	while (node) {
		entry = rb_entry(node, struct daxfs_delta_inode_entry, rb_node);

		if (ino < entry->ino)
			node = node->rb_left;
		else if (ino > entry->ino)
			node = node->rb_right;
		else {
			spin_unlock_irqrestore(&branch->index_lock, flags);
			return entry->hdr;
		}
	}

	spin_unlock_irqrestore(&branch->index_lock, flags);
	return NULL;
}

/*
 * Lookup dirent in delta log index
 */
struct daxfs_delta_hdr *daxfs_delta_lookup_dirent(struct daxfs_branch_ctx *branch,
						  u64 parent_ino,
						  const char *name, int namelen)
{
	struct rb_node *node;
	struct daxfs_delta_dirent_entry *entry;
	u32 hash = dirent_hash(parent_ino, name, namelen);
	u64 key = (parent_ino << 32) | hash;
	unsigned long flags;

	spin_lock_irqsave(&branch->index_lock, flags);

	node = branch->dirent_index.rb_node;
	while (node) {
		u64 entry_key;

		entry = rb_entry(node, struct daxfs_delta_dirent_entry, rb_node);
		entry_key = ((u64)entry->parent_ino << 32) | entry->name_hash;

		if (key < entry_key) {
			node = node->rb_left;
		} else if (key > entry_key) {
			node = node->rb_right;
		} else {
			/* Check actual name */
			if (namelen == entry->name_len &&
			    memcmp(name, entry->name, namelen) == 0) {
				spin_unlock_irqrestore(&branch->index_lock, flags);
				return entry->hdr;
			}
			node = node->rb_right;
		}
	}

	spin_unlock_irqrestore(&branch->index_lock, flags);
	return NULL;
}

/*
 * Check if inode is deleted in this branch
 */
bool daxfs_delta_is_deleted(struct daxfs_branch_ctx *branch, u64 ino)
{
	struct rb_node *node;
	struct daxfs_delta_inode_entry *entry;
	unsigned long flags;

	spin_lock_irqsave(&branch->index_lock, flags);

	node = branch->inode_index.rb_node;
	while (node) {
		entry = rb_entry(node, struct daxfs_delta_inode_entry, rb_node);

		if (ino < entry->ino)
			node = node->rb_left;
		else if (ino > entry->ino)
			node = node->rb_right;
		else {
			bool deleted = entry->deleted;
			spin_unlock_irqrestore(&branch->index_lock, flags);
			return deleted;
		}
	}

	spin_unlock_irqrestore(&branch->index_lock, flags);
	return false;
}

/*
 * Get current size of an inode (from delta or return -1 if not found)
 */
int daxfs_delta_get_size(struct daxfs_branch_ctx *branch, u64 ino, loff_t *size)
{
	struct rb_node *node;
	struct daxfs_delta_inode_entry *entry;
	unsigned long flags;

	spin_lock_irqsave(&branch->index_lock, flags);

	node = branch->inode_index.rb_node;
	while (node) {
		entry = rb_entry(node, struct daxfs_delta_inode_entry, rb_node);

		if (ino < entry->ino)
			node = node->rb_left;
		else if (ino > entry->ino)
			node = node->rb_right;
		else {
			*size = entry->size;
			spin_unlock_irqrestore(&branch->index_lock, flags);
			return 0;
		}
	}

	spin_unlock_irqrestore(&branch->index_lock, flags);
	return -ENOENT;
}

/*
 * Resolve inode through branch chain
 */
int daxfs_resolve_inode(struct super_block *sb, u64 ino,
			   umode_t *mode, loff_t *size, bool *deleted)
{
	struct daxfs_info *info = DAXFS_SB(sb);
	struct daxfs_branch_ctx *b;

	*deleted = false;

	/* Walk branch chain from child to parent */
	for (b = info->current_branch; b != NULL; b = b->parent) {
		if (daxfs_delta_is_deleted(b, ino)) {
			*deleted = true;
			return 0;
		}

		struct daxfs_delta_hdr *hdr = daxfs_delta_lookup_inode(b, ino);
		if (hdr) {
			u32 type = le32_to_cpu(hdr->type);

			if (type == DAXFS_DELTA_DELETE) {
				*deleted = true;
				return 0;
			}

			/* Get mode and size from index */
			struct rb_node *node;
			struct daxfs_delta_inode_entry *entry;

			node = b->inode_index.rb_node;
			while (node) {
				entry = rb_entry(node,
						 struct daxfs_delta_inode_entry,
						 rb_node);
				if (ino < entry->ino)
					node = node->rb_left;
				else if (ino > entry->ino)
					node = node->rb_right;
				else {
					*mode = entry->mode;
					*size = entry->size;
					return 0;
				}
			}
		}
	}

	/* Check base image */
	if (info->base_inodes && ino <= info->base_inode_count) {
		struct daxfs_base_inode *raw = &info->base_inodes[ino - 1];
		*mode = le32_to_cpu(raw->mode);
		*size = le64_to_cpu(raw->size);
		return 0;
	}

	return -ENOENT;
}

/*
 * Resolve file data through branch chain
 * Returns pointer to data and actual length available
 */
void *daxfs_resolve_file_data(struct super_block *sb, u64 ino,
			      loff_t pos, size_t len, size_t *out_len)
{
	struct daxfs_info *info = DAXFS_SB(sb);
	struct daxfs_branch_ctx *b;

	/* Walk branch chain from child to parent looking for write at pos */
	for (b = info->current_branch; b != NULL; b = b->parent) {
		/* Scan delta log for WRITE entries covering this position */
		u64 offset = 0;

		while (offset < b->delta_size) {
			struct daxfs_delta_hdr *hdr = b->delta_log + offset;
			u32 type = le32_to_cpu(hdr->type);
			u32 total_size = le32_to_cpu(hdr->total_size);

			if (total_size == 0)
				break;

			if (type == DAXFS_DELTA_WRITE &&
			    le64_to_cpu(hdr->ino) == ino) {
				struct daxfs_delta_write *wr =
					(void *)hdr + sizeof(*hdr);
				u64 wr_offset = le64_to_cpu(wr->offset);
				u32 wr_len = le32_to_cpu(wr->len);

				if (pos >= wr_offset &&
				    pos < wr_offset + wr_len) {
					/* Found data at this position */
					u64 data_off = pos - wr_offset;
					void *data = (void *)(wr + 1) + data_off;
					size_t avail = wr_len - data_off;

					*out_len = min(len, avail);
					return data;
				}
			}

			offset += total_size;
		}
	}

	/* Fall back to base image using storage layer */
	if (info->base_inodes && ino <= info->base_inode_count) {
		struct daxfs_base_inode *raw = &info->base_inodes[ino - 1];
		u64 data_offset = le64_to_cpu(raw->data_offset);
		loff_t file_size = le64_to_cpu(raw->size);
		u64 abs_offset;

		if (pos >= file_size) {
			*out_len = 0;
			return NULL;
		}

		*out_len = min(len, (size_t)(file_size - pos));
		abs_offset = le64_to_cpu(info->super->base_offset) +
			     data_offset + pos;
		return daxfs_mem_ptr((struct daxfs_info *)info, abs_offset);
	}

	*out_len = 0;
	return NULL;
}

/*
 * Merge child's deltas into parent's log
 */
int daxfs_delta_merge(struct daxfs_branch_ctx *parent,
		      struct daxfs_branch_ctx *child)
{
	struct daxfs_info *info = parent->info;
	void *dest;
	u64 child_size = child->delta_size;

	if (child_size == 0)
		return 0;	/* Nothing to merge */

	/* Check if parent has space */
	if (parent->delta_size + child_size > parent->delta_capacity) {
		pr_err("daxfs: parent delta log too small for merge\n");
		return -ENOSPC;
	}

	/* Copy child's delta log to parent */
	dest = daxfs_delta_alloc(info, parent, child_size);
	if (!dest)
		return -ENOSPC;

	memcpy(dest, child->delta_log, child_size);

	/* Rebuild parent's index to include merged entries */
	/* Note: This is simple but not optimal - could merge indices instead */
	free_inode_index(parent);
	free_dirent_index(parent);
	daxfs_delta_build_index(parent);

	return 0;
}
