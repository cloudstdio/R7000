/*
 *  linux/fs/hfsplus/extents.c
 *
 * Copyright (C) 2001
 * Brad Boyer (flar@allandria.com)
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 *
 * Handling of Extents both in catalog and extents overflow trees
 */

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/pagemap.h>

#include "hfsplus_fs.h"
#include "hfsplus_raw.h"

/* Compare two extents keys, returns 0 on same, pos/neg for difference */
int hfsplus_ext_cmp_key(const hfsplus_btree_key *k1,
			const hfsplus_btree_key *k2)
{
	__be32 k1id, k2id;
	__be32 k1s, k2s;

	k1id = k1->ext.cnid;
	k2id = k2->ext.cnid;
	if (k1id != k2id)
		return be32_to_cpu(k1id) < be32_to_cpu(k2id) ? -1 : 1;

	if (k1->ext.fork_type != k2->ext.fork_type)
		return k1->ext.fork_type < k2->ext.fork_type ? -1 : 1;

	k1s = k1->ext.start_block;
	k2s = k2->ext.start_block;
	if (k1s == k2s)
		return 0;
	return be32_to_cpu(k1s) < be32_to_cpu(k2s) ? -1 : 1;
}

static void hfsplus_ext_build_key(hfsplus_btree_key *key, u32 cnid,
				  u32 block, u8 type)
{
	key->key_len = cpu_to_be16(HFSPLUS_EXT_KEYLEN - 2);
	key->ext.cnid = cpu_to_be32(cnid);
	key->ext.start_block = cpu_to_be32(block);
	key->ext.fork_type = type;
	key->ext.pad = 0;
}

static u32 hfsplus_ext_find_block(struct hfsplus_extent *ext, u32 off)
{
	int i;
	u32 count;

	for (i = 0; i < 8; ext++, i++) {
		count = be32_to_cpu(ext->block_count);
		if (off < count)
			return be32_to_cpu(ext->start_block) + off;
		off -= count;
	}
	/* panic? */
	return 0;
}

static int hfsplus_ext_block_count(struct hfsplus_extent *ext)
{
	int i;
	u32 count = 0;

	for (i = 0; i < 8; ext++, i++)
		count += be32_to_cpu(ext->block_count);
	return count;
}

static u32 hfsplus_ext_lastblock(struct hfsplus_extent *ext)
{
	int i;

	ext += 7;
	for (i = 0; i < 7; ext--, i++)
		if (ext->block_count)
			break;
	return be32_to_cpu(ext->start_block) + be32_to_cpu(ext->block_count);
}

static void __hfsplus_ext_write_extent(hfsplus_handle_t *hfsplus_handle, struct inode *inode, struct hfs_find_data *fd)
{
	int res;

	WARN_ON(!mutex_is_locked(&HFSPLUS_I(inode).extents_lock));
	hfsplus_ext_build_key(fd->search_key, inode->i_ino, HFSPLUS_I(inode).cached_start,
			      HFSPLUS_IS_RSRC(inode) ?  HFSPLUS_TYPE_RSRC : HFSPLUS_TYPE_DATA);
	res = hfs_brec_find(hfsplus_handle, fd);
	if (HFSPLUS_I(inode).flags & HFSPLUS_FLG_EXT_NEW) {
		if (res != -ENOENT)
			return;
		hfs_brec_insert(hfsplus_handle, fd, HFSPLUS_I(inode).cached_extents, sizeof(hfsplus_extent_rec));
		HFSPLUS_I(inode).flags &= ~(HFSPLUS_FLG_EXT_DIRTY | HFSPLUS_FLG_EXT_NEW);
	} else {
		if (res)
			return;
		hfs_bnode_write(hfsplus_handle, fd->bnode, HFSPLUS_I(inode).cached_extents, fd->entryoffset, fd->entrylength);
		HFSPLUS_I(inode).flags &= ~HFSPLUS_FLG_EXT_DIRTY;
	}

	/*
	 * We can't just use hfsplus_mark_inode_dirty here, because we
	 * also get called from hfsplus_write_inode, which should not
	 * redirty the inode.  Instead the callers have to be careful
	 * to explicily mark the inode dirty, too.
	 */
}

