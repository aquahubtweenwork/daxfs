// SPDX-License-Identifier: GPL-2.0
/*
 * daxfs DAX memory storage layer
 *
 * This module provides the storage abstraction for DAXFS, handling
 * memory mapping (memremap/dma-buf), region allocation, and pointer
 * access. This layer enables clean separation between the VFS/branch
 * logic and the underlying DAX memory management.
 *
 * Copyright (C) 2026 Multikernel Technologies, Inc. All rights reserved.
 */

#include <linux/io.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include "daxfs.h"

/**
 * daxfs_mem_init_dmabuf - Initialize storage from dma-buf
 * @info: filesystem info structure to initialize
 * @dmabuf_file: file reference to dma-buf
 *
 * Maps a dma-buf into kernel virtual address space for use as
 * the backing store for the filesystem.
 *
 * Returns 0 on success, negative error code on failure.
 */
int daxfs_mem_init_dmabuf(struct daxfs_info *info, struct file *dmabuf_file)
{
	struct dma_buf *dmabuf;
	int ret;

	dmabuf = dmabuf_file->private_data;
	if (!dmabuf || dmabuf->file != dmabuf_file) {
		pr_err("daxfs: not a dma-buf fd\n");
		return -EINVAL;
	}

	get_dma_buf(dmabuf);
	ret = dma_buf_vmap(dmabuf, &info->dma_map);
	if (ret) {
		dma_buf_put(dmabuf);
		return ret;
	}

	info->dmabuf = dmabuf;
	info->mem = info->dma_map.vaddr;
	info->size = dmabuf->size;

	return 0;
}

/**
 * daxfs_mem_init_phys - Initialize storage from physical address
 * @info: filesystem info structure to initialize
 * @phys_addr: physical address of the DAX region
 * @size: size of the DAX region in bytes
 *
 * Maps a physical memory region using memremap for use as
 * the backing store for the filesystem.
 *
 * Returns 0 on success, negative error code on failure.
 */
int daxfs_mem_init_phys(struct daxfs_info *info, phys_addr_t phys_addr,
			size_t size)
{
	info->phys_addr = phys_addr;
	info->size = size;

	info->mem = memremap(phys_addr, size, MEMREMAP_WB);
	if (!info->mem) {
		pr_err("daxfs: failed to map %pa size %zu\n",
		       &phys_addr, size);
		return -ENOMEM;
	}

	return 0;
}

/**
 * daxfs_mem_exit - Release storage mapping
 * @info: filesystem info structure
 *
 * Releases the memory mapping, handling both dma-buf and
 * memremap cases appropriately.
 */
void daxfs_mem_exit(struct daxfs_info *info)
{
	if (info->dmabuf) {
		dma_buf_vunmap(info->dmabuf, &info->dma_map);
		dma_buf_put(info->dmabuf);
		info->dmabuf = NULL;
	} else if (info->mem) {
		memunmap(info->mem);
	}
	info->mem = NULL;
}

/**
 * daxfs_mem_ptr - Convert offset to pointer
 * @info: filesystem info structure
 * @offset: byte offset from start of DAX region
 *
 * Converts a byte offset into the DAX region to a kernel
 * virtual address pointer.
 *
 * Returns pointer to the specified offset, or NULL if offset
 * is beyond the mapped region.
 */
void *daxfs_mem_ptr(struct daxfs_info *info, u64 offset)
{
	if (offset >= info->size)
		return NULL;
	return info->mem + offset;
}

/**
 * daxfs_mem_offset - Convert pointer to offset
 * @info: filesystem info structure
 * @ptr: pointer within DAX region
 *
 * Converts a kernel virtual address pointer to a byte offset
 * from the start of the DAX region.
 *
 * Returns byte offset, or -1 if pointer is outside the region.
 */
u64 daxfs_mem_offset(struct daxfs_info *info, void *ptr)
{
	if (ptr < info->mem || ptr >= info->mem + info->size)
		return (u64)-1;
	return ptr - info->mem;
}

/**
 * daxfs_mem_phys - Get physical address for offset
 * @info: filesystem info structure
 * @offset: byte offset from start of DAX region
 *
 * Returns the physical address corresponding to the given offset.
 * Only valid when mounted via phys/size (not dma-buf).
 *
 * Returns physical address, or 0 if using dma-buf.
 */
phys_addr_t daxfs_mem_phys(struct daxfs_info *info, u64 offset)
{
	if (info->dmabuf)
		return 0;
	return info->phys_addr + offset;
}

/**
 * daxfs_mem_sync - Ensure writes are visible
 * @info: filesystem info structure
 * @ptr: pointer to start of region to sync
 * @size: size of region to sync in bytes
 *
 * Ensures that writes to the DAX region are visible to other
 * observers. For DAX memory, this may involve cache line flushes.
 *
 * Note: Current implementation is a no-op as MEMREMAP_WB provides
 * write-back caching. If needed for specific platforms, add
 * clflush/clwb instructions here.
 */
void daxfs_mem_sync(struct daxfs_info *info, void *ptr, size_t size)
{
	/*
	 * For persistent memory with ADR (Asynchronous DRAM Refresh),
	 * writes are automatically persisted. For other platforms,
	 * cache line flushes may be needed:
	 *
	 * arch_wb_cache_pmem(ptr, size);
	 *
	 * Currently a no-op - can be extended for specific hardware.
	 */
	(void)info;
	(void)ptr;
	(void)size;
}

/**
 * daxfs_mem_alloc_region - Allocate space in delta region
 * @info: filesystem info structure
 * @size: size to allocate in bytes
 *
 * Allocates contiguous space from the delta region for storing
 * delta log entries. Thread-safe via alloc_lock.
 *
 * Returns byte offset of allocated region, or 0 if out of space.
 */
u64 daxfs_mem_alloc_region(struct daxfs_info *info, size_t size)
{
	u64 offset;
	u64 end;
	u64 region_end;

	spin_lock(&info->alloc_lock);

	offset = info->delta_alloc_offset;
	end = offset + size;
	region_end = le64_to_cpu(info->super->delta_region_offset) +
		     le64_to_cpu(info->super->delta_region_size);

	if (end > region_end) {
		spin_unlock(&info->alloc_lock);
		return 0;	/* Out of space */
	}

	info->delta_alloc_offset = end;
	info->super->delta_alloc_offset = cpu_to_le64(end);

	spin_unlock(&info->alloc_lock);

	return offset;
}

/**
 * daxfs_mem_free_region - Mark delta region space as free
 * @info: filesystem info structure
 * @offset: byte offset of region to free
 * @size: size of region in bytes
 *
 * Marks space in the delta region as free for potential reclamation.
 * Note: Current implementation is a no-op - proper space reclamation
 * would require a more sophisticated allocator.
 */
void daxfs_mem_free_region(struct daxfs_info *info, u64 offset, u64 size)
{
	/* TODO: Implement proper space reclamation/GC */
	(void)info;
	(void)offset;
	(void)size;
}
