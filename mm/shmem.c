/*
 * Resizable virtual memory filesystem for Linux.
 *
 * Copyright (C) 2000 Linus Torvalds.
 *		 2000 Transmeta Corp.
 *		 2000-2001 Christoph Rohland
 *		 2000-2001 SAP AG
 *		 2002 Red Hat Inc.
 * Copyright (C) 2002-2005 Hugh Dickins.
 * Copyright (C) 2002-2005 VERITAS Software Corporation.
 * Copyright (C) 2004 Andi Kleen, SuSE Labs
 *
 * Extended attribute support for tmpfs:
 * Copyright (c) 2004, Luke Kenneth Casson Leighton <lkcl@lkcl.net>
 * Copyright (c) 2004 Red Hat, Inc., James Morris <jmorris@redhat.com>
 *
 * tiny-shmem:
 * Copyright (c) 2004, 2008 Matt Mackall <mpm@selenic.com>
 *
 * This file is released under the GPL.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/vfs.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/percpu_counter.h>
#include <linux/swap.h>

static struct vfsmount *shm_mnt;

#ifdef CONFIG_SHMEM
/*
 * This virtual memory filesystem is heavily based on the ramfs. It
 * extends ramfs by the ability to use swap and honor resource limits
 * which makes it a completely usable filesystem.
 */

#include <linux/xattr.h>
#include <linux/exportfs.h>
#include <linux/posix_acl.h>
#include <linux/generic_acl.h>
#include <linux/mman.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/shmem_fs.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/splice.h>
#include <linux/security.h>
#include <linux/swapops.h>
#include <linux/mempolicy.h>
#include <linux/namei.h>
#include <linux/ctype.h>
#include <linux/migrate.h>
#include <linux/highmem.h>
#include <linux/seq_file.h>
#include <linux/magic.h>

#include <asm/uaccess.h>
#include <asm/div64.h>
#include <asm/pgtable.h>

/*
 * The maximum size of a shmem/tmpfs file is limited by the maximum size of
 * its triple-indirect swap vector - see illustration at shmem_swp_entry().
 *
 * With 4kB page size, maximum file size is just over 2TB on a 32-bit kernel,
 * but one eighth of that on a 64-bit kernel.  With 8kB page size, maximum
 * file size is just over 4TB on a 64-bit kernel, but 16TB on a 32-bit kernel,
 * MAX_LFS_FILESIZE being then more restrictive than swap vector layout.
 *
 * We use / and * instead of shifts in the definitions below, so that the swap
 * vector can be tested with small even values (e.g. 20) for ENTRIES_PER_PAGE.
 */
#define ENTRIES_PER_PAGE (PAGE_CACHE_SIZE/sizeof(unsigned long))
#define ENTRIES_PER_PAGEPAGE ((unsigned long long)ENTRIES_PER_PAGE*ENTRIES_PER_PAGE)

#define SHMSWP_MAX_INDEX (SHMEM_NR_DIRECT + (ENTRIES_PER_PAGEPAGE/2) * (ENTRIES_PER_PAGE+1))
#define SHMSWP_MAX_BYTES (SHMSWP_MAX_INDEX << PAGE_CACHE_SHIFT)

#define SHMEM_MAX_BYTES  min_t(unsigned long long, SHMSWP_MAX_BYTES, MAX_LFS_FILESIZE)
#define SHMEM_MAX_INDEX  ((unsigned long)((SHMEM_MAX_BYTES+1) >> PAGE_CACHE_SHIFT))

#define BLOCKS_PER_PAGE  (PAGE_CACHE_SIZE/512)
#define VM_ACCT(size)    (PAGE_CACHE_ALIGN(size) >> PAGE_SHIFT)

/* info->flags needs VM_flags to handle pagein/truncate races efficiently */
#define SHMEM_PAGEIN	 VM_READ
#define SHMEM_TRUNCATE	 VM_WRITE

/* Definition to limit shmem_truncate's steps between cond_rescheds */
#define LATENCY_LIMIT	 64

/* Pretend that each entry is of this size in directory's i_size */
#define BOGO_DIRENT_SIZE 20

struct shmem_xattr {
	struct list_head list;	/* anchored by shmem_inode_info->xattr_list */
	char *name;		/* xattr name */
	size_t size;
	char value[0];
};

/* Flag allocation requirements to shmem_getpage and shmem_swp_alloc */
enum sgp_type {
	SGP_READ,	/* don't exceed i_size, don't allocate page */
	SGP_CACHE,	/* don't exceed i_size, may allocate page */
	SGP_DIRTY,	/* like SGP_CACHE, but set new page dirty */
	SGP_WRITE,	/* may exceed i_size, may allocate page */
};

#ifdef CONFIG_TMPFS
static unsigned long shmem_default_max_blocks(void)
{
	return totalram_pages / 2;
}

static unsigned long shmem_default_max_inodes(void)
{
	return min(totalram_pages - totalhigh_pages, totalram_pages / 2);
}
#endif

static int shmem_getpage_gfp(struct inode *inode, pgoff_t index,
	struct page **pagep, enum sgp_type sgp, gfp_t gfp, int *fault_type);

static inline int shmem_getpage(struct inode *inode, pgoff_t index,
	struct page **pagep, enum sgp_type sgp, int *fault_type)
{
	return shmem_getpage_gfp(inode, index, pagep, sgp,
			mapping_gfp_mask(inode->i_mapping), fault_type);
}

static inline struct page *shmem_dir_alloc(gfp_t gfp_mask)
{
	/*
	 * The above definition of ENTRIES_PER_PAGE, and the use of
	 * BLOCKS_PER_PAGE on indirect pages, assume PAGE_CACHE_SIZE:
	 * might be reconsidered if it ever diverges from PAGE_SIZE.
	 *
	 * Mobility flags are masked out as swap vectors cannot move
	 */
	return alloc_pages((gfp_mask & ~GFP_MOVABLE_MASK) | __GFP_ZERO,
				PAGE_CACHE_SHIFT-PAGE_SHIFT);
}

static inline void shmem_dir_free(struct page *page)
{
	__free_pages(page, PAGE_CACHE_SHIFT-PAGE_SHIFT);
}

static struct page **shmem_dir_map(struct page *page)
{
	return (struct page **)kmap_atomic(page, KM_USER0);
}

static inline void shmem_dir_unmap(struct page **dir)
{
	kunmap_atomic(dir, KM_USER0);
}

static swp_entry_t *shmem_swp_map(struct page *page)
{
	return (swp_entry_t *)kmap_atomic(page, KM_USER1);
}

static inline void shmem_swp_balance_unmap(void)
{
	/*
	 * When passing a pointer to an i_direct entry, to code which
	 * also handles indirect entries and so will shmem_swp_unmap,
	 * we must arrange for the preempt count to remain in balance.
	 * What kmap_atomic of a lowmem page does depends on config
	 * and architecture, so pretend to kmap_atomic some lowmem page.
	 */
	(void) kmap_atomic(ZERO_PAGE(0), KM_USER1);
}

static inline void shmem_swp_unmap(swp_entry_t *entry)
{
	kunmap_atomic(entry, KM_USER1);
}

static inline struct shmem_sb_info *SHMEM_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

/*
 * shmem_file_setup pre-accounts the whole fixed size of a VM object,
 * for shared memory and for shared anonymous (/dev/zero) mappings
 * (unless MAP_NORESERVE and sysctl_overcommit_memory <= 1),
 * consistent with the pre-accounting of private mappings ...
 */
static inline int shmem_acct_size(unsigned long flags, loff_t size)
{
	return (flags & VM_NORESERVE) ?
		0 : security_vm_enough_memory_kern(VM_ACCT(size));
}

static inline void shmem_unacct_size(unsigned long flags, loff_t size)
{
	if (!(flags & VM_NORESERVE))
		vm_unacct_memory(VM_ACCT(size));
}

/*
 * ... whereas tmpfs objects are accounted incrementally as
 * pages are allocated, in order to allow huge sparse files.
 * shmem_getpage reports shmem_acct_block failure as -ENOSPC not -ENOMEM,
 * so that a failure on a sparse tmpfs mapping will give SIGBUS not OOM.
 */
static inline int shmem_acct_block(unsigned long flags)
{
	return (flags & VM_NORESERVE) ?
		security_vm_enough_memory_kern(VM_ACCT(PAGE_CACHE_SIZE)) : 0;
}

static inline void shmem_unacct_blocks(unsigned long flags, long pages)
{
	if (flags & VM_NORESERVE)
		vm_unacct_memory(pages * VM_ACCT(PAGE_CACHE_SIZE));
}

static const struct super_operations shmem_ops;
static const struct address_space_operations shmem_aops;
static const struct file_operations shmem_file_operations;
static const struct inode_operations shmem_inode_operations;
static const struct inode_operations shmem_dir_inode_operations;
static const struct inode_operations shmem_special_inode_operations;
static const struct vm_operations_struct shmem_vm_ops;

static struct backing_dev_info shmem_backing_dev_info  __read_mostly = {
	.ra_pages	= 0,	/* No readahead */
	.capabilities	= BDI_CAP_NO_ACCT_AND_WRITEBACK | BDI_CAP_SWAP_BACKED,
};

static LIST_HEAD(shmem_swaplist);
static DEFINE_MUTEX(shmem_swaplist_mutex);

static void shmem_free_blocks(struct inode *inode, long pages)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(inode->i_sb);
	if (sbinfo->max_blocks) {
		percpu_counter_add(&sbinfo->used_blocks, -pages);
		inode->i_blocks -= pages*BLOCKS_PER_PAGE;
	}
}

static int shmem_reserve_inode(struct super_block *sb)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(sb);
	if (sbinfo->max_inodes) {
		spin_lock(&sbinfo->stat_lock);
		if (!sbinfo->free_inodes) {
			spin_unlock(&sbinfo->stat_lock);
			return -ENOSPC;
		}
		sbinfo->free_inodes--;
		spin_unlock(&sbinfo->stat_lock);
	}
	return 0;
}

static void shmem_free_inode(struct super_block *sb)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(sb);
	if (sbinfo->max_inodes) {
		spin_lock(&sbinfo->stat_lock);
		sbinfo->free_inodes++;
		spin_unlock(&sbinfo->stat_lock);
	}
}

/**
 * shmem_recalc_inode - recalculate the size of an inode
 * @inode: inode to recalc
 *
 * We have to calculate the free blocks since the mm can drop
 * undirtied hole pages behind our back.
 *
 * But normally   info->alloced == inode->i_mapping->nrpages + info->swapped
 * So mm freed is info->alloced - (inode->i_mapping->nrpages + info->swapped)
 *
 * It has to be called with the spinlock held.
 */
static void shmem_recalc_inode(struct inode *inode)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	long freed;

	freed = info->alloced - info->swapped - inode->i_mapping->nrpages;
	if (freed > 0) {
		info->alloced -= freed;
		shmem_unacct_blocks(info->flags, freed);
		shmem_free_blocks(inode, freed);
	}
}

/**
 * shmem_swp_entry - find the swap vector position in the info structure
 * @info:  info structure for the inode
 * @index: index of the page to find
 * @page:  optional page to add to the structure. Has to be preset to
 *         all zeros
 *
 * If there is no space allocated yet it will return NULL when
 * page is NULL, else it will use the page for the needed block,
 * setting it to NULL on return to indicate that it has been used.
 *
 * The swap vector is organized the following way:
 *
 * There are SHMEM_NR_DIRECT entries directly stored in the
 * shmem_inode_info structure. So small files do not need an addional
 * allocation.
 *
 * For pages with index > SHMEM_NR_DIRECT there is the pointer
 * i_indirect which points to a page which holds in the first half
 * doubly indirect blocks, in the second half triple indirect blocks:
 *
 * For an artificial ENTRIES_PER_PAGE = 4 this would lead to the
 * following layout (for SHMEM_NR_DIRECT == 16):
 *
 * i_indirect -> dir --> 16-19
 * 	      |	     +-> 20-23
 * 	      |
 * 	      +-->dir2 --> 24-27
 * 	      |	       +-> 28-31
 * 	      |	       +-> 32-35
 * 	      |	       +-> 36-39
 * 	      |
 * 	      +-->dir3 --> 40-43
 * 	       	       +-> 44-47
 * 	      	       +-> 48-51
 * 	      	       +-> 52-55
 */
