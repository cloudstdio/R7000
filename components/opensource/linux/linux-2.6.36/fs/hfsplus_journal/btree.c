/*
 *  linux/fs/hfsplus/btree.c
 *
 * Copyright (C) 2001
 * Brad Boyer (flar@allandria.com)
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 *
 * Handle opening/closing btree
 */

#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/log2.h>

#include "hfsplus_fs.h"
#include "hfsplus_raw.h"


/* Get a reference to a B*Tree and do some initial checks */
struct hfs_btree *hfs_btree_open(struct super_block *sb, u32 id)
{
	struct hfs_btree *tree;
	struct hfs_btree_header_rec *head;
	struct address_space *mapping;
	struct inode *inode;
	struct page *page;
	unsigned int size;

	tree = kzalloc(sizeof(*tree), GFP_KERNEL);
	if (!tree)
		return NULL;

	mutex_init(&tree->tree_lock);
	spin_lock_init(&tree->hash_lock);
	tree->sb = sb;
	tree->cnid = id;
	inode = hfsplus_iget(sb, id);
	if (IS_ERR(inode))
		goto free_tree;
	tree->inode = inode;

	if (!HFSPLUS_I(tree->inode).first_blocks) {
		printk(KERN_ERR
		       "hfs: invalid btree extent records (0 size).\n");
		goto free_inode;
	}

	mapping = tree->inode->i_mapping;
	page = read_mapping_page(mapping, 0, NULL);
	if (IS_ERR(page))
		goto free_inode;

	/* Load the header */
	head = (struct hfs_btree_header_rec *)(kmap(page) + sizeof(struct hfs_bnode_desc));
	tree->root = be32_to_cpu(head->root);
	tree->leaf_count = be32_to_cpu(head->leaf_count);
	tree->leaf_head = be32_to_cpu(head->leaf_head);
	tree->leaf_tail = be32_to_cpu(head->leaf_tail);
	tree->node_count = be32_to_cpu(head->node_count);
	tree->free_nodes = be32_to_cpu(head->free_nodes);
	tree->attributes = be32_to_cpu(head->attributes);
	tree->node_size = be16_to_cpu(head->node_size);
	tree->max_key_len = be16_to_cpu(head->max_key_len);
	tree->depth = be16_to_cpu(head->depth);

	/* Verify the tree and set the correct compare function */
	switch (id) {
	case HFSPLUS_EXT_CNID:
		if (tree->max_key_len != HFSPLUS_EXT_KEYLEN - sizeof(u16)) {
			printk(KERN_ERR "hfs: invalid extent max_key_len %d\n",
				tree->max_key_len);
			goto fail_page;
		}
		if (tree->attributes & HFS_TREE_VARIDXKEYS) {
			printk(KERN_ERR "hfs: invalid extent btree flag\n");
			goto fail_page;
		}

		tree->keycmp = hfsplus_ext_cmp_key;
		break;
	case HFSPLUS_CAT_CNID:
		if (tree->max_key_len != HFSPLUS_CAT_KEYLEN - sizeof(u16)) {
			printk(KERN_ERR "hfs: invalid catalog max_key_len %d\n",
				tree->max_key_len);
			goto fail_page;
		}
		if (!(tree->attributes & HFS_TREE_VARIDXKEYS)) {
			printk(KERN_ERR "hfs: invalid catalog btree flag\n");
			goto fail_page;
		}

		if (test_bit(HFSPLUS_SB_HFSX, &HFSPLUS_SB(sb).flags) &&
		    (head->key_type == HFSPLUS_KEY_BINARY))
			tree->keycmp = hfsplus_cat_bin_cmp_key;
		else {
			tree->keycmp = hfsplus_cat_case_cmp_key;
			set_bit(HFSPLUS_SB_CASEFOLD, &HFSPLUS_SB(sb).flags);
		}
		break;
	default:
		printk(KERN_ERR "hfs: unknown B*Tree requested\n");
		goto fail_page;
	}

	if (!(tree->attributes & HFS_TREE_BIGKEYS)) {
		printk(KERN_ERR "hfs: invalid btree flag\n");
		goto fail_page;
	}

	size = tree->node_size;
	if (!is_power_of_2(size))
		goto fail_page;
	if (!tree->node_count)
		goto fail_page;
	tree->node_size_shift = ffs(size) - 1;

