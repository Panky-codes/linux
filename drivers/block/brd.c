// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ram backed block device driver.
 *
 * Copyright (C) 2007 Nick Piggin
 * Copyright (C) 2007 Novell Inc.
 *
 * Parts derived from drivers/block/rd.c, and drivers/block/loop.c, copyright
 * of their respective owners.
 */

#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/radix-tree.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/debugfs.h>

#include <linux/uaccess.h>

/*
 * Each block ramdisk device has a radix_tree brd_folios of folios that stores
 * the folios containing the block device's contents. A brd folio's ->index is
 * its offset in brd_blksize units. This is similar to, but in no way connected
 * with, the kernel's pagecache or buffer cache (which sit above our block
 * device).
 */
struct brd_device {
	int			brd_number;
	struct gendisk		*brd_disk;
	struct list_head	brd_list;

	/*
	 * Backing store of folios and lock to protect it.
	 * This is the contents of the block device.
	 */
	spinlock_t		brd_lock;
	struct radix_tree_root	brd_folios;
	u64			brd_nr_folios;
	unsigned int		brd_sector_shift;
	unsigned int		brd_sector_size;
};

/*
 * Look up and return a brd's folio for a given sector.
 */
static struct folio *brd_lookup_folio(struct brd_device *brd, sector_t sector)
{
	pgoff_t idx;
	struct folio *folio;
	unsigned int rd_sector_shift = brd->brd_sector_shift - SECTOR_SHIFT;

	/*
	 * The folio lifetime is protected by the fact that we have opened the
	 * device node -- brd folios will never be deleted under us, so we
	 * don't need any further locking or refcounting.
	 *
	 * This is strictly true for the radix-tree nodes as well (ie. we
	 * don't actually need the rcu_read_lock()), however that is not a
	 * documented feature of the radix-tree API so it is better to be
	 * safe here (we don't have total exclusion from radix tree updates
	 * here, only deletes).
	 */
	rcu_read_lock();
	idx = sector >> rd_sector_shift; /* sector to folio index */
	folio = radix_tree_lookup(&brd->brd_folios, idx);
	rcu_read_unlock();

	BUG_ON(folio && folio->index != idx);

	return folio;
}

/*
 * Insert a new folio for a given sector, if one does not already exist.
 */
static int brd_insert_folio(struct brd_device *brd, sector_t sector, gfp_t gfp)
{
	pgoff_t idx;
	struct folio *folio;
	unsigned int rd_sector_shift = brd->brd_sector_shift - SECTOR_SHIFT;
	unsigned int rd_sector_order = get_order(brd->brd_sector_size);
	int ret = 0;

	folio = brd_lookup_folio(brd, sector);
	if (folio)
		return 0;

	folio = folio_alloc(gfp | __GFP_ZERO, rd_sector_order);
	if (!folio)
		return -ENOMEM;

	if (radix_tree_maybe_preload(gfp)) {
		folio_put(folio);
		return -ENOMEM;
	}

	spin_lock(&brd->brd_lock);
	idx = sector >> rd_sector_shift;
	folio->index = idx;
	if (radix_tree_insert(&brd->brd_folios, idx, folio)) {
		folio_put(folio);
		folio = radix_tree_lookup(&brd->brd_folios, idx);
		if (!folio)
			ret = -ENOMEM;
		else if (folio->index != idx)
			ret = -EIO;
	} else {
		brd->brd_nr_folios++;
	}
	spin_unlock(&brd->brd_lock);

	radix_tree_preload_end();
	return ret;
}

/*
 * Free all backing store folios and radix tree. This must only be called when
 * there are no other users of the device.
 */