void hfsplus_ext_write_extent_locked(hfsplus_handle_t *hfsplus_handle, struct inode *inode)
{
	if (HFSPLUS_I(inode).flags & HFSPLUS_FLG_EXT_DIRTY) {
		struct hfs_find_data fd;

		hfs_find_init(HFSPLUS_SB(inode->i_sb).ext_tree, &fd);
		__hfsplus_ext_write_extent(hfsplus_handle, inode, &fd);
		hfs_find_exit(hfsplus_handle, &fd);
	}
}

void hfsplus_ext_write_extent(hfsplus_handle_t *hfsplus_handle,struct inode *inode)
{

	mutex_lock(&HFSPLUS_I(inode).extents_lock);
	hfsplus_ext_write_extent_locked(hfsplus_handle,inode);
	mutex_unlock(&HFSPLUS_I(inode).extents_lock);
}
static inline int __hfsplus_ext_read_extent(hfsplus_handle_t *hfsplus_handle, struct hfs_find_data *fd,
					    struct hfsplus_extent *extent,
					    u32 cnid, u32 block, u8 type)
{
	int res;

	hfsplus_ext_build_key(fd->search_key, cnid, block, type);
	fd->key->ext.cnid = 0;
	res = hfs_brec_find(hfsplus_handle, fd);
	if (res && res != -ENOENT)
		return res;
	if (fd->key->ext.cnid != fd->search_key->ext.cnid ||
	    fd->key->ext.fork_type != fd->search_key->ext.fork_type)
		return -ENOENT;
	if (fd->entrylength != sizeof(hfsplus_extent_rec))
		return -EIO;
	hfs_bnode_read(fd->bnode, extent, fd->entryoffset, sizeof(hfsplus_extent_rec));
	return 0;
}

static inline int __hfsplus_ext_cache_extent(hfsplus_handle_t *hfsplus_handle, struct hfs_find_data *fd, struct inode *inode, u32 block)
{
	int res;

	WARN_ON(!mutex_is_locked(&HFSPLUS_I(inode).extents_lock));
	if (HFSPLUS_I(inode).flags & HFSPLUS_FLG_EXT_DIRTY)
		__hfsplus_ext_write_extent(hfsplus_handle, inode, fd);

	res = __hfsplus_ext_read_extent(hfsplus_handle, fd, HFSPLUS_I(inode).cached_extents, inode->i_ino,
					block, HFSPLUS_IS_RSRC(inode) ? HFSPLUS_TYPE_RSRC : HFSPLUS_TYPE_DATA);
	if (!res) {
		HFSPLUS_I(inode).cached_start = be32_to_cpu(fd->key->ext.start_block);
		HFSPLUS_I(inode).cached_blocks = hfsplus_ext_block_count(HFSPLUS_I(inode).cached_extents);
	} else {
		HFSPLUS_I(inode).cached_start = HFSPLUS_I(inode).cached_blocks = 0;
		HFSPLUS_I(inode).flags &= ~(HFSPLUS_FLG_EXT_DIRTY | HFSPLUS_FLG_EXT_NEW);
	}
	return res;
}

static int hfsplus_ext_read_extent(hfsplus_handle_t *hfsplus_handle, struct inode *inode, u32 block)
{
	struct hfs_find_data fd;
	int res;

	if (block >= HFSPLUS_I(inode).cached_start &&
	    block < HFSPLUS_I(inode).cached_start + HFSPLUS_I(inode).cached_blocks)
		return 0;

	hfs_find_init(HFSPLUS_SB(inode->i_sb).ext_tree, &fd);
	res = __hfsplus_ext_cache_extent(hfsplus_handle, &fd, inode, block);
	hfs_find_exit(hfsplus_handle, &fd);
	return res;
}

