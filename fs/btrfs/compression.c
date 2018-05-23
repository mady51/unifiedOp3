/*
 * Copyright (C) 2008 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/buffer_head.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/mpage.h>
#include <linux/swap.h>
#include <linux/writeback.h>
#include <linux/bit_spinlock.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include <linux/log2.h>
#include "ctree.h"
#include "disk-io.h"
#include "transaction.h"
#include "btrfs_inode.h"
#include "volumes.h"
#include "ordered-data.h"
#include "compression.h"
#include "extent_io.h"
#include "extent_map.h"

struct compressed_bio {
	/* number of bios pending for this compressed extent */
	atomic_t pending_bios;

	/* the pages with the compressed data on them */
	struct page **compressed_pages;

	/* inode that owns this data */
	struct inode *inode;

	/* starting offset in the inode for our pages */
	u64 start;

	/* number of bytes in the inode we're working on */
	unsigned long len;

	/* number of bytes on disk */
	unsigned long compressed_len;

	/* the compression algorithm for this bio */
	int compress_type;

	/* number of compressed pages in the array */
	unsigned long nr_pages;

	/* IO errors */
	int errors;
	int mirror_num;

	/* for reads, this is the bio we are copying the data into */
	struct bio *orig_bio;

	/*
	 * the start of a variable length array of checksums only
	 * used by reads
	 */
	u32 sums;
};

static int btrfs_decompress_biovec(int type, struct page **pages_in,
				   u64 disk_start, struct bio_vec *bvec,
				   int vcnt, size_t srclen);

static inline int compressed_bio_size(struct btrfs_root *root,
				      unsigned long disk_size)
{
	u16 csum_size = btrfs_super_csum_size(root->fs_info->super_copy);

	return sizeof(struct compressed_bio) +
		(DIV_ROUND_UP(disk_size, root->sectorsize)) * csum_size;
}

static struct bio *compressed_bio_alloc(struct block_device *bdev,
					u64 first_byte, gfp_t gfp_flags)
{
	int nr_vecs;

	nr_vecs = bio_get_nr_vecs(bdev);
	return btrfs_bio_alloc(bdev, first_byte >> 9, nr_vecs, gfp_flags);
}

static int check_compressed_csum(struct inode *inode,
				 struct compressed_bio *cb,
				 u64 disk_start)
{
	int ret;
	struct page *page;
	unsigned long i;
	char *kaddr;
	u32 csum;
	u32 *cb_sum = &cb->sums;

	if (BTRFS_I(inode)->flags & BTRFS_INODE_NODATASUM)
		return 0;

	for (i = 0; i < cb->nr_pages; i++) {
		page = cb->compressed_pages[i];
		csum = ~(u32)0;

		kaddr = kmap_atomic(page);
		csum = btrfs_csum_data(kaddr, csum, PAGE_CACHE_SIZE);
		btrfs_csum_final(csum, (char *)&csum);
		kunmap_atomic(kaddr);

		if (csum != *cb_sum) {
			btrfs_info(BTRFS_I(inode)->root->fs_info,
			   "csum failed ino %llu extent %llu csum %u wanted %u mirror %d",
			   btrfs_ino(inode), disk_start, csum, *cb_sum,
			   cb->mirror_num);
			ret = -EIO;
			goto fail;
		}
		cb_sum++;

	}
	ret = 0;
fail:
	return ret;
}

/* when we finish reading compressed pages from the disk, we
 * decompress them and then run the bio end_io routines on the
 * decompressed pages (in the inode address space).
 *
 * This allows the checksumming and other IO error handling routines
 * to work normally
 *
 * The compressed pages are freed here, and it must be run
 * in process context
 */
static void end_compressed_bio_read(struct bio *bio, int err)
{
	struct compressed_bio *cb = bio->bi_private;
	struct inode *inode;
	struct page *page;
	unsigned long index;
	int ret;

	if (err)
		cb->errors = 1;

	/* if there are more bios still pending for this compressed
	 * extent, just exit
	 */
	if (!atomic_dec_and_test(&cb->pending_bios))
		goto out;

	inode = cb->inode;
	ret = check_compressed_csum(inode, cb,
				    (u64)bio->bi_iter.bi_sector << 9);
	if (ret)
		goto csum_failed;

	/* ok, we're the last bio for this extent, lets start
	 * the decompression.
	 */
	ret = btrfs_decompress_biovec(cb->compress_type,
				      cb->compressed_pages,
				      cb->start,
				      cb->orig_bio->bi_io_vec,
				      cb->orig_bio->bi_vcnt,
				      cb->compressed_len);
csum_failed:
	if (ret)
		cb->errors = 1;

	/* release the compressed pages */
	index = 0;
	for (index = 0; index < cb->nr_pages; index++) {
		page = cb->compressed_pages[index];
		page->mapping = NULL;
		page_cache_release(page);
	}

	/* do io completion on the original bio */
	if (cb->errors) {
		bio_io_error(cb->orig_bio);
	} else {
		int i;
		struct bio_vec *bvec;

		/*
		 * we have verified the checksum already, set page
		 * checked so the end_io handlers know about it
		 */
		bio_for_each_segment_all(bvec, cb->orig_bio, i)
			SetPageChecked(bvec->bv_page);

		bio_endio(cb->orig_bio, 0);
	}

	/* finally free the cb struct */
	kfree(cb->compressed_pages);
	kfree(cb);
out:
	bio_put(bio);
}

/*
 * Clear the writeback bits on all of the file
 * pages for a compressed write
 */
