// SPDX-License-Identifier: GPL-2.0
/*
 * daxfs directory operations
 *
 * Copyright (C) 2026 Multikernel Technologies, Inc. All rights reserved.
 */

#include <linux/fs.h>
#include "daxfs.h"

/*
 * Check if name exists in directory (checking deltas first, then base)
 */
static bool daxfs_name_exists(struct super_block *sb, u64 parent_ino,
			      const char *name, int namelen, u64 *ino_out)
{
	struct daxfs_info *info = DAXFS_SB(sb);
	struct daxfs_branch_ctx *b;

	/* Check delta logs from child to parent */
	for (b = info->current_branch; b != NULL; b = b->parent) {
		struct daxfs_delta_hdr *hdr;

		hdr = daxfs_delta_lookup_dirent(b, parent_ino, name, namelen);
		if (hdr) {
			u32 type = le32_to_cpu(hdr->type);

			if (type == DAXFS_DELTA_DELETE)
				return false;	/* Deleted */

			if (type == DAXFS_DELTA_CREATE ||
			    type == DAXFS_DELTA_MKDIR) {
				struct daxfs_delta_create *cr =
					(void *)hdr + sizeof(*hdr);
				if (ino_out)
					*ino_out = le64_to_cpu(cr->new_ino);
				return true;
			}
		}
	}

	/* Check base image */
	if (info->base_inodes) {
		struct daxfs_base_inode *parent_raw;
		u32 child_ino;

		if (parent_ino > info->base_inode_count)
			return false;

		parent_raw = &info->base_inodes[parent_ino - 1];
		child_ino = le32_to_cpu(parent_raw->first_child);

		while (child_ino && child_ino <= info->base_inode_count) {
			struct daxfs_base_inode *child = &info->base_inodes[child_ino - 1];
			char *child_name = info->base_strtab +
					   le32_to_cpu(child->name_offset);
			u32 child_name_len = le32_to_cpu(child->name_len);

			if (namelen == child_name_len &&
			    memcmp(name, child_name, namelen) == 0) {
				/* Check if deleted in delta */
				for (b = info->current_branch; b; b = b->parent) {
					if (daxfs_delta_is_deleted(b, child_ino))
						return false;
				}
				if (ino_out)
					*ino_out = child_ino;
				return true;
			}

			child_ino = le32_to_cpu(child->next_sibling);
		}
	}

	return false;
}

static struct dentry *daxfs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = NULL;
	u64 ino;

	if (daxfs_name_exists(sb, dir->i_ino,
			      dentry->d_name.name, dentry->d_name.len,
			      &ino)) {
		inode = daxfs_iget(sb, ino);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}

	return d_splice_alias(inode, dentry);
}

static int daxfs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	struct super_block *sb = dir->i_sb;
	struct daxfs_info *info = DAXFS_SB(sb);
	struct daxfs_branch_ctx *branch = info->current_branch;
	struct inode *inode;
	struct daxfs_delta_create cr;
	char *entry_data;
	size_t entry_size;
	u64 new_ino;
	int ret;

	/* Check if name already exists */
	if (daxfs_name_exists(sb, dir->i_ino,
			      dentry->d_name.name, dentry->d_name.len, NULL))
		return -EEXIST;

	/* Allocate new inode number */
	new_ino = daxfs_alloc_ino(branch);

	/* Update global counter */
	if (new_ino >= le64_to_cpu(info->super->next_inode_id))
		info->super->next_inode_id = cpu_to_le64(new_ino + 1);

	/* Prepare CREATE entry */
	cr.parent_ino = cpu_to_le64(dir->i_ino);
	cr.new_ino = cpu_to_le64(new_ino);
	cr.mode = cpu_to_le32(mode);
	cr.name_len = cpu_to_le16(dentry->d_name.len);
	cr.flags = 0;

	/* Append to delta log */
	entry_size = sizeof(cr) + dentry->d_name.len;
	entry_data = kmalloc(entry_size, GFP_KERNEL);
	if (!entry_data)
		return -ENOMEM;

	memcpy(entry_data, &cr, sizeof(cr));
	memcpy(entry_data + sizeof(cr), dentry->d_name.name, dentry->d_name.len);

	ret = daxfs_delta_append(branch, DAXFS_DELTA_CREATE, new_ino,
				 entry_data, entry_size);
	kfree(entry_data);
	if (ret)
		return ret;

	/* Create VFS inode */
	inode = daxfs_new_inode(sb, mode, new_ino);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	d_instantiate(dentry, inode);
	return 0;
}