	tree->pages_per_bnode = (tree->node_size + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;

	kunmap(page);
	page_cache_release(page);
	return tree;

 fail_page:
	tree->inode->i_mapping->a_ops = &hfsplus_aops;
	page_cache_release(page);
 free_inode:
	iput(tree->inode);
 free_tree:
	kfree(tree);
	return NULL;
}

/* Release resources used by a btree */
void hfs_btree_close(struct hfs_btree *tree)
{
	struct hfs_bnode *node;
	int i;

	if (!tree)
		return;

	for (i = 0; i < NODE_HASH_SIZE; i++) {
		while ((node = tree->node_hash[i])) {
			tree->node_hash[i] = node->next_hash;
			if (atomic_read(&node->refcnt))
				printk(KERN_CRIT "hfs: node %d:%d still has %d user(s)!\n",
					node->tree->cnid, node->this, atomic_read(&node->refcnt));
			hfs_bnode_free(node);
			tree->node_hash_cnt--;
		}
	}
	iput(tree->inode);
	kfree(tree);
}

void hfs_btree_write(hfsplus_handle_t *hfsplus_handle, struct hfs_btree *tree)
{
	struct hfs_btree_header_rec *head;
	struct hfs_bnode *node;
	struct page *page;

	node = hfs_bnode_find(hfsplus_handle, tree, 0);
	if (IS_ERR(node))
		/* panic? */
		return;
	/* Load the header */
	page = node->page[0];
	head = (struct hfs_btree_header_rec *)(kmap(page) + sizeof(struct hfs_bnode_desc));

	head->root = cpu_to_be32(tree->root);
	head->leaf_count = cpu_to_be32(tree->leaf_count);
	head->leaf_head = cpu_to_be32(tree->leaf_head);
	head->leaf_tail = cpu_to_be32(tree->leaf_tail);
	head->node_count = cpu_to_be32(tree->node_count);
	head->free_nodes = cpu_to_be32(tree->free_nodes);
	head->attributes = cpu_to_be32(tree->attributes);
	head->depth = cpu_to_be16(tree->depth);

	kunmap(page);
	hfsplus_journalled_set_page_dirty(hfsplus_handle, page);
	hfs_bnode_put(hfsplus_handle, node);
}

static struct hfs_bnode *hfs_bmap_new_bmap(hfsplus_handle_t *hfsplus_handle, struct hfs_bnode *prev, u32 idx)
{
	struct hfs_btree *tree = prev->tree;
	struct hfs_bnode *node;
	struct hfs_bnode_desc desc;
	__be32 cnid;

	node = hfs_bnode_create(hfsplus_handle, tree, idx);
	if (IS_ERR(node))
		return node;

	tree->free_nodes--;
	prev->next = idx;
	cnid = cpu_to_be32(idx);
	hfs_bnode_write(hfsplus_handle, prev, &cnid, offsetof(struct hfs_bnode_desc, next), 4);

	node->type = HFS_NODE_MAP;
	node->num_recs = 1;
	hfs_bnode_clear(hfsplus_handle, node, 0, tree->node_size);
	desc.next = 0;
	desc.prev = 0;
	desc.type = HFS_NODE_MAP;
	desc.height = 0;
	desc.num_recs = cpu_to_be16(1);
	desc.reserved = 0;
	hfs_bnode_write(hfsplus_handle, node, &desc, 0, sizeof(desc));
	hfs_bnode_write_u16(hfsplus_handle, node, 14, 0x8000);
	hfs_bnode_write_u16(hfsplus_handle, node, tree->node_size - 2, 14);
	hfs_bnode_write_u16(hfsplus_handle, node, tree->node_size - 4, tree->node_size - 6);

	return node;
}

struct hfs_bnode *hfs_bmap_alloc(hfsplus_handle_t *hfsplus_handle, struct hfs_btree *tree)
{
	struct hfs_bnode *node, *next_node;
	struct page **pagep;
	u32 nidx, idx;
	unsigned off;
	u16 off16;
	u16 len;
	u8 *data, byte, m;
	int i;

	while (!tree->free_nodes) {
		struct inode *inode = tree->inode;
		u32 count;
		int res;

		res = hfsplus_file_extend(hfsplus_handle, inode);
		if (res)
			return ERR_PTR(res);
		HFSPLUS_I(inode).phys_size = inode->i_size =
				(loff_t)HFSPLUS_I(inode).alloc_blocks <<
				HFSPLUS_SB(tree->sb).alloc_blksz_shift;
		HFSPLUS_I(inode).fs_blocks = HFSPLUS_I(inode).alloc_blocks <<
					     HFSPLUS_SB(tree->sb).fs_shift;
		inode_set_bytes(inode, inode->i_size);
        /* Foxconn added start pling 05/31/2010 */
        /* Set the i_blocks field properly */
        inode->i_blocks = inode->i_size/512;
        if (inode->i_size % 512)
            inode->i_blocks++;
        /* Foxconn added end pling 05/31/2010 */
		count = inode->i_size >> tree->node_size_shift;
		tree->free_nodes = count - tree->node_count;
		tree->node_count = count;
	}