static noinline void end_compressed_writeback(struct inode *inode, u64 start,
					      unsigned long ram_size)
{
	unsigned long index = start >> PAGE_CACHE_SHIFT;
	unsigned long end_index = (start + ram_size - 1) >> PAGE_CACHE_SHIFT;
	struct page *pages[16];
	unsigned long nr_pages = end_index - index + 1;
	int i;
	int ret;

	while (nr_pages > 0) {
		ret = find_get_pages_contig(inode->i_mapping, index,
				     min_t(unsigned long,
				     nr_pages, ARRAY_SIZE(pages)), pages);
		if (ret == 0) {
			nr_pages -= 1;
			index += 1;
			continue;
		}
		for (i = 0; i < ret; i++) {
			end_page_writeback(pages[i]);
			page_cache_release(pages[i]);
		}
		nr_pages -= ret;
		index += ret;
	}
	/* the inode may be gone now */
}

/*
 * do the cleanup once all the compressed pages hit the disk.
 * This will clear writeback on the file pages and free the compressed
 * pages.
 *
 * This also calls the writeback end hooks for the file pages so that
 * metadata and checksums can be updated in the file.
 */
static void end_compressed_bio_write(struct bio *bio, int err)
{
	struct extent_io_tree *tree;
	struct compressed_bio *cb = bio->bi_private;
	struct inode *inode;
	struct page *page;
	unsigned long index;

	if (err)
		cb->errors = 1;

	/* if there are more bios still pending for this compressed
	 * extent, just exit
	 */
	if (!atomic_dec_and_test(&cb->pending_bios))
		goto out;

	/* ok, we're the last bio for this extent, step one is to
	 * call back into the FS and do all the end_io operations
	 */
	inode = cb->inode;
	tree = &BTRFS_I(inode)->io_tree;
	cb->compressed_pages[0]->mapping = cb->inode->i_mapping;
	tree->ops->writepage_end_io_hook(cb->compressed_pages[0],
					 cb->start,
					 cb->start + cb->len - 1,
					 NULL, 1);
	cb->compressed_pages[0]->mapping = NULL;

	end_compressed_writeback(inode, cb->start, cb->len);
	/* note, our inode could be gone now */

	/*
	 * release the compressed pages, these came from alloc_page and
	 * are not attached to the inode at all
	 */
	index = 0;
	for (index = 0; index < cb->nr_pages; index++) {
		page = cb->compressed_pages[index];
		page->mapping = NULL;
		page_cache_release(page);
	}

	/* finally free the cb struct */
	kfree(cb->compressed_pages);
	kfree(cb);
out:
	bio_put(bio);
}

/*
 * worker function to build and submit bios for previously compressed pages.
 * The corresponding pages in the inode should be marked for writeback
 * and the compressed pages should have a reference on them for dropping
 * when the IO is complete.
 *
 * This also checksums the file bytes and gets things ready for
 * the end io hooks.
 */
int btrfs_submit_compressed_write(struct inode *inode, u64 start,
				 unsigned long len, u64 disk_start,
				 unsigned long compressed_len,
				 struct page **compressed_pages,
				 unsigned long nr_pages)
{
	struct bio *bio = NULL;
	struct btrfs_root *root = BTRFS_I(inode)->root;
	struct compressed_bio *cb;
	unsigned long bytes_left;
	struct extent_io_tree *io_tree = &BTRFS_I(inode)->io_tree;
	int pg_index = 0;
	struct page *page;
	u64 first_byte = disk_start;
	struct block_device *bdev;
	int ret;
	int skip_sum = BTRFS_I(inode)->flags & BTRFS_INODE_NODATASUM;

	WARN_ON(start & ((u64)PAGE_CACHE_SIZE - 1));
	cb = kmalloc(compressed_bio_size(root, compressed_len), GFP_NOFS);
	if (!cb)
		return -ENOMEM;
	atomic_set(&cb->pending_bios, 0);
	cb->errors = 0;
	cb->inode = inode;
	cb->start = start;
	cb->len = len;
	cb->mirror_num = 0;
	cb->compressed_pages = compressed_pages;
	cb->compressed_len = compressed_len;
	cb->orig_bio = NULL;
	cb->nr_pages = nr_pages;

	bdev = BTRFS_I(inode)->root->fs_info->fs_devices->latest_bdev;

	bio = compressed_bio_alloc(bdev, first_byte, GFP_NOFS);
	if (!bio) {
		kfree(cb);
		return -ENOMEM;
	}
	bio->bi_private = cb;
	bio->bi_end_io = end_compressed_bio_write;
	atomic_inc(&cb->pending_bios);

	/* create and submit bios for the compressed pages */
	bytes_left = compressed_len;
	for (pg_index = 0; pg_index < cb->nr_pages; pg_index++) {
		page = compressed_pages[pg_index];
		page->mapping = inode->i_mapping;
		if (bio->bi_iter.bi_size)
			ret = io_tree->ops->merge_bio_hook(WRITE, page, 0,
							   PAGE_CACHE_SIZE,
							   bio, 0);
		else
			ret = 0;

		page->mapping = NULL;
		if (ret || bio_add_page(bio, page, PAGE_CACHE_SIZE, 0) <
		    PAGE_CACHE_SIZE) {
			bio_get(bio);

			/*
			 * inc the count before we submit the bio so
			 * we know the end IO handler won't happen before
			 * we inc the count.  Otherwise, the cb might get
			 * freed before we're done setting it up
			 */
			atomic_inc(&cb->pending_bios);
			ret = btrfs_bio_wq_end_io(root->fs_info, bio,
					BTRFS_WQ_ENDIO_DATA);
			BUG_ON(ret); /* -ENOMEM */

			if (!skip_sum) {
				ret = btrfs_csum_one_bio(root, inode, bio,
							 start, 1);
				BUG_ON(ret); /* -ENOMEM */
			}

			ret = btrfs_map_bio(root, WRITE, bio, 0, 1);
			BUG_ON(ret); /* -ENOMEM */

			bio_put(bio);

			bio = compressed_bio_alloc(bdev, first_byte, GFP_NOFS);
			BUG_ON(!bio);
			bio->bi_private = cb;
			bio->bi_end_io = end_compressed_bio_write;
			bio_add_page(bio, page, PAGE_CACHE_SIZE, 0);
		}
		if (bytes_left < PAGE_CACHE_SIZE) {
			btrfs_info(BTRFS_I(inode)->root->fs_info,
					"bytes left %lu compress len %lu nr %lu",
			       bytes_left, cb->compressed_len, cb->nr_pages);
		}
		bytes_left -= PAGE_CACHE_SIZE;
		first_byte += PAGE_CACHE_SIZE;
		cond_resched();
	}
	bio_get(bio);

	ret = btrfs_bio_wq_end_io(root->fs_info, bio, BTRFS_WQ_ENDIO_DATA);
	BUG_ON(ret); /* -ENOMEM */

	if (!skip_sum) {
		ret = btrfs_csum_one_bio(root, inode, bio, start, 1);
		BUG_ON(ret); /* -ENOMEM */
	}

	ret = btrfs_map_bio(root, WRITE, bio, 0, 1);
	BUG_ON(ret); /* -ENOMEM */

	bio_put(bio);
	return 0;
}