/* Get a block at iblock for inode, possibly allocating if create */
int hfsplus_get_block(struct inode *inode, sector_t iblock,
		      struct buffer_head *bh_result, int create)
{
	struct super_block *sb;
	int res = -EIO;
	u32 ablock, dblock, mask;
	int shift;
	hfsplus_handle_t *hfsplus_handle, tmp_hfsplus_handle;

	tmp_hfsplus_handle.journaled = !HFSPLUS_JOURNAL_PRESENT;
	tmp_hfsplus_handle.handle = NULL;

	sb = inode->i_sb;

	/* Journal device */
	if (HFSPLUS_SB(sb).jnl.journaled == HFSPLUS_JOURNAL_PRESENT) {
		/* Write Metadata */
		if (((inode->i_mapping->a_ops == &hfsplus_journalled_btree_aops) || 
			(inode->i_mapping->a_ops == &hfsplus_journalled_aops)) && create) {
			hfsplus_handle = hfsplus_jbd_current_handle();
			if (hfsplus_handle == NULL) {
				printk("hfsplus_handle is NULL\n");
				hfsplus_handle = &tmp_hfsplus_handle; 
			}
		}
		else { 
			hfsplus_handle = &tmp_hfsplus_handle;
		}
	}
	/* Non-journal device */ 
	else {
		hfsplus_handle = &tmp_hfsplus_handle;
	}

	/* Convert inode block to disk allocation block */
	shift = HFSPLUS_SB(sb).alloc_blksz_shift - sb->s_blocksize_bits;
	ablock = iblock >> HFSPLUS_SB(sb).fs_shift;

	if (iblock >= HFSPLUS_I(inode).fs_blocks) {
		if (iblock > HFSPLUS_I(inode).fs_blocks || !create) {
			return -EIO;
		}
		if (ablock >= HFSPLUS_I(inode).alloc_blocks) {
			res = hfsplus_file_extend(hfsplus_handle, inode);
			if (res) {
				return res;
			}
		}
	} else
		create = 0;

	if (ablock < HFSPLUS_I(inode).first_blocks) {
		dblock = hfsplus_ext_find_block(HFSPLUS_I(inode).first_extents, ablock);
		goto done;
	}

	if (inode->i_ino == HFSPLUS_EXT_CNID)
		return -EIO;

	mutex_lock(&HFSPLUS_I(inode).extents_lock);
	res = hfsplus_ext_read_extent(hfsplus_handle, inode, ablock);
	if (res) {
		mutex_unlock(&HFSPLUS_I(inode).extents_lock);
		return -EIO;
	}
	dblock = hfsplus_ext_find_block(HFSPLUS_I(inode).cached_extents,
					ablock - HFSPLUS_I(inode).cached_start);
	mutex_unlock(&HFSPLUS_I(inode).extents_lock);

done:
	dprint(DBG_EXTENT, "get_block(%lu): %llu - %u\n", inode->i_ino, (long long)iblock, dblock);
	mask = (1 << HFSPLUS_SB(sb).fs_shift) - 1;
	map_bh(bh_result, sb, (dblock << HFSPLUS_SB(sb).fs_shift) + HFSPLUS_SB(sb).blockoffset + (iblock & mask));
	if (create) {
		set_buffer_new(bh_result);
		HFSPLUS_I(inode).phys_size += sb->s_blocksize;
		HFSPLUS_I(inode).fs_blocks++;
		inode_add_bytes(inode, sb->s_blocksize);
		if (hfsplus_journalled_mark_inode_dirty(__FUNCTION__, hfsplus_handle, inode)) {
			printk("HFS+-fs: Error in %s()\n", __FUNCTION__);
			return -1;
		}
	}
	if (create)
		mark_inode_dirty(inode);
	return 0;
}

static void hfsplus_dump_extent(struct hfsplus_extent *extent)
{
	int i;

	dprint(DBG_EXTENT, "   ");
	for (i = 0; i < 8; i++)
		dprint(DBG_EXTENT, " %u:%u", be32_to_cpu(extent[i].start_block),
				 be32_to_cpu(extent[i].block_count));
	dprint(DBG_EXTENT, "\n");
}