#define FREE_BATCH 16
static void brd_free_folios(struct brd_device *brd)
{
	unsigned long pos = 0;
	struct folio *folios[FREE_BATCH];
	int nr_folios;

	do {
		int i;

		nr_folios = radix_tree_gang_lookup(&brd->brd_folios,
				(void **)folios, pos, FREE_BATCH);

		for (i = 0; i < nr_folios; i++) {
			void *ret;

			BUG_ON(folios[i]->index < pos);
			pos = folios[i]->index;
			ret = radix_tree_delete(&brd->brd_folios, pos);
			BUG_ON(!ret || ret != folios[i]);
			folio_put(folios[i]);
		}

		pos++;

		/*
		 * It takes 3.4 seconds to remove 80GiB ramdisk.
		 * So, we need cond_resched to avoid stalling the CPU.
		 */
		cond_resched();

		/*
		 * This assumes radix_tree_gang_lookup always returns as
		 * many folios as possible. If the radix-tree code changes,
		 * so will this have to.
		 */
	} while (nr_folios == FREE_BATCH);
}

/*
 * copy_to_brd_setup must be called before copy_to_brd. It may sleep.
 */
static int copy_to_brd_setup(struct brd_device *brd, sector_t sector, size_t n,
			     gfp_t gfp)
{
	unsigned int rd_sectors_shift = brd->brd_sector_shift - SECTOR_SHIFT;
	unsigned int rd_sectors = 1 << rd_sectors_shift;
	unsigned int rd_sector_size = brd->brd_sector_size;
	unsigned int offset = (sector & (rd_sectors - 1)) << SECTOR_SHIFT;
	size_t copy;
	int ret;

	copy = min_t(size_t, n, rd_sector_size - offset);
	ret = brd_insert_folio(brd, sector, gfp);
	if (ret)
		return ret;
	if (copy < n) {
		sector += copy >> SECTOR_SHIFT;
		ret = brd_insert_folio(brd, sector, gfp);
	}
	return ret;
}

/*
 * Copy n bytes from src to the brd starting at sector. Does not sleep.
 */
static void copy_to_brd(struct brd_device *brd, const void *src,
			sector_t sector, size_t n)
{
	struct folio *folio;
	void *dst;
	unsigned int rd_sectors_shift = brd->brd_sector_shift - SECTOR_SHIFT;
	unsigned int rd_sectors = 1 << rd_sectors_shift;
	unsigned int rd_sector_size = brd->brd_sector_size;
	unsigned int offset = (sector & (rd_sectors - 1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, rd_sector_size - offset);
	folio = brd_lookup_folio(brd, sector);
	BUG_ON(!folio);

	dst = kmap_local_folio(folio, offset);
	memcpy(dst, src, copy);
	kunmap_local(dst);

	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		folio = brd_lookup_folio(brd, sector);
		BUG_ON(!folio);

		dst = kmap_local_folio(folio, 0);
		memcpy(dst, src, copy);
		kunmap_local(dst);
	}
}

/*
 * Copy n bytes to dst from the brd starting at sector. Does not sleep.
 */
static void copy_from_brd(void *dst, struct brd_device *brd,
			sector_t sector, size_t n)
{
	struct folio *folio;
	void *src;
	unsigned int rd_sectors_shift = brd->brd_sector_shift - SECTOR_SHIFT;
	unsigned int rd_sectors = 1 << rd_sectors_shift;
	unsigned int rd_sector_size = brd->brd_sector_size;
	unsigned int offset = (sector & (rd_sectors - 1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, rd_sector_size - offset);
	folio = brd_lookup_folio(brd, sector);
	if (folio) {
		src = kmap_local_folio(folio, offset);
		memcpy(dst, src, copy);
		kunmap_local(src);
	} else
		memset(dst, 0, copy);

	if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		folio = brd_lookup_folio(brd, sector);
		if (folio) {
			src = kmap_local_folio(folio, 0);
			memcpy(dst, src, copy);
			kunmap_local(src);
		} else
			memset(dst, 0, copy);
	}
}

/*
 * Process a single folio of a bio.
 */