static swp_entry_t *shmem_swp_entry(struct shmem_inode_info *info, unsigned long index, struct page **page)
{
	unsigned long offset;
	struct page **dir;
	struct page *subdir;

	if (index < SHMEM_NR_DIRECT) {
		shmem_swp_balance_unmap();
		return info->i_direct+index;
	}
	if (!info->i_indirect) {
		if (page) {
			info->i_indirect = *page;
			*page = NULL;
		}
		return NULL;			/* need another page */
	}

	index -= SHMEM_NR_DIRECT;
	offset = index % ENTRIES_PER_PAGE;
	index /= ENTRIES_PER_PAGE;
	dir = shmem_dir_map(info->i_indirect);

	if (index >= ENTRIES_PER_PAGE/2) {
		index -= ENTRIES_PER_PAGE/2;
		dir += ENTRIES_PER_PAGE/2 + index/ENTRIES_PER_PAGE;
		index %= ENTRIES_PER_PAGE;
		subdir = *dir;
		if (!subdir) {
			if (page) {
				*dir = *page;
				*page = NULL;
			}
			shmem_dir_unmap(dir);
			return NULL;		/* need another page */
		}
		shmem_dir_unmap(dir);
		dir = shmem_dir_map(subdir);
	}

	dir += index;
	subdir = *dir;
	if (!subdir) {
		if (!page || !(subdir = *page)) {
			shmem_dir_unmap(dir);
			return NULL;		/* need a page */
		}
		*dir = subdir;
		*page = NULL;
	}
	shmem_dir_unmap(dir);
	return shmem_swp_map(subdir) + offset;
}

static void shmem_swp_set(struct shmem_inode_info *info, swp_entry_t *entry, unsigned long value)
{
	long incdec = value? 1: -1;

	entry->val = value;
	info->swapped += incdec;
	if ((unsigned long)(entry - info->i_direct) >= SHMEM_NR_DIRECT) {
		struct page *page = kmap_atomic_to_page(entry);
		set_page_private(page, page_private(page) + incdec);
	}
}

/**
 * shmem_swp_alloc - get the position of the swap entry for the page.
 * @info:	info structure for the inode
 * @index:	index of the page to find
 * @sgp:	check and recheck i_size? skip allocation?
 * @gfp:	gfp mask to use for any page allocation
 *
 * If the entry does not exist, allocate it.
 */
static swp_entry_t *shmem_swp_alloc(struct shmem_inode_info *info,
			unsigned long index, enum sgp_type sgp, gfp_t gfp)
{
	struct inode *inode = &info->vfs_inode;
	struct shmem_sb_info *sbinfo = SHMEM_SB(inode->i_sb);
	struct page *page = NULL;
	swp_entry_t *entry;

	if (sgp != SGP_WRITE &&
	    ((loff_t) index << PAGE_CACHE_SHIFT) >= i_size_read(inode))
		return ERR_PTR(-EINVAL);

	while (!(entry = shmem_swp_entry(info, index, &page))) {
		if (sgp == SGP_READ)
			return shmem_swp_map(ZERO_PAGE(0));
		/*
		 * Test used_blocks against 1 less max_blocks, since we have 1 data
		 * page (and perhaps indirect index pages) yet to allocate:
		 * a waste to allocate index if we cannot allocate data.
		 */
		if (sbinfo->max_blocks) {
			if (percpu_counter_compare(&sbinfo->used_blocks,
						sbinfo->max_blocks - 1) >= 0)
				return ERR_PTR(-ENOSPC);
			percpu_counter_inc(&sbinfo->used_blocks);
			inode->i_blocks += BLOCKS_PER_PAGE;
		}

		spin_unlock(&info->lock);
		page = shmem_dir_alloc(gfp);
		spin_lock(&info->lock);

		if (!page) {
			shmem_free_blocks(inode, 1);
			return ERR_PTR(-ENOMEM);
		}
		if (sgp != SGP_WRITE &&
		    ((loff_t) index << PAGE_CACHE_SHIFT) >= i_size_read(inode)) {
			entry = ERR_PTR(-EINVAL);
			break;
		}
		if (info->next_index <= index)
			info->next_index = index + 1;
	}
	if (page) {
		/* another task gave its page, or truncated the file */
		shmem_free_blocks(inode, 1);
		shmem_dir_free(page);
	}
	if (info->next_index <= index && !IS_ERR(entry))
		info->next_index = index + 1;
	return entry;
}

/**
 * shmem_free_swp - free some swap entries in a directory
 * @dir:        pointer to the directory
 * @edir:       pointer after last entry of the directory
 * @punch_lock: pointer to spinlock when needed for the holepunch case
 */
static int shmem_free_swp(swp_entry_t *dir, swp_entry_t *edir,
						spinlock_t *punch_lock)
{
	spinlock_t *punch_unlock = NULL;
	swp_entry_t *ptr;
	int freed = 0;

	for (ptr = dir; ptr < edir; ptr++) {
		if (ptr->val) {
			if (unlikely(punch_lock)) {
				punch_unlock = punch_lock;
				punch_lock = NULL;
				spin_lock(punch_unlock);
				if (!ptr->val)
					continue;
			}
			free_swap_and_cache(*ptr);
			*ptr = (swp_entry_t){0};
			freed++;
		}
	}
	if (punch_unlock)
		spin_unlock(punch_unlock);
	return freed;
}

static int shmem_map_and_free_swp(struct page *subdir, int offset,
		int limit, struct page ***dir, spinlock_t *punch_lock)
{
	swp_entry_t *ptr;
	int freed = 0;

	ptr = shmem_swp_map(subdir);
	for (; offset < limit; offset += LATENCY_LIMIT) {
		int size = limit - offset;
		if (size > LATENCY_LIMIT)
			size = LATENCY_LIMIT;
		freed += shmem_free_swp(ptr+offset, ptr+offset+size,
							punch_lock);
		if (need_resched()) {
			shmem_swp_unmap(ptr);
			if (*dir) {
				shmem_dir_unmap(*dir);
				*dir = NULL;
			}
			cond_resched();
			ptr = shmem_swp_map(subdir);
		}
	}
	shmem_swp_unmap(ptr);
	return freed;
}

static void shmem_free_pages(struct list_head *next)
{
	struct page *page;
	int freed = 0;

	do {
		page = container_of(next, struct page, lru);
		next = next->next;
		shmem_dir_free(page);
		freed++;
		if (freed >= LATENCY_LIMIT) {
			cond_resched();
			freed = 0;
		}
	} while (next);
}

void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	unsigned long idx;
	unsigned long size;
	unsigned long limit;
	unsigned long stage;
	unsigned long diroff;
	struct page **dir;
	struct page *topdir;
	struct page *middir;
	struct page *subdir;
	swp_entry_t *ptr;
	LIST_HEAD(pages_to_free);
	long nr_pages_to_free = 0;
	long nr_swaps_freed = 0;
	int offset;
	int freed;
	int punch_hole;
	spinlock_t *needs_lock;
	spinlock_t *punch_lock;
	unsigned long upper_limit;

	truncate_inode_pages_range(inode->i_mapping, start, end);

	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	idx = (start + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	if (idx >= info->next_index)
		return;

	spin_lock(&info->lock);
	info->flags |= SHMEM_TRUNCATE;
	if (likely(end == (loff_t) -1)) {
		limit = info->next_index;
		upper_limit = SHMEM_MAX_INDEX;
		info->next_index = idx;
		needs_lock = NULL;
		punch_hole = 0;
	} else {
		if (end + 1 >= inode->i_size) {	/* we may free a little more */
			limit = (inode->i_size + PAGE_CACHE_SIZE - 1) >>
							PAGE_CACHE_SHIFT;
			upper_limit = SHMEM_MAX_INDEX;
		} else {
			limit = (end + 1) >> PAGE_CACHE_SHIFT;
			upper_limit = limit;
		}
		needs_lock = &info->lock;
		punch_hole = 1;
	}

	topdir = info->i_indirect;
	if (topdir && idx <= SHMEM_NR_DIRECT && !punch_hole) {
		info->i_indirect = NULL;
		nr_pages_to_free++;
		list_add(&topdir->lru, &pages_to_free);
	}
	spin_unlock(&info->lock);

	if (info->swapped && idx < SHMEM_NR_DIRECT) {
		ptr = info->i_direct;
		size = limit;
		if (size > SHMEM_NR_DIRECT)
			size = SHMEM_NR_DIRECT;
		nr_swaps_freed = shmem_free_swp(ptr+idx, ptr+size, needs_lock);
	}

	/*
	 * If there are no indirect blocks or we are punching a hole
	 * below indirect blocks, nothing to be done.
	 */
	if (!topdir || limit <= SHMEM_NR_DIRECT)
		goto done2;

	/*
	 * The truncation case has already dropped info->lock, and we're safe
	 * because i_size and next_index have already been lowered, preventing
	 * access beyond.  But in the punch_hole case, we still need to take
	 * the lock when updating the swap directory, because there might be
	 * racing accesses by shmem_getpage(SGP_CACHE), shmem_unuse_inode or
	 * shmem_writepage.  However, whenever we find we can remove a whole
	 * directory page (not at the misaligned start or end of the range),
	 * we first NULLify its pointer in the level above, and then have no
	 * need to take the lock when updating its contents: needs_lock and
	 * punch_lock (either pointing to info->lock or NULL) manage this.
	 */

	upper_limit -= SHMEM_NR_DIRECT;
	limit -= SHMEM_NR_DIRECT;
	idx = (idx > SHMEM_NR_DIRECT)? (idx - SHMEM_NR_DIRECT): 0;
	offset = idx % ENTRIES_PER_PAGE;
	idx -= offset;

	dir = shmem_dir_map(topdir);
	stage = ENTRIES_PER_PAGEPAGE/2;
	if (idx < ENTRIES_PER_PAGEPAGE/2) {
		middir = topdir;
		diroff = idx/ENTRIES_PER_PAGE;
	} else {
		dir += ENTRIES_PER_PAGE/2;
		dir += (idx - ENTRIES_PER_PAGEPAGE/2)/ENTRIES_PER_PAGEPAGE;
		while (stage <= idx)
			stage += ENTRIES_PER_PAGEPAGE;
		middir = *dir;
		if (*dir) {
			diroff = ((idx - ENTRIES_PER_PAGEPAGE/2) %
				ENTRIES_PER_PAGEPAGE) / ENTRIES_PER_PAGE;
			if (!diroff && !offset && upper_limit >= stage) {
				if (needs_lock) {
					spin_lock(needs_lock);
					*dir = NULL;
					spin_unlock(needs_lock);
					needs_lock = NULL;
				} else
					*dir = NULL;
				nr_pages_to_free++;
				list_add(&middir->lru, &pages_to_free);
			}
			shmem_dir_unmap(dir);
			dir = shmem_dir_map(middir);
		} else {
			diroff = 0;
			offset = 0;
			idx = stage;
		}
	}

	for (; idx < limit; idx += ENTRIES_PER_PAGE, diroff++) {
		if (unlikely(idx == stage)) {
			shmem_dir_unmap(dir);
			dir = shmem_dir_map(topdir) +
			    ENTRIES_PER_PAGE/2 + idx/ENTRIES_PER_PAGEPAGE;
			while (!*dir) {
				dir++;
				idx += ENTRIES_PER_PAGEPAGE;
				if (idx >= limit)
					goto done1;
			}
			stage = idx + ENTRIES_PER_PAGEPAGE;
			middir = *dir;
			if (punch_hole)
				needs_lock = &info->lock;
			if (upper_limit >= stage) {
				if (needs_lock) {
					spin_lock(needs_lock);
					*dir = NULL;
					spin_unlock(needs_lock);
					needs_lock = NULL;
				} else
					*dir = NULL;
				nr_pages_to_free++;
				list_add(&middir->lru, &pages_to_free);
			}
			shmem_dir_unmap(dir);
			cond_resched();
			dir = shmem_dir_map(middir);
			diroff = 0;
		}
		punch_lock = needs_lock;
		subdir = dir[diroff];
		if (subdir && !offset && upper_limit-idx >= ENTRIES_PER_PAGE) {
			if (needs_lock) {
				spin_lock(needs_lock);
				dir[diroff] = NULL;
				spin_unlock(needs_lock);
				punch_lock = NULL;
			} else
				dir[diroff] = NULL;
			nr_pages_to_free++;
			list_add(&subdir->lru, &pages_to_free);
		}
		if (subdir && page_private(subdir) /* has swap entries */) {
			size = limit - idx;
			if (size > ENTRIES_PER_PAGE)
				size = ENTRIES_PER_PAGE;
			freed = shmem_map_and_free_swp(subdir,
					offset, size, &dir, punch_lock);
			if (!dir)
				dir = shmem_dir_map(middir);
			nr_swaps_freed += freed;
			if (offset || punch_lock) {
				spin_lock(&info->lock);
				set_page_private(subdir,
					page_private(subdir) - freed);
				spin_unlock(&info->lock);
			} else
				BUG_ON(page_private(subdir) != freed);
		}
		offset = 0;
	}
done1:
	shmem_dir_unmap(dir);
done2:
	if (inode->i_mapping->nrpages && (info->flags & SHMEM_PAGEIN)) {
		/*
		 * Call truncate_inode_pages again: racing shmem_unuse_inode
		 * may have swizzled a page in from swap since
		 * truncate_pagecache or generic_delete_inode did it, before we
		 * lowered next_index.  Also, though shmem_getpage checks
		 * i_size before adding to cache, no recheck after: so fix the
		 * narrow window there too.
		 */
		truncate_inode_pages_range(inode->i_mapping, start, end);
	}

	spin_lock(&info->lock);
	info->flags &= ~SHMEM_TRUNCATE;
	info->swapped -= nr_swaps_freed;
	if (nr_pages_to_free)
		shmem_free_blocks(inode, nr_pages_to_free);
	shmem_recalc_inode(inode);
	spin_unlock(&info->lock);

	/*
	 * Empty swap vector directory pages to be freed?
	 */
	if (!list_empty(&pages_to_free)) {
		pages_to_free.prev->next = NULL;
		shmem_free_pages(pages_to_free.next);
	}
}
EXPORT_SYMBOL_GPL(shmem_truncate_range);