static struct dentry *daxfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				  struct dentry *dentry, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct daxfs_info *info = DAXFS_SB(sb);
	struct daxfs_branch_ctx *branch = info->current_branch;
	struct inode *inode;
	struct daxfs_delta_create cr;
	char *entry_data;
	size_t entry_size;
	u64 new_ino;
	int ret;

	/* Check if name already exists */
	if (daxfs_name_exists(sb, dir->i_ino,
			      dentry->d_name.name, dentry->d_name.len, NULL))
		return ERR_PTR(-EEXIST);

	/* Allocate new inode number */
	new_ino = daxfs_alloc_ino(branch);

	if (new_ino >= le64_to_cpu(info->super->next_inode_id))
		info->super->next_inode_id = cpu_to_le64(new_ino + 1);

	/* Prepare MKDIR entry */
	cr.parent_ino = cpu_to_le64(dir->i_ino);
	cr.new_ino = cpu_to_le64(new_ino);
	cr.mode = cpu_to_le32(mode | S_IFDIR);
	cr.name_len = cpu_to_le16(dentry->d_name.len);
	cr.flags = 0;

	entry_size = sizeof(cr) + dentry->d_name.len;
	entry_data = kmalloc(entry_size, GFP_KERNEL);
	if (!entry_data)
		return ERR_PTR(-ENOMEM);

	memcpy(entry_data, &cr, sizeof(cr));
	memcpy(entry_data + sizeof(cr), dentry->d_name.name, dentry->d_name.len);

	ret = daxfs_delta_append(branch, DAXFS_DELTA_MKDIR, new_ino,
				 entry_data, entry_size);
	kfree(entry_data);
	if (ret)
		return ERR_PTR(ret);

	inode = daxfs_new_inode(sb, mode | S_IFDIR, new_ino);
	if (IS_ERR(inode))
		return ERR_CAST(inode);

	inc_nlink(dir);
	d_instantiate(dentry, inode);
	return NULL;
}

static int daxfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct daxfs_info *info = DAXFS_SB(sb);
	struct daxfs_branch_ctx *branch = info->current_branch;
	struct inode *inode = d_inode(dentry);
	struct daxfs_delta_delete del;
	char *entry_data;
	size_t entry_size;
	int ret;

	del.parent_ino = cpu_to_le64(dir->i_ino);
	del.name_len = cpu_to_le16(dentry->d_name.len);
	del.flags = 0;
	del.reserved = 0;

	entry_size = sizeof(del) + dentry->d_name.len;
	entry_data = kmalloc(entry_size, GFP_KERNEL);
	if (!entry_data)
		return -ENOMEM;

	memcpy(entry_data, &del, sizeof(del));
	memcpy(entry_data + sizeof(del), dentry->d_name.name, dentry->d_name.len);

	ret = daxfs_delta_append(branch, DAXFS_DELTA_DELETE, inode->i_ino,
				 entry_data, entry_size);
	kfree(entry_data);
	if (ret)
		return ret;

	drop_nlink(inode);
	return 0;
}

static int daxfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	/* TODO: Check if directory is empty */
	return daxfs_unlink(dir, dentry);
}

static int daxfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
			struct dentry *old_dentry, struct inode *new_dir,
			struct dentry *new_dentry, unsigned int flags)
{
	struct super_block *sb = old_dir->i_sb;
	struct daxfs_info *info = DAXFS_SB(sb);
	struct daxfs_branch_ctx *branch = info->current_branch;
	struct inode *inode = d_inode(old_dentry);
	struct daxfs_delta_rename rn;
	char *entry_data;
	size_t entry_size;
	int ret;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	/* Check if target exists */
	if (daxfs_name_exists(sb, new_dir->i_ino,
			      new_dentry->d_name.name,
			      new_dentry->d_name.len, NULL)) {
		if (flags & RENAME_NOREPLACE)
			return -EEXIST;
		/* TODO: Handle overwrite case */
	}

	rn.old_parent_ino = cpu_to_le64(old_dir->i_ino);
	rn.new_parent_ino = cpu_to_le64(new_dir->i_ino);
	rn.ino = cpu_to_le64(inode->i_ino);
	rn.old_name_len = cpu_to_le16(old_dentry->d_name.len);
	rn.new_name_len = cpu_to_le16(new_dentry->d_name.len);
	rn.reserved = 0;

	entry_size = sizeof(rn) + old_dentry->d_name.len + new_dentry->d_name.len;
	entry_data = kmalloc(entry_size, GFP_KERNEL);
	if (!entry_data)
		return -ENOMEM;