static int brd_do_folio(struct brd_device *brd, struct folio *folio,
			unsigned int len, unsigned int off, blk_opf_t opf,
			sector_t sector)
{
	void *mem;
	int err = 0;

	if (op_is_write(opf)) {
		/*
		 * Must use NOIO because we don't want to recurse back into the
		 * block or filesystem layers from folio reclaim.
		 */
		gfp_t gfp = opf & REQ_NOWAIT ? GFP_NOWAIT : GFP_NOIO;

		err = copy_to_brd_setup(brd, sector, len, gfp);
		if (err)
			goto out;
	}

	mem = kmap_local_folio(folio, off);
	if (!op_is_write(opf)) {
		copy_from_brd(mem, brd, sector, len);
		flush_dcache_folio(folio);
	} else {
		flush_dcache_folio(folio);
		copy_to_brd(brd, mem, sector, len);
	}
	kunmap_local(mem);

out:
	return err;
}

static void brd_submit_bio(struct bio *bio)
{
	struct brd_device *brd = bio->bi_bdev->bd_disk->private_data;
	sector_t sector = bio->bi_iter.bi_sector;
	struct folio_iter iter;

	bio_for_each_folio_all(iter, bio) {
		unsigned int len = iter.length;
		int err;

		/* Don't support un-aligned buffer */
		WARN_ON_ONCE((iter.offset & (SECTOR_SIZE - 1)) ||
				(len & (SECTOR_SIZE - 1)));

		err = brd_do_folio(brd, iter.folio, len, iter.offset,
				   bio->bi_opf, sector);
		if (err) {
			if (err == -ENOMEM && bio->bi_opf & REQ_NOWAIT) {
				bio_wouldblock_error(bio);
				return;
			}
			bio_io_error(bio);
			return;
		}
		sector += len >> SECTOR_SHIFT;
	}

	bio_endio(bio);
}

static const struct block_device_operations brd_fops = {
	.owner =		THIS_MODULE,
	.submit_bio =		brd_submit_bio,
};

/*
 * And now the modules code and kernel interface.
 */
static int rd_nr = CONFIG_BLK_DEV_RAM_COUNT;
module_param(rd_nr, int, 0444);
MODULE_PARM_DESC(rd_nr, "Maximum number of brd devices");

unsigned long rd_size = CONFIG_BLK_DEV_RAM_SIZE;
module_param(rd_size, ulong, 0444);
MODULE_PARM_DESC(rd_size, "Size of each RAM disk in kbytes.");

static int max_part = 1;
module_param(max_part, int, 0444);
MODULE_PARM_DESC(max_part, "Num Minors to reserve between devices");

static unsigned int rd_blksize = PAGE_SIZE;
module_param(rd_blksize, uint, 0444);
MODULE_PARM_DESC(rd_blksize, "Blocksize of each RAM disk in bytes.");

MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(RAMDISK_MAJOR);
MODULE_ALIAS("rd");

#ifndef MODULE
/* Legacy boot options - nonmodular */
static int __init ramdisk_size(char *str)
{
	rd_size = simple_strtol(str, NULL, 0);
	return 1;
}
__setup("ramdisk_size=", ramdisk_size);
#endif

/*
 * The device scheme is derived from loop.c. Keep them in synch where possible
 * (should share code eventually).
 */
static LIST_HEAD(brd_devices);
static struct dentry *brd_debugfs_dir;

