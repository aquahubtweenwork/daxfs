// SPDX-License-Identifier: GPL-2.0
/*
 * daxfs file operations
 *
 * Copyright (C) 2026 Multikernel Technologies, Inc. All rights reserved.
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/uio.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include "daxfs.h"

static ssize_t daxfs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct super_block *sb = inode->i_sb;
	loff_t pos = iocb->ki_pos;
	size_t count = iov_iter_count(to);
	size_t total = 0;

	if (pos >= inode->i_size)
		return 0;

	if (pos + count > inode->i_size)
		count = inode->i_size - pos;

	while (count > 0) {
		size_t chunk;
		void *src;

		src = daxfs_resolve_file_data(sb, inode->i_ino, pos, count, &chunk);
		if (!src || chunk == 0)
			break;

		if (copy_to_iter(src, chunk, to) != chunk)
			return total ? total : -EFAULT;

		pos += chunk;
		count -= chunk;
		total += chunk;
	}

	iocb->ki_pos = pos;
	return total;
}

static ssize_t daxfs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct super_block *sb = inode->i_sb;
	struct daxfs_info *info = DAXFS_SB(sb);
	struct daxfs_branch_ctx *branch = info->current_branch;
	loff_t pos = iocb->ki_pos;
	size_t len = iov_iter_count(from);
	size_t entry_size;
	void *entry;
	struct daxfs_delta_hdr *hdr;
	struct daxfs_delta_write *wr;
	void *data;

	if (len == 0)
		return 0;

	/* Allocate space for delta entry */
	entry_size = sizeof(struct daxfs_delta_hdr) +
		     sizeof(struct daxfs_delta_write) + len;

	entry = daxfs_delta_alloc(info, branch, entry_size);
	if (!entry)
		return -ENOSPC;

	/* Fill header */
	hdr = entry;
	hdr->type = cpu_to_le32(DAXFS_DELTA_WRITE);
	hdr->total_size = cpu_to_le32(entry_size);
	hdr->ino = cpu_to_le64(inode->i_ino);
	hdr->timestamp = cpu_to_le64(ktime_get_real_ns());

	/* Fill write info */
	wr = (void *)(hdr + 1);
	wr->offset = cpu_to_le64(pos);
	wr->len = cpu_to_le32(len);
	wr->flags = 0;

	/* Copy data from user */
	data = (void *)(wr + 1);
	if (copy_from_iter(data, len, from) != len)
		return -EFAULT;

	/* Update inode size if extending */
	if (pos + len > inode->i_size) {
		inode->i_size = pos + len;
		DAXFS_I(inode)->delta_size = inode->i_size;
	}

	/* Manually update index for this write */
	{
		struct rb_node **link = &branch->inode_index.rb_node;
		struct rb_node *parent = NULL;
		struct daxfs_delta_inode_entry *ie;
		unsigned long flags;

		spin_lock_irqsave(&branch->index_lock, flags);

		while (*link) {
			parent = *link;
			ie = rb_entry(parent, struct daxfs_delta_inode_entry,
				      rb_node);

			if (inode->i_ino < ie->ino)
				link = &parent->rb_left;
			else if (inode->i_ino > ie->ino)
				link = &parent->rb_right;
			else {
				/* Update existing */
				ie->hdr = hdr;
				if (pos + len > ie->size)
					ie->size = pos + len;
				spin_unlock_irqrestore(&branch->index_lock, flags);
				goto done;
			}
		}

		ie = kzalloc(sizeof(*ie), GFP_ATOMIC);
		if (ie) {
			ie->ino = inode->i_ino;
			ie->hdr = hdr;
			ie->size = pos + len;
			ie->mode = inode->i_mode;
			ie->deleted = false;
			rb_link_node(&ie->rb_node, parent, link);
			rb_insert_color(&ie->rb_node, &branch->inode_index);
		}
		spin_unlock_irqrestore(&branch->index_lock, flags);
	}

done:
	iocb->ki_pos = pos + len;
	inode_set_mtime_to_ts(inode, inode_set_ctime_to_ts(inode, current_time(inode)));
	return len;
}

static int daxfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			 struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct super_block *sb = inode->i_sb;
	struct daxfs_info *info = DAXFS_SB(sb);
	struct daxfs_branch_ctx *branch = info->current_branch;
	int ret;

	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;

	/* Handle truncate */
	if (attr->ia_valid & ATTR_SIZE) {
		struct daxfs_delta_truncate tr;

		tr.new_size = cpu_to_le64(attr->ia_size);

		ret = daxfs_delta_append(branch, DAXFS_DELTA_TRUNCATE,
					 inode->i_ino, &tr, sizeof(tr));
		if (ret)
			return ret;

		i_size_write(inode, attr->ia_size);
		DAXFS_I(inode)->delta_size = attr->ia_size;
	}

	/* Handle mode/uid/gid changes */
	if (attr->ia_valid & (ATTR_MODE | ATTR_UID | ATTR_GID)) {
		struct daxfs_delta_setattr sa = {0};

		if (attr->ia_valid & ATTR_MODE) {
			sa.mode = cpu_to_le32(attr->ia_mode);
			sa.valid |= cpu_to_le32(DAXFS_ATTR_MODE);
		}
		if (attr->ia_valid & ATTR_UID) {
			sa.uid = cpu_to_le32(from_kuid(&init_user_ns, attr->ia_uid));
			sa.valid |= cpu_to_le32(DAXFS_ATTR_UID);
		}
		if (attr->ia_valid & ATTR_GID) {
			sa.gid = cpu_to_le32(from_kgid(&init_user_ns, attr->ia_gid));
			sa.valid |= cpu_to_le32(DAXFS_ATTR_GID);
		}

		ret = daxfs_delta_append(branch, DAXFS_DELTA_SETATTR,
					 inode->i_ino, &sa, sizeof(sa));
		if (ret)
			return ret;
	}

	setattr_copy(idmap, inode, attr);
	return 0;
}

static int daxfs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	struct super_block *sb = inode->i_sb;
	loff_t pos = folio_pos(folio);
	size_t len = folio_size(folio);
	size_t filled = 0;

	if (pos >= inode->i_size) {
		folio_zero_range(folio, 0, len);
		goto out;
	}

	while (filled < len && pos + filled < inode->i_size) {
		size_t chunk;
		void *src;

		src = daxfs_resolve_file_data(sb, inode->i_ino,
					      pos + filled, len - filled, &chunk);
		if (!src || chunk == 0) {
			/* Hole or EOF */
			break;
		}

		memcpy_to_folio(folio, filled, src, chunk);
		filled += chunk;
	}

	if (filled < len)
		folio_zero_range(folio, filled, len - filled);

out:
	folio_mark_uptodate(folio);
	folio_unlock(folio);
	return 0;
}

const struct address_space_operations daxfs_aops = {
	.read_folio	= daxfs_read_folio,
};

const struct file_operations daxfs_file_ops = {
	.llseek		= generic_file_llseek,
	.read_iter	= daxfs_read_iter,
	.write_iter	= daxfs_write_iter,
	.splice_read	= filemap_splice_read,
};

const struct inode_operations daxfs_file_inode_ops = {
	.getattr	= simple_getattr,
	.setattr	= daxfs_setattr,
};