	memcpy(entry_data, &rn, sizeof(rn));
	memcpy(entry_data + sizeof(rn), old_dentry->d_name.name,
	       old_dentry->d_name.len);
	memcpy(entry_data + sizeof(rn) + old_dentry->d_name.len,
	       new_dentry->d_name.name, new_dentry->d_name.len);

	ret = daxfs_delta_append(branch, DAXFS_DELTA_RENAME, inode->i_ino,
				 entry_data, entry_size);
	kfree(entry_data);

	return ret;
}

static int daxfs_iterate(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	struct super_block *sb = dir->i_sb;
	struct daxfs_info *info = DAXFS_SB(sb);
	struct daxfs_branch_ctx *branch;
	loff_t pos = 2;  /* Start after . and .. */

	if (!dir_emit_dots(file, ctx))
		return 0;

	/* First emit entries from base image (if not deleted) */
	if (info->base_inodes && dir->i_ino <= info->base_inode_count) {
		struct daxfs_base_inode *dir_raw = &info->base_inodes[dir->i_ino - 1];
		u32 child_ino = le32_to_cpu(dir_raw->first_child);

		while (child_ino && child_ino <= info->base_inode_count) {
			struct daxfs_base_inode *child = &info->base_inodes[child_ino - 1];
			bool deleted = false;
			char *name;
			u32 name_len, mode;
			unsigned char dtype;

			/* Check if deleted in any branch */
			for (branch = info->current_branch; branch; branch = branch->parent) {
				if (daxfs_delta_is_deleted(branch, child_ino)) {
					deleted = true;
					break;
				}
			}

			if (!deleted) {
				if (pos >= ctx->pos) {
					name = info->base_strtab + le32_to_cpu(child->name_offset);
					name_len = le32_to_cpu(child->name_len);
					mode = le32_to_cpu(child->mode);

					switch (mode & S_IFMT) {
					case S_IFREG: dtype = DT_REG; break;
					case S_IFDIR: dtype = DT_DIR; break;
					case S_IFLNK: dtype = DT_LNK; break;
					default: dtype = DT_UNKNOWN; break;
					}

					if (!dir_emit(ctx, name, name_len, child_ino, dtype))
						return 0;
					ctx->pos = pos + 1;
				}
				pos++;
			}
			child_ino = le32_to_cpu(child->next_sibling);
		}
	}

	/* Then emit entries from delta logs */
	for (branch = info->current_branch; branch; branch = branch->parent) {
		u64 offset = 0;

		while (offset < branch->delta_size) {
			struct daxfs_delta_hdr *hdr = branch->delta_log + offset;
			u32 type = le32_to_cpu(hdr->type);
			u32 total_size = le32_to_cpu(hdr->total_size);

			if (total_size == 0)
				break;

			if ((type == DAXFS_DELTA_CREATE || type == DAXFS_DELTA_MKDIR)) {
				struct daxfs_delta_create *cr = (void *)hdr + sizeof(*hdr);

				if (le64_to_cpu(cr->parent_ino) == dir->i_ino) {
					char *name = (char *)(cr + 1);
					u16 name_len = le16_to_cpu(cr->name_len);
					u64 ino = le64_to_cpu(cr->new_ino);
					u32 mode = le32_to_cpu(cr->mode);
					unsigned char dtype;
					bool deleted = false;

					/* Check if subsequently deleted */
					struct daxfs_branch_ctx *b2;
					for (b2 = info->current_branch; b2 != branch; b2 = b2->parent) {
						if (daxfs_delta_is_deleted(b2, ino)) {
							deleted = true;
							break;
						}
					}

					if (!deleted) {
						if (pos >= ctx->pos) {
							switch (mode & S_IFMT) {
							case S_IFREG: dtype = DT_REG; break;
							case S_IFDIR: dtype = DT_DIR; break;
							case S_IFLNK: dtype = DT_LNK; break;
							default: dtype = DT_UNKNOWN; break;
							}

							if (!dir_emit(ctx, name, name_len, ino, dtype))
								return 0;
							ctx->pos = pos + 1;
						}
						pos++;
					}
				}
			}

			offset += total_size;
		}
	}

	return 0;
}

const struct inode_operations daxfs_dir_inode_ops = {
	.lookup		= daxfs_lookup,
	.create		= daxfs_create,
	.mkdir		= daxfs_mkdir,
	.unlink		= daxfs_unlink,
	.rmdir		= daxfs_rmdir,
	.rename		= daxfs_rename,
};

const struct file_operations daxfs_dir_ops = {
	.iterate_shared	= daxfs_iterate,
	.read		= generic_read_dir,
	.llseek		= generic_file_llseek,
};