static noinline int add_ra_bio_pages(struct inode *inode,
				     u64 compressed_end,
				     struct compressed_bio *cb)
{
	unsigned long end_index;
	unsigned long pg_index;
	u64 last_offset;
	u64 isize = i_size_read(inode);
	int ret;
	struct page *page;
	unsigned long nr_pages = 0;
	struct extent_map *em;
	struct address_space *mapping = inode->i_mapping;
	struct extent_map_tree *em_tree;
	struct extent_io_tree *tree;
	u64 end;
	int misses = 0;

	page = cb->orig_bio->bi_io_vec[cb->orig_bio->bi_vcnt - 1].bv_page;
	last_offset = (page_offset(page) + PAGE_CACHE_SIZE);
	em_tree = &BTRFS_I(inode)->extent_tree;
	tree = &BTRFS_I(inode)->io_tree;

	if (isize == 0)
		return 0;

	end_index = (i_size_read(inode) - 1) >> PAGE_CACHE_SHIFT;

	while (last_offset < compressed_end) {
		pg_index = last_offset >> PAGE_CACHE_SHIFT;

		if (pg_index > end_index)
			break;

		rcu_read_lock();
		page = radix_tree_lookup(&mapping->page_tree, pg_index);
		rcu_read_unlock();
		if (page && !radix_tree_exceptional_entry(page)) {
			misses++;
			if (misses > 4)
				break;
			goto next;
		}

		page = __page_cache_alloc(mapping_gfp_mask(mapping) &
								~__GFP_FS);
		if (!page)
			break;

		if (add_to_page_cache_lru(page, mapping, pg_index,
								GFP_NOFS)) {
			page_cache_release(page);
			goto next;
		}

		end = last_offset + PAGE_CACHE_SIZE - 1;
		/*
		 * at this point, we have a locked page in the page cache
		 * for these bytes in the file.  But, we have to make
		 * sure they map to this compressed extent on disk.
		 */
		set_page_extent_mapped(page);
		lock_extent(tree, last_offset, end);
		read_lock(&em_tree->lock);
		em = lookup_extent_mapping(em_tree, last_offset,
					   PAGE_CACHE_SIZE);
		read_unlock(&em_tree->lock);

		if (!em || last_offset < em->start ||
		    (last_offset + PAGE_CACHE_SIZE > extent_map_end(em)) ||
		    (em->block_start >> 9) != cb->orig_bio->bi_iter.bi_sector) {
			free_extent_map(em);
			unlock_extent(tree, last_offset, end);
			unlock_page(page);
			page_cache_release(page);
			break;
		}
		free_extent_map(em);

		if (page->index == end_index) {
			char *userpage;
			size_t zero_offset = isize & (PAGE_CACHE_SIZE - 1);

			if (zero_offset) {
				int zeros;
				zeros = PAGE_CACHE_SIZE - zero_offset;
				userpage = kmap_atomic(page);
				memset(userpage + zero_offset, 0, zeros);
				flush_dcache_page(page);
				kunmap_atomic(userpage);
			}
		}

		ret = bio_add_page(cb->orig_bio, page,
				   PAGE_CACHE_SIZE, 0);

		if (ret == PAGE_CACHE_SIZE) {
			nr_pages++;
			page_cache_release(page);
		} else {
			unlock_extent(tree, last_offset, end);
			unlock_page(page);
			page_cache_release(page);
			break;
		}
next:
		last_offset += PAGE_CACHE_SIZE;
	}
	return 0;
}

/*
 * for a compressed read, the bio we get passed has all the inode pages
 * in it.  We don't actually do IO on those pages but allocate new ones
 * to hold the compressed pages on disk.
 *
 * bio->bi_iter.bi_sector points to the compressed extent on disk
 * bio->bi_io_vec points to all of the inode pages
 * bio->bi_vcnt is a count of pages
 *
 * After the compressed pages are read, we copy the bytes into the
 * bio we were passed and then call the bio end_io calls
 */