static int shmem_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	int error;

	error = inode_change_ok(inode, attr);
	if (error)
		return error;

	if (S_ISREG(inode->i_mode) && (attr->ia_valid & ATTR_SIZE)) {
		loff_t oldsize = inode->i_size;
		loff_t newsize = attr->ia_size;
		struct page *page = NULL;

		if (newsize < oldsize) {
			/*
			 * If truncating down to a partial page, then
			 * if that page is already allocated, hold it
			 * in memory until the truncation is over, so
			 * truncate_partial_page cannot miss it were
			 * it assigned to swap.
			 */
			if (newsize & (PAGE_CACHE_SIZE-1)) {
				(void) shmem_getpage(inode,
					newsize >> PAGE_CACHE_SHIFT,
						&page, SGP_READ, NULL);
				if (page)
					unlock_page(page);
			}
			/*
			 * Reset SHMEM_PAGEIN flag so that shmem_truncate can
			 * detect if any pages might have been added to cache
			 * after truncate_inode_pages.  But we needn't bother
			 * if it's being fully truncated to zero-length: the
			 * nrpages check is efficient enough in that case.
			 */
			if (newsize) {
				struct shmem_inode_info *info = SHMEM_I(inode);
				spin_lock(&info->lock);
				info->flags &= ~SHMEM_PAGEIN;
				spin_unlock(&info->lock);
			}
		}
		if (newsize != oldsize) {
			i_size_write(inode, newsize);
			inode->i_ctime = inode->i_mtime = CURRENT_TIME;
		}
		if (newsize < oldsize) {
			loff_t holebegin = round_up(newsize, PAGE_SIZE);
			unmap_mapping_range(inode->i_mapping, holebegin, 0, 1);
			shmem_truncate_range(inode, newsize, (loff_t)-1);
			/* unmap again to remove racily COWed private pages */
			unmap_mapping_range(inode->i_mapping, holebegin, 0, 1);
		}
		if (page)
			page_cache_release(page);
	}

	setattr_copy(inode, attr);
#ifdef CONFIG_TMPFS_POSIX_ACL
	if (attr->ia_valid & ATTR_MODE)
		error = generic_acl_chmod(inode);
#endif
	return error;
}

static void shmem_evict_inode(struct inode *inode)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct shmem_xattr *xattr, *nxattr;

	if (inode->i_mapping->a_ops == &shmem_aops) {
		shmem_unacct_size(info->flags, inode->i_size);
		inode->i_size = 0;
		shmem_truncate_range(inode, 0, (loff_t)-1);
		if (!list_empty(&info->swaplist)) {
			mutex_lock(&shmem_swaplist_mutex);
			list_del_init(&info->swaplist);
			mutex_unlock(&shmem_swaplist_mutex);
		}
	}

	list_for_each_entry_safe(xattr, nxattr, &info->xattr_list, list) {
		kfree(xattr->name);
		kfree(xattr);
	}
	BUG_ON(inode->i_blocks);
	shmem_free_inode(inode->i_sb);
	end_writeback(inode);
}

static inline int shmem_find_swp(swp_entry_t entry, swp_entry_t *dir, swp_entry_t *edir)
{
	swp_entry_t *ptr;

	for (ptr = dir; ptr < edir; ptr++) {
		if (ptr->val == entry.val)
			return ptr - dir;
	}
	return -1;
}

static int shmem_unuse_inode(struct shmem_inode_info *info, swp_entry_t entry, struct page *page)
{
	struct address_space *mapping;
	unsigned long idx;
	unsigned long size;
	unsigned long limit;
	unsigned long stage;
	struct page **dir;
	struct page *subdir;
	swp_entry_t *ptr;
	int offset;
	int error;

	idx = 0;
	ptr = info->i_direct;
	spin_lock(&info->lock);
	if (!info->swapped) {
		list_del_init(&info->swaplist);
		goto lost2;
	}
	limit = info->next_index;
	size = limit;
	if (size > SHMEM_NR_DIRECT)
		size = SHMEM_NR_DIRECT;
	offset = shmem_find_swp(entry, ptr, ptr+size);
	if (offset >= 0) {
		shmem_swp_balance_unmap();
		goto found;
	}
	if (!info->i_indirect)
		goto lost2;

	dir = shmem_dir_map(info->i_indirect);
	stage = SHMEM_NR_DIRECT + ENTRIES_PER_PAGEPAGE/2;

	for (idx = SHMEM_NR_DIRECT; idx < limit; idx += ENTRIES_PER_PAGE, dir++) {
		if (unlikely(idx == stage)) {
			shmem_dir_unmap(dir-1);
			if (cond_resched_lock(&info->lock)) {
				/* check it has not been truncated */
				if (limit > info->next_index) {
					limit = info->next_index;
					if (idx >= limit)
						goto lost2;
				}
			}
			dir = shmem_dir_map(info->i_indirect) +
			    ENTRIES_PER_PAGE/2 + idx/ENTRIES_PER_PAGEPAGE;
			while (!*dir) {
				dir++;
				idx += ENTRIES_PER_PAGEPAGE;
				if (idx >= limit)
					goto lost1;
			}
			stage = idx + ENTRIES_PER_PAGEPAGE;
			subdir = *dir;
			shmem_dir_unmap(dir);
			dir = shmem_dir_map(subdir);
		}
		subdir = *dir;
		if (subdir && page_private(subdir)) {
			ptr = shmem_swp_map(subdir);
			size = limit - idx;
			if (size > ENTRIES_PER_PAGE)
				size = ENTRIES_PER_PAGE;
			offset = shmem_find_swp(entry, ptr, ptr+size);
			shmem_swp_unmap(ptr);
			if (offset >= 0) {
				shmem_dir_unmap(dir);
				ptr = shmem_swp_map(subdir);
				goto found;
			}
		}
	}
lost1:
	shmem_dir_unmap(dir-1);
lost2:
	spin_unlock(&info->lock);
	return 0;
found:
	idx += offset;
	ptr += offset;

	/*
	 * Move _head_ to start search for next from here.
	 * But be careful: shmem_evict_inode checks list_empty without taking
	 * mutex, and there's an instant in list_move_tail when info->swaplist
	 * would appear empty, if it were the only one on shmem_swaplist.  We
	 * could avoid doing it if inode NULL; or use this minor optimization.
	 */
	if (shmem_swaplist.next != &info->swaplist)
		list_move_tail(&shmem_swaplist, &info->swaplist);

	/*
	 * We rely on shmem_swaplist_mutex, not only to protect the swaplist,
	 * but also to hold up shmem_evict_inode(): so inode cannot be freed
	 * beneath us (pagelock doesn't help until the page is in pagecache).
	 */
	mapping = info->vfs_inode.i_mapping;
	error = add_to_page_cache_locked(page, mapping, idx, GFP_NOWAIT);
	/* which does mem_cgroup_uncharge_cache_page on error */

	if (error != -ENOMEM) {
		delete_from_swap_cache(page);
		set_page_dirty(page);
		info->flags |= SHMEM_PAGEIN;
		shmem_swp_set(info, ptr, 0);
		swap_free(entry);
		error = 1;	/* not an error, but entry was found */
	}
	shmem_swp_unmap(ptr);
	spin_unlock(&info->lock);
	return error;
}

/*
 * shmem_unuse() search for an eventually swapped out shmem page.
 */
int shmem_unuse(swp_entry_t entry, struct page *page)
{
	struct list_head *p, *next;
	struct shmem_inode_info *info;
	int found = 0;
	int error;

	/*
	 * Charge page using GFP_KERNEL while we can wait, before taking
	 * the shmem_swaplist_mutex which might hold up shmem_writepage().
	 * Charged back to the user (not to caller) when swap account is used.
	 * add_to_page_cache() will be called with GFP_NOWAIT.
	 */
	error = mem_cgroup_cache_charge(page, current->mm, GFP_KERNEL);
	if (error)
		goto out;
	/*
	 * Try to preload while we can wait, to not make a habit of
	 * draining atomic reserves; but don't latch on to this cpu,
	 * it's okay if sometimes we get rescheduled after this.
	 */
	error = radix_tree_preload(GFP_KERNEL);
	if (error)
		goto uncharge;
	radix_tree_preload_end();

	mutex_lock(&shmem_swaplist_mutex);
	list_for_each_safe(p, next, &shmem_swaplist) {
		info = list_entry(p, struct shmem_inode_info, swaplist);
		found = shmem_unuse_inode(info, entry, page);
		cond_resched();
		if (found)
			break;
	}
	mutex_unlock(&shmem_swaplist_mutex);

uncharge:
	if (!found)
		mem_cgroup_uncharge_cache_page(page);
	if (found < 0)
		error = found;
out:
	unlock_page(page);
	page_cache_release(page);
	return error;
}

/*
 * Move the page from the page cache to the swap cache.
 */