static int hfsplus_add_extent(struct hfsplus_extent *extent, u32 offset,
			      u32 alloc_block, u32 block_count)
{
	u32 count, start;
	int i;

	hfsplus_dump_extent(extent);
	for (i = 0; i < 8; extent++, i++) {
		count = be32_to_cpu(extent->block_count);
		if (offset == count) {
			start = be32_to_cpu(extent->start_block);
			if (alloc_block != start + count) {
				if (++i >= 8)
					return -ENOSPC;
				extent++;
				extent->start_block = cpu_to_be32(alloc_block);
			} else
				block_count += count;
			extent->block_count = cpu_to_be32(block_count);
			return 0;
		} else if (offset < count)
			break;
		offset -= count;
	}
	/* panic? */
	return -EIO;
}

static int hfsplus_free_extents(hfsplus_handle_t *hfsplus_handle, struct super_block *sb,
				struct hfsplus_extent *extent,
				u32 offset, u32 block_nr)
{
	u32 count, start;
	int i;

	hfsplus_dump_extent(extent);
	for (i = 0; i < 8; extent++, i++) {
		count = be32_to_cpu(extent->block_count);
		if (offset == count)
			goto found;
		else if (offset < count)
			break;
		offset -= count;
	}
	/* panic? */
	return -EIO;
found:
	for (;;) {
		start = be32_to_cpu(extent->start_block);
		if (count <= block_nr) {
			hfsplus_block_free(hfsplus_handle, sb, start, count);
			extent->block_count = 0;
			extent->start_block = 0;
			block_nr -= count;
		} else {
			count -= block_nr;
			hfsplus_block_free(hfsplus_handle, sb, start + count, block_nr);
			extent->block_count = cpu_to_be32(count);
			block_nr = 0;
		}
		if (!block_nr || !i)
			return 0;
		i--;
		extent--;
		count = be32_to_cpu(extent->block_count);
	}
}

int hfsplus_free_fork(hfsplus_handle_t *hfsplus_handle, struct super_block *sb, u32 cnid, struct hfsplus_fork_raw *fork, int type)
{
	struct hfs_find_data fd;
	hfsplus_extent_rec ext_entry;
	u32 total_blocks, blocks, start;
	int res, i;

	total_blocks = be32_to_cpu(fork->total_blocks);
	if (!total_blocks)
		return 0;

	blocks = 0;
	for (i = 0; i < 8; i++)
		blocks += be32_to_cpu(fork->extents[i].block_count);

	res = hfsplus_free_extents(hfsplus_handle, sb, fork->extents, blocks, blocks);
	if (res)
		return res;
	if (total_blocks == blocks)
		return 0;

	hfs_find_init(HFSPLUS_SB(sb).ext_tree, &fd);
	do {
		res = __hfsplus_ext_read_extent(hfsplus_handle, &fd, ext_entry, cnid,
						total_blocks, type);
		if (res)
			break;
		start = be32_to_cpu(fd.key->ext.start_block);
		hfsplus_free_extents(hfsplus_handle, sb, ext_entry,
				     total_blocks - start,
				     total_blocks);
		hfs_brec_remove(hfsplus_handle, &fd);
		total_blocks = start;
	} while (total_blocks > blocks);
	hfs_find_exit(hfsplus_handle, &fd);

	return res;
}