int btrfs_submit_compressed_read(struct inode *inode, struct bio *bio,
				 int mirror_num, unsigned long bio_flags)
{
	struct extent_io_tree *tree;
	struct extent_map_tree *em_tree;
	struct compressed_bio *cb;
	struct btrfs_root *root = BTRFS_I(inode)->root;
	unsigned long uncompressed_len = bio->bi_vcnt * PAGE_CACHE_SIZE;
	unsigned long compressed_len;
	unsigned long nr_pages;
	unsigned long pg_index;
	struct page *page;
	struct block_device *bdev;
	struct bio *comp_bio;
	u64 cur_disk_byte = (u64)bio->bi_iter.bi_sector << 9;
	u64 em_len;
	u64 em_start;
	struct extent_map *em;
	int ret = -ENOMEM;
	int faili = 0;
	u32 *sums;

	tree = &BTRFS_I(inode)->io_tree;
	em_tree = &BTRFS_I(inode)->extent_tree;

	/* we need the actual starting offset of this extent in the file */
	read_lock(&em_tree->lock);
	em = lookup_extent_mapping(em_tree,
				   page_offset(bio->bi_io_vec->bv_page),
				   PAGE_CACHE_SIZE);
	read_unlock(&em_tree->lock);
	if (!em)
		return -EIO;

	compressed_len = em->block_len;
	cb = kmalloc(compressed_bio_size(root, compressed_len), GFP_NOFS);
	if (!cb)
		goto out;

	atomic_set(&cb->pending_bios, 0);
	cb->errors = 0;
	cb->inode = inode;
	cb->mirror_num = mirror_num;
	sums = &cb->sums;

	cb->start = em->orig_start;
	em_len = em->len;
	em_start = em->start;

	free_extent_map(em);
	em = NULL;

	cb->len = uncompressed_len;
	cb->compressed_len = compressed_len;
	cb->compress_type = extent_compress_type(bio_flags);
	cb->orig_bio = bio;

	nr_pages = DIV_ROUND_UP(compressed_len, PAGE_CACHE_SIZE);
	cb->compressed_pages = kzalloc(sizeof(struct page *) * nr_pages,
				       GFP_NOFS);
	if (!cb->compressed_pages)
		goto fail1;

	bdev = BTRFS_I(inode)->root->fs_info->fs_devices->latest_bdev;

	for (pg_index = 0; pg_index < nr_pages; pg_index++) {
		cb->compressed_pages[pg_index] = alloc_page(GFP_NOFS |
							      __GFP_HIGHMEM);
		if (!cb->compressed_pages[pg_index]) {
			faili = pg_index - 1;
			ret = -ENOMEM;
			goto fail2;
		}
	}
	faili = nr_pages - 1;
	cb->nr_pages = nr_pages;

	/* In the parent-locked case, we only locked the range we are
	 * interested in.  In all other cases, we can opportunistically
	 * cache decompressed data that goes beyond the requested range. */
	if (!(bio_flags & EXTENT_BIO_PARENT_LOCKED))
		add_ra_bio_pages(inode, em_start + em_len, cb);

	/* include any pages we added in add_ra-bio_pages */
	uncompressed_len = bio->bi_vcnt * PAGE_CACHE_SIZE;
	cb->len = uncompressed_len;

	comp_bio = compressed_bio_alloc(bdev, cur_disk_byte, GFP_NOFS);
	if (!comp_bio)
		goto fail2;
	comp_bio->bi_private = cb;
	comp_bio->bi_end_io = end_compressed_bio_read;
	atomic_inc(&cb->pending_bios);

	for (pg_index = 0; pg_index < nr_pages; pg_index++) {
		page = cb->compressed_pages[pg_index];
		page->mapping = inode->i_mapping;
		page->index = em_start >> PAGE_CACHE_SHIFT;

		if (comp_bio->bi_iter.bi_size)
			ret = tree->ops->merge_bio_hook(READ, page, 0,
							PAGE_CACHE_SIZE,
							comp_bio, 0);
		else
			ret = 0;

		page->mapping = NULL;
		if (ret || bio_add_page(comp_bio, page, PAGE_CACHE_SIZE, 0) <
		    PAGE_CACHE_SIZE) {
			bio_get(comp_bio);

			ret = btrfs_bio_wq_end_io(root->fs_info, comp_bio,
					BTRFS_WQ_ENDIO_DATA);
			BUG_ON(ret); /* -ENOMEM */

			/*
			 * inc the count before we submit the bio so
			 * we know the end IO handler won't happen before
			 * we inc the count.  Otherwise, the cb might get
			 * freed before we're done setting it up
			 */
			atomic_inc(&cb->pending_bios);

			if (!(BTRFS_I(inode)->flags & BTRFS_INODE_NODATASUM)) {
				ret = btrfs_lookup_bio_sums(root, inode,
							comp_bio, sums);
				BUG_ON(ret); /* -ENOMEM */
			}
			sums += DIV_ROUND_UP(comp_bio->bi_iter.bi_size,
					     root->sectorsize);

			ret = btrfs_map_bio(root, READ, comp_bio,
					    mirror_num, 0);
			if (ret)
				bio_endio(comp_bio, ret);

			bio_put(comp_bio);

			comp_bio = compressed_bio_alloc(bdev, cur_disk_byte,
							GFP_NOFS);
			BUG_ON(!comp_bio);
			comp_bio->bi_private = cb;
			comp_bio->bi_end_io = end_compressed_bio_read;

			bio_add_page(comp_bio, page, PAGE_CACHE_SIZE, 0);
		}
		cur_disk_byte += PAGE_CACHE_SIZE;
	}
	bio_get(comp_bio);

	ret = btrfs_bio_wq_end_io(root->fs_info, comp_bio,
			BTRFS_WQ_ENDIO_DATA);
	BUG_ON(ret); /* -ENOMEM */

	if (!(BTRFS_I(inode)->flags & BTRFS_INODE_NODATASUM)) {
		ret = btrfs_lookup_bio_sums(root, inode, comp_bio, sums);
		BUG_ON(ret); /* -ENOMEM */
	}

	ret = btrfs_map_bio(root, READ, comp_bio, mirror_num, 0);
	if (ret)
		bio_endio(comp_bio, ret);

	bio_put(comp_bio);
	return 0;

fail2:
	while (faili >= 0) {
		__free_page(cb->compressed_pages[faili]);
		faili--;
	}

	kfree(cb->compressed_pages);
fail1:
	kfree(cb);
out:
	free_extent_map(em);
	return ret;
}