static int shmem_writepage(struct page *page, struct writeback_control *wbc)
{
	struct shmem_inode_info *info;
	swp_entry_t *entry, swap;
	struct address_space *mapping;
	unsigned long index;
	struct inode *inode;

	BUG_ON(!PageLocked(page));
	mapping = page->mapping;
	index = page->index;
	inode = mapping->host;
	info = SHMEM_I(inode);
	if (info->flags & VM_LOCKED)
		goto redirty;
	if (!total_swap_pages)
		goto redirty;

	/*
	 * shmem_backing_dev_info's capabilities prevent regular writeback or
	 * sync from ever calling shmem_writepage; but a stacking filesystem
	 * might use ->writepage of its underlying filesystem, in which case
	 * tmpfs should write out to swap only in response to memory pressure,
	 * and not for the writeback threads or sync.
	 */
	if (!wbc->for_reclaim) {
		WARN_ON_ONCE(1);	/* Still happens? Tell us about it! */
		goto redirty;
	}
	swap = get_swap_page();
	if (!swap.val)
		goto redirty;

	/*
	 * Add inode to shmem_unuse()'s list of swapped-out inodes,
	 * if it's not already there.  Do it now because we cannot take
	 * mutex while holding spinlock, and must do so before the page
	 * is moved to swap cache, when its pagelock no longer protects
	 * the inode from eviction.  But don't unlock the mutex until
	 * we've taken the spinlock, because shmem_unuse_inode() will
	 * prune a !swapped inode from the swaplist under both locks.
	 */
	mutex_lock(&shmem_swaplist_mutex);
	if (list_empty(&info->swaplist))
		list_add_tail(&info->swaplist, &shmem_swaplist);

	spin_lock(&info->lock);
	mutex_unlock(&shmem_swaplist_mutex);

	if (index >= info->next_index) {
		BUG_ON(!(info->flags & SHMEM_TRUNCATE));
		goto unlock;
	}
	entry = shmem_swp_entry(info, index, NULL);
	if (entry->val) {
		WARN_ON_ONCE(1);	/* Still happens? Tell us about it! */
		free_swap_and_cache(*entry);
		shmem_swp_set(info, entry, 0);
	}
	shmem_recalc_inode(inode);

	if (add_to_swap_cache(page, swap, GFP_ATOMIC) == 0) {
		delete_from_page_cache(page);
		shmem_swp_set(info, entry, swap.val);
		shmem_swp_unmap(entry);
		swap_shmem_alloc(swap);
		spin_unlock(&info->lock);
		BUG_ON(page_mapped(page));
		swap_writepage(page, wbc);
		return 0;
	}

	shmem_swp_unmap(entry);
unlock:
	spin_unlock(&info->lock);
	/*
	 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
	 * clear SWAP_HAS_CACHE flag.
	 */
	swapcache_free(swap, NULL);
redirty:
	set_page_dirty(page);
	if (wbc->for_reclaim)
		return AOP_WRITEPAGE_ACTIVATE;	/* Return with page locked */
	unlock_page(page);
	return 0;
}

#ifdef CONFIG_NUMA
#ifdef CONFIG_TMPFS
static void shmem_show_mpol(struct seq_file *seq, struct mempolicy *mpol)
{
	char buffer[64];

	if (!mpol || mpol->mode == MPOL_DEFAULT)
		return;		/* show nothing */

	mpol_to_str(buffer, sizeof(buffer), mpol, 1);

	seq_printf(seq, ",mpol=%s", buffer);
}

static struct mempolicy *shmem_get_sbmpol(struct shmem_sb_info *sbinfo)
{
	struct mempolicy *mpol = NULL;
	if (sbinfo->mpol) {
		spin_lock(&sbinfo->stat_lock);	/* prevent replace/use races */
		mpol = sbinfo->mpol;
		mpol_get(mpol);
		spin_unlock(&sbinfo->stat_lock);
	}
	return mpol;
}
#endif /* CONFIG_TMPFS */

static struct page *shmem_swapin(swp_entry_t entry, gfp_t gfp,
			struct shmem_inode_info *info, unsigned long idx)
{
	struct vm_area_struct pvma;
	struct page *page;

	/* Create a pseudo vma that just contains the policy */
	pvma.vm_start = 0;
	pvma.vm_pgoff = idx;
	pvma.vm_ops = NULL;
	pvma.vm_policy = mpol_shared_policy_lookup(&info->policy, idx);

	page = swapin_readahead(entry, gfp, &pvma, 0);

	/* Drop reference taken by mpol_shared_policy_lookup() */
	mpol_cond_put(pvma.vm_policy);

	return page;
}

static struct page *shmem_alloc_page(gfp_t gfp,
			struct shmem_inode_info *info, unsigned long idx)
{
	struct vm_area_struct pvma;
	struct page *page;

	/* Create a pseudo vma that just contains the policy */
	pvma.vm_start = 0;
	pvma.vm_pgoff = idx;
	pvma.vm_ops = NULL;
	pvma.vm_policy = mpol_shared_policy_lookup(&info->policy, idx);

	page = alloc_page_vma(gfp, &pvma, 0);

	/* Drop reference taken by mpol_shared_policy_lookup() */
	mpol_cond_put(pvma.vm_policy);

	return page;
}
#else /* !CONFIG_NUMA */
#ifdef CONFIG_TMPFS
static inline void shmem_show_mpol(struct seq_file *seq, struct mempolicy *p)
{
}
#endif /* CONFIG_TMPFS */

static inline struct page *shmem_swapin(swp_entry_t entry, gfp_t gfp,
			struct shmem_inode_info *info, unsigned long idx)
{
	return swapin_readahead(entry, gfp, NULL, 0);
}

static inline struct page *shmem_alloc_page(gfp_t gfp,
			struct shmem_inode_info *info, unsigned long idx)
{
	return alloc_page(gfp);
}
#endif /* CONFIG_NUMA */

#if !defined(CONFIG_NUMA) || !defined(CONFIG_TMPFS)
static inline struct mempolicy *shmem_get_sbmpol(struct shmem_sb_info *sbinfo)
{
	return NULL;
}
#endif

/*
 * shmem_getpage_gfp - find page in cache, or get from swap, or allocate
 *
 * If we allocate a new one we do not mark it dirty. That's up to the
 * vm. If we swap it in we mark it dirty since we also free the swap
 * entry since a page cannot live in both the swap and page cache
 */
static int shmem_getpage_gfp(struct inode *inode, pgoff_t idx,
	struct page **pagep, enum sgp_type sgp, gfp_t gfp, int *fault_type)
{
	struct address_space *mapping = inode->i_mapping;
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct shmem_sb_info *sbinfo;
	struct page *page;
	struct page *prealloc_page = NULL;
	swp_entry_t *entry;
	swp_entry_t swap;
	int error;
	int ret;

	if (idx >= SHMEM_MAX_INDEX)
		return -EFBIG;
repeat:
	page = find_lock_page(mapping, idx);
	if (page) {
		/*
		 * Once we can get the page lock, it must be uptodate:
		 * if there were an error in reading back from swap,
		 * the page would not be inserted into the filecache.
		 */
		BUG_ON(!PageUptodate(page));
		goto done;
	}

	/*
	 * Try to preload while we can wait, to not make a habit of
	 * draining atomic reserves; but don't latch on to this cpu.
	 */
	error = radix_tree_preload(gfp & GFP_RECLAIM_MASK);
	if (error)
		goto out;
	radix_tree_preload_end();

	if (sgp != SGP_READ && !prealloc_page) {
		prealloc_page = shmem_alloc_page(gfp, info, idx);
		if (prealloc_page) {
			SetPageSwapBacked(prealloc_page);
			if (mem_cgroup_cache_charge(prealloc_page,
					current->mm, GFP_KERNEL)) {
				page_cache_release(prealloc_page);
				prealloc_page = NULL;
			}
		}
	}

	spin_lock(&info->lock);
	shmem_recalc_inode(inode);
	entry = shmem_swp_alloc(info, idx, sgp, gfp);
	if (IS_ERR(entry)) {
		spin_unlock(&info->lock);
		error = PTR_ERR(entry);
		goto out;
	}
	swap = *entry;

	if (swap.val) {
		/* Look it up and read it in.. */
		page = lookup_swap_cache(swap);
		if (!page) {
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			/* here we actually do the io */
			if (fault_type)
				*fault_type |= VM_FAULT_MAJOR;
			page = shmem_swapin(swap, gfp, info, idx);
			if (!page) {
				spin_lock(&info->lock);
				entry = shmem_swp_alloc(info, idx, sgp, gfp);
				if (IS_ERR(entry))
					error = PTR_ERR(entry);
				else {
					if (entry->val == swap.val)
						error = -ENOMEM;
					shmem_swp_unmap(entry);
				}
				spin_unlock(&info->lock);
				if (error)
					goto out;
				goto repeat;
			}
			wait_on_page_locked(page);
			page_cache_release(page);
			goto repeat;
		}

		/* We have to do this with page locked to prevent races */
		if (!trylock_page(page)) {
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			wait_on_page_locked(page);
			page_cache_release(page);
			goto repeat;
		}
		if (PageWriteback(page)) {
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			wait_on_page_writeback(page);
			unlock_page(page);
			page_cache_release(page);
			goto repeat;
		}
		if (!PageUptodate(page)) {
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			unlock_page(page);
			page_cache_release(page);
			error = -EIO;
			goto out;
		}

		error = add_to_page_cache_locked(page, mapping,
						 idx, GFP_NOWAIT);
		if (error) {
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			if (error == -ENOMEM) {
				/*
				 * reclaim from proper memory cgroup and
				 * call memcg's OOM if needed.
				 */
				error = mem_cgroup_shmem_charge_fallback(
						page, current->mm, gfp);
				if (error) {
					unlock_page(page);
					page_cache_release(page);
					goto out;
				}
			}
			unlock_page(page);
			page_cache_release(page);
			goto repeat;
		}

		info->flags |= SHMEM_PAGEIN;
		shmem_swp_set(info, entry, 0);
		shmem_swp_unmap(entry);
		delete_from_swap_cache(page);
		spin_unlock(&info->lock);
		set_page_dirty(page);
		swap_free(swap);

	} else if (sgp == SGP_READ) {
		shmem_swp_unmap(entry);
		page = find_get_page(mapping, idx);
		if (page && !trylock_page(page)) {
			spin_unlock(&info->lock);
			wait_on_page_locked(page);
			page_cache_release(page);
			goto repeat;
		}
		spin_unlock(&info->lock);

	} else if (prealloc_page) {
		shmem_swp_unmap(entry);
		sbinfo = SHMEM_SB(inode->i_sb);
		if (sbinfo->max_blocks) {
			if (percpu_counter_compare(&sbinfo->used_blocks,
						sbinfo->max_blocks) >= 0 ||
			    shmem_acct_block(info->flags))
				goto nospace;
			percpu_counter_inc(&sbinfo->used_blocks);
			inode->i_blocks += BLOCKS_PER_PAGE;
		} else if (shmem_acct_block(info->flags))
			goto nospace;

		page = prealloc_page;
		prealloc_page = NULL;

		entry = shmem_swp_alloc(info, idx, sgp, gfp);
		if (IS_ERR(entry))
			error = PTR_ERR(entry);
		else {
			swap = *entry;
			shmem_swp_unmap(entry);
		}
		ret = error || swap.val;
		if (ret)
			mem_cgroup_uncharge_cache_page(page);
		else
			ret = add_to_page_cache_lru(page, mapping,
						idx, GFP_NOWAIT);
		/*
		 * At add_to_page_cache_lru() failure,
		 * uncharge will be done automatically.
		 */
		if (ret) {
			shmem_unacct_blocks(info->flags, 1);
			shmem_free_blocks(inode, 1);
			spin_unlock(&info->lock);
			page_cache_release(page);
			if (error)
				goto out;
			goto repeat;
		}

		info->flags |= SHMEM_PAGEIN;
		info->alloced++;
		spin_unlock(&info->lock);
		clear_highpage(page);
		flush_dcache_page(page);
		SetPageUptodate(page);
		if (sgp == SGP_DIRTY)
			set_page_dirty(page);

	} else {
		spin_unlock(&info->lock);
		error = -ENOMEM;
		goto out;
	}
done:
	*pagep = page;
	error = 0;
out:
	if (prealloc_page) {
		mem_cgroup_uncharge_cache_page(prealloc_page);
		page_cache_release(prealloc_page);
	}
	return error;

nospace:
	/*
	 * Perhaps the page was brought in from swap between find_lock_page
	 * and taking info->lock?  We allow for that at add_to_page_cache_lru,
	 * but must also avoid reporting a spurious ENOSPC while working on a
	 * full tmpfs.
	 */
	page = find_get_page(mapping, idx);
	spin_unlock(&info->lock);
	if (page) {
		page_cache_release(page);
		goto repeat;
	}
	error = -ENOSPC;
	goto out;
}