int hfsplus_file_extend(hfsplus_handle_t *hfsplus_handle, struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	u32 start, len, goal;
	int res;

	if (HFSPLUS_SB(sb).alloc_file->i_size * 8 < HFSPLUS_SB(sb).total_blocks - HFSPLUS_SB(sb).free_blocks + 8) {
		// extend alloc file
		printk(KERN_ERR "hfs: extend alloc file! (%Lu,%u,%u)\n", HFSPLUS_SB(sb).alloc_file->i_size * 8,
			HFSPLUS_SB(sb).total_blocks, HFSPLUS_SB(sb).free_blocks);
		return -ENOSPC;
	}

	mutex_lock(&HFSPLUS_I(inode).extents_lock);
	if (HFSPLUS_I(inode).alloc_blocks == HFSPLUS_I(inode).first_blocks)
		goal = hfsplus_ext_lastblock(HFSPLUS_I(inode).first_extents);
	else {
		res = hfsplus_ext_read_extent(hfsplus_handle, inode, HFSPLUS_I(inode).alloc_blocks);
		if (res)
			goto out;
		goal = hfsplus_ext_lastblock(HFSPLUS_I(inode).cached_extents);
	}

//	len = HFSPLUS_I(inode).clump_blocks;
	len = 0x100;
	start = hfsplus_block_allocate(hfsplus_handle, sb, HFSPLUS_SB(sb).total_blocks, goal, &len);
	if (start >= HFSPLUS_SB(sb).total_blocks) {
		start = hfsplus_block_allocate(hfsplus_handle, sb, goal, 0, &len);
		if (start >= goal) {
			res = -ENOSPC;
			goto out;
		}
	}

	dprint(DBG_EXTENT, "extend %lu: %u,%u\n", inode->i_ino, start, len);
	if (HFSPLUS_I(inode).alloc_blocks <= HFSPLUS_I(inode).first_blocks) {
		if (!HFSPLUS_I(inode).first_blocks) {
			dprint(DBG_EXTENT, "first extents\n");
			/* no extents yet */
			HFSPLUS_I(inode).first_extents[0].start_block = cpu_to_be32(start);
			HFSPLUS_I(inode).first_extents[0].block_count = cpu_to_be32(len);
			res = 0;
		} else {
			/* try to append to extents in inode */
			res = hfsplus_add_extent(HFSPLUS_I(inode).first_extents,
						 HFSPLUS_I(inode).alloc_blocks,
						 start, len);
			if (res == -ENOSPC)
				goto insert_extent;
		}
		if (!res) {
			hfsplus_dump_extent(HFSPLUS_I(inode).first_extents);
			HFSPLUS_I(inode).first_blocks += len;
		}
	} else {
		res = hfsplus_add_extent(HFSPLUS_I(inode).cached_extents,
					 HFSPLUS_I(inode).alloc_blocks -
					 HFSPLUS_I(inode).cached_start,
					 start, len);
		if (!res) {
			hfsplus_dump_extent(HFSPLUS_I(inode).cached_extents);
			HFSPLUS_I(inode).flags |= HFSPLUS_FLG_EXT_DIRTY;
			HFSPLUS_I(inode).cached_blocks += len;
		} else if (res == -ENOSPC)
			goto insert_extent;
	}
out:
	mutex_unlock(&HFSPLUS_I(inode).extents_lock);
	if (!res) {
		HFSPLUS_I(inode).alloc_blocks += len;
		res = hfsplus_journalled_mark_inode_dirty(__FUNCTION__, hfsplus_handle, inode);
	}
	return res;

insert_extent:
	dprint(DBG_EXTENT, "insert new extent\n");
	hfsplus_ext_write_extent_locked(hfsplus_handle,inode);

	memset(HFSPLUS_I(inode).cached_extents, 0, sizeof(hfsplus_extent_rec));
	HFSPLUS_I(inode).cached_extents[0].start_block = cpu_to_be32(start);
	HFSPLUS_I(inode).cached_extents[0].block_count = cpu_to_be32(len);
	hfsplus_dump_extent(HFSPLUS_I(inode).cached_extents);
	HFSPLUS_I(inode).flags |= HFSPLUS_FLG_EXT_DIRTY | HFSPLUS_FLG_EXT_NEW;
	HFSPLUS_I(inode).cached_start = HFSPLUS_I(inode).alloc_blocks;
	HFSPLUS_I(inode).cached_blocks = len;

	res = 0;
	goto out;
}