/*
 * Heuristic uses systematic sampling to collect data from the input data
 * range, the logic can be tuned by the following constants:
 *
 * @SAMPLING_READ_SIZE - how many bytes will be copied from for each sample
 * @SAMPLING_INTERVAL  - range from which the sampled data can be collected
 */
#define SAMPLING_READ_SIZE	(16)
#define SAMPLING_INTERVAL	(256)

/*
 * For statistical analysis of the input data we consider bytes that form a
 * Galois Field of 256 objects. Each object has an attribute count, ie. how
 * many times the object appeared in the sample.
 */
#define BUCKET_SIZE		(256)

/*
 * The size of the sample is based on a statistical sampling rule of thumb.
 * The common way is to perform sampling tests as long as the number of
 * elements in each cell is at least 5.
 *
 * Instead of 5, we choose 32 to obtain more accurate results.
 * If the data contain the maximum number of symbols, which is 256, we obtain a
 * sample size bound by 8192.
 *
 * For a sample of at most 8KB of data per data range: 16 consecutive bytes
 * from up to 512 locations.
 */
#define MAX_SAMPLE_SIZE		(BTRFS_MAX_UNCOMPRESSED *		\
				 SAMPLING_READ_SIZE / SAMPLING_INTERVAL)

struct bucket_item {
	u32 count;
};

struct heuristic_ws {
	/* Partial copy of input data */
	u8 *sample;
	u32 sample_size;
	/* Buckets store counters for each byte value */
	struct bucket_item *bucket;
	/* Sorting buffer */
	struct bucket_item *bucket_b;
	struct list_head list;
};

static void free_heuristic_ws(struct list_head *ws)
{
	struct heuristic_ws *workspace;

	workspace = list_entry(ws, struct heuristic_ws, list);

	kvfree(workspace->sample);
	kfree(workspace->bucket);
	kfree(workspace->bucket_b);
	kfree(workspace);
}

static struct list_head *alloc_heuristic_ws(void)
{
	struct heuristic_ws *ws;

	ws = kzalloc(sizeof(*ws), GFP_KERNEL);
	if (!ws)
		return ERR_PTR(-ENOMEM);

	ws->sample = kvmalloc(MAX_SAMPLE_SIZE, GFP_KERNEL);
	if (!ws->sample)
		goto fail;

	ws->bucket = kcalloc(BUCKET_SIZE, sizeof(*ws->bucket), GFP_KERNEL);
	if (!ws->bucket)
		goto fail;

	ws->bucket_b = kcalloc(BUCKET_SIZE, sizeof(*ws->bucket_b), GFP_KERNEL);
	if (!ws->bucket_b)
		goto fail;

	INIT_LIST_HEAD(&ws->list);
	return &ws->list;
fail:
	free_heuristic_ws(&ws->list);
	return ERR_PTR(-ENOMEM);
}

struct workspaces_list {
	struct list_head idle_ws;
	spinlock_t ws_lock;
	/* Number of free workspaces */
	int free_ws;
	/* Total number of allocated workspaces */
	atomic_t total_ws;
	/* Waiters for a free workspace */
	wait_queue_head_t ws_wait;
};

static struct workspaces_list btrfs_comp_ws[BTRFS_COMPRESS_TYPES];

static struct workspaces_list btrfs_heuristic_ws;

static const struct btrfs_compress_op * const btrfs_compress_op[] = {
	&btrfs_zlib_compress,
	&btrfs_lzo_compress,
};

void __init btrfs_init_compress(void)
{
	int i;

	for (i = 0; i < BTRFS_COMPRESS_TYPES; i++) {
		INIT_LIST_HEAD(&comp_idle_workspace[i]);
		spin_lock_init(&comp_workspace_lock[i]);
		atomic_set(&comp_alloc_workspace[i], 0);
		init_waitqueue_head(&comp_workspace_wait[i]);
	}
}

/*
 * this finds an available workspace or allocates a new one
 * ERR_PTR is returned if things go bad.
 */
static struct list_head *find_workspace(int type)
{
	struct list_head *workspace;
	int cpus = num_online_cpus();
	int idx = type - 1;

	struct list_head *idle_workspace	= &comp_idle_workspace[idx];
	spinlock_t *workspace_lock		= &comp_workspace_lock[idx];
	atomic_t *alloc_workspace		= &comp_alloc_workspace[idx];
	wait_queue_head_t *workspace_wait	= &comp_workspace_wait[idx];
	int *num_workspace			= &comp_num_workspace[idx];
again:
	spin_lock(workspace_lock);
	if (!list_empty(idle_workspace)) {
		workspace = idle_workspace->next;
		list_del(workspace);
		(*num_workspace)--;
		spin_unlock(workspace_lock);
		return workspace;

	}
	if (atomic_read(alloc_workspace) > cpus) {
		DEFINE_WAIT(wait);

		spin_unlock(workspace_lock);
		prepare_to_wait(workspace_wait, &wait, TASK_UNINTERRUPTIBLE);
		if (atomic_read(alloc_workspace) > cpus && !*num_workspace)
			schedule();
		finish_wait(workspace_wait, &wait);
		goto again;
	}
	atomic_inc(alloc_workspace);
	spin_unlock(workspace_lock);