static int shmem_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct inode *inode = vma->vm_file->f_path.dentry->d_inode;
	int error;
	int ret = VM_FAULT_LOCKED;

	if (((loff_t)vmf->pgoff << PAGE_CACHE_SHIFT) >= i_size_read(inode))
		return VM_FAULT_SIGBUS;

	error = shmem_getpage(inode, vmf->pgoff, &vmf->page, SGP_CACHE, &ret);
	if (error)
		return ((error == -ENOMEM) ? VM_FAULT_OOM : VM_FAULT_SIGBUS);

	if (ret & VM_FAULT_MAJOR) {
		count_vm_event(PGMAJFAULT);
		mem_cgroup_count_vm_event(vma->vm_mm, PGMAJFAULT);
	}
	return ret;
}

#ifdef CONFIG_NUMA
static int shmem_set_policy(struct vm_area_struct *vma, struct mempolicy *new)
{
	struct inode *i = vma->vm_file->f_path.dentry->d_inode;
	return mpol_set_shared_policy(&SHMEM_I(i)->policy, vma, new);
}

static struct mempolicy *shmem_get_policy(struct vm_area_struct *vma,
					  unsigned long addr)
{
	struct inode *i = vma->vm_file->f_path.dentry->d_inode;
	unsigned long idx;

	idx = ((addr - vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff;
	return mpol_shared_policy_lookup(&SHMEM_I(i)->policy, idx);
}
#endif

int shmem_lock(struct file *file, int lock, struct user_struct *user)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	struct shmem_inode_info *info = SHMEM_I(inode);
	int retval = -ENOMEM;

	spin_lock(&info->lock);
	if (lock && !(info->flags & VM_LOCKED)) {
		if (!user_shm_lock(inode->i_size, user))
			goto out_nomem;
		info->flags |= VM_LOCKED;
		mapping_set_unevictable(file->f_mapping);
	}
	if (!lock && (info->flags & VM_LOCKED) && user) {
		user_shm_unlock(inode->i_size, user);
		info->flags &= ~VM_LOCKED;
		mapping_clear_unevictable(file->f_mapping);
	}
	retval = 0;

out_nomem:
	spin_unlock(&info->lock);
	return retval;
}

static int shmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	file_accessed(file);
	vma->vm_ops = &shmem_vm_ops;
	vma->vm_flags |= VM_CAN_NONLINEAR;
	return 0;
}

static struct inode *shmem_get_inode(struct super_block *sb, const struct inode *dir,
				     int mode, dev_t dev, unsigned long flags)
{
	struct inode *inode;
	struct shmem_inode_info *info;
	struct shmem_sb_info *sbinfo = SHMEM_SB(sb);

	if (shmem_reserve_inode(sb))
		return NULL;

	inode = new_inode(sb);
	if (inode) {
		inode->i_ino = get_next_ino();
		inode_init_owner(inode, dir, mode);
		inode->i_blocks = 0;
		inode->i_mapping->backing_dev_info = &shmem_backing_dev_info;
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		inode->i_generation = get_seconds();
		info = SHMEM_I(inode);
		memset(info, 0, (char *)inode - (char *)info);
		spin_lock_init(&info->lock);
		info->flags = flags & VM_NORESERVE;
		INIT_LIST_HEAD(&info->swaplist);
		INIT_LIST_HEAD(&info->xattr_list);
		cache_no_acl(inode);

		switch (mode & S_IFMT) {
		default:
			inode->i_op = &shmem_special_inode_operations;
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_mapping->a_ops = &shmem_aops;
			inode->i_op = &shmem_inode_operations;
			inode->i_fop = &shmem_file_operations;
			mpol_shared_policy_init(&info->policy,
						 shmem_get_sbmpol(sbinfo));
			break;
		case S_IFDIR:
			inc_nlink(inode);
			/* Some things misbehave if size == 0 on a directory */
			inode->i_size = 2 * BOGO_DIRENT_SIZE;
			inode->i_op = &shmem_dir_inode_operations;
			inode->i_fop = &simple_dir_operations;
			break;
		case S_IFLNK:
			/*
			 * Must not load anything in the rbtree,
			 * mpol_free_shared_policy will not be called.
			 */
			mpol_shared_policy_init(&info->policy, NULL);
			break;
		}
	} else
		shmem_free_inode(sb);
	return inode;
}

#ifdef CONFIG_TMPFS
static const struct inode_operations shmem_symlink_inode_operations;
static const struct inode_operations shmem_symlink_inline_operations;

static int
shmem_write_begin(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned flags,
			struct page **pagep, void **fsdata)
{
	struct inode *inode = mapping->host;
	pgoff_t index = pos >> PAGE_CACHE_SHIFT;
	return shmem_getpage(inode, index, pagep, SGP_WRITE, NULL);
}

static int
shmem_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;

	if (pos + copied > inode->i_size)
		i_size_write(inode, pos + copied);

	set_page_dirty(page);
	unlock_page(page);
	page_cache_release(page);

	return copied;
}

static void do_shmem_file_read(struct file *filp, loff_t *ppos, read_descriptor_t *desc, read_actor_t actor)
{
	struct inode *inode = filp->f_path.dentry->d_inode;
	struct address_space *mapping = inode->i_mapping;
	unsigned long index, offset;
	enum sgp_type sgp = SGP_READ;

	/*
	 * Might this read be for a stacking filesystem?  Then when reading
	 * holes of a sparse file, we actually need to allocate those pages,
	 * and even mark them dirty, so it cannot exceed the max_blocks limit.
	 */
	if (segment_eq(get_fs(), KERNEL_DS))
		sgp = SGP_DIRTY;

	index = *ppos >> PAGE_CACHE_SHIFT;
	offset = *ppos & ~PAGE_CACHE_MASK;

	for (;;) {
		struct page *page = NULL;
		unsigned long end_index, nr, ret;
		loff_t i_size = i_size_read(inode);

		end_index = i_size >> PAGE_CACHE_SHIFT;
		if (index > end_index)
			break;
		if (index == end_index) {
			nr = i_size & ~PAGE_CACHE_MASK;
			if (nr <= offset)
				break;
		}

		desc->error = shmem_getpage(inode, index, &page, sgp, NULL);
		if (desc->error) {
			if (desc->error == -EINVAL)
				desc->error = 0;
			break;
		}
		if (page)
			unlock_page(page);

		/*
		 * We must evaluate after, since reads (unlike writes)
		 * are called without i_mutex protection against truncate
		 */
		nr = PAGE_CACHE_SIZE;
		i_size = i_size_read(inode);
		end_index = i_size >> PAGE_CACHE_SHIFT;
		if (index == end_index) {
			nr = i_size & ~PAGE_CACHE_MASK;
			if (nr <= offset) {
				if (page)
					page_cache_release(page);
				break;
			}
		}
		nr -= offset;

		if (page) {
			/*
			 * If users can be writing to this page using arbitrary
			 * virtual addresses, take care about potential aliasing
			 * before reading the page on the kernel side.
			 */
			if (mapping_writably_mapped(mapping))
				flush_dcache_page(page);
			/*
			 * Mark the page accessed if we read the beginning.
			 */
			if (!offset)
				mark_page_accessed(page);
		} else {
			page = ZERO_PAGE(0);
			page_cache_get(page);
		}

		/*
		 * Ok, we have the page, and it's up-to-date, so
		 * now we can copy it to user space...
		 *
		 * The actor routine returns how many bytes were actually used..
		 * NOTE! This may not be the same as how much of a user buffer
		 * we filled up (we may be padding etc), so we can only update
		 * "pos" here (the actor routine has to update the user buffer
		 * pointers and the remaining count).
		 */
		ret = actor(desc, page, offset, nr);
		offset += ret;
		index += offset >> PAGE_CACHE_SHIFT;
		offset &= ~PAGE_CACHE_MASK;

		page_cache_release(page);
		if (ret != nr || !desc->count)
			break;

		cond_resched();
	}

	*ppos = ((loff_t) index << PAGE_CACHE_SHIFT) + offset;
	file_accessed(filp);
}

static ssize_t shmem_file_aio_read(struct kiocb *iocb,
		const struct iovec *iov, unsigned long nr_segs, loff_t pos)
{
	struct file *filp = iocb->ki_filp;
	ssize_t retval;
	unsigned long seg;
	size_t count;
	loff_t *ppos = &iocb->ki_pos;

	retval = generic_segment_checks(iov, &nr_segs, &count, VERIFY_WRITE);
	if (retval)
		return retval;

	for (seg = 0; seg < nr_segs; seg++) {
		read_descriptor_t desc;

		desc.written = 0;
		desc.arg.buf = iov[seg].iov_base;
		desc.count = iov[seg].iov_len;
		if (desc.count == 0)
			continue;
		desc.error = 0;
		do_shmem_file_read(filp, ppos, &desc, file_read_actor);
		retval += desc.written;
		if (desc.error) {
			retval = retval ?: desc.error;
			break;
		}
		if (desc.count > 0)
			break;
	}
	return retval;
}

static ssize_t shmem_file_splice_read(struct file *in, loff_t *ppos,
				struct pipe_inode_info *pipe, size_t len,
				unsigned int flags)
{
	struct address_space *mapping = in->f_mapping;
	struct inode *inode = mapping->host;
	unsigned int loff, nr_pages, req_pages;
	struct page *pages[PIPE_DEF_BUFFERS];
	struct partial_page partial[PIPE_DEF_BUFFERS];
	struct page *page;
	pgoff_t index, end_index;
	loff_t isize, left;
	int error, page_nr;
	struct splice_pipe_desc spd = {
		.pages = pages,
		.partial = partial,
		.flags = flags,
		.ops = &page_cache_pipe_buf_ops,
		.spd_release = spd_release_page,
	};

	isize = i_size_read(inode);
	if (unlikely(*ppos >= isize))
		return 0;

	left = isize - *ppos;
	if (unlikely(left < len))
		len = left;

	if (splice_grow_spd(pipe, &spd))
		return -ENOMEM;

	index = *ppos >> PAGE_CACHE_SHIFT;
	loff = *ppos & ~PAGE_CACHE_MASK;
	req_pages = (len + loff + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	nr_pages = min(req_pages, pipe->buffers);

	spd.nr_pages = find_get_pages_contig(mapping, index,
						nr_pages, spd.pages);
	index += spd.nr_pages;
	error = 0;

	while (spd.nr_pages < nr_pages) {
		error = shmem_getpage(inode, index, &page, SGP_CACHE, NULL);
		if (error)
			break;
		unlock_page(page);
		spd.pages[spd.nr_pages++] = page;
		index++;
	}

	index = *ppos >> PAGE_CACHE_SHIFT;
	nr_pages = spd.nr_pages;
	spd.nr_pages = 0;

	for (page_nr = 0; page_nr < nr_pages; page_nr++) {
		unsigned int this_len;

		if (!len)
			break;

		this_len = min_t(unsigned long, len, PAGE_CACHE_SIZE - loff);
		page = spd.pages[page_nr];

		if (!PageUptodate(page) || page->mapping != mapping) {
			error = shmem_getpage(inode, index, &page,
							SGP_CACHE, NULL);
			if (error)
				break;
			unlock_page(page);
			page_cache_release(spd.pages[page_nr]);
			spd.pages[page_nr] = page;
		}

		isize = i_size_read(inode);
		end_index = (isize - 1) >> PAGE_CACHE_SHIFT;
		if (unlikely(!isize || index > end_index))
			break;

		if (end_index == index) {
			unsigned int plen;

			plen = ((isize - 1) & ~PAGE_CACHE_MASK) + 1;
			if (plen <= loff)
				break;

			this_len = min(this_len, plen - loff);
			len = this_len;
		}

		spd.partial[page_nr].offset = loff;
		spd.partial[page_nr].len = this_len;
		len -= this_len;
		loff = 0;
		spd.nr_pages++;
		index++;
	}

	while (page_nr < nr_pages)
		page_cache_release(spd.pages[page_nr++]);

	if (spd.nr_pages)
		error = splice_to_pipe(pipe, &spd);

	splice_shrink_spd(pipe, &spd);

	if (error > 0) {
		*ppos += error;
		file_accessed(in);
	}
	return error;
}

static int shmem_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(dentry->d_sb);

	buf->f_type = TMPFS_MAGIC;
	buf->f_bsize = PAGE_CACHE_SIZE;
	buf->f_namelen = NAME_MAX;
	if (sbinfo->max_blocks) {
		buf->f_blocks = sbinfo->max_blocks;
		buf->f_bavail = buf->f_bfree =
				sbinfo->max_blocks - percpu_counter_sum(&sbinfo->used_blocks);
	}
	if (sbinfo->max_inodes) {
		buf->f_files = sbinfo->max_inodes;
		buf->f_ffree = sbinfo->free_inodes;
	}
	/* else leave those fields 0 like simple_statfs */
	return 0;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
static int
shmem_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
	struct inode *inode;
	int error = -ENOSPC;

	inode = shmem_get_inode(dir->i_sb, dir, mode, dev, VM_NORESERVE);
	if (inode) {
		error = security_inode_init_security(inode, dir,
						     &dentry->d_name, NULL,
						     NULL, NULL);
		if (error) {
			if (error != -EOPNOTSUPP) {
				iput(inode);
				return error;
			}
		}
#ifdef CONFIG_TMPFS_POSIX_ACL
		error = generic_acl_init(inode, dir);
		if (error) {
			iput(inode);
			return error;
		}
#else
		error = 0;
#endif
		dir->i_size += BOGO_DIRENT_SIZE;
		dir->i_ctime = dir->i_mtime = CURRENT_TIME;
		d_instantiate(dentry, inode);
		dget(dentry); /* Extra count - pin the dentry in core */
	}
	return error;
}