	nidx = 0;
	node = hfs_bnode_find(hfsplus_handle, tree, nidx);
	if (IS_ERR(node))
		return node;
	len = hfs_brec_lenoff(node, 2, &off16);
	off = off16;

	off += node->page_offset;
	pagep = node->page + (off >> PAGE_CACHE_SHIFT);
	data = kmap(*pagep);
	off &= ~PAGE_CACHE_MASK;
	idx = 0;

	for (;;) {
		while (len) {
			byte = data[off];
			if (byte != 0xff) {
				for (m = 0x80, i = 0; i < 8; m >>= 1, i++) {
					if (!(byte & m)) {
						idx += i;
						data[off] |= m;
						hfsplus_journalled_set_page_dirty(hfsplus_handle, *pagep);
						kunmap(*pagep);
						tree->free_nodes--;
						if (hfsplus_journalled_mark_inode_dirty(__FUNCTION__, hfsplus_handle, tree->inode))
							return NULL;
						hfs_bnode_put(hfsplus_handle, node);
						return hfs_bnode_create(hfsplus_handle, tree, idx);
					}
				}
			}
			if (++off >= PAGE_CACHE_SIZE) {
				kunmap(*pagep);
				data = kmap(*++pagep);
				off = 0;
			}
			idx += 8;
			len--;
		}
		kunmap(*pagep);
		nidx = node->next;
		if (!nidx) {
			printk(KERN_DEBUG "hfs: create new bmap node...\n");
			next_node = hfs_bmap_new_bmap(hfsplus_handle, node, idx);
		} else
			next_node = hfs_bnode_find(hfsplus_handle, tree, nidx);
		hfs_bnode_put(hfsplus_handle, node);
		if (IS_ERR(next_node))
			return next_node;
		node = next_node;

		len = hfs_brec_lenoff(node, 0, &off16);
		off = off16;
		off += node->page_offset;
		pagep = node->page + (off >> PAGE_CACHE_SHIFT);
		data = kmap(*pagep);
		off &= ~PAGE_CACHE_MASK;
	}
}

void hfs_bmap_free(hfsplus_handle_t *hfsplus_handle, struct hfs_bnode *node)
{
	struct hfs_btree *tree;
	struct page *page;
	u16 off, len;
	u32 nidx;
	u8 *data, byte, m;

	dprint(DBG_BNODE_MOD, "btree_free_node: %u\n", node->this);
	BUG_ON(!node->this);
	tree = node->tree;
	nidx = node->this;
	node = hfs_bnode_find(hfsplus_handle, tree, 0);
	if (IS_ERR(node))
		return;
	len = hfs_brec_lenoff(node, 2, &off);
	while (nidx >= len * 8) {
		u32 i;

		nidx -= len * 8;
		i = node->next;
		hfs_bnode_put(hfsplus_handle, node);
		if (!i) {
			/* panic */;
			printk(KERN_CRIT "hfs: unable to free bnode %u. bmap not found!\n", node->this);
			return;
		}
		node = hfs_bnode_find(hfsplus_handle, tree, i);
		if (IS_ERR(node))
			return;
		if (node->type != HFS_NODE_MAP) {
			/* panic */;
			printk(KERN_CRIT "hfs: invalid bmap found! (%u,%d)\n", node->this, node->type);
			hfs_bnode_put(hfsplus_handle, node);
			return;
		}
		len = hfs_brec_lenoff(node, 0, &off);
	}
	off += node->page_offset + nidx / 8;
	page = node->page[off >> PAGE_CACHE_SHIFT];
	data = kmap(page);
	off &= ~PAGE_CACHE_MASK;
	m = 1 << (~nidx & 7);
	byte = data[off];
	if (!(byte & m)) {
		printk(KERN_CRIT "hfs: trying to free free bnode %u(%d)\n", node->this, node->type);
		kunmap(page);
		hfs_bnode_put(hfsplus_handle, node);
		return;
	}
	data[off] = byte & ~m;
	hfsplus_journalled_set_page_dirty(hfsplus_handle, page);
	kunmap(page);
	hfs_bnode_put(hfsplus_handle, node);
	tree->free_nodes++;
	if (hfsplus_journalled_mark_inode_dirty(__FUNCTION__, hfsplus_handle, tree->inode))
		return;
}