	workspace = btrfs_compress_op[idx]->alloc_workspace();
	if (IS_ERR(workspace)) {
		atomic_dec(alloc_workspace);
		wake_up(workspace_wait);
	}
	return workspace;
}

/*
 * put a workspace struct back on the list or free it if we have enough
 * idle ones sitting around
 */
static void free_workspace(int type, struct list_head *workspace)
{
	int idx = type - 1;
	struct list_head *idle_workspace	= &comp_idle_workspace[idx];
	spinlock_t *workspace_lock		= &comp_workspace_lock[idx];
	atomic_t *alloc_workspace		= &comp_alloc_workspace[idx];
	wait_queue_head_t *workspace_wait	= &comp_workspace_wait[idx];
	int *num_workspace			= &comp_num_workspace[idx];

	spin_lock(workspace_lock);
	if (*num_workspace < num_online_cpus()) {
		list_add(workspace, idle_workspace);
		(*num_workspace)++;
		spin_unlock(workspace_lock);
		goto wake;
	}
	spin_unlock(workspace_lock);

	btrfs_compress_op[idx]->free_workspace(workspace);
	atomic_dec(alloc_workspace);
wake:
	smp_mb();
	if (waitqueue_active(workspace_wait))
		wake_up(workspace_wait);
}

/*
 * cleanup function for module exit
 */
static void free_workspaces(void)
{
	struct list_head *workspace;
	int i;

	for (i = 0; i < BTRFS_COMPRESS_TYPES; i++) {
		while (!list_empty(&comp_idle_workspace[i])) {
			workspace = comp_idle_workspace[i].next;
			list_del(workspace);
			btrfs_compress_op[i]->free_workspace(workspace);
			atomic_dec(&comp_alloc_workspace[i]);
		}
	}
}

/*
 * given an address space and start/len, compress the bytes.
 *
 * pages are allocated to hold the compressed result and stored
 * in 'pages'
 *
 * out_pages is used to return the number of pages allocated.  There
 * may be pages allocated even if we return an error
 *
 * total_in is used to return the number of bytes actually read.  It
 * may be smaller then len if we had to exit early because we
 * ran out of room in the pages array or because we cross the
 * max_out threshold.
 *
 * total_out is used to return the total number of compressed bytes
 *
 * max_out tells us the max number of bytes that we're allowed to
 * stuff into pages
 */
int btrfs_compress_pages(int type, struct address_space *mapping,
			 u64 start, unsigned long len,
			 struct page **pages,
			 unsigned long nr_dest_pages,
			 unsigned long *out_pages,
			 unsigned long *total_in,
			 unsigned long *total_out,
			 unsigned long max_out)
{
	struct list_head *workspace;
	int ret;

	workspace = find_workspace(type);
	if (IS_ERR(workspace))
		return PTR_ERR(workspace);

	ret = btrfs_compress_op[type-1]->compress_pages(workspace, mapping,
						      start, len, pages,
						      nr_dest_pages, out_pages,
						      total_in, total_out,
						      max_out);
	free_workspace(type, workspace);
	return ret;
}

/*
 * pages_in is an array of pages with compressed data.
 *
 * disk_start is the starting logical offset of this array in the file
 *
 * bvec is a bio_vec of pages from the file that we want to decompress into
 *
 * vcnt is the count of pages in the biovec
 *
 * srclen is the number of bytes in pages_in
 *
 * The basic idea is that we have a bio that was created by readpages.
 * The pages in the bio are for the uncompressed data, and they may not
 * be contiguous.  They all correspond to the range of bytes covered by
 * the compressed extent.
 */
static int btrfs_decompress_biovec(int type, struct page **pages_in,
				   u64 disk_start, struct bio_vec *bvec,
				   int vcnt, size_t srclen)
{
	struct list_head *workspace;
	int ret;

	workspace = find_workspace(type);
	if (IS_ERR(workspace))
		return PTR_ERR(workspace);

	ret = btrfs_compress_op[type-1]->decompress_biovec(workspace, pages_in,
							 disk_start,
							 bvec, vcnt, srclen);
	free_workspace(type, workspace);
	return ret;
}

/*
 * a less complex decompression routine.  Our compressed data fits in a
 * single page, and we want to read a single page out of it.
 * start_byte tells us the offset into the compressed data we're interested in
 */
int btrfs_decompress(int type, unsigned char *data_in, struct page *dest_page,
		     unsigned long start_byte, size_t srclen, size_t destlen)
{
	struct list_head *workspace;
	int ret;

	workspace = find_workspace(type);
	if (IS_ERR(workspace))
		return PTR_ERR(workspace);

	ret = btrfs_compress_op[type-1]->decompress(workspace, data_in,
						  dest_page, start_byte,
						  srclen, destlen);

	free_workspace(type, workspace);
	return ret;
}

void btrfs_exit_compress(void)
{
	free_workspaces();
}

/*
 * Copy uncompressed data from working buffer to pages.
 *
 * buf_start is the byte offset we're of the start of our workspace buffer.
 *
 * total_out is the last byte of the buffer
 */