static int shmem_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int error;

	if ((error = shmem_mknod(dir, dentry, mode | S_IFDIR, 0)))
		return error;
	inc_nlink(dir);
	return 0;
}

static int shmem_create(struct inode *dir, struct dentry *dentry, int mode,
		struct nameidata *nd)
{
	return shmem_mknod(dir, dentry, mode | S_IFREG, 0);
}

/*
 * Link a file..
 */
static int shmem_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;
	int ret;

	/*
	 * No ordinary (disk based) filesystem counts links as inodes;
	 * but each new link needs a new dentry, pinning lowmem, and
	 * tmpfs dentries cannot be pruned until they are unlinked.
	 */
	ret = shmem_reserve_inode(inode->i_sb);
	if (ret)
		goto out;

	dir->i_size += BOGO_DIRENT_SIZE;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	inc_nlink(inode);
	ihold(inode);	/* New dentry reference */
	dget(dentry);		/* Extra pinning count for the created dentry */
	d_instantiate(dentry, inode);
out:
	return ret;
}

static int shmem_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;

	if (inode->i_nlink > 1 && !S_ISDIR(inode->i_mode))
		shmem_free_inode(inode->i_sb);

	dir->i_size -= BOGO_DIRENT_SIZE;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	drop_nlink(inode);
	dput(dentry);	/* Undo the count from "create" - this does all the work */
	return 0;
}

static int shmem_rmdir(struct inode *dir, struct dentry *dentry)
{
	if (!simple_empty(dentry))
		return -ENOTEMPTY;

	drop_nlink(dentry->d_inode);
	drop_nlink(dir);
	return shmem_unlink(dir, dentry);
}

/*
 * The VFS layer already does all the dentry stuff for rename,
 * we just have to decrement the usage count for the target if
 * it exists so that the VFS layer correctly free's it when it
 * gets overwritten.
 */
static int shmem_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *inode = old_dentry->d_inode;
	int they_are_dirs = S_ISDIR(inode->i_mode);

	if (!simple_empty(new_dentry))
		return -ENOTEMPTY;

	if (new_dentry->d_inode) {
		(void) shmem_unlink(new_dir, new_dentry);
		if (they_are_dirs)
			drop_nlink(old_dir);
	} else if (they_are_dirs) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
	}

	old_dir->i_size -= BOGO_DIRENT_SIZE;
	new_dir->i_size += BOGO_DIRENT_SIZE;
	old_dir->i_ctime = old_dir->i_mtime =
	new_dir->i_ctime = new_dir->i_mtime =
	inode->i_ctime = CURRENT_TIME;
	return 0;
}

static int shmem_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	int error;
	int len;
	struct inode *inode;
	struct page *page;
	char *kaddr;
	struct shmem_inode_info *info;

	len = strlen(symname) + 1;
	if (len > PAGE_CACHE_SIZE)
		return -ENAMETOOLONG;

	inode = shmem_get_inode(dir->i_sb, dir, S_IFLNK|S_IRWXUGO, 0, VM_NORESERVE);
	if (!inode)
		return -ENOSPC;

	error = security_inode_init_security(inode, dir, &dentry->d_name, NULL,
					     NULL, NULL);
	if (error) {
		if (error != -EOPNOTSUPP) {
			iput(inode);
			return error;
		}
		error = 0;
	}

	info = SHMEM_I(inode);
	inode->i_size = len-1;
	if (len <= SHMEM_SYMLINK_INLINE_LEN) {
		/* do it inline */
		memcpy(info->inline_symlink, symname, len);
		inode->i_op = &shmem_symlink_inline_operations;
	} else {
		error = shmem_getpage(inode, 0, &page, SGP_WRITE, NULL);
		if (error) {
			iput(inode);
			return error;
		}
		inode->i_mapping->a_ops = &shmem_aops;
		inode->i_op = &shmem_symlink_inode_operations;
		kaddr = kmap_atomic(page, KM_USER0);
		memcpy(kaddr, symname, len);
		kunmap_atomic(kaddr, KM_USER0);
		set_page_dirty(page);
		unlock_page(page);
		page_cache_release(page);
	}
	dir->i_size += BOGO_DIRENT_SIZE;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	d_instantiate(dentry, inode);
	dget(dentry);
	return 0;
}

static void *shmem_follow_link_inline(struct dentry *dentry, struct nameidata *nd)
{
	nd_set_link(nd, SHMEM_I(dentry->d_inode)->inline_symlink);
	return NULL;
}

static void *shmem_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct page *page = NULL;
	int res = shmem_getpage(dentry->d_inode, 0, &page, SGP_READ, NULL);
	nd_set_link(nd, res ? ERR_PTR(res) : kmap(page));
	if (page)
		unlock_page(page);
	return page;
}

static void shmem_put_link(struct dentry *dentry, struct nameidata *nd, void *cookie)
{
	if (!IS_ERR(nd_get_link(nd))) {
		struct page *page = cookie;
		kunmap(page);
		mark_page_accessed(page);
		page_cache_release(page);
	}
}

#ifdef CONFIG_TMPFS_XATTR
/*
 * Superblocks without xattr inode operations may get some security.* xattr
 * support from the LSM "for free". As soon as we have any other xattrs
 * like ACLs, we also need to implement the security.* handlers at
 * filesystem level, though.
 */

static int shmem_xattr_get(struct dentry *dentry, const char *name,
			   void *buffer, size_t size)
{
	struct shmem_inode_info *info;
	struct shmem_xattr *xattr;
	int ret = -ENODATA;

	info = SHMEM_I(dentry->d_inode);

	spin_lock(&info->lock);
	list_for_each_entry(xattr, &info->xattr_list, list) {
		if (strcmp(name, xattr->name))
			continue;

		ret = xattr->size;
		if (buffer) {
			if (size < xattr->size)
				ret = -ERANGE;
			else
				memcpy(buffer, xattr->value, xattr->size);
		}
		break;
	}
	spin_unlock(&info->lock);
	return ret;
}

static int shmem_xattr_set(struct dentry *dentry, const char *name,
			   const void *value, size_t size, int flags)
{
	struct inode *inode = dentry->d_inode;
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct shmem_xattr *xattr;
	struct shmem_xattr *new_xattr = NULL;
	size_t len;
	int err = 0;

	/* value == NULL means remove */
	if (value) {
		/* wrap around? */
		len = sizeof(*new_xattr) + size;
		if (len <= sizeof(*new_xattr))
			return -ENOMEM;

		new_xattr = kmalloc(len, GFP_KERNEL);
		if (!new_xattr)
			return -ENOMEM;

		new_xattr->name = kstrdup(name, GFP_KERNEL);
		if (!new_xattr->name) {
			kfree(new_xattr);
			return -ENOMEM;
		}

		new_xattr->size = size;
		memcpy(new_xattr->value, value, size);
	}

	spin_lock(&info->lock);
	list_for_each_entry(xattr, &info->xattr_list, list) {
		if (!strcmp(name, xattr->name)) {
			if (flags & XATTR_CREATE) {
				xattr = new_xattr;
				err = -EEXIST;
			} else if (new_xattr) {
				list_replace(&xattr->list, &new_xattr->list);
			} else {
				list_del(&xattr->list);
			}
			goto out;
		}
	}
	if (flags & XATTR_REPLACE) {
		xattr = new_xattr;
		err = -ENODATA;
	} else {
		list_add(&new_xattr->list, &info->xattr_list);
		xattr = NULL;
	}
out:
	spin_unlock(&info->lock);
	if (xattr)
		kfree(xattr->name);
	kfree(xattr);
	return err;
}


static const struct xattr_handler *shmem_xattr_handlers[] = {
#ifdef CONFIG_TMPFS_POSIX_ACL
	&generic_acl_access_handler,
	&generic_acl_default_handler,
#endif
	NULL
};

static int shmem_xattr_validate(const char *name)
{
	struct { const char *prefix; size_t len; } arr[] = {
		{ XATTR_SECURITY_PREFIX, XATTR_SECURITY_PREFIX_LEN },
		{ XATTR_TRUSTED_PREFIX, XATTR_TRUSTED_PREFIX_LEN }
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(arr); i++) {
		size_t preflen = arr[i].len;
		if (strncmp(name, arr[i].prefix, preflen) == 0) {
			if (!name[preflen])
				return -EINVAL;
			return 0;
		}
	}
	return -EOPNOTSUPP;
}

static ssize_t shmem_getxattr(struct dentry *dentry, const char *name,
			      void *buffer, size_t size)
{
	int err;

	/*
	 * If this is a request for a synthetic attribute in the system.*
	 * namespace use the generic infrastructure to resolve a handler
	 * for it via sb->s_xattr.
	 */
	if (!strncmp(name, XATTR_SYSTEM_PREFIX, XATTR_SYSTEM_PREFIX_LEN))
		return generic_getxattr(dentry, name, buffer, size);

	err = shmem_xattr_validate(name);
	if (err)
		return err;

	return shmem_xattr_get(dentry, name, buffer, size);
}

static int shmem_setxattr(struct dentry *dentry, const char *name,
			  const void *value, size_t size, int flags)
{
	int err;

	/*
	 * If this is a request for a synthetic attribute in the system.*
	 * namespace use the generic infrastructure to resolve a handler
	 * for it via sb->s_xattr.
	 */
	if (!strncmp(name, XATTR_SYSTEM_PREFIX, XATTR_SYSTEM_PREFIX_LEN))
		return generic_setxattr(dentry, name, value, size, flags);

	err = shmem_xattr_validate(name);
	if (err)
		return err;

	if (size == 0)
		value = "";  /* empty EA, do not remove */

	return shmem_xattr_set(dentry, name, value, size, flags);

}

static int shmem_removexattr(struct dentry *dentry, const char *name)
{
	int err;

	/*
	 * If this is a request for a synthetic attribute in the system.*
	 * namespace use the generic infrastructure to resolve a handler
	 * for it via sb->s_xattr.
	 */
	if (!strncmp(name, XATTR_SYSTEM_PREFIX, XATTR_SYSTEM_PREFIX_LEN))
		return generic_removexattr(dentry, name);

	err = shmem_xattr_validate(name);
	if (err)
		return err;

	return shmem_xattr_set(dentry, name, NULL, 0, XATTR_REPLACE);
}

static bool xattr_is_trusted(const char *name)
{
	return !strncmp(name, XATTR_TRUSTED_PREFIX, XATTR_TRUSTED_PREFIX_LEN);
}

static ssize_t shmem_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	bool trusted = capable(CAP_SYS_ADMIN);
	struct shmem_xattr *xattr;
	struct shmem_inode_info *info;
	size_t used = 0;

	info = SHMEM_I(dentry->d_inode);

	spin_lock(&info->lock);
	list_for_each_entry(xattr, &info->xattr_list, list) {
		size_t len;

		/* skip "trusted." attributes for unprivileged callers */
		if (!trusted && xattr_is_trusted(xattr->name))
			continue;

		len = strlen(xattr->name) + 1;
		used += len;
		if (buffer) {
			if (size < used) {
				used = -ERANGE;
				break;
			}
			memcpy(buffer, xattr->name, len);
			buffer += len;
		}
	}
	spin_unlock(&info->lock);

	return used;
}
#endif /* CONFIG_TMPFS_XATTR */