static int brd_alloc(int i)
{
	struct brd_device *brd;
	struct gendisk *disk;
	char buf[DISK_NAME_LEN];
	int err = -ENOMEM;

	list_for_each_entry(brd, &brd_devices, brd_list)
		if (brd->brd_number == i)
			return -EEXIST;
	brd = kzalloc(sizeof(*brd), GFP_KERNEL);
	if (!brd)
		return -ENOMEM;
	brd->brd_number		= i;
	list_add_tail(&brd->brd_list, &brd_devices);
	brd->brd_sector_shift = ilog2(rd_blksize);
	brd->brd_sector_size = rd_blksize;

	spin_lock_init(&brd->brd_lock);
	INIT_RADIX_TREE(&brd->brd_folios, GFP_ATOMIC);

	snprintf(buf, DISK_NAME_LEN, "ram%d", i);
	if (!IS_ERR_OR_NULL(brd_debugfs_dir))
		debugfs_create_u64(buf, 0444, brd_debugfs_dir,
				&brd->brd_nr_folios);

	disk = brd->brd_disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!disk)
		goto out_free_dev;

	disk->major		= RAMDISK_MAJOR;
	disk->first_minor	= i * max_part;
	disk->minors		= max_part;
	disk->fops		= &brd_fops;
	disk->private_data	= brd;
	strscpy(disk->disk_name, buf, DISK_NAME_LEN);
	set_capacity(disk, rd_size * 2);
	
	/*
	 * This is so fdisk will align partitions on 4k, because of
	 * direct_access API needing 4k alignment, returning a PFN
	 * (This is only a problem on very small devices <= 4M,
	 *  otherwise fdisk will align on 1M. Regardless this call
	 *  is harmless)
	 */
	blk_queue_physical_block_size(disk->queue, rd_blksize);

	/* Tell the block layer that this is not a rotational device */
	blk_queue_flag_set(QUEUE_FLAG_NONROT, disk->queue);
	blk_queue_flag_set(QUEUE_FLAG_SYNCHRONOUS, disk->queue);
	blk_queue_flag_set(QUEUE_FLAG_NOWAIT, disk->queue);
	err = add_disk(disk);
	if (err)
		goto out_cleanup_disk;

	return 0;

out_cleanup_disk:
	put_disk(disk);
out_free_dev:
	list_del(&brd->brd_list);
	kfree(brd);
	return err;
}

static void brd_probe(dev_t dev)
{
	brd_alloc(MINOR(dev) / max_part);
}

static void brd_cleanup(void)
{
	struct brd_device *brd, *next;

	debugfs_remove_recursive(brd_debugfs_dir);

	list_for_each_entry_safe(brd, next, &brd_devices, brd_list) {
		del_gendisk(brd->brd_disk);
		put_disk(brd->brd_disk);
		brd_free_folios(brd);
		list_del(&brd->brd_list);
		kfree(brd);
	}
}

static inline void brd_check_and_reset_par(void)
{
	if (unlikely(!max_part))
		max_part = 1;

	/*
	 * make sure 'max_part' can be divided exactly by (1U << MINORBITS),
	 * otherwise, it is possiable to get same dev_t when adding partitions.
	 */
	if ((1U << MINORBITS) % max_part != 0)
		max_part = 1UL << fls(max_part);

	if (max_part > DISK_MAX_PARTS) {
		pr_info("brd: max_part can't be larger than %d, reset max_part = %d.\n",
			DISK_MAX_PARTS, DISK_MAX_PARTS);
		max_part = DISK_MAX_PARTS;
	}
}

static int __init brd_init(void)
{
	int err, i;

	brd_check_and_reset_par();

	brd_debugfs_dir = debugfs_create_dir("ramdisk_folios", NULL);

	for (i = 0; i < rd_nr; i++) {
		err = brd_alloc(i);
		if (err)
			goto out_free;
	}

	/*
	 * brd module now has a feature to instantiate underlying device
	 * structure on-demand, provided that there is an access dev node.
	 *
	 * (1) if rd_nr is specified, create that many upfront. else
	 *     it defaults to CONFIG_BLK_DEV_RAM_COUNT
	 * (2) User can further extend brd devices by create dev node themselves
	 *     and have kernel automatically instantiate actual device
	 *     on-demand. Example:
	 *		mknod /path/devnod_name b 1 X	# 1 is the rd major
	 *		fdisk -l /path/devnod_name
	 *	If (X / max_part) was not already created it will be created
	 *	dynamically.
	 */

	if (__register_blkdev(RAMDISK_MAJOR, "ramdisk", brd_probe)) {
		err = -EIO;
		goto out_free;
	}

	pr_info("brd: module loaded\n");
	return 0;

out_free:
	brd_cleanup();

	pr_info("brd: module NOT loaded !!!\n");
	return err;
}

static void __exit brd_exit(void)
{

	unregister_blkdev(RAMDISK_MAJOR, "ramdisk");
	brd_cleanup();

	pr_info("brd: module unloaded\n");
}

module_init(brd_init);
module_exit(brd_exit);