int btrfs_decompress_buf2page(char *buf, unsigned long buf_start,
			      unsigned long total_out, u64 disk_start,
			      struct bio_vec *bvec, int vcnt,
			      unsigned long *pg_index,
			      unsigned long *pg_offset)
{
	unsigned long buf_offset;
	unsigned long current_buf_start;
	unsigned long start_byte;
	unsigned long working_bytes = total_out - buf_start;
	unsigned long bytes;
	char *kaddr;
	struct page *page_out = bvec[*pg_index].bv_page;

	/*
	 * start byte is the first byte of the page we're currently
	 * copying into relative to the start of the compressed data.
	 */
	start_byte = page_offset(page_out) - disk_start;

	/* we haven't yet hit data corresponding to this page */
	if (total_out <= start_byte)
		return 1;

	/*
	 * the start of the data we care about is offset into
	 * the middle of our working buffer
	 */
	if (total_out > start_byte && buf_start < start_byte) {
		buf_offset = start_byte - buf_start;
		working_bytes -= buf_offset;
	} else {
		buf_offset = 0;
	}
	current_buf_start = buf_start;

	/* copy bytes from the working buffer into the pages */
	while (working_bytes > 0) {
		bytes = min(PAGE_CACHE_SIZE - *pg_offset,
			    PAGE_CACHE_SIZE - buf_offset);
		bytes = min(bytes, working_bytes);
		kaddr = kmap_atomic(page_out);
		memcpy(kaddr + *pg_offset, buf + buf_offset, bytes);
		kunmap_atomic(kaddr);
		flush_dcache_page(page_out);

		*pg_offset += bytes;
		buf_offset += bytes;
		working_bytes -= bytes;
		current_buf_start += bytes;

		/* check if we need to pick another page */
		if (*pg_offset == PAGE_CACHE_SIZE) {
			(*pg_index)++;
			if (*pg_index >= vcnt)
				return 0;

			page_out = bvec[*pg_index].bv_page;
			*pg_offset = 0;
			start_byte = page_offset(page_out) - disk_start;

			/*
			 * make sure our new page is covered by this
			 * working buffer
			 */
			if (total_out <= start_byte)
				return 1;

			/*
			 * the next page in the biovec might not be adjacent
			 * to the last page, but it might still be found
			 * inside this working buffer. bump our offset pointer
			 */
			if (total_out > start_byte &&
			    current_buf_start < start_byte) {
				buf_offset = start_byte - buf_start;
				working_bytes = total_out - start_byte;
				current_buf_start = buf_start + buf_offset;
			}
		}
	}

	return 1;
}

/*
 * Pure byte distribution analysis fails to determine compressiability of data.
 * Try calculating entropy to estimate the average minimum number of bits
 * needed to encode the sampled data.
 *
 * For convenience, return the percentage of needed bits, instead of amount of
 * bits directly.
 *
 * @ENTROPY_LVL_ACEPTABLE - below that threshold, sample has low byte entropy
 *			    and can be compressible with high probability
 *
 * @ENTROPY_LVL_HIGH - data are not compressible with high probability
 *
 * Use of ilog2() decreases precision, we lower the LVL to 5 to compensate.
 */
#define ENTROPY_LVL_ACEPTABLE		(65)
#define ENTROPY_LVL_HIGH		(80)

/*
 * For increasead precision in shannon_entropy calculation,
 * let's do pow(n, M) to save more digits after comma:
 *
 * - maximum int bit length is 64
 * - ilog2(MAX_SAMPLE_SIZE)	-> 13
 * - 13 * 4 = 52 < 64		-> M = 4
 *
 * So use pow(n, 4).
 */
static inline u32 ilog2_w(u64 n)
{
	return ilog2(n * n * n * n);
}

static u32 shannon_entropy(struct heuristic_ws *ws)
{
	const u32 entropy_max = 8 * ilog2_w(2);
	u32 entropy_sum = 0;
	u32 p, p_base, sz_base;
	u32 i;

	sz_base = ilog2_w(ws->sample_size);
	for (i = 0; i < BUCKET_SIZE && ws->bucket[i].count > 0; i++) {
		p = ws->bucket[i].count;
		p_base = ilog2_w(p);
		entropy_sum += p * (sz_base - p_base);
	}

	entropy_sum /= ws->sample_size;
	return entropy_sum * 100 / entropy_max;
}

#define RADIX_BASE		4U
#define COUNTERS_SIZE		(1U << RADIX_BASE)

static u8 get4bits(u64 num, int shift) {
	u8 low4bits;

	num >>= shift;
	/* Reverse order */
	low4bits = (COUNTERS_SIZE - 1) - (num % COUNTERS_SIZE);
	return low4bits;
}

static void copy_cell(void *dst, int dest_i, void *src, int src_i)
{
	struct bucket_item *dstv = (struct bucket_item *)dst;
	struct bucket_item *srcv = (struct bucket_item *)src;
	dstv[dest_i] = srcv[src_i];
}

static u64 get_num(const void *a, int i)
{
	struct bucket_item *av = (struct bucket_item *)a;
	return av[i].count;
}

/*
 * Use 4 bits as radix base
 * Use 16 u32 counters for calculating new possition in buf array
 *
 * @array     - array that will be sorted
 * @array_buf - buffer array to store sorting results
 *              must be equal in size to @array
 * @num       - array size
 * @get_num   - function to extract number from array
 * @copy_cell - function to copy data from array to array_buf and vice versa
 * @get4bits  - function to get 4 bits from number at specified offset
 */