static const struct inode_operations shmem_symlink_inline_operations = {
	.readlink	= generic_readlink,
	.follow_link	= shmem_follow_link_inline,
#ifdef CONFIG_TMPFS_XATTR
	.setxattr	= shmem_setxattr,
	.getxattr	= shmem_getxattr,
	.listxattr	= shmem_listxattr,
	.removexattr	= shmem_removexattr,
#endif
};

static const struct inode_operations shmem_symlink_inode_operations = {
	.readlink	= generic_readlink,
	.follow_link	= shmem_follow_link,
	.put_link	= shmem_put_link,
#ifdef CONFIG_TMPFS_XATTR
	.setxattr	= shmem_setxattr,
	.getxattr	= shmem_getxattr,
	.listxattr	= shmem_listxattr,
	.removexattr	= shmem_removexattr,
#endif
};

static struct dentry *shmem_get_parent(struct dentry *child)
{
	return ERR_PTR(-ESTALE);
}

static int shmem_match(struct inode *ino, void *vfh)
{
	__u32 *fh = vfh;
	__u64 inum = fh[2];
	inum = (inum << 32) | fh[1];
	return ino->i_ino == inum && fh[0] == ino->i_generation;
}

static struct dentry *shmem_fh_to_dentry(struct super_block *sb,
		struct fid *fid, int fh_len, int fh_type)
{
	struct inode *inode;
	struct dentry *dentry = NULL;
	u64 inum;

	if (fh_len < 3)
		return NULL;

	inum = fid->raw[2];
	inum = (inum << 32) | fid->raw[1];

	inode = ilookup5(sb, (unsigned long)(inum + fid->raw[0]),
			shmem_match, fid->raw);
	if (inode) {
		dentry = d_find_alias(inode);
		iput(inode);
	}

	return dentry;
}

static int shmem_encode_fh(struct dentry *dentry, __u32 *fh, int *len,
				int connectable)
{
	struct inode *inode = dentry->d_inode;

	if (*len < 3) {
		*len = 3;
		return 255;
	}

	if (inode_unhashed(inode)) {
		/* Unfortunately insert_inode_hash is not idempotent,
		 * so as we hash inodes here rather than at creation
		 * time, we need a lock to ensure we only try
		 * to do it once
		 */
		static DEFINE_SPINLOCK(lock);
		spin_lock(&lock);
		if (inode_unhashed(inode))
			__insert_inode_hash(inode,
					    inode->i_ino + inode->i_generation);
		spin_unlock(&lock);
	}

	fh[0] = inode->i_generation;
	fh[1] = inode->i_ino;
	fh[2] = ((__u64)inode->i_ino) >> 32;

	*len = 3;
	return 1;
}

static const struct export_operations shmem_export_ops = {
	.get_parent     = shmem_get_parent,
	.encode_fh      = shmem_encode_fh,
	.fh_to_dentry	= shmem_fh_to_dentry,
};

static int shmem_parse_options(char *options, struct shmem_sb_info *sbinfo,
			       bool remount)
{
	char *this_char, *value, *rest;
	struct mempolicy *mpol = NULL;

	while (options != NULL) {
		this_char = options;
		for (;;) {
			/*
			 * NUL-terminate this option: unfortunately,
			 * mount options form a comma-separated list,
			 * but mpol's nodelist may also contain commas.
			 */
			options = strchr(options, ',');
			if (options == NULL)
				break;
			options++;
			if (!isdigit(*options)) {
				options[-1] = '\0';
				break;
			}
		}
		if (!*this_char)
			continue;
		if ((value = strchr(this_char,'=')) != NULL) {
			*value++ = 0;
		} else {
			printk(KERN_ERR
			    "tmpfs: No value for mount option '%s'\n",
			    this_char);
			goto error;
		}

		if (!strcmp(this_char,"size")) {
			unsigned long long size;
			size = memparse(value,&rest);
			if (*rest == '%') {
				size <<= PAGE_SHIFT;
				size *= totalram_pages;
				do_div(size, 100);
				rest++;
			}
			if (*rest)
				goto bad_val;
			sbinfo->max_blocks =
				DIV_ROUND_UP(size, PAGE_CACHE_SIZE);
		} else if (!strcmp(this_char,"nr_blocks")) {
			sbinfo->max_blocks = memparse(value, &rest);
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"nr_inodes")) {
			sbinfo->max_inodes = memparse(value, &rest);
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"mode")) {
			if (remount)
				continue;
			sbinfo->mode = simple_strtoul(value, &rest, 8) & 07777;
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"uid")) {
			if (remount)
				continue;
			sbinfo->uid = simple_strtoul(value, &rest, 0);
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"gid")) {
			if (remount)
				continue;
			sbinfo->gid = simple_strtoul(value, &rest, 0);
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"mpol")) {
			mpol_put(mpol);
			if (mpol_parse_str(value, &mpol, 1)) {
				mpol = NULL;
				goto bad_val;
			}
		} else {
			printk(KERN_ERR "tmpfs: Bad mount option %s\n",
			       this_char);
			goto error;
		}
	}
	sbinfo->mpol = mpol;
	return 0;

bad_val:
	printk(KERN_ERR "tmpfs: Bad value '%s' for mount option '%s'\n",
	       value, this_char);
error:
	mpol_put(mpol);
	return 1;

}

static int shmem_remount_fs(struct super_block *sb, int *flags, char *data)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(sb);
	struct shmem_sb_info config = *sbinfo;
	unsigned long inodes;
	int error = -EINVAL;

	config.mpol = NULL;
	if (shmem_parse_options(data, &config, true))
		return error;

	spin_lock(&sbinfo->stat_lock);
	inodes = sbinfo->max_inodes - sbinfo->free_inodes;
	if (percpu_counter_compare(&sbinfo->used_blocks, config.max_blocks) > 0)
		goto out;
	if (config.max_inodes < inodes)
		goto out;
	/*
	 * Those tests also disallow limited->unlimited while any are in
	 * use, so i_blocks will always be zero when max_blocks is zero;
	 * but we must separately disallow unlimited->limited, because
	 * in that case we have no record of how much is already in use.
	 */
	if (config.max_blocks && !sbinfo->max_blocks)
		goto out;
	if (config.max_inodes && !sbinfo->max_inodes)
		goto out;

	error = 0;
	sbinfo->max_blocks  = config.max_blocks;
	sbinfo->max_inodes  = config.max_inodes;
	sbinfo->free_inodes = config.max_inodes - inodes;

	/*
	 * Preserve previous mempolicy unless mpol remount option was specified.
	 */
	if (config.mpol) {
		mpol_put(sbinfo->mpol);
		sbinfo->mpol = config.mpol;	/* transfers initial ref */
	}
out:
	spin_unlock(&sbinfo->stat_lock);
	return error;
}

static int shmem_show_options(struct seq_file *seq, struct vfsmount *vfs)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(vfs->mnt_sb);

	if (sbinfo->max_blocks != shmem_default_max_blocks())
		seq_printf(seq, ",size=%luk",
			sbinfo->max_blocks << (PAGE_CACHE_SHIFT - 10));
	if (sbinfo->max_inodes != shmem_default_max_inodes())
		seq_printf(seq, ",nr_inodes=%lu", sbinfo->max_inodes);
	if (sbinfo->mode != (S_IRWXUGO | S_ISVTX))
		seq_printf(seq, ",mode=%03o", sbinfo->mode);
	if (sbinfo->uid != 0)
		seq_printf(seq, ",uid=%u", sbinfo->uid);
	if (sbinfo->gid != 0)
		seq_printf(seq, ",gid=%u", sbinfo->gid);
	shmem_show_mpol(seq, sbinfo->mpol);
	return 0;
}
#endif /* CONFIG_TMPFS */

static void shmem_put_super(struct super_block *sb)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(sb);

	percpu_counter_destroy(&sbinfo->used_blocks);
	mpol_put(sbinfo->mpol);
	kfree(sbinfo);
	sb->s_fs_info = NULL;
}

int shmem_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode *inode;
	struct dentry *root;
	struct shmem_sb_info *sbinfo;
	int err = -ENOMEM;

	/* Round up to L1_CACHE_BYTES to resist false sharing */
	sbinfo = kzalloc(max((int)sizeof(struct shmem_sb_info),
				L1_CACHE_BYTES), GFP_KERNEL);
	if (!sbinfo)
		return -ENOMEM;

	sbinfo->mode = S_IRWXUGO | S_ISVTX;
	sbinfo->uid = current_fsuid();
	sbinfo->gid = current_fsgid();
	sb->s_fs_info = sbinfo;

#ifdef CONFIG_TMPFS
	/*
	 * Per default we only allow half of the physical ram per
	 * tmpfs instance, limiting inodes to one per page of lowmem;
	 * but the internal instance is left unlimited.
	 */
	if (!(sb->s_flags & MS_NOUSER)) {
		sbinfo->max_blocks = shmem_default_max_blocks();
		sbinfo->max_inodes = shmem_default_max_inodes();
		if (shmem_parse_options(data, sbinfo, false)) {
			err = -EINVAL;
			goto failed;
		}
	}
	sb->s_export_op = &shmem_export_ops;
#else
	sb->s_flags |= MS_NOUSER;
#endif

	spin_lock_init(&sbinfo->stat_lock);
	if (percpu_counter_init(&sbinfo->used_blocks, 0))
		goto failed;
	sbinfo->free_inodes = sbinfo->max_inodes;

	sb->s_maxbytes = SHMEM_MAX_BYTES;
	sb->s_blocksize = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	sb->s_magic = TMPFS_MAGIC;
	sb->s_op = &shmem_ops;
	sb->s_time_gran = 1;
#ifdef CONFIG_TMPFS_XATTR
	sb->s_xattr = shmem_xattr_handlers;
#endif
#ifdef CONFIG_TMPFS_POSIX_ACL
	sb->s_flags |= MS_POSIXACL;
#endif

	inode = shmem_get_inode(sb, NULL, S_IFDIR | sbinfo->mode, 0, VM_NORESERVE);
	if (!inode)
		goto failed;
	inode->i_uid = sbinfo->uid;
	inode->i_gid = sbinfo->gid;
	root = d_alloc_root(inode);
	if (!root)
		goto failed_iput;
	sb->s_root = root;
	return 0;

failed_iput:
	iput(inode);
failed:
	shmem_put_super(sb);
	return err;
}

static struct kmem_cache *shmem_inode_cachep;

static struct inode *shmem_alloc_inode(struct super_block *sb)
{
	struct shmem_inode_info *p;
	p = (struct shmem_inode_info *)kmem_cache_alloc(shmem_inode_cachep, GFP_KERNEL);
	if (!p)
		return NULL;
	return &p->vfs_inode;
}

static void shmem_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(shmem_inode_cachep, SHMEM_I(inode));
}

static void shmem_destroy_inode(struct inode *inode)
{
	if ((inode->i_mode & S_IFMT) == S_IFREG) {
		/* only struct inode is valid if it's an inline symlink */
		mpol_free_shared_policy(&SHMEM_I(inode)->policy);
	}
	call_rcu(&inode->i_rcu, shmem_i_callback);
}

static void init_once(void *foo)
{
	struct shmem_inode_info *p = (struct shmem_inode_info *) foo;

	inode_init_once(&p->vfs_inode);
}

static int init_inodecache(void)
{
	shmem_inode_cachep = kmem_cache_create("shmem_inode_cache",
				sizeof(struct shmem_inode_info),
				0, SLAB_PANIC, init_once);
	return 0;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(shmem_inode_cachep);
}

static const struct address_space_operations shmem_aops = {
	.writepage	= shmem_writepage,
	.set_page_dirty	= __set_page_dirty_no_writeback,
#ifdef CONFIG_TMPFS
	.write_begin	= shmem_write_begin,
	.write_end	= shmem_write_end,
#endif
	.migratepage	= migrate_page,
	.error_remove_page = generic_error_remove_page,
};