void hfsplus_file_truncate(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct hfs_find_data fd;
	u32 alloc_cnt, blk_cnt, start;
	int res;
	hfsplus_handle_t hfsplus_handle;

	dprint(DBG_INODE, "truncate: %lu, %Lu -> %Lu\n", inode->i_ino,
	       (long long)HFSPLUS_I(inode).phys_size, inode->i_size);
	if (inode->i_size > HFSPLUS_I(inode).phys_size) {
		struct address_space *mapping = inode->i_mapping;
		struct page *page;
		void *fsdata;
		u32 size = inode->i_size;
		int res;

		res = pagecache_write_begin(NULL, mapping, size, 0,
						AOP_FLAG_UNINTERRUPTIBLE,
						&page, &fsdata);
		if (res)
			return;
		res = pagecache_write_end(NULL, mapping, size, 0, 0, page, fsdata);
		if (res < 0)
		if (hfsplus_journal_start(__FUNCTION__, sb, &hfsplus_handle))
			return;

		hfsplus_journalled_mark_inode_dirty(__FUNCTION__, &hfsplus_handle, inode);
		hfsplus_journal_stop(&hfsplus_handle);
		return;
	} else if (inode->i_size == HFSPLUS_I(inode).phys_size)
		return;

	if (hfsplus_journal_start(__FUNCTION__, sb, &hfsplus_handle))
		return;

	blk_cnt = (inode->i_size + HFSPLUS_SB(sb).alloc_blksz - 1) >> HFSPLUS_SB(sb).alloc_blksz_shift;
	alloc_cnt = HFSPLUS_I(inode).alloc_blocks;
	if (blk_cnt == alloc_cnt)
		goto out;

	mutex_lock(&HFSPLUS_I(inode).extents_lock);
	hfs_find_init(HFSPLUS_SB(sb).ext_tree, &fd);
	while (1) {
		if (alloc_cnt == HFSPLUS_I(inode).first_blocks) {
			hfsplus_free_extents(&hfsplus_handle, sb, HFSPLUS_I(inode).first_extents,
					     alloc_cnt, alloc_cnt - blk_cnt);
			hfsplus_dump_extent(HFSPLUS_I(inode).first_extents);
			HFSPLUS_I(inode).first_blocks = blk_cnt;
			break;
		}
		res = __hfsplus_ext_cache_extent(&hfsplus_handle, &fd, inode, alloc_cnt);
		if (res)
			break;
		start = HFSPLUS_I(inode).cached_start;
		hfsplus_free_extents(&hfsplus_handle, sb, HFSPLUS_I(inode).cached_extents,
				     alloc_cnt - start, alloc_cnt - blk_cnt);
		hfsplus_dump_extent(HFSPLUS_I(inode).cached_extents);
		if (blk_cnt > start) {
			HFSPLUS_I(inode).flags |= HFSPLUS_FLG_EXT_DIRTY;
			break;
		}
		alloc_cnt = start;
		HFSPLUS_I(inode).cached_start = HFSPLUS_I(inode).cached_blocks = 0;
		HFSPLUS_I(inode).flags &= ~(HFSPLUS_FLG_EXT_DIRTY | HFSPLUS_FLG_EXT_NEW);
		hfs_brec_remove(&hfsplus_handle, &fd);
	}
	hfs_find_exit(&hfsplus_handle, &fd);
	mutex_unlock(&HFSPLUS_I(inode).extents_lock);

	HFSPLUS_I(inode).alloc_blocks = blk_cnt;
out:
	HFSPLUS_I(inode).phys_size = inode->i_size;
	HFSPLUS_I(inode).fs_blocks = (inode->i_size + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
	inode_set_bytes(inode, HFSPLUS_I(inode).fs_blocks << sb->s_blocksize_bits);
	hfsplus_journalled_mark_inode_dirty(__FUNCTION__, &hfsplus_handle, inode);
    /* Foxconn added start pling 05/31/2010 */
    /* Set the i_blocks field properly */
    inode->i_blocks = inode->i_size/512;
    if (inode->i_size % 512)
        inode->i_blocks++;
    /* Foxconn added end pling 05/31/2010 */
	hfsplus_journal_stop(&hfsplus_handle);
}