static void radix_sort(void *array, void *array_buf, int num,
		       u64 (*get_num)(const void *, int i),
		       void (*copy_cell)(void *dest, int dest_i,
					 void* src, int src_i),
		       u8 (*get4bits)(u64 num, int shift))
{
	u64 max_num;
	u64 buf_num;
	u32 counters[COUNTERS_SIZE];
	u32 new_addr;
	u32 addr;
	int bitlen;
	int shift;
	int i;

	/*
	 * Try avoid useless loop iterations for small numbers stored in big
	 * counters.  Example: 48 33 4 ... in 64bit array
	 */
	max_num = get_num(array, 0);
	for (i = 1; i < num; i++) {
		buf_num = get_num(array, i);
		if (buf_num > max_num)
			max_num = buf_num;
	}

	buf_num = ilog2(max_num);
	bitlen = ALIGN(buf_num, RADIX_BASE * 2);

	shift = 0;
	while (shift < bitlen) {
		memset(counters, 0, sizeof(counters));

		for (i = 0; i < num; i++) {
			buf_num = get_num(array, i);
			addr = get4bits(buf_num, shift);
			counters[addr]++;
		}

		for (i = 1; i < COUNTERS_SIZE; i++)
			counters[i] += counters[i - 1];

		for (i = num - 1; i >= 0; i--) {
			buf_num = get_num(array, i);
			addr = get4bits(buf_num, shift);
			counters[addr]--;
			new_addr = counters[addr];
			copy_cell(array_buf, new_addr, array, i);
		}

		shift += RADIX_BASE;

		/*
		 * Normal radix expects to move data from a temporary array, to
		 * the main one.  But that requires some CPU time. Avoid that
		 * by doing another sort iteration to original array instead of
		 * memcpy()
		 */
		memset(counters, 0, sizeof(counters));

		for (i = 0; i < num; i ++) {
			buf_num = get_num(array_buf, i);
			addr = get4bits(buf_num, shift);
			counters[addr]++;
		}

		for (i = 1; i < COUNTERS_SIZE; i++)
			counters[i] += counters[i - 1];

		for (i = num - 1; i >= 0; i--) {
			buf_num = get_num(array_buf, i);
			addr = get4bits(buf_num, shift);
			counters[addr]--;
			new_addr = counters[addr];
			copy_cell(array, new_addr, array_buf, i);
		}

		shift += RADIX_BASE;
	}
}

/*
 * Size of the core byte set - how many bytes cover 90% of the sample
 *
 * There are several types of structured binary data that use nearly all byte
 * values. The distribution can be uniform and counts in all buckets will be
 * nearly the same (eg. encrypted data). Unlikely to be compressible.
 *
 * Other possibility is normal (Gaussian) distribution, where the data could
 * be potentially compressible, but we have to take a few more steps to decide
 * how much.
 *
 * @BYTE_CORE_SET_LOW  - main part of byte values repeated frequently,
 *                       compression algo can easy fix that
 * @BYTE_CORE_SET_HIGH - data have uniform distribution and with high
 *                       probability is not compressible
 */
#define BYTE_CORE_SET_LOW		(64)
#define BYTE_CORE_SET_HIGH		(200)

static int byte_core_set_size(struct heuristic_ws *ws)
{
	u32 i;
	u32 coreset_sum = 0;
	const u32 core_set_threshold = ws->sample_size * 90 / 100;
	struct bucket_item *bucket = ws->bucket;

	/* Sort in reverse order */
	radix_sort(ws->bucket, ws->bucket_b, BUCKET_SIZE, get_num, copy_cell,
			get4bits);

	for (i = 0; i < BYTE_CORE_SET_LOW; i++)
		coreset_sum += bucket[i].count;

	if (coreset_sum > core_set_threshold)
		return i;

	for (; i < BYTE_CORE_SET_HIGH && bucket[i].count > 0; i++) {
		coreset_sum += bucket[i].count;
		if (coreset_sum > core_set_threshold)
			break;
	}

	return i;
}

/*
 * Count byte values in buckets.
 * This heuristic can detect textual data (configs, xml, json, html, etc).
 * Because in most text-like data byte set is restricted to limited number of
 * possible characters, and that restriction in most cases makes data easy to
 * compress.
 *
 * @BYTE_SET_THRESHOLD - consider all data within this byte set size:
 *	less - compressible
 *	more - need additional analysis
 */
#define BYTE_SET_THRESHOLD		(64)

static u32 byte_set_size(const struct heuristic_ws *ws)
{
	u32 i;
	u32 byte_set_size = 0;

	for (i = 0; i < BYTE_SET_THRESHOLD; i++) {
		if (ws->bucket[i].count > 0)
			byte_set_size++;
	}

	/*
	 * Continue collecting count of byte values in buckets.  If the byte
	 * set size is bigger then the threshold, it's pointless to continue,
	 * the detection technique would fail for this type of data.
	 */
	for (; i < BUCKET_SIZE; i++) {
		if (ws->bucket[i].count > 0) {
			byte_set_size++;
			if (byte_set_size > BYTE_SET_THRESHOLD)
				return byte_set_size;
		}
	}

	return byte_set_size;
}

static bool sample_repeated_patterns(struct heuristic_ws *ws)
{
	const u32 half_of_sample = ws->sample_size / 2;
	const u8 *data = ws->sample;

	return memcmp(&data[0], &data[half_of_sample], half_of_sample) == 0;
}

static void heuristic_collect_sample(struct inode *inode, u64 start, u64 end,
				     struct heuristic_ws *ws)
{
	while (pg_index < vcnt) {
		struct page *page = bvec[pg_index].bv_page;
		unsigned long off = bvec[pg_index].bv_offset;
		unsigned long len = bvec[pg_index].bv_len;

		if (pg_offset < off)
			pg_offset = off;
		if (pg_offset < off + len) {
			unsigned long bytes = off + len - pg_offset;
			char *kaddr;

			kaddr = kmap_atomic(page);
			memset(kaddr + pg_offset, 0, bytes);
			kunmap_atomic(kaddr);
		}
		pg_index++;
		pg_offset = 0;
	}
}