static const struct file_operations shmem_file_operations = {
	.mmap		= shmem_mmap,
#ifdef CONFIG_TMPFS
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.aio_read	= shmem_file_aio_read,
	.aio_write	= generic_file_aio_write,
	.fsync		= noop_fsync,
	.splice_read	= shmem_file_splice_read,
	.splice_write	= generic_file_splice_write,
#endif
};

static const struct inode_operations shmem_inode_operations = {
	.setattr	= shmem_setattr,
	.truncate_range	= shmem_truncate_range,
#ifdef CONFIG_TMPFS_XATTR
	.setxattr	= shmem_setxattr,
	.getxattr	= shmem_getxattr,
	.listxattr	= shmem_listxattr,
	.removexattr	= shmem_removexattr,
#endif
#ifdef CONFIG_TMPFS_POSIX_ACL
	.check_acl	= generic_check_acl,
#endif

};

static const struct inode_operations shmem_dir_inode_operations = {
#ifdef CONFIG_TMPFS
	.create		= shmem_create,
	.lookup		= simple_lookup,
	.link		= shmem_link,
	.unlink		= shmem_unlink,
	.symlink	= shmem_symlink,
	.mkdir		= shmem_mkdir,
	.rmdir		= shmem_rmdir,
	.mknod		= shmem_mknod,
	.rename		= shmem_rename,
#endif
#ifdef CONFIG_TMPFS_XATTR
	.setxattr	= shmem_setxattr,
	.getxattr	= shmem_getxattr,
	.listxattr	= shmem_listxattr,
	.removexattr	= shmem_removexattr,
#endif
#ifdef CONFIG_TMPFS_POSIX_ACL
	.setattr	= shmem_setattr,
	.check_acl	= generic_check_acl,
#endif
};

static const struct inode_operations shmem_special_inode_operations = {
#ifdef CONFIG_TMPFS_XATTR
	.setxattr	= shmem_setxattr,
	.getxattr	= shmem_getxattr,
	.listxattr	= shmem_listxattr,
	.removexattr	= shmem_removexattr,
#endif
#ifdef CONFIG_TMPFS_POSIX_ACL
	.setattr	= shmem_setattr,
	.check_acl	= generic_check_acl,
#endif
};

static const struct super_operations shmem_ops = {
	.alloc_inode	= shmem_alloc_inode,
	.destroy_inode	= shmem_destroy_inode,
#ifdef CONFIG_TMPFS
	.statfs		= shmem_statfs,
	.remount_fs	= shmem_remount_fs,
	.show_options	= shmem_show_options,
#endif
	.evict_inode	= shmem_evict_inode,
	.drop_inode	= generic_delete_inode,
	.put_super	= shmem_put_super,
};

static const struct vm_operations_struct shmem_vm_ops = {
	.fault		= shmem_fault,
#ifdef CONFIG_NUMA
	.set_policy     = shmem_set_policy,
	.get_policy     = shmem_get_policy,
#endif
};


static struct dentry *shmem_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, shmem_fill_super);
}

static struct file_system_type tmpfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "tmpfs",
	.mount		= shmem_mount,
	.kill_sb	= kill_litter_super,
};

int __init init_tmpfs(void)
{
	int error;

	error = bdi_init(&shmem_backing_dev_info);
	if (error)
		goto out4;

	error = init_inodecache();
	if (error)
		goto out3;

	error = register_filesystem(&tmpfs_fs_type);
	if (error) {
		printk(KERN_ERR "Could not register tmpfs\n");
		goto out2;
	}

	shm_mnt = vfs_kern_mount(&tmpfs_fs_type, MS_NOUSER,
				tmpfs_fs_type.name, NULL);
	if (IS_ERR(shm_mnt)) {
		error = PTR_ERR(shm_mnt);
		printk(KERN_ERR "Could not kern_mount tmpfs\n");
		goto out1;
	}
	return 0;

out1:
	unregister_filesystem(&tmpfs_fs_type);
out2:
	destroy_inodecache();
out3:
	bdi_destroy(&shmem_backing_dev_info);
out4:
	shm_mnt = ERR_PTR(error);
	return error;
}

#ifdef CONFIG_CGROUP_MEM_RES_CTLR
/**
 * mem_cgroup_get_shmem_target - find a page or entry assigned to the shmem file
 * @inode: the inode to be searched
 * @pgoff: the offset to be searched
 * @pagep: the pointer for the found page to be stored
 * @ent: the pointer for the found swap entry to be stored
 *
 * If a page is found, refcount of it is incremented. Callers should handle
 * these refcount.
 */
void mem_cgroup_get_shmem_target(struct inode *inode, pgoff_t pgoff,
					struct page **pagep, swp_entry_t *ent)
{
	swp_entry_t entry = { .val = 0 }, *ptr;
	struct page *page = NULL;
	struct shmem_inode_info *info = SHMEM_I(inode);

	if ((pgoff << PAGE_CACHE_SHIFT) >= i_size_read(inode))
		goto out;

	spin_lock(&info->lock);
	ptr = shmem_swp_entry(info, pgoff, NULL);
#ifdef CONFIG_SWAP
	if (ptr && ptr->val) {
		entry.val = ptr->val;
		page = find_get_page(&swapper_space, entry.val);
	} else
#endif
		page = find_get_page(inode->i_mapping, pgoff);
	if (ptr)
		shmem_swp_unmap(ptr);
	spin_unlock(&info->lock);
out:
	*pagep = page;
	*ent = entry;
}
#endif

#else /* !CONFIG_SHMEM */

/*
 * tiny-shmem: simple shmemfs and tmpfs using ramfs code
 *
 * This is intended for small system where the benefits of the full
 * shmem code (swap-backed and resource-limited) are outweighed by
 * their complexity. On systems without swap this code should be
 * effectively equivalent, but much lighter weight.
 */

#include <linux/ramfs.h>

static struct file_system_type tmpfs_fs_type = {
	.name		= "tmpfs",
	.mount		= ramfs_mount,
	.kill_sb	= kill_litter_super,
};

int __init init_tmpfs(void)
{
	BUG_ON(register_filesystem(&tmpfs_fs_type) != 0);

	shm_mnt = kern_mount(&tmpfs_fs_type);
	BUG_ON(IS_ERR(shm_mnt));

	return 0;
}

int shmem_unuse(swp_entry_t entry, struct page *page)
{
	return 0;
}

int shmem_lock(struct file *file, int lock, struct user_struct *user)
{
	return 0;
}

void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end)
{
	truncate_inode_pages_range(inode->i_mapping, start, end);
}
EXPORT_SYMBOL_GPL(shmem_truncate_range);

#ifdef CONFIG_CGROUP_MEM_RES_CTLR
/**
 * mem_cgroup_get_shmem_target - find a page or entry assigned to the shmem file
 * @inode: the inode to be searched
 * @pgoff: the offset to be searched
 * @pagep: the pointer for the found page to be stored
 * @ent: the pointer for the found swap entry to be stored
 *
 * If a page is found, refcount of it is incremented. Callers should handle
 * these refcount.
 */
void mem_cgroup_get_shmem_target(struct inode *inode, pgoff_t pgoff,
					struct page **pagep, swp_entry_t *ent)
{
	struct page *page = NULL;

	if ((pgoff << PAGE_CACHE_SHIFT) >= i_size_read(inode))
		goto out;
	page = find_get_page(inode->i_mapping, pgoff);
out:
	*pagep = page;
	*ent = (swp_entry_t){ .val = 0 };
}
#endif

#define shmem_vm_ops				generic_file_vm_ops
#define shmem_file_operations			ramfs_file_operations
#define shmem_get_inode(sb, dir, mode, dev, flags)	ramfs_get_inode(sb, dir, mode, dev)
#define shmem_acct_size(flags, size)		0
#define shmem_unacct_size(flags, size)		do {} while (0)
#define SHMEM_MAX_BYTES				MAX_LFS_FILESIZE

#endif /* CONFIG_SHMEM */

/* common code */

/**
 * shmem_file_setup - get an unlinked file living in tmpfs
 * @name: name for dentry (to be seen in /proc/<pid>/maps
 * @size: size to be set for the file
 * @flags: VM_NORESERVE suppresses pre-accounting of the entire object size
 */
struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags)
{
	int error;
	struct file *file;
	struct inode *inode;
	struct path path;
	struct dentry *root;
	struct qstr this;

	if (IS_ERR(shm_mnt))
		return (void *)shm_mnt;

	if (size < 0 || size > SHMEM_MAX_BYTES)
		return ERR_PTR(-EINVAL);

	if (shmem_acct_size(flags, size))
		return ERR_PTR(-ENOMEM);

	error = -ENOMEM;
	this.name = name;
	this.len = strlen(name);
	this.hash = 0; /* will go */
	root = shm_mnt->mnt_root;
	path.dentry = d_alloc(root, &this);
	if (!path.dentry)
		goto put_memory;
	path.mnt = mntget(shm_mnt);

	error = -ENOSPC;
	inode = shmem_get_inode(root->d_sb, NULL, S_IFREG | S_IRWXUGO, 0, flags);
	if (!inode)
		goto put_dentry;

	d_instantiate(path.dentry, inode);
	inode->i_size = size;
	inode->i_nlink = 0;	/* It is unlinked */
#ifndef CONFIG_MMU
	error = ramfs_nommu_expand_for_mapping(inode, size);
	if (error)
		goto put_dentry;
#endif

	error = -ENFILE;
	file = alloc_file(&path, FMODE_WRITE | FMODE_READ,
		  &shmem_file_operations);
	if (!file)
		goto put_dentry;

	return file;

put_dentry:
	path_put(&path);
put_memory:
	shmem_unacct_size(flags, size);
	return ERR_PTR(error);
}
EXPORT_SYMBOL_GPL(shmem_file_setup);

void shmem_set_file(struct vm_area_struct *vma, struct file *file)
{
	if (vma->vm_file)
		fput(vma->vm_file);
	vma->vm_file = file;
	vma->vm_ops = &shmem_vm_ops;
	vma->vm_flags |= VM_CAN_NONLINEAR;
}

/**
 * shmem_zero_setup - setup a shared anonymous mapping
 * @vma: the vma to be mmapped is prepared by do_mmap_pgoff
 */
int shmem_zero_setup(struct vm_area_struct *vma)
{
	struct file *file;
	loff_t size = vma->vm_end - vma->vm_start;

	file = shmem_file_setup("dev/zero", size, vma->vm_flags);
	if (IS_ERR(file))
		return PTR_ERR(file);

	shmem_set_file(vma, file);
	return 0;
}

/**
 * shmem_read_mapping_page_gfp - read into page cache, using specified page allocation flags.
 * @mapping:	the page's address_space
 * @index:	the page index
 * @gfp:	the page allocator flags to use if allocating
 *
 * This behaves as a tmpfs "read_cache_page_gfp(mapping, index, gfp)",
 * with any new page allocations done using the specified allocation flags.
 * But read_cache_page_gfp() uses the ->readpage() method: which does not
 * suit tmpfs, since it may have pages in swapcache, and needs to find those
 * for itself; although drivers/gpu/drm i915 and ttm rely upon this support.
 *
 * i915_gem_object_get_pages_gtt() mixes __GFP_NORETRY | __GFP_NOWARN in
 * with the mapping_gfp_mask(), to avoid OOMing the machine unnecessarily.
 */
struct page *shmem_read_mapping_page_gfp(struct address_space *mapping,
					 pgoff_t index, gfp_t gfp)
{
#ifdef CONFIG_SHMEM
	struct inode *inode = mapping->host;
	struct page *page;
	int error;

	BUG_ON(mapping->a_ops != &shmem_aops);
	error = shmem_getpage_gfp(inode, index, &page, SGP_CACHE, gfp, NULL);
	if (error)
		page = ERR_PTR(error);
	else
		unlock_page(page);
	return page;
#else
	/*
	 * The tiny !SHMEM case uses ramfs without swap
	 */
	return read_cache_page_gfp(mapping, index, gfp);
#endif
}
EXPORT_SYMBOL_GPL(shmem_read_mapping_page_gfp);
