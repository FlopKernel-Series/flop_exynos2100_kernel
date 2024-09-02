/*
 * Compressed RAM block device
 *
 * Copyright (C) 2008, 2009, 2010  Nitin Gupta
 *               2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#define KMSG_COMPONENT "zram"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/sysfs.h>
#include <linux/debugfs.h>
#include <linux/cpuhotplug.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/statfs.h>
#include <linux/swap.h>
#include <uapi/linux/falloc.h>
#include <uapi/linux/sched/types.h>
#include <linux/kernel_read_file.h>

#include "zram_drv.h"
#include "../loop.h"

#define NON_LRU_SWAPPINESS 99

#define print_hex_dump_fmt(src, size) \
	print_hex_dump(KERN_ERR, "", DUMP_PREFIX_OFFSET, 16, 1, src, size, 1)

static DEFINE_IDR(zram_index_idr);
/* idr index must be protected */
static DEFINE_MUTEX(zram_index_mutex);

static int zram_major;
static const char *default_compressor = CONFIG_ZRAM_DEFAULT_COMP_ALGORITHM;

static bool is_lzorle;
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
static unsigned char lzo_marker[4] = {0x11, 0x00, 0x00};
#endif

/* Module params (documentation at end) */
static unsigned int num_devices = 1;
/*
 * Pages that compress to sizes equals or greater than this are stored
 * uncompressed in memory.
 */
static size_t huge_class_size;

static const struct block_device_operations zram_devops;

static void zram_free_page(struct zram *zram, size_t index);
static int zram_read_page(struct zram *zram, struct page *page,
			u32 index, struct bio *parent);

static int zram_slot_trylock(struct zram *zram, u32 index)
{
	return spin_trylock(&zram->table[index].lock);
}

static void zram_slot_lock(struct zram *zram, u32 index)
{
	spin_lock(&zram->table[index].lock);
}

static void zram_slot_unlock(struct zram *zram, u32 index)
{
	spin_unlock(&zram->table[index].lock);
}

static inline bool init_done(struct zram *zram)
{
	return zram->disksize;
}

static inline struct zram *dev_to_zram(struct device *dev)
{
	return (struct zram *)dev_to_disk(dev)->private_data;
}

static unsigned long zram_get_handle(struct zram *zram, u32 index)
{
	return zram->table[index].handle;
}

static void zram_set_handle(struct zram *zram, u32 index, unsigned long handle)
{
	zram->table[index].handle = handle;
}

/* flag operations require table entry bit_spin_lock() being held */
static bool zram_test_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	return zram->table[index].flags & BIT(flag);
}

static void zram_set_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags |= BIT(flag);
}

static void zram_clear_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags &= ~BIT(flag);
}

static inline void zram_set_element(struct zram *zram, u32 index,
			unsigned long element)
{
	zram->table[index].element = element;
}

static unsigned long zram_get_element(struct zram *zram, u32 index)
{
	return zram->table[index].element;
}

static size_t zram_get_obj_size(struct zram *zram, u32 index)
{
	return zram->table[index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1);
}

static void zram_set_obj_size(struct zram *zram,
					u32 index, size_t size)
{
	unsigned long flags = zram->table[index].flags >> ZRAM_FLAG_SHIFT;

	zram->table[index].flags = (flags << ZRAM_FLAG_SHIFT) | size;
}

static inline bool zram_allocated(struct zram *zram, u32 index)
{
	return zram_get_obj_size(zram, index) ||
			zram_test_flag(zram, index, ZRAM_SAME) ||
			zram_test_flag(zram, index, ZRAM_WB);
}

#if PAGE_SIZE != 4096
static inline bool is_partial_io(struct bio_vec *bvec)
{
	return bvec->bv_len != PAGE_SIZE;
}
#define ZRAM_PARTIAL_IO		1
#else
static inline bool is_partial_io(struct bio_vec *bvec)
{
	return false;
}
#endif

static inline void zram_set_priority(struct zram *zram, u32 index, u32 prio)
{
	prio &= ZRAM_COMP_PRIORITY_MASK;
	/*
	 * Clear previous priority value first, in case if we recompress
	 * further an already recompressed page
	 */
	zram->table[index].flags &= ~(ZRAM_COMP_PRIORITY_MASK <<
				      ZRAM_COMP_PRIORITY_BIT1);
	zram->table[index].flags |= (prio << ZRAM_COMP_PRIORITY_BIT1);
}

static inline u32 zram_get_priority(struct zram *zram, u32 index)
{
	u32 prio = zram->table[index].flags >> ZRAM_COMP_PRIORITY_BIT1;

	return prio & ZRAM_COMP_PRIORITY_MASK;
}

static void update_position(u32 *index, int *offset, struct bio_vec *bvec)
{
	*index  += (*offset + bvec->bv_len) / PAGE_SIZE;
	*offset = (*offset + bvec->bv_len) % PAGE_SIZE;
}

static inline void update_used_max(struct zram *zram,
					const unsigned long pages)
{
	unsigned long cur_max = atomic_long_read(&zram->stats.max_used_pages);

	do {
		if (cur_max >= pages)
			return;
	} while (!atomic_long_try_cmpxchg(&zram->stats.max_used_pages,
					  &cur_max, pages));
}

static inline void zram_fill_page(void *ptr, unsigned long len,
					unsigned long value)
{
	WARN_ON_ONCE(!IS_ALIGNED(len, sizeof(unsigned long)));
	memset_l(ptr, value, len / sizeof(unsigned long));
}

static bool page_same_filled(void *ptr, unsigned long *element)
{
	unsigned int pos;
	unsigned long *page;
	unsigned long val;

	page = (unsigned long *)ptr;
	val = page[0];

	for (pos = 1; pos < PAGE_SIZE / sizeof(*page); pos++) {
		if (val != page[pos])
			return false;
	}

	*element = val;

	return true;
}

static ssize_t initstate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u32 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	val = init_done(zram);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t disksize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", zram->disksize);
}

static ssize_t mem_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 limit;
	char *tmp;
	struct zram *zram = dev_to_zram(dev);

	limit = memparse(buf, &tmp);
	if (buf == tmp) /* no chars parsed, invalid input */
		return -EINVAL;

	down_write(&zram->init_lock);
	zram->limit_pages = PAGE_ALIGN(limit) >> PAGE_SHIFT;
	up_write(&zram->init_lock);

	return len;
}

static ssize_t mem_used_max_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int err;
	unsigned long val;
	struct zram *zram = dev_to_zram(dev);

	err = kstrtoul(buf, 10, &val);
	if (err || val != 0)
		return -EINVAL;

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		atomic_long_set(&zram->stats.max_used_pages,
				zs_get_total_pages(zram->mem_pool));
	}
	up_read(&zram->init_lock);

	return len;
}

/*
 * Mark all pages which are older than or equal to cutoff as IDLE.
 * Callers should hold the zram init lock in read mode
 */
static void mark_idle(struct zram *zram, ktime_t cutoff)
{
	int is_idle = 1;
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	int index;

	for (index = 0; index < nr_pages; index++) {
		/*
		 * Do not mark ZRAM_UNDER_WB slot as ZRAM_IDLE to close race.
		 * See the comment in writeback_store.
		 */
		zram_slot_lock(zram, index);
		if (zram_allocated(zram, index) &&
				!zram_test_flag(zram, index, ZRAM_UNDER_WB)) {
#ifdef CONFIG_ZRAM_MEMORY_TRACKING
			is_idle = !cutoff || ktime_after(cutoff, zram->table[index].ac_time);
#endif
			if (is_idle)
				zram_set_flag(zram, index, ZRAM_IDLE);
		}
		zram_slot_unlock(zram, index);
	}
}

static ssize_t idle_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	ktime_t cutoff_time = 0;
	ssize_t rv = -EINVAL;

	if (!sysfs_streq(buf, "all")) {
		/*
		 * If it did not parse as 'all' try to treat it as an integer
		 * when we have memory tracking enabled.
		 */
		u64 age_sec;

		if (IS_ENABLED(CONFIG_ZRAM_MEMORY_TRACKING) && !kstrtoull(buf, 0, &age_sec))
			cutoff_time = ktime_sub(ktime_get_boottime(),
					ns_to_ktime(age_sec * NSEC_PER_SEC));
		else
			goto out;
	}

	down_read(&zram->init_lock);
	if (!init_done(zram))
		goto out_unlock;

	/*
	 * A cutoff_time of 0 marks everything as idle, this is the
	 * "all" behavior.
	 */
	mark_idle(zram, cutoff_time);
	rv = len;

out_unlock:
	up_read(&zram->init_lock);
out:
	return rv;
}

#ifdef CONFIG_ZRAM_WRITEBACK
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
#define LRU_LIMIT_RATIO 3
#define ZWBS_ALIGN_MASK (~(NR_ZWBS - 1))
static int zram_wbd(void *);
static struct zram *g_zram;
static bool is_app_launch;

static void fallocate_block(struct zram *zram, unsigned long blk_idx)
{
	struct block_device *bdev = zram->bdev;

	if (!bdev)
		return;

	mutex_lock(&zram->blk_bitmap_lock);
	/* check 2MB block bitmap. if unset, fallocate 2MB block at once */
	if (!test_and_set_bit(blk_idx / NR_FALLOC_PAGES, zram->blk_bitmap)) {
		struct loop_device *lo = bdev->bd_disk->private_data;
		struct file *file = lo->lo_backing_file;
		loff_t pos = (blk_idx & FALLOC_ALIGN_MASK) << PAGE_SHIFT;
		loff_t len = NR_FALLOC_PAGES << PAGE_SHIFT;
		int mode = FALLOC_FL_KEEP_SIZE;
		int ret;

		file_start_write(file);
		ret = file->f_op->fallocate(file, mode, pos, len);
		if (ret)
			pr_err("%s pos %lx failed %d\n", __func__, pos, ret);
		file_end_write(file);
	}
	mutex_unlock(&zram->blk_bitmap_lock);
}

static int init_lru_writeback(struct zram *zram)
{
	struct sched_param param = { .sched_priority = 0 };
	int ret = 0;
	int bitmap_sz;

	init_waitqueue_head(&zram->wbd_wait);
	zram->wb_table = kvzalloc(sizeof(u8) * zram->nr_pages, GFP_KERNEL);
	if (!zram->wb_table) {
		ret = -ENOMEM;
		return ret;
	}
	/* bitmap for 2MB block */
	bitmap_sz = (BITS_TO_LONGS(zram->nr_pages) * sizeof(long)) / NR_FALLOC_PAGES;
	zram->blk_bitmap = kvzalloc(bitmap_sz, GFP_KERNEL);
	if (!zram->blk_bitmap) {
		ret = -ENOMEM;
		goto out;
	}

	bitmap_sz = BITS_TO_LONGS(zram->nr_pages) * sizeof(long) / NR_ZWBS;
	/* backing dev should be large enough for chunk writeback */
	if (!bitmap_sz)
		return -EINVAL;
	zram->chunk_bitmap = kvzalloc(bitmap_sz, GFP_KERNEL);
	if (!zram->chunk_bitmap) {
		ret = -ENOMEM;
		goto out;
	}
	zram->read_req_bitmap = kvzalloc(bitmap_sz, GFP_KERNEL);
	if (!zram->read_req_bitmap) {
		ret = -ENOMEM;
		goto out;
	}

	zram->wbd = kthread_run(zram_wbd, zram, "%s_wbd", zram->disk->disk_name);
	if (IS_ERR(zram->wbd)) {
		ret = PTR_ERR(zram->wbd);
		goto out;
	}

	zram->wb_limit_enable = true;
	sched_setscheduler(zram->wbd, SCHED_IDLE, &param);
	zram->nr_lru_pages = (zram->nr_pages * LRU_LIMIT_RATIO / 10) & ZWBS_ALIGN_MASK;

	return ret;
out:
	if (zram->read_req_bitmap) {
		kvfree(zram->read_req_bitmap);
		zram->read_req_bitmap = NULL;
	}
	if (zram->chunk_bitmap) {
		kvfree(zram->chunk_bitmap);
		zram->chunk_bitmap = NULL;
	}
	if (zram->blk_bitmap) {
		kvfree(zram->blk_bitmap);
		zram->blk_bitmap = NULL;
	}
	kvfree(zram->wb_table);
	zram->wb_table = NULL;
	return ret;
}

static void stop_lru_writeback(struct zram *zram)
{
	if (!IS_ERR_OR_NULL(zram->wbd)) {
		kthread_stop(zram->wbd);
		zram->wbd = NULL;
	}
}

static void deinit_lru_writeback(struct zram *zram)
{
	unsigned long flags;
	u8 *wb_table_tmp = zram->wb_table;

	stop_lru_writeback(zram);
	if (zram->read_req_bitmap) {
		kvfree(zram->read_req_bitmap);
		zram->read_req_bitmap = NULL;
	}
	if (zram->chunk_bitmap) {
		kvfree(zram->chunk_bitmap);
		zram->chunk_bitmap = NULL;
	}
	if (zram->blk_bitmap) {
		kvfree(zram->blk_bitmap);
		zram->blk_bitmap = NULL;
	}
	spin_lock_irqsave(&zram->wb_table_lock, flags);
	zram->wb_table = NULL;
	spin_unlock_irqrestore(&zram->wb_table_lock, flags);
	kvfree(wb_table_tmp);
}
#endif

static ssize_t writeback_limit_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 val;
	ssize_t ret = -EINVAL;

	if (kstrtoull(buf, 10, &val))
		return ret;

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	zram->wb_limit_enable = val;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);
	ret = len;

	return ret;
}

static ssize_t writeback_limit_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	bool val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	val = zram->wb_limit_enable;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t writeback_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 val;
	ssize_t ret = -EINVAL;

	if (kstrtoull(buf, 10, &val))
		return ret;

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	zram->bd_wb_limit = val;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);
	ret = len;

	return ret;
}

static ssize_t writeback_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	val = zram->bd_wb_limit;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", val);
}

static void reset_bdev(struct zram *zram)
{
	struct block_device *bdev;

	if (!zram->backing_dev)
		return;

	bdev = zram->bdev;
	blkdev_put(bdev, zram);
	/* hope filp_close flush all of IO */
	filp_close(zram->backing_dev, NULL);
	zram->backing_dev = NULL;
	zram->bdev = NULL;
	zram->disk->fops = &zram_devops;
	kvfree(zram->bitmap);
	zram->bitmap = NULL;
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	deinit_lru_writeback(zram);
#endif
}

static ssize_t backing_dev_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct file *file;
	struct zram *zram = dev_to_zram(dev);
	char *p;
	ssize_t ret;

	down_read(&zram->init_lock);
	file = zram->backing_dev;
	if (!file) {
		memcpy(buf, "none\n", 5);
		up_read(&zram->init_lock);
		return 5;
	}

	p = file_path(file, buf, PAGE_SIZE - 1);
	if (IS_ERR(p)) {
		ret = PTR_ERR(p);
		goto out;
	}

	ret = strlen(p);
	memmove(buf, p, ret);
	buf[ret++] = '\n';
out:
	up_read(&zram->init_lock);
	return ret;
}

static ssize_t backing_dev_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	char *file_name;
	size_t sz;
	struct file *backing_dev = NULL;
	struct inode *inode;
	struct address_space *mapping;
	unsigned int bitmap_sz;
	unsigned long nr_pages, *bitmap = NULL;
	struct block_device *bdev = NULL;
	int err;
	struct zram *zram = dev_to_zram(dev);

	file_name = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!file_name)
		return -ENOMEM;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Can't setup backing device for initialized device\n");
		err = -EBUSY;
		goto out;
	}

	strscpy(file_name, buf, PATH_MAX);
	/* ignore trailing newline */
	sz = strlen(file_name);
	if (sz > 0 && file_name[sz - 1] == '\n')
		file_name[sz - 1] = 0x00;

	backing_dev = filp_open_block(file_name, O_RDWR|O_LARGEFILE, 0);
	if (IS_ERR(backing_dev)) {
		err = PTR_ERR(backing_dev);
		backing_dev = NULL;
		goto out;
	}

	mapping = backing_dev->f_mapping;
	inode = mapping->host;

	/* Support only block device in this moment */
	if (!S_ISBLK(inode->i_mode)) {
		err = -ENOTBLK;
		goto out;
	}

	bdev = blkdev_get_by_dev(inode->i_rdev, BLK_OPEN_READ | BLK_OPEN_WRITE,
				 zram, NULL);
	if (IS_ERR(bdev)) {
		err = PTR_ERR(bdev);
		bdev = NULL;
		goto out;
	}

	nr_pages = i_size_read(inode) >> PAGE_SHIFT;
	/* Refuse to use zero sized device (also prevents self reference) */
	if (!nr_pages) {
		err = -EINVAL;
		goto out;
	}

	bitmap_sz = BITS_TO_LONGS(nr_pages) * sizeof(long);
	bitmap = kvzalloc(bitmap_sz, GFP_KERNEL);
	if (!bitmap) {
		err = -ENOMEM;
		goto out;
	}

	reset_bdev(zram);

	zram->bdev = bdev;
	zram->backing_dev = backing_dev;
	zram->bitmap = bitmap;
	zram->nr_pages = nr_pages;
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	err = init_lru_writeback(zram);
	if (err)
		goto init_lru_writeback_fail;
#endif
	up_write(&zram->init_lock);

	pr_info("setup backing device %s\n", file_name);
	kfree(file_name);

	return len;
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
init_lru_writeback_fail:
	zram->old_block_size = 0;
	zram->bdev = NULL;
	zram->backing_dev = NULL;
	zram->bitmap = NULL;
	zram->nr_pages = 0;
#endif
out:
	kvfree(bitmap);

	if (bdev)
		blkdev_put(bdev, zram);

	if (backing_dev)
		filp_close(backing_dev, NULL);

	up_write(&zram->init_lock);

	kfree(file_name);

	return err;
}

#ifdef CONFIG_ZRAM_LRU_WRITEBACK
static unsigned long chunk_to_blk_idx(unsigned long idx)
{
	return idx * NR_ZWBS;
}
static unsigned long blk_to_chunk_idx(unsigned long idx)
{
	return idx / NR_ZWBS;
}

static unsigned long alloc_chunk_bdev(struct zram *zram, bool ppr)
{
	unsigned long chunk_idx;
	unsigned long max_idx;
	unsigned long blk_idx;
	unsigned long flags;
	int i;

	if (ppr) {
		chunk_idx = blk_to_chunk_idx(zram->nr_lru_pages) + 1;
		max_idx = blk_to_chunk_idx(zram->nr_pages);
	} else {
		chunk_idx = 1;
		max_idx = blk_to_chunk_idx(zram->nr_lru_pages);
	}
retry:
	/* skip 0 bit to confuse zram.handle = 0 */
	chunk_idx = find_next_zero_bit(zram->chunk_bitmap, max_idx, chunk_idx);
	if (chunk_idx == max_idx)
		return 0;

	spin_lock_irqsave(&zram->bitmap_lock, flags);
	if (test_and_set_bit(chunk_idx, zram->chunk_bitmap)) {
		spin_unlock_irqrestore(&zram->bitmap_lock, flags);
		goto retry;
	}
	blk_idx = chunk_to_blk_idx(chunk_idx);
	for (i = 0; i < NR_ZWBS; i++)
		BUG_ON(test_and_set_bit(blk_idx + i, zram->bitmap));
	spin_unlock_irqrestore(&zram->bitmap_lock, flags);
	atomic64_add(NR_ZWBS, &zram->stats.bd_count);
	if (ppr)
		atomic64_add(NR_ZWBS, &zram->stats.bd_ppr_count);
	return blk_idx;
}

static unsigned long alloc_block_bdev(struct zram *zram)
{
	unsigned long blk_idx = 1;
	unsigned long flags;
retry:
	/* skip 0 bit to confuse zram.handle = 0 */
	blk_idx = find_next_zero_bit(zram->bitmap, zram->nr_lru_pages, blk_idx);
	if (blk_idx == zram->nr_lru_pages)
		return 0;

	spin_lock_irqsave(&zram->bitmap_lock, flags);
	if (test_and_set_bit(blk_idx, zram->bitmap)) {
		spin_unlock_irqrestore(&zram->bitmap_lock, flags);
		goto retry;
	}
	set_bit(blk_to_chunk_idx(blk_idx), zram->chunk_bitmap);
	spin_unlock_irqrestore(&zram->bitmap_lock, flags);
	atomic64_inc(&zram->stats.bd_count);
	return blk_idx;
}

static void free_chunk_bdev(struct zram *zram, unsigned long chunk_idx)
{
	unsigned long blk_idx;
	unsigned long flags;
	int i;

	blk_idx = chunk_to_blk_idx(chunk_idx);
	spin_lock_irqsave(&zram->bitmap_lock, flags);
	for (i = 0; i < NR_ZWBS; i++) {
		if (test_bit(blk_idx + i, zram->bitmap)) {
			spin_unlock_irqrestore(&zram->bitmap_lock, flags);
			return;
		}
	}
	clear_bit(chunk_idx, zram->chunk_bitmap);
	spin_unlock_irqrestore(&zram->bitmap_lock, flags);
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx, bool ppr)
{
	int was_set;
	unsigned long flags;

	spin_lock_irqsave(&zram->wb_table_lock, flags);
	if (!zram->wb_table || zram->wb_table[blk_idx] == 0)
		goto out;
	zram->wb_table[blk_idx]--;
	atomic64_dec(&zram->stats.bd_objcnt);
	if (ppr)
		atomic64_dec(&zram->stats.bd_ppr_objcnt);
	if (zram->wb_table[blk_idx] > 0) {
		spin_unlock_irqrestore(&zram->wb_table_lock, flags);
		return;
	}
out:
	spin_unlock_irqrestore(&zram->wb_table_lock, flags);
	was_set = test_and_clear_bit(blk_idx, zram->bitmap);
	WARN_ON_ONCE(!was_set);
	atomic64_dec(&zram->stats.bd_count);
	if (ppr)
		atomic64_dec(&zram->stats.bd_ppr_count);
	free_chunk_bdev(zram, blk_to_chunk_idx(blk_idx));
}

static void zram_inc_wb_table(struct zram *zram, unsigned long blk_idx)
{
	unsigned long flags;

	spin_lock_irqsave(&zram->wb_table_lock, flags);
	if (zram->wb_table)
		zram->wb_table[blk_idx]++;
	spin_unlock_irqrestore(&zram->wb_table_lock, flags);
}

static void zram_dec_wb_table(struct zram *zram, unsigned long blk_idx, bool ppr)
{
	unsigned long flags;

	spin_lock_irqsave(&zram->wb_table_lock, flags);
	if (!zram->wb_table) {
		spin_unlock_irqrestore(&zram->wb_table_lock, flags);
		return;
	}
	zram->wb_table[blk_idx]--;
	if (zram->wb_table[blk_idx] > 0) {
		spin_unlock_irqrestore(&zram->wb_table_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&zram->wb_table_lock, flags);
	clear_bit(blk_idx, zram->bitmap);
	atomic64_dec(&zram->stats.bd_count);
	if (ppr)
		atomic64_dec(&zram->stats.bd_ppr_count);
	free_chunk_bdev(zram, blk_to_chunk_idx(blk_idx));
}
#else
static unsigned long alloc_block_bdev(struct zram *zram)
{
	unsigned long blk_idx = 1;
retry:
	/* skip 0 bit to confuse zram.handle = 0 */
	blk_idx = find_next_zero_bit(zram->bitmap, zram->nr_pages, blk_idx);
	if (blk_idx == zram->nr_pages)
		return 0;

	if (test_and_set_bit(blk_idx, zram->bitmap))
		goto retry;

	atomic64_inc(&zram->stats.bd_count);
	return blk_idx;
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx)
{
	int was_set;

	was_set = test_and_clear_bit(blk_idx, zram->bitmap);
	WARN_ON_ONCE(!was_set);
	atomic64_dec(&zram->stats.bd_count);
}
#endif

static void read_from_bdev_async(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	struct bio *bio;

	bio = bio_alloc(zram->bdev, 1, parent->bi_opf, GFP_NOIO);
	bio->bi_iter.bi_sector = entry * (PAGE_SIZE >> 9);
	__bio_add_page(bio, page, PAGE_SIZE, 0);
	bio_chain(bio, parent);
	submit_bio(bio);
}

#define PAGE_WB_SIG "page_index="

#define PAGE_WRITEBACK 0
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
static int zram_balance_ratio = 25;	/* nand writeback ratio */
module_param(zram_balance_ratio, int, 0644);

static bool is_bdev_avail(struct zram *zram)
{
	struct loop_device *lo;
	struct inode *inode;
	struct dentry *root;
	struct kstatfs statbuf;
	u64 min_free_blocks;
	int ret;

	if (!zram->bdev->bd_disk)
		return false;

	lo = zram->bdev->bd_disk->private_data;
	if (!lo || !lo->lo_backing_file)
		return false;

	inode = lo->lo_backing_file->f_mapping->host;
	root = inode->i_sb->s_root;
	if (!root->d_sb->s_op->statfs)
		return false;

	ret = root->d_sb->s_op->statfs(root, &statbuf);
	if (ret)
		return false;
	/*
	 * To guarantee "reserved block(133MB on Q-os)" for system,
	 * SQZR is triggered only when devices have enough storage free space
	 * more than SZ_1G or reserved block * 2.
	 */
	min_free_blocks = max_t(u64, SZ_1G / statbuf.f_bsize,
			(statbuf.f_bfree - statbuf.f_bavail) * 2);
	if (statbuf.f_bavail < min_free_blocks)
		return false;

	return true;
}

static inline bool zram_throttle_writeback_size(struct zram *zram)
{
	long objcnt = atomic64_read(&zram->stats.bd_objcnt);

	if ((unsigned long)objcnt >= zram->nr_pages * 4)
		return true;
	else
		return false;
}

static bool zram_wb_available(struct zram *zram)
{
	if (!is_bdev_avail(zram))
		return false;

	if (!zram->wb_table)
		return false;
	spin_lock(&zram->wb_limit_lock);
	if (zram->wb_limit_enable && !zram->bd_wb_limit) {
		spin_unlock(&zram->wb_limit_lock);
		return false;
	}
	spin_unlock(&zram->wb_limit_lock);

	if (zram_throttle_writeback_size(zram))
		return false;
	return true;
}

static u32 entry_to_index(struct zram *zram, struct zram_table_entry *entry)
{
	if ((unsigned long)entry < (unsigned long)zram->table)
		return zram->disksize >> PAGE_SHIFT;
	return (u32)(((unsigned long)entry - (unsigned long)zram->table) /
			sizeof(struct zram_table_entry));
}

#define SKIP 1
#define ABORT 2
static int zram_try_mark_page(struct zram *zram, u32 index)
{
	/* invalid index */
	if (index >= (zram->disksize >> PAGE_SHIFT))
		return ABORT;

	if (!zram_slot_trylock(zram, index))
		return SKIP;

	if (!zram_allocated(zram, index) ||
			zram_test_flag(zram, index, ZRAM_UNDER_PPR)) {
		zram_slot_unlock(zram, index);
		return ABORT;
	} else if (zram_test_flag(zram, index, ZRAM_UNDER_WB)) {
		zram_slot_unlock(zram, index);
		return SKIP;
	}
	zram_set_flag(zram, index, ZRAM_IDLE);
	zram_slot_unlock(zram, index);
	return 0;
}

static void free_writeback_buffer(struct zram_writeback_buffer *buf)
{
	struct zwbs **zwbs;
	int i;

	if (!buf)
		return;

	zwbs = buf->zwbs;
	for (i = 0; i < NR_ZWBS; i++) {
		if (!zwbs[i])
			break;
		if (zwbs[i]->page)
			__free_page(zwbs[i]->page);
		kfree(zwbs[i]);
	}
	kfree(buf);
}

static struct zram_writeback_buffer *alloc_writeback_buffer(void)
{
	struct zram_writeback_buffer *buf;
	struct zwbs **zwbs;
	int i;

	buf = kzalloc(sizeof(struct zram_writeback_buffer), GFP_KERNEL);
	if (!buf)
		return NULL;

	zwbs = buf->zwbs;
	for (i = 0; i < NR_ZWBS; i++) {
		zwbs[i] = kzalloc(sizeof(struct zwbs), GFP_KERNEL);
		if (!zwbs[i])
			goto out;
		zwbs[i]->page = alloc_page(GFP_KERNEL);
		if (!zwbs[i]->page)
			goto out;
	}
	return buf;

out:
	free_writeback_buffer(buf);
	return NULL;
}

bool zram_is_app_launch(void)
{
	return is_app_launch;
}

#define ZRAM_WBD_INTERVAL ((10)*(HZ))
static bool zram_should_writeback(struct zram *zram,
				unsigned long pages, bool trigger)
{
	unsigned long stored = atomic64_read(&zram->stats.lru_pages);
	unsigned long writtenback = atomic64_read(&zram->stats.bd_objcnt) -
				    atomic64_read(&zram->stats.bd_ppr_objcnt) -
				    atomic64_read(&zram->stats.bd_expire);
	unsigned long min_stored_byte;
	int writtenback_ratio = stored ? (writtenback * 100) / stored : 0;
	int min_writtenback_ratio = zram_balance_ratio;
	int margin = max_t(int, 1, zram_balance_ratio / 10);
	int max_pages = CONFIG_ZRAM_LRU_WRITEBACK_LIMIT;
	static unsigned long time_stamp;
	bool ret = true;

	/* avoid app launch time */
	if (is_app_launch)
		return false;

	/* stop thread when writtenback enough */
	if (pages > max_pages)
		return false;

	/* do not trigger again before time interval */
	if (trigger && time_is_after_jiffies(time_stamp))
		return false;

	if (trigger)
		min_writtenback_ratio -= margin;
	else
		min_writtenback_ratio += margin;
	if (min_writtenback_ratio < writtenback_ratio)
		ret = false;

	if (zram->disksize / 4 > SZ_1G)
		min_stored_byte = SZ_1G;
	else
		min_stored_byte = zram->disksize / 4;

	if ((stored << PAGE_SHIFT) < min_stored_byte)
		ret = false;

	if (trigger && ret == true)
		time_stamp = jiffies + ZRAM_WBD_INTERVAL;

	return ret;
}

static void try_wakeup_zram_wbd(struct zram *zram)
{
	unsigned long bd_count;

	if (zram->backing_dev && !zram->wbd_running &&
			zram_wb_available(zram) &&
			zram_should_writeback(zram, 0, true)) {
		bd_count = atomic64_read(&zram->stats.bd_count);
		/* wakeup zram_wbd with enough free blocks */
		if (zram->nr_pages - bd_count < NR_ZWBS)
			return;

		zram->wbd_running = true;
		wake_up(&zram->wbd_wait);
	}
}

static int zram_app_launch_notifier(struct notifier_block *nb,
				unsigned long action, void *data)
{
	is_app_launch = action ? true : false;

	if (!is_app_launch && g_zram)
		try_wakeup_zram_wbd(g_zram);

	return 0;
}

static struct notifier_block zram_app_launch_nb = {
	.notifier_call = zram_app_launch_notifier,
};

static void mark_end_of_page(struct zwbs *zwbs)
{
	struct zram_wb_header *zhdr;
	struct page *page = zwbs->page;
	int offset = zwbs->off;
	void *mem;

	if (offset + sizeof(struct zram_wb_header) < PAGE_SIZE) {
		mem = kmap_atomic(page);
		zhdr = (struct zram_wb_header *)(mem + offset);
		zhdr->index = UINT_MAX;
		zhdr->size = 0;
		kunmap_atomic(mem);
	}
}

struct hex_dump_pages {
	struct page **pages;
	int nr_pages;
	unsigned int idx;
};

static void print_hex_dump_pages(struct page **src_page, int nr_pages,
				int cur_idx)
{
	void *src;

	if (cur_idx < 0 || cur_idx > NR_ZWBS - 1)
		return;

	if (nr_pages == NR_ZWBS && cur_idx != 0) {
		pr_err("Previous page\n");
		src = kmap_atomic(src_page[cur_idx - 1]);
		print_hex_dump_fmt(src, PAGE_SIZE);
		kunmap_atomic(src);
	}

	pr_err("This page\n");
	src = kmap_atomic(src_page[cur_idx]);
	print_hex_dump_fmt(src, PAGE_SIZE);
	kunmap_atomic(src);

	if (nr_pages == NR_ZWBS && cur_idx != NR_ZWBS - 1) {
		pr_err("Next page\n");
		src = kmap_atomic(src_page[cur_idx + 1]);
		print_hex_dump_fmt(src, PAGE_SIZE);
		kunmap_atomic(src);
	}
}

static void check_marker(void *addr, int size, struct hex_dump_pages *hdp)
{
	if (!is_lzorle)
		return;

	if (size == PAGE_SIZE)
		return;

	if (!memcmp(addr + size - 3, lzo_marker, 3))
		return;

	pr_err("%ps marker error, addr=0x%px len=%u\n", _RET_IP_, addr, size);
	if (hdp)
		print_hex_dump_pages(hdp->pages, hdp->nr_pages, hdp->idx);
	else
		print_hex_dump_fmt(addr, size);
	BUG();
}

static void handle_decomp_fail(char *comp, int err, u32 index, void *src,
			       unsigned int size, struct hex_dump_pages *hdp)
{
	bool is_marker_err = false;

	pr_err("%ps %s Decompression failed! err=%d %s=%u src=0x%px len=%u\n",
			_RET_IP_, comp, err, hdp ? "offset" : "index", index,
			src, size);
	if (is_lzorle && size != PAGE_SIZE) {
		if (memcmp(src + size - 3, lzo_marker, 3)) {
			pr_err("%s marker error\n", __func__);
			is_marker_err = true;
		}
	}

	if (hdp)
		print_hex_dump_pages(hdp->pages, hdp->nr_pages, hdp->idx);
	else
		print_hex_dump_fmt(src, size);

	if (is_marker_err)
		BUG();
	else
		panic("zram decomp failed");
}

static int zram_writeback_fill_page(struct zram *zram, u32 index,
				struct zwbs **zwbs, int idx, bool ppr)
{
	struct zram_wb_header *zhdr;
	struct page *page = zwbs[idx]->page;
	int offset = zwbs[idx]->off;
	unsigned long handle;
	void *src, *dst;
	int size, sizes[2];
	int header_sz = 0;

	zram_slot_lock(zram, index);
	if (!zram_allocated(zram, index) ||
			!zram_test_flag(zram, index, ZRAM_IDLE) ||
			zram_test_flag(zram, index, ZRAM_WB) ||
			zram_test_flag(zram, index, ZRAM_SAME) ||
			zram_test_flag(zram, index, ZRAM_UNDER_WB)) {
		zram_slot_unlock(zram, index);
		return 0;
	}
	size = zram_get_obj_size(zram, index);
	if (ppr || size != PAGE_SIZE)
		header_sz = sizeof(struct zram_wb_header);

	if (((!ppr || idx == NR_ZWBS - 1) &&
			offset + header_sz + size > PAGE_SIZE) ||
			offset + header_sz > PAGE_SIZE) {
		zram_slot_unlock(zram, index);
		return -ENOSPC;
	}
	/*
	 * Clearing ZRAM_UNDER_WB is duty of caller.
	 * IOW, zram_free_page never clear it.
	 */
	zram_set_flag(zram, index, ZRAM_UNDER_WB);
	/* Need for hugepage writeback racing */
	zram_set_flag(zram, index, ZRAM_IDLE);

	handle = zram_get_element(zram, index);
	if (!handle) {
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		zram_clear_flag(zram, index, ZRAM_IDLE);
		zram_slot_unlock(zram, index);
		return -ENOENT;
	}
	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	dst = kmap_atomic(page);
	if (header_sz) {
		zhdr = (struct zram_wb_header *)(dst + offset);
		zhdr->index = index;
		zhdr->size = size;
		dst = (u8 *)(zhdr + 1);
	}
	if (offset + header_sz + size > PAGE_SIZE) {
		sizes[0] = PAGE_SIZE - (offset + header_sz);
		sizes[1] = size - sizes[0];
		memcpy(dst, src, sizes[0]);
		kunmap_atomic(dst);
		dst = kmap_atomic(zwbs[idx + 1]->page);
		memcpy(dst, src + sizes[0], sizes[1]);
		zwbs[idx + 1]->off = sizes[1];
	} else {
		memcpy(dst, src, size);
	}
	kunmap_atomic(dst);
	check_marker(src, size, NULL);
	zs_unmap_object(zram->mem_pool, handle);
	zram_slot_unlock(zram, index);

	return size;
}

static void zram_writeback_clear_flag(struct zram *zram, u32 index)
{
	unsigned long flags;

	zram_slot_lock(zram, index);
	if (zram_allocated(zram, index)) {
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		zram_clear_flag(zram, index, ZRAM_IDLE);
		zram_clear_flag(zram, index, ZRAM_UNDER_PPR);

		/* putback halted entry to zram lru list */
		spin_lock_irqsave(&zram->list_lock, flags);
		if (!list_empty(&zram->table[index].lru_list))
			list_move_tail(&zram->table[index].lru_list, &zram->list);
		else
			list_add_tail(&zram->table[index].lru_list, &zram->list);
		spin_unlock_irqrestore(&zram->list_lock, flags);
		zram_set_flag(zram, index, ZRAM_LRU);
		atomic64_inc(&zram->stats.lru_pages);
	}
	zram_slot_unlock(zram, index);
}

static void zram_writeback_clear_flags(struct zram *zram, struct zwbs **zwbs)
{
	int i, j;

	for (i = 0; i < NR_ZWBS; i++)
		for (j = 0; j < zwbs[i]->cnt; j++)
			zram_writeback_clear_flag(zram, zwbs[i]->entry[j].index);
}

static void zram_update_max_stats(struct zram *zram)
{
	unsigned long bd_count, bd_size, bd_ppr_count, bd_ppr_size;

	bd_count = atomic64_read(&zram->stats.bd_count);
	if (bd_count <= atomic64_read(&zram->stats.bd_max_count))
		return;

	bd_size = atomic64_read(&zram->stats.bd_size);
	bd_ppr_count = atomic64_read(&zram->stats.bd_ppr_count);
	bd_ppr_size = atomic64_read(&zram->stats.bd_ppr_size);
	atomic64_set(&zram->stats.bd_max_count, bd_count);
	atomic64_set(&zram->stats.bd_max_size, bd_size);
	atomic64_set(&zram->stats.bd_ppr_max_count, bd_ppr_count);
	atomic64_set(&zram->stats.bd_ppr_max_size, bd_ppr_size);
}

static void zram_reset_stats(struct zram *zram)
{
	atomic64_set(&zram->stats.bd_max_count, 0);
	atomic64_set(&zram->stats.bd_max_size, 0);
	atomic64_set(&zram->stats.bd_ppr_max_count, 0);
	atomic64_set(&zram->stats.bd_ppr_max_size, 0);
}

static void zram_writeback_done(struct zram *zram,
		struct zwbs *zwbs, unsigned long blk_idx, bool ppr)
{
	unsigned long index;
	unsigned int offset;
	unsigned int size;
	unsigned int count = zwbs->cnt;
	struct zram_wb_entry *entry = zwbs->entry;
	int i;
	unsigned long flags;

	if (!count) {
		free_block_bdev(zram, blk_idx, ppr);
		return;
	}
	spin_lock_irqsave(&zram->wb_table_lock, flags);
	if (!zram->wb_table) {
		spin_unlock_irqrestore(&zram->wb_table_lock, flags);
		return;
	}
	zram->wb_table[blk_idx] = count;
	spin_unlock_irqrestore(&zram->wb_table_lock, flags);
	atomic64_add(count, &zram->stats.bd_objwrites);
	atomic64_add(count, &zram->stats.bd_objcnt);
	if (ppr)
		atomic64_add(count, &zram->stats.bd_ppr_objcnt);

	for (i = 0; i < count; i++) {
		index = entry[i].index;
		offset = entry[i].offset;
		size = entry[i].size;
		/*
		 * We released zram_slot_lock so need to check if the slot was
		 * changed. If there is freeing for the slot, we can catch it
		 * easily by zram_allocated.
		 * A subtle case is the slot is freed/reallocated/marked as
		 * ZRAM_IDLE again. To close the race, idle_store doesn't
		 * mark ZRAM_IDLE once it found the slot was ZRAM_UNDER_WB.
		 * Thus, we could close the race by checking ZRAM_IDLE bit.
		 */
		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index) ||
				!zram_test_flag(zram, index, ZRAM_IDLE)) {
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			zram_clear_flag(zram, index, ZRAM_UNDER_PPR);
			free_block_bdev(zram, blk_idx, ppr);
			zram_slot_unlock(zram, index);
			continue;
		}

		zram_free_page(zram, index);
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		zram_set_flag(zram, index, ZRAM_WB);
		atomic64_add(size, &zram->stats.bd_size);
		if (ppr) {
			zram_set_flag(zram, index, ZRAM_PPR);
			atomic64_add(size, &zram->stats.bd_ppr_size);
		}
		/* record element as "blk_idx|offset|size" */
		if (size == PAGE_SIZE)
			size = 0;
		zram_set_element(zram, index,
				(blk_idx << (PAGE_SHIFT * 2)) | (offset << PAGE_SHIFT) | size);
		zram_slot_unlock(zram, index);
		atomic64_inc(&zram->stats.pages_stored);
	}
}

static void zram_writeback_done_work(struct work_struct *work)
{
	struct zram_wb_work *zw = container_of(work, struct zram_wb_work, work);
	struct zram *zram = zw->zram;
	struct zram_writeback_buffer *buf = zw->buf;
	struct bio *bio = zw->bio;
	unsigned long blk_idx = zw->handle;
	int nr_pages = zw->nr_pages;
	int i;
	bool ppr = zw->ppr;

	if (bio->bi_status)
		zram_writeback_clear_flags(zram, buf->zwbs);

	for (i = 0; i < nr_pages; i++)
		zram_writeback_done(zram, buf->zwbs[i], blk_idx + i, ppr);

	zram_update_max_stats(zram);
	atomic64_add(nr_pages, &zram->stats.bd_writes);
	if (ppr)
		atomic64_add(nr_pages, &zram->stats.bd_ppr_writes);
	spin_lock(&zram->wb_limit_lock);
	if (zram->wb_limit_enable) {
		if (zram->bd_wb_limit > nr_pages)
			zram->bd_wb_limit -= nr_pages;
		else
			zram->bd_wb_limit = 0;
	}
	spin_unlock(&zram->wb_limit_lock);

	bio_put(bio);
	free_writeback_buffer(buf);
	kfree(zw);
}

static void zram_writeback_page_end_io(struct bio *bio)
{
	struct page *page = bio->bi_io_vec[0].bv_page;
	struct zram_wb_work *zw = (struct zram_wb_work *)page_private(page);
	int errno = blk_status_to_errno(bio->bi_status);

	if (errno)
		pr_info("%s errno %d\n", __func__, errno);

	INIT_WORK(&zw->work, zram_writeback_done_work);
	schedule_work(&zw->work);
}

static int zram_writeback_page(struct zram *zram, struct zram_writeback_buffer *buf, bool ppr)
{
	struct zram_wb_work *zw;
	struct zwbs **zwbs = buf->zwbs;
	struct bio *bio;
	unsigned long blk_idx;
	int i;

	blk_idx = alloc_chunk_bdev(zram, ppr);
	if (!blk_idx)
		goto out;

	/* fallocate 2MB block if not allocated yet */
	fallocate_block(zram, blk_idx);

	zw = kzalloc(sizeof(struct zram_wb_work), GFP_KERNEL);
	if (!zw)
		goto out;

	bio = bio_alloc(GFP_KERNEL, NR_ZWBS);
	if (!bio) {
		kfree(zw);
		goto out;
	}
	bio->bi_opf = REQ_OP_WRITE;
	bio->bi_end_io = zram_writeback_page_end_io;
	bio->bi_iter.bi_sector = blk_idx * (PAGE_SIZE >> 9);
	bio_set_dev(bio, zram->bdev);
	for (i = 0; i < NR_ZWBS; i++)
		bio_add_page(bio, zwbs[i]->page, PAGE_SIZE, 0);

	zw->nr_pages = NR_ZWBS;
	zw->zram = zram;
	zw->handle = blk_idx;
	zw->buf = buf;
	zw->bio = bio;
	zw->ppr = ppr;
	set_page_private(zwbs[0]->page, (unsigned long)zw);

	submit_bio(bio);

	return 0;
out:
	if (blk_idx)
		for (i = 0; i < NR_ZWBS; i++)
			free_block_bdev(zram, blk_idx + i, ppr);
	zram_writeback_clear_flags(zram, zwbs);
	free_writeback_buffer(buf);

	return -ENOMEM;
}

static int zram_writeback_index(struct zram *zram, u32 index,
		struct zram_writeback_buffer **buf, bool ppr)
{
	struct zram_writeback_buffer *tmpbuf = *buf;
	struct zwbs **zwbs;
	int size, i, ret = 0;

retry:
	/* allocate new buffer for writeback */
	if (tmpbuf == NULL) {
		tmpbuf = alloc_writeback_buffer();
		if (tmpbuf == NULL)
			return -ENOMEM;
	}
	zwbs = tmpbuf->zwbs;
	i = tmpbuf->idx;

	size = zram_writeback_fill_page(zram, index, zwbs, i, ppr);
	if (size > 0) {
		struct zram_wb_entry *entry = zwbs[i]->entry;

		entry[zwbs[i]->cnt].index = index;
		entry[zwbs[i]->cnt].offset = zwbs[i]->off;
		entry[zwbs[i]->cnt].size = size;
		zwbs[i]->off += (size + sizeof(struct zram_wb_header));
		zwbs[i]->cnt++;
	}
	/* writeback if page is full/entry is full */
	if (size == -ENOSPC || zwbs[i]->cnt == ZRAM_WB_THRESHOLD) {
		mark_end_of_page(zwbs[i]);
		if (++tmpbuf->idx == NR_ZWBS) {
			ret = zram_writeback_page(zram, tmpbuf, ppr);
			tmpbuf = NULL;
		}
		if (ret == 0)
			goto retry;
	}
	*buf = tmpbuf;
	return ret;
}

static void zram_comp_writeback(struct zram *zram)
{
	struct zram_writeback_buffer *buf = NULL;
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	unsigned long index;

	for (index = 0; index < nr_pages; index++) {
		if (!zram_wb_available(zram))
			break;
		if (zram_writeback_index(zram, index, &buf, false))
			break;
	}
	if (buf) {
		mark_end_of_page(buf->zwbs[buf->idx]);
		zram_writeback_page(zram, buf, false);
	}
	pr_info("%s done", __func__);
}

static int zram_wbd(void *p)
{
	struct zram *zram = (struct zram *)p;
	struct zram_table_entry *zram_entry, *n;
	struct zram_writeback_buffer *buf = NULL;
	u32 index;
	int ret;

	set_freezable();

	while (!kthread_should_stop()) {
		unsigned long nr_pages = 0;

		wait_event_freezable(zram->wbd_wait,
				zram->wbd_running || kthread_should_stop());
		list_for_each_entry_safe(zram_entry, n, &zram->list, lru_list) {
			if (try_to_freeze() || kthread_should_stop())
				break;
			if (!zram_wb_available(zram))
				break;
			index = entry_to_index(zram, zram_entry);
			ret = zram_try_mark_page(zram, index);
			if (!ret) {
				if (zram_writeback_index(zram, index, &buf, false))
					break;
			} else if (ret == ABORT) {
				n = list_first_entry(&zram->list,
						struct zram_table_entry, lru_list);
			}
			if (!zram_should_writeback(zram, ++nr_pages, false))
				break;
		}
		zram->wbd_running = false;
		pr_info("%s done", __func__);
	}
	free_writeback_buffer(buf);

	return 0;
}

void zram_add_to_writeback_list(struct list_head *list, unsigned long index)
{
	struct zram *zram = g_zram;
	unsigned long flags;

	if (!zram_wb_available(zram))
		return;
	if (index >= (zram->disksize >> PAGE_SHIFT))
		return;
	if (!zram_slot_trylock(zram, index))
		return;

	if (zram_allocated(zram, index) &&
			!zram_test_flag(zram, index, ZRAM_IDLE) &&
			!zram_test_flag(zram, index, ZRAM_WB) &&
			!zram_test_flag(zram, index, ZRAM_SAME) &&
			!zram_test_flag(zram, index, ZRAM_UNDER_WB) &&
			!zram_test_flag(zram, index, ZRAM_UNDER_PPR)) {
		zram_set_flag(zram, index, ZRAM_IDLE);
		zram_set_flag(zram, index, ZRAM_UNDER_PPR);
		spin_lock_irqsave(&zram->list_lock, flags);
		if (!list_empty(&zram->table[index].lru_list)) {
			list_move(&zram->table[index].lru_list, list);
			if (zram_test_flag(zram, index, ZRAM_LRU)) {
				zram_clear_flag(zram, index, ZRAM_LRU);
				atomic64_dec(&zram->stats.lru_pages);
			}
		}
		spin_unlock_irqrestore(&zram->list_lock, flags);
	}
	zram_slot_unlock(zram, index);
}

int zram_writeback_list(struct list_head *list)
{
	struct zram *zram = g_zram;
	struct zram_table_entry *entry;
	u32 index;
	unsigned long flags;

	while (!list_empty(list)) {
		entry = list_first_entry(list, typeof(*entry), lru_list);
		index = entry_to_index(zram, entry);
		if (index >= (zram->disksize >> PAGE_SHIFT))
			return -EINVAL;
		if (is_app_launch || !zram_wb_available(zram) ||
		    zram_writeback_index(zram, index, &zram->buf, true))
			return -EINVAL;
		zram_slot_lock(zram, index);
		/* skip touched entry */
		if (!zram_test_flag(zram, index, ZRAM_UNDER_PPR)) {
			zram_slot_unlock(zram, index);
			continue;
		}
		zram_clear_flag(zram, index, ZRAM_UNDER_PPR);
		spin_lock_irqsave(&zram->list_lock, flags);
		if (!list_empty(&zram->table[index].lru_list))
			list_del_init(&zram->table[index].lru_list);
		spin_unlock_irqrestore(&zram->list_lock, flags);
		zram_slot_unlock(zram, index);
	}
	return 0;
}

void flush_writeback_buffer(struct list_head *list)
{
	struct zram *zram = g_zram;
	struct zram_table_entry *entry;
	u32 index;

	if (list_empty(list) && zram->buf) {
		mark_end_of_page(zram->buf->zwbs[zram->buf->idx]);
		zram_writeback_page(zram, zram->buf, true);
		zram->buf = NULL;
		return;
	}

	/* putback all remaining zram entries */
	while (!list_empty(list)) {
		entry = list_first_entry(list, typeof(*entry), lru_list);
		index = entry_to_index(zram, entry);
		if (index >= (zram->disksize >> PAGE_SHIFT))
			break;
		zram_writeback_clear_flag(zram, index);
	}

	if (zram->buf) {
		zram_writeback_clear_flags(zram, zram->buf->zwbs);
		free_writeback_buffer(zram->buf);
		zram->buf = NULL;
	}
}

int zram_get_entry_type(unsigned long index)
{
	struct zram *zram = g_zram;
	int ret = 0;

	if (index >= (zram->disksize >> PAGE_SHIFT))
		return ret;

	zram_slot_lock(zram, index);
	if (zram_allocated(zram, index)) {
		if (zram_test_flag(zram, index, ZRAM_WB))
			ret = zram_get_element(zram, index) & (PAGE_SIZE - 1) ?
					ZRAM_WB_TYPE : ZRAM_WB_HUGE_TYPE;
		else if (zram_test_flag(zram, index, ZRAM_SAME))
			ret = ZRAM_SAME_TYPE;
		else if (zram_test_flag(zram, index, ZRAM_HUGE))
			ret = ZRAM_HUGE_TYPE;
	}
	zram_slot_unlock(zram, index);

	return ret;
}

static int read_comp_from_bdev(struct zram *zram, struct bio_vec *bvec,
			unsigned long handle, struct bio *parent, bool ppr);

int zram_prefetch_entry(unsigned long index)
{
	struct zram *zram = g_zram;
	unsigned long handle;
	unsigned long chunk_idx;
	unsigned long blk_idx;

	if (index >= (zram->disksize >> PAGE_SHIFT))
		return -1;

	if (!zram_slot_trylock(zram, index))
		return -1;

	if (!zram_allocated(zram, index) ||
			!zram_test_flag(zram, index, ZRAM_WB) ||
			!zram_test_flag(zram, index, ZRAM_PPR) ||
			zram_test_flag(zram, index, ZRAM_READ_BDEV)) {
		zram_slot_unlock(zram, index);
		return -1;
	}
	handle = zram_get_element(zram, index);
	blk_idx = handle >> (PAGE_SHIFT * 2);
	chunk_idx = blk_to_chunk_idx(blk_idx);
	if (test_and_set_bit(chunk_idx, zram->read_req_bitmap)) {
		zram_slot_unlock(zram, index);
		return -1;
	}
	zram_inc_wb_table(zram, blk_idx);
	zram_slot_unlock(zram, index);
	if (read_comp_from_bdev(zram, NULL, handle, NULL, true) < 0)
		zram_dec_wb_table(zram, blk_idx, true);
	atomic64_inc(&zram->stats.bd_ppr_reads);

	return 0;
}
#endif
#define HUGE_WRITEBACK			(1<<0)
#define IDLE_WRITEBACK			(1<<1)
#define INCOMPRESSIBLE_WRITEBACK	(1<<2)


static ssize_t writeback_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	unsigned long index = 0;
	struct bio bio;
	struct bio_vec bio_vec;
	struct page *page;
	ssize_t ret = len;
	int mode, err;
	unsigned long blk_idx = 0;

	if (sysfs_streq(buf, "idle"))
		mode = IDLE_WRITEBACK;
	else if (sysfs_streq(buf, "huge"))
		mode = HUGE_WRITEBACK;
	else if (sysfs_streq(buf, "huge_idle"))
		mode = IDLE_WRITEBACK | HUGE_WRITEBACK;
	else if (sysfs_streq(buf, "incompressible"))
		mode = INCOMPRESSIBLE_WRITEBACK;
	else {
		if (strncmp(buf, PAGE_WB_SIG, sizeof(PAGE_WB_SIG) - 1))
			return -EINVAL;

		if (kstrtol(buf + sizeof(PAGE_WB_SIG) - 1, 10, &index) ||
				index >= nr_pages)
			return -EINVAL;

		nr_pages = 1;
		mode = PAGE_WRITEBACK;
	}

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		goto release_init_lock;
	}

	if (!zram->backing_dev) {
		ret = -ENODEV;
		goto release_init_lock;
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto release_init_lock;
	}

#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	if (mode == IDLE_WRITEBACK) {
		if (zram_wb_available(zram))
			zram_comp_writeback(zram);
		ret = len;
		__free_page(page);
		goto release_init_lock;
	}
#endif
	for (; nr_pages != 0; index++, nr_pages--) {
		spin_lock(&zram->wb_limit_lock);
		if (zram->wb_limit_enable && !zram->bd_wb_limit) {
			spin_unlock(&zram->wb_limit_lock);
			ret = -EIO;
			break;
		}
		spin_unlock(&zram->wb_limit_lock);

		if (!blk_idx) {
			blk_idx = alloc_block_bdev(zram);
			if (!blk_idx) {
				ret = -ENOSPC;
				break;
			}
		}

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		if (zram_test_flag(zram, index, ZRAM_WB) ||
				zram_test_flag(zram, index, ZRAM_SAME) ||
				zram_test_flag(zram, index, ZRAM_UNDER_WB))
			goto next;

		if (mode & IDLE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_IDLE))
			goto next;
		if (mode & HUGE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_HUGE))
			goto next;
		if (mode & INCOMPRESSIBLE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
			goto next;

		/*
		 * Clearing ZRAM_UNDER_WB is duty of caller.
		 * IOW, zram_free_page never clear it.
		 */
		zram_set_flag(zram, index, ZRAM_UNDER_WB);
		/*
		 * For hugepage writeback, we also need to set ZRAM_IDLE bit
		 * to prevent race window between writing the huge page and
		 * populating new allocated hugepage in the same slot.
		 * In that case, new slot will not have ZRAM_IDLE bit so
		 * we could prevent the race.  Please find the detail below
		 * comments.
		 */
		zram_set_flag(zram, index, ZRAM_IDLE);
		zram_slot_unlock(zram, index);
		if (zram_read_page(zram, page, index, NULL)) {
			zram_slot_lock(zram, index);
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			zram_slot_unlock(zram, index);
			continue;
		}

		bio_init(&bio, zram->bdev, &bio_vec, 1,
			 REQ_OP_WRITE | REQ_SYNC);
		bio.bi_iter.bi_sector = blk_idx * (PAGE_SIZE >> 9);
		__bio_add_page(&bio, page, PAGE_SIZE, 0);

		/*
		 * XXX: A single page IO would be inefficient for write
		 * but it would be not bad as starter.
		 */
		err = submit_bio_wait(&bio);
		if (err) {
			zram_slot_lock(zram, index);
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			zram_slot_unlock(zram, index);
			/*
			 * BIO errors are not fatal, we continue and simply
			 * attempt to writeback the remaining objects (pages).
			 * At the same time we need to signal user-space that
			 * some writes (at least one, but also could be all of
			 * them) were not successful and we do so by returning
			 * the most recent BIO error.
			 */
			ret = err;
			continue;
		}

		atomic64_inc(&zram->stats.bd_writes);
		/*
		 * We released zram_slot_lock so need to verify if the slot was
		 * changed under us. If slot was freed, we can catch it by
		 * zram_allocated below.
		 * A subtle case is the slot was freed/reallocated/marked as
		 * ZRAM_IDLE again so we just wrote stale data but has freed
		 * fresh data in the slot so user will see stale data in upcoming
		 * access. To close the race, idle_store doesn't allow to mark
		 * ZRAM_IDLE on the slot with ZRAM_UNDER_WB. Thus, newl populated
		 * page will not have ZRAM_IDLE bit any longer so we can catch it
		 * by checking ZRAM_IDLE bit.
		 */
		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index) ||
			  !zram_test_flag(zram, index, ZRAM_IDLE)) {
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			goto next;
		}

		zram_free_page(zram, index);
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		zram_set_flag(zram, index, ZRAM_WB);
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
		zram_set_element(zram, index, blk_idx << (PAGE_SHIFT * 2));
#else
		zram_set_element(zram, index, blk_idx);
#endif
		blk_idx = 0;
		atomic64_inc(&zram->stats.pages_stored);
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
		atomic64_inc(&zram->stats.bd_objcnt);
#endif
		spin_lock(&zram->wb_limit_lock);
		if (zram->wb_limit_enable && zram->bd_wb_limit > 0)
			zram->bd_wb_limit -=  1UL << (PAGE_SHIFT - 12);
		spin_unlock(&zram->wb_limit_lock);
next:
		zram_slot_unlock(zram, index);
	}

	if (blk_idx)
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
		free_block_bdev(zram, blk_idx, false);
#else
		free_block_bdev(zram, blk_idx);
#endif
	__free_page(page);
release_init_lock:
	up_read(&zram->init_lock);

	return ret;
}

struct zram_work {
	struct work_struct work;
	struct zram *zram;
	unsigned long entry;
	struct page *page;
	int error;
};

static void zram_sync_read(struct work_struct *work)
{
	struct zram_work *zw = container_of(work, struct zram_work, work);
	struct bio_vec bv;
	struct bio bio;

	bio_init(&bio, zw->zram->bdev, &bv, 1, REQ_OP_READ);
	bio.bi_iter.bi_sector = zw->entry * (PAGE_SIZE >> 9);
	__bio_add_page(&bio, zw->page, PAGE_SIZE, 0);
	zw->error = submit_bio_wait(&bio);
}

/*
 * Block layer want one ->make_request_fn to be active at a time
 * so if we use chained IO with parent IO in same context,
 * it's a deadlock. To avoid, it, it uses worker thread context.
 */
static int read_from_bdev_sync(struct zram *zram, struct page *page,
				unsigned long entry)
{
	struct zram_work work;

	work.page = page;
	work.zram = zram;
	work.entry = entry;

	INIT_WORK_ONSTACK(&work.work, zram_sync_read);
	queue_work(system_unbound_wq, &work.work);
	flush_work(&work.work);
	destroy_work_on_stack(&work.work);

	return work.error;
}

static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	atomic64_inc(&zram->stats.bd_reads);
	if (!parent) {
		if (WARN_ON_ONCE(!IS_ENABLED(ZRAM_PARTIAL_IO)))
			return -EIO;
		return read_from_bdev_sync(zram, page, entry);
	}
	read_from_bdev_async(zram, page, entry, parent);
	return 0;
}

#ifdef CONFIG_ZRAM_LRU_WRITEBACK
static void free_zw_pages(struct zram_wb_work *zw)
{
	int i;

	for (i = 0; i < zw->nr_pages; i++) {
		if (!zw->src_page[i])
			return;
		__free_page(zw->src_page[i]);
	}
}

static int alloc_zw_pages(struct zram_wb_work *zw)
{
	int i;

	for (i = 0; i < zw->nr_pages; i++) {
		zw->src_page[i] = alloc_page(GFP_NOIO|__GFP_HIGHMEM);
		if (!zw->src_page[i]) {
			pr_info("%s failed to alloc page", __func__);
			free_zw_pages(zw);
			return -ENOMEM;
		}
	}
	return 0;
}

static void copy_to_buf(void *dst, struct page **pages,
			unsigned int idx, unsigned int offset,
			unsigned int size)
{
	int sizes[2];
	u8 *src;

	sizes[0] = min_t(int, size, PAGE_SIZE - offset);
	sizes[1] = size - sizes[0];

	if (sizes[0]) {
		src = kmap_atomic(pages[idx]);
		memcpy(dst, src + offset, sizes[0]);
		kunmap_atomic(src);
	}
	if (sizes[1]) {
		src = kmap_atomic(pages[idx + 1]);
		memcpy(dst + sizes[0], src, sizes[1]);
		kunmap_atomic(src);
	}
}

static void zram_handle_remain(struct zram *zram, struct page **pages,
				unsigned int blk_idx, int nr_pages)
{
	struct zram_wb_header *zhdr;
	unsigned long alloced_pages;
	unsigned long handle;
	unsigned long flags;
	unsigned int idx = 0;
	unsigned int offset = 0;
	unsigned int size;
	int header_sz = sizeof(struct zram_wb_header);
	u32 index;
	u8 *mem, *dst;
	struct hex_dump_pages hdp;

	while (idx < nr_pages) {
		mem = kmap_atomic(pages[idx]);
		zhdr = (struct zram_wb_header *)(mem + offset);
		index = zhdr->index;
		size = zhdr->size;
		kunmap_atomic(mem);

		/* invalid index or size, this means last object or corrupted page */
		if (index >= (zram->disksize >> PAGE_SHIFT) || size > PAGE_SIZE) {
			index = -EINVAL;
			goto next;
		}

		if (!zram_slot_trylock(zram, index))
			goto next;

		if (!zram_allocated(zram, index) ||
			!zram_test_flag(zram, index, ZRAM_WB) ||
			zram_test_flag(zram, index, ZRAM_READ_BDEV)) {
			zram_slot_unlock(zram, index);
			goto next;
		}
		handle = zram_get_element(zram, index);
		if ((handle >> (PAGE_SHIFT * 2)) != blk_idx + idx ||
			((handle >> PAGE_SHIFT) & (PAGE_SIZE - 1)) != offset ||
			(size == PAGE_SIZE && (handle & (PAGE_SIZE - 1)) != 0) ||
			(size != PAGE_SIZE && (handle & (PAGE_SIZE - 1)) != size)) {
			zram_slot_unlock(zram, index);
			goto next;
		}
		atomic64_inc(&zram->stats.bd_objreads);

		handle = zs_malloc(zram->mem_pool, size,
				__GFP_KSWAPD_RECLAIM |
				__GFP_NOWARN |
				__GFP_HIGHMEM |
				__GFP_MOVABLE |
				__GFP_CMA);
		if (!handle) {
			zram_slot_unlock(zram, index);
			break;
		}
		alloced_pages = zs_get_total_pages(zram->mem_pool);
		update_used_max(zram, alloced_pages);

		dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);
		copy_to_buf(dst, pages, idx, offset + header_sz, size);
		hdp.pages = pages;
		hdp.nr_pages = nr_pages;
		hdp.idx = idx;
		check_marker(dst, size, &hdp);
		zs_unmap_object(zram->mem_pool, handle);

		atomic64_add(size, &zram->stats.compr_data_size);
		zram_free_page(zram, index);
		zram_set_element(zram, index, handle);
		zram_set_obj_size(zram, index, size);
		spin_lock_irqsave(&zram->list_lock, flags);
		list_add_tail(&zram->table[index].lru_list, &zram->list);
		spin_unlock_irqrestore(&zram->list_lock, flags);
		zram_set_flag(zram, index, ZRAM_LRU);
		atomic64_inc(&zram->stats.lru_pages);
		zram_slot_unlock(zram, index);
		atomic64_inc(&zram->stats.pages_stored);
next:
		offset += (size + header_sz);
		if (offset + header_sz > PAGE_SIZE || index == -EINVAL) {
			idx++;
			if (offset < PAGE_SIZE || index == -EINVAL)
				offset = 0;
			else
				offset %= PAGE_SIZE;

			/* check next offset again */
			if (offset + header_sz > PAGE_SIZE) {
				idx++;
				offset = 0;
			}
		}
	}
}

static void zram_handle_comp_page(struct work_struct *work)
{
	struct zram_wb_work *zw = container_of(work, struct zram_wb_work, work);
	struct zram_wb_header *zhdr;
	struct zram *zram = zw->zram;
	struct zcomp_strm *zstrm;
	struct page **src_page = zw->src_page;
	struct page *dst_page = zw->dst_page;
	struct bio *bio = zw->bio;
	unsigned int blk_idx = zw->handle >> (PAGE_SHIFT * 2);
	unsigned int offset = (zw->handle >> PAGE_SHIFT) & (PAGE_SIZE - 1);
	unsigned int size = zw->handle & (PAGE_SIZE - 1);
	unsigned int page_idx = 0;
	int header_sz = sizeof(struct zram_wb_header);
	int ret = 0;
	u32 index;
	u8 *src, *dst, *src_decomp;
	bool spanned;

	if (zw->ppr) {
		page_idx = blk_idx & ~ZWBS_ALIGN_MASK;
		blk_idx &= ZWBS_ALIGN_MASK;
	}

	src = kmap_atomic(src_page[page_idx]);
	zhdr = (struct zram_wb_header *)(src + offset);
	index = zhdr->index;
	if (size == 0)
		size = PAGE_SIZE;
	if (zhdr->size != size) {
		pr_err("%s %s zhdr error, size should be %u but was %u src=0x%px offset=%u\n",
			__func__, zram->comp_algs[ZRAM_PRIMARY_COMP], size, zhdr->size, src,
			offset);
		print_hex_dump_pages(src_page, zw->nr_pages, page_idx);
		BUG();
	}

	if (!dst_page) {
		kunmap_atomic(src);
		goto out;
	}

	dst = kmap_atomic(dst_page);
	zstrm = zcomp_stream_get(zram->comps[ZRAM_PRIMARY_COMP]);
	spanned = (offset + header_sz + size > PAGE_SIZE) ? true : false;
	if (spanned) {
		kunmap_atomic(src);
		if (size == PAGE_SIZE) {
			copy_to_buf(dst, src_page, page_idx, offset + header_sz, size);
			goto out_huge;
		}
		src = zstrm->tmpbuf;
		copy_to_buf(src, src_page, page_idx, offset + header_sz, size);
		src_decomp = src;
	} else {
		src_decomp = src + offset + header_sz;
	}
	ret = zcomp_decompress(zram->comps[ZRAM_PRIMARY_COMP], zstrm,
			       src_decomp, size, dst);
out_huge:
	zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
	if (ret) {
		struct hex_dump_pages hdp;

		hdp.pages = src_page;
		hdp.nr_pages = zw->nr_pages;
		hdp.idx = page_idx;
		handle_decomp_fail(zram->comp_algs[ZRAM_PRIMARY_COMP], ret, offset + header_sz,
				   src_decomp, size, &hdp);
	}
	kunmap_atomic(dst);
	if (!spanned)
		kunmap_atomic(src);

	zram_slot_lock(zram, index);
	zram_clear_flag(zram, index, ZRAM_READ_BDEV);
	zram_slot_unlock(zram, index);

	if (zw->bio_chain)
		bio_endio(zw->bio_chain);
	else
		page_endio(dst_page, false, 0);
out:
	bio_put(bio);

	zram_handle_remain(zram, src_page, blk_idx, zw->nr_pages);

	if (!dst_page)
		clear_bit(blk_to_chunk_idx(blk_idx), zram->read_req_bitmap);

	zram_dec_wb_table(zram, blk_idx + page_idx, zw->ppr);
	free_zw_pages(zw);
	kfree(zw);
}

static void zram_comp_page_end_io(struct bio *bio)
{
	struct page *page = bio->bi_io_vec[0].bv_page;
	struct zram_wb_work *zw = (struct zram_wb_work *)page_private(page);
	int errno = blk_status_to_errno(bio->bi_status);

	if (errno)
		pr_err("%s submit_bio errno %d\n", __func__, errno);
	INIT_WORK(&zw->work, zram_handle_comp_page);
	schedule_work(&zw->work);
}

static int read_comp_from_bdev(struct zram *zram, struct bio_vec *bvec,
			unsigned long handle, struct bio *parent, bool ppr)
{
	struct zram_wb_work *zw;
	struct bio *bio;
	unsigned long blk_idx;
	int i, nr_pages;

	if (ppr) {
		blk_idx = handle >> (PAGE_SHIFT * 2) & ZWBS_ALIGN_MASK;
		nr_pages = NR_ZWBS;
	} else {
		blk_idx = handle >> (PAGE_SHIFT * 2);
		nr_pages = 1;
	}

	atomic64_inc(&zram->stats.bd_reads);

	bio = bio_alloc(GFP_NOIO, nr_pages);
	if (!bio)
		return -ENOMEM;

	zw = kzalloc(sizeof(struct zram_wb_work), GFP_NOIO);
	if (!zw) {
		bio_put(bio);
		return -ENOMEM;
	}
	zw->nr_pages = nr_pages;
	if (alloc_zw_pages(zw)) {
		kfree(zw);
		bio_put(bio);
		return -ENOMEM;
	}
	zw->dst_page = bvec ? bvec->bv_page : NULL;
	zw->zram = zram;
	zw->bio = bio;
	zw->handle = handle;
	zw->ppr = ppr;
	set_page_private(zw->src_page[0], (unsigned long)zw);

	bio->bi_iter.bi_sector = blk_idx * (PAGE_SIZE >> 9);
	bio_set_dev(bio, zram->bdev);
	for (i = 0; i < nr_pages; i++) {
		if (!bio_add_page(bio, zw->src_page[i], PAGE_SIZE, 0)) {
			free_zw_pages(zw);
			kfree(zw);
			bio_put(bio);
			return -EIO;
		}
	}

	bio->bi_opf = REQ_OP_READ;
	bio->bi_end_io = zram_comp_page_end_io;

	if (parent) {
		zw->bio_chain = bio_alloc(GFP_NOIO, 1);
		if (!zw->bio_chain) {
			free_zw_pages(zw);
			kfree(zw);
			bio_put(bio);
			return -ENOMEM;
		}
		zw->bio_chain->bi_opf = parent->bi_opf;
		bio_chain(zw->bio_chain, parent);
	}

	submit_bio(bio);
	return 1;
}
#endif
#else
static inline void reset_bdev(struct zram *zram) {};
static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	return -EIO;
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx) {};
#endif

#ifdef CONFIG_ZRAM_MEMORY_TRACKING

static struct dentry *zram_debugfs_root;

static void zram_debugfs_create(void)
{
	zram_debugfs_root = debugfs_create_dir("zram", NULL);
}

static void zram_debugfs_destroy(void)
{
	debugfs_remove_recursive(zram_debugfs_root);
}

static void zram_accessed(struct zram *zram, u32 index)
{
	zram_clear_flag(zram, index, ZRAM_IDLE);
	zram->table[index].ac_time = ktime_get_boottime();
}

static ssize_t read_block_state(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	char *kbuf;
	ssize_t index, written = 0;
	struct zram *zram = file->private_data;
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	struct timespec64 ts;

	kbuf = kvmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		kvfree(kbuf);
		return -EINVAL;
	}

	for (index = *ppos; index < nr_pages; index++) {
		int copied;

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		ts = ktime_to_timespec64(zram->table[index].ac_time);
		copied = snprintf(kbuf + written, count,
			"%12zd %12lld.%06lu %c%c%c%c%c%c\n",
			index, (s64)ts.tv_sec,
			ts.tv_nsec / NSEC_PER_USEC,
			zram_test_flag(zram, index, ZRAM_SAME) ? 's' : '.',
			zram_test_flag(zram, index, ZRAM_WB) ? 'w' : '.',
			zram_test_flag(zram, index, ZRAM_HUGE) ? 'h' : '.',
			zram_test_flag(zram, index, ZRAM_IDLE) ? 'i' : '.',
			zram_get_priority(zram, index) ? 'r' : '.',
			zram_test_flag(zram, index,
				       ZRAM_INCOMPRESSIBLE) ? 'n' : '.');

		if (count <= copied) {
			zram_slot_unlock(zram, index);
			break;
		}
		written += copied;
		count -= copied;
next:
		zram_slot_unlock(zram, index);
		*ppos += 1;
	}

	up_read(&zram->init_lock);
	if (copy_to_user(buf, kbuf, written))
		written = -EFAULT;
	kvfree(kbuf);

	return written;
}

static const struct file_operations proc_zram_block_state_op = {
	.open = simple_open,
	.read = read_block_state,
	.llseek = default_llseek,
};

static void zram_debugfs_register(struct zram *zram)
{
	if (!zram_debugfs_root)
		return;

	zram->debugfs_dir = debugfs_create_dir(zram->disk->disk_name,
						zram_debugfs_root);
	debugfs_create_file("block_state", 0400, zram->debugfs_dir,
				zram, &proc_zram_block_state_op);
}

static void zram_debugfs_unregister(struct zram *zram)
{
	debugfs_remove_recursive(zram->debugfs_dir);
}
#else
static void zram_debugfs_create(void) {};
static void zram_debugfs_destroy(void) {};
static void zram_accessed(struct zram *zram, u32 index)
{
	zram_clear_flag(zram, index, ZRAM_IDLE);
};
static void zram_debugfs_register(struct zram *zram) {};
static void zram_debugfs_unregister(struct zram *zram) {};
#endif

/*
 * We switched to per-cpu streams and this attr is not needed anymore.
 * However, we will keep it around for some time, because:
 * a) we may revert per-cpu streams in the future
 * b) it's visible to user space and we need to follow our 2 years
 *    retirement rule; but we already have a number of 'soon to be
 *    altered' attrs, so max_comp_streams need to wait for the next
 *    layoff cycle.
 */
static ssize_t max_comp_streams_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", num_online_cpus());
}

static ssize_t max_comp_streams_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	return len;
}

static void comp_algorithm_set(struct zram *zram, u32 prio, const char *alg)
{
	/* Do not free statically defined compression algorithms */
	if (zram->comp_algs[prio] != default_compressor)
		kfree(zram->comp_algs[prio]);

	zram->comp_algs[prio] = alg;
}

static ssize_t __comp_algorithm_show(struct zram *zram, u32 prio, char *buf)
{
	ssize_t sz;

	down_read(&zram->init_lock);
	sz = zcomp_available_show(zram->comp_algs[prio], buf);
	up_read(&zram->init_lock);

	return sz;
}

static int __comp_algorithm_store(struct zram *zram, u32 prio, const char *buf)
{
#if 0
	char *compressor;
	size_t sz;

	sz = strlen(buf);
	if (sz >= CRYPTO_MAX_ALG_NAME)
		return -E2BIG;

	compressor = kstrdup(buf, GFP_KERNEL);
	if (!compressor)
		return -ENOMEM;

	/* ignore trailing newline */
	if (sz > 0 && compressor[sz - 1] == '\n')
		compressor[sz - 1] = 0x00;

	if (!zcomp_available_algorithm(compressor)) {
		kfree(compressor);
		return -EINVAL;
	}

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		up_write(&zram->init_lock);
		kfree(compressor);
		pr_info("Can't change algorithm for initialized device\n");
		return -EBUSY;
	}

	comp_algorithm_set(zram, prio, compressor);
	up_write(&zram->init_lock);
#endif
	return 0;
}

static void comp_params_reset(struct zram *zram, u32 prio)
{
	struct zcomp_params *params = &zram->params[prio];

	vfree(params->dict);
	params->level = ZCOMP_PARAM_NO_LEVEL;
	params->dict_sz = 0;
	params->dict = NULL;
}

static int comp_params_store(struct zram *zram, u32 prio, s32 level,
			     const char *dict_path)
{
	ssize_t sz = 0;

	comp_params_reset(zram, prio);

	if (dict_path) {
		sz = kernel_read_file_from_path(dict_path, 0,
						&zram->params[prio].dict,
						INT_MAX,
						NULL,
						READING_POLICY);
		if (sz < 0)
			return -EINVAL;
	}

	zram->params[prio].dict_sz = sz;
	zram->params[prio].level = level;
	return 0;
}

static ssize_t algorithm_params_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t len)
{
	s32 prio = ZRAM_PRIMARY_COMP, level = ZCOMP_PARAM_NO_LEVEL;
	char *args, *param, *val, *algo = NULL, *dict_path = NULL;
	struct zram *zram = dev_to_zram(dev);
	int ret;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "priority")) {
			ret = kstrtoint(val, 10, &prio);
			if (ret)
				return ret;
			continue;
		}

		if (!strcmp(param, "level")) {
			ret = kstrtoint(val, 10, &level);
			if (ret)
				return ret;
			continue;
		}

		if (!strcmp(param, "algo")) {
			algo = val;
			continue;
		}

		if (!strcmp(param, "dict")) {
			dict_path = val;
			continue;
		}
	}

	/* Lookup priority by algorithm name */
	if (algo) {
		s32 p;

		prio = -EINVAL;
		for (p = ZRAM_PRIMARY_COMP; p < ZRAM_MAX_COMPS; p++) {
			if (!zram->comp_algs[p])
				continue;

			if (!strcmp(zram->comp_algs[p], algo)) {
				prio = p;
				break;
			}
		}
	}

	if (prio < ZRAM_PRIMARY_COMP || prio >= ZRAM_MAX_COMPS)
		return -EINVAL;

	ret = comp_params_store(zram, prio, level, dict_path);
	return ret ? ret : len;
}

static ssize_t comp_algorithm_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return __comp_algorithm_show(zram, ZRAM_PRIMARY_COMP, buf);
}

static ssize_t comp_algorithm_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	int ret;

	ret = __comp_algorithm_store(zram, ZRAM_PRIMARY_COMP, buf);
	return ret ? ret : len;
}

#ifdef CONFIG_ZRAM_MULTI_COMP
static ssize_t recomp_algorithm_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t sz = 0;
	u32 prio;

	for (prio = ZRAM_SECONDARY_COMP; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2, "#%d: ", prio);
		sz += __comp_algorithm_show(zram, prio, buf + sz);
	}

	return sz;
}

static ssize_t recomp_algorithm_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	int prio = ZRAM_SECONDARY_COMP;
	char *args, *param, *val;
	char *alg = NULL;
	int ret;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "algo")) {
			alg = val;
			continue;
		}

		if (!strcmp(param, "priority")) {
			ret = kstrtoint(val, 10, &prio);
			if (ret)
				return ret;
			continue;
		}
	}

	if (!alg)
		return -EINVAL;

	if (prio < ZRAM_SECONDARY_COMP || prio >= ZRAM_MAX_COMPS)
		return -EINVAL;

	ret = __comp_algorithm_store(zram, prio, alg);
	return ret ? ret : len;
}
#endif

static ssize_t compact_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -EINVAL;
	}

	zs_compact(zram->mem_pool);
	up_read(&zram->init_lock);

	return len;
}

static ssize_t io_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu 0 %8llu\n",
			(u64)atomic64_read(&zram->stats.failed_reads),
			(u64)atomic64_read(&zram->stats.failed_writes),
			(u64)atomic64_read(&zram->stats.notify_free));
	up_read(&zram->init_lock);

	return ret;
}

static ssize_t mm_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	struct zs_pool_stats pool_stats;
	u64 orig_size, mem_used = 0;
	long max_used;
	ssize_t ret;

	memset(&pool_stats, 0x00, sizeof(struct zs_pool_stats));

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		mem_used = zs_get_total_pages(zram->mem_pool);
		zs_pool_stats(zram->mem_pool, &pool_stats);
	}

	orig_size = atomic64_read(&zram->stats.pages_stored);
	max_used = atomic_long_read(&zram->stats.max_used_pages);

	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu %8llu %8lu %8ld %8llu %8lu %8llu %8llu\n",
			orig_size << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.compr_data_size),
			mem_used << PAGE_SHIFT,
			zram->limit_pages << PAGE_SHIFT,
			max_used << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.same_pages),
			atomic_long_read(&pool_stats.pages_compacted),
			(u64)atomic64_read(&zram->stats.huge_pages),
			(u64)atomic64_read(&zram->stats.huge_pages_since));
	up_read(&zram->init_lock);

	return ret;
}

#ifdef CONFIG_ZRAM_WRITEBACK
#define FOUR_K(x) ((x) * (1 << (PAGE_SHIFT - 12)))
static ssize_t bd_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	ret = scnprintf(buf, PAGE_SIZE,
		"%8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu "
		"%8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu\n",
			FOUR_K((u64)atomic64_read(&zram->stats.bd_expire)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_count)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_reads)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_writes)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_objcnt)),
			(u64)(atomic64_read(&zram->stats.bd_size) >> PAGE_SHIFT),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_max_count)),
			(u64)(atomic64_read(&zram->stats.bd_max_size) >> PAGE_SHIFT),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_ppr_count)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_ppr_reads)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_ppr_writes)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_ppr_objcnt)),
			(u64)(atomic64_read(&zram->stats.bd_ppr_size) >> PAGE_SHIFT),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_ppr_max_count)),
			(u64)(atomic64_read(&zram->stats.bd_ppr_max_size) >> PAGE_SHIFT),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_objreads)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_objwrites)));
#else
	ret = scnprintf(buf, PAGE_SIZE,
		"%8llu %8llu %8llu\n",
			FOUR_K((u64)atomic64_read(&zram->stats.bd_count)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_reads)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_writes)));
#endif
	up_read(&zram->init_lock);

	return ret;
}

#ifdef CONFIG_ZRAM_LRU_WRITEBACK
static ssize_t bd_stat_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);

	zram_reset_stats(zram);
	return len;
}
#endif
#endif

static ssize_t debug_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int version = 1;
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"version: %d\n%8llu %8llu\n",
			version,
			(u64)atomic64_read(&zram->stats.writestall),
			(u64)atomic64_read(&zram->stats.miss_free));
	up_read(&zram->init_lock);

	return ret;
}

static DEVICE_ATTR_RO(io_stat);
static DEVICE_ATTR_RO(mm_stat);
#ifdef CONFIG_ZRAM_WRITEBACK
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
static DEVICE_ATTR_RW(bd_stat);
#else
static DEVICE_ATTR_RO(bd_stat);
#endif
#endif
static DEVICE_ATTR_RO(debug_stat);

static void zram_meta_free(struct zram *zram, u64 disksize)
{
	size_t num_pages = disksize >> PAGE_SHIFT;
	size_t index;

	/* Free all pages that are still in this zram device */
	for (index = 0; index < num_pages; index++)
		zram_free_page(zram, index);

	zs_destroy_pool(zram->mem_pool);
	vfree(zram->table);
}

static bool zram_meta_alloc(struct zram *zram, u64 disksize)
{
	size_t num_pages, index;
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	int i;
#endif

	num_pages = disksize >> PAGE_SHIFT;
	zram->table = vzalloc(array_size(num_pages, sizeof(*zram->table)));
	if (!zram->table)
		return false;

#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	for (i = 0; i < num_pages; i++)
		INIT_LIST_HEAD(&zram->table[i].lru_list);
#endif
	zram->mem_pool = zs_create_pool(zram->disk->disk_name);
	if (!zram->mem_pool) {
		vfree(zram->table);
		return false;
	}

	if (!huge_class_size)
		huge_class_size = zs_huge_class_size(zram->mem_pool);

	for (index = 0; index < num_pages; index++)
		spin_lock_init(&zram->table[index].lock);
	return true;
}

/*
 * To protect concurrent access to the same index entry,
 * caller should hold this table index entry's bit_spinlock to
 * indicate this index entry is accessing.
 */
static void zram_free_page(struct zram *zram, size_t index)
{
	unsigned long handle;
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	unsigned long flags;
#endif

#ifdef CONFIG_ZRAM_MEMORY_TRACKING
	zram->table[index].ac_time = 0;
#endif
	if (zram_test_flag(zram, index, ZRAM_IDLE))
		zram_clear_flag(zram, index, ZRAM_IDLE);

	if (zram_test_flag(zram, index, ZRAM_HUGE)) {
		zram_clear_flag(zram, index, ZRAM_HUGE);
		atomic64_dec(&zram->stats.huge_pages);
	}

	if (zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
		zram_clear_flag(zram, index, ZRAM_INCOMPRESSIBLE);

	zram_set_priority(zram, index, 0);

	if (zram_test_flag(zram, index, ZRAM_WB)) {
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
		int size;
		bool ppr = zram_test_flag(zram, index, ZRAM_PPR);

		handle = zram_get_element(zram, index);
		size = handle & (PAGE_SIZE - 1);
		if (size == 0)
			size = PAGE_SIZE;
		atomic64_sub(size, &zram->stats.bd_size);
		if (ppr) {
			zram_clear_flag(zram, index, ZRAM_PPR);
			atomic64_sub(size, &zram->stats.bd_ppr_size);
		}
		if (zram_test_flag(zram, index, ZRAM_EXPIRE)) {
			zram_clear_flag(zram, index, ZRAM_EXPIRE);
			atomic64_dec(&zram->stats.bd_expire);
		}
		zram_clear_flag(zram, index, ZRAM_WB);
		free_block_bdev(zram, handle >> (PAGE_SHIFT * 2), ppr);
#else
		zram_clear_flag(zram, index, ZRAM_WB);
		free_block_bdev(zram, zram_get_element(zram, index));
#endif
		goto out;
	}

	/*
	 * No memory is allocated for same element filled pages.
	 * Simply clear same page flag.
	 */
	if (zram_test_flag(zram, index, ZRAM_SAME)) {
		zram_clear_flag(zram, index, ZRAM_SAME);
		atomic64_dec(&zram->stats.same_pages);
		goto out;
	}

	handle = zram_get_handle(zram, index);
	if (!handle)
		return;

	zs_free(zram->mem_pool, handle);

	atomic64_sub(zram_get_obj_size(zram, index),
			&zram->stats.compr_data_size);
out:
	atomic64_dec(&zram->stats.pages_stored);
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	if (zram_test_flag(zram, index, ZRAM_UNDER_PPR))
		zram_clear_flag(zram, index, ZRAM_UNDER_PPR);
	spin_lock_irqsave(&zram->list_lock, flags);
	if (!list_empty(&zram->table[index].lru_list)) {
		list_del_init(&zram->table[index].lru_list);
		if (zram_test_flag(zram, index, ZRAM_LRU)) {
			zram_clear_flag(zram, index, ZRAM_LRU);
			atomic64_dec(&zram->stats.lru_pages);
		}
	}
	spin_unlock_irqrestore(&zram->list_lock, flags);
#endif
	WARN_ON_ONCE(zram->table[index].flags &
		~(1UL << ZRAM_UNDER_WB));
}

/*
 * Reads (decompresses if needed) a page from zspool (zsmalloc).
 * Corresponding ZRAM slot should be locked.
 */
static int zram_read_from_zspool(struct zram *zram, struct page *page,
				 u32 index)
{
	struct zcomp_strm *zstrm;
	unsigned long handle;
	unsigned int size;
	void *src, *dst;
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	unsigned long flags;
	unsigned long blk_idx;
	bool ppr;
#endif
	u32 prio;
	int ret;

	handle = zram_get_handle(zram, index);
	if (!handle || zram_test_flag(zram, index, ZRAM_SAME)) {
		unsigned long value;
		void *mem;

		value = handle ? zram_get_element(zram, index) : 0;
		mem = kmap_local_page(page);
		zram_fill_page(mem, PAGE_SIZE, value);
		kunmap_local(mem);
		return 0;
	}

	size = zram_get_obj_size(zram, index);

	if (size != PAGE_SIZE) {
		prio = zram_get_priority(zram, index);
		zstrm = zcomp_stream_get(zram->comps[prio]);
	}

	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	if (size == PAGE_SIZE) {
		dst = kmap_local_page(page);
		copy_page(dst, src);
		kunmap_local(dst);
		ret = 0;
	} else {
		dst = kmap_local_page(page);
		ret = zcomp_decompress(zram->comps[prio], zstrm,
				       src, size, dst);
		kunmap_local(dst);
		zcomp_stream_put(zram->comps[prio]);
	}

	/* Should NEVER happen. BUG() if it does. */
	if (WARN_ON(ret)) {
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
		handle_decomp_fail(zram->comp_algs[prio], ret, index, src, size,
				   NULL);
#endif
	}

	zs_unmap_object(zram->mem_pool, handle);
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	if (zram_test_flag(zram, index, ZRAM_UNDER_PPR))
		zram_clear_flag(zram, index, ZRAM_UNDER_PPR);
	spin_lock_irqsave(&zram->list_lock, flags);
	if (!list_empty(&zram->table[index].lru_list)) {
		list_del_init(&zram->table[index].lru_list);
		if (zram_test_flag(zram, index, ZRAM_LRU)) {
			zram_clear_flag(zram, index, ZRAM_LRU);
			atomic64_dec(&zram->stats.lru_pages);
		}
	}
	spin_unlock_irqrestore(&zram->list_lock, flags);
#endif
	return ret;
}

static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent)
{
	int ret;

	zram_slot_lock(zram, index);
	if (!zram_test_flag(zram, index, ZRAM_WB)) {
		/* Slot should be locked through out the function call */
		ret = zram_read_from_zspool(zram, page, index);
		zram_slot_unlock(zram, index);
	} else {
		/*
		 * The slot should be unlocked before reading from the backing
		 * device.
		 */
		zram_slot_unlock(zram, index);

#ifdef CONFIG_ZRAM_LRU_WRITEBACK
		{
			unsigned long lru_handle;
			unsigned long blk_idx;
			bool ppr;
			struct bio_vec bvec;

			atomic64_inc(&zram->stats.bd_objreads);
			ppr = zram_test_flag(zram, index, ZRAM_PPR);
			if (ppr)
				atomic64_inc(&zram->stats.bd_ppr_reads);
			if (!zram_test_flag(zram, index, ZRAM_EXPIRE)) {
				zram_set_flag(zram, index, ZRAM_EXPIRE);
				atomic64_inc(&zram->stats.bd_expire);
			}
			lru_handle = zram_get_element(zram, index);
			blk_idx = lru_handle >> (PAGE_SHIFT * 2);
			if (((lru_handle & (PAGE_SIZE - 1)) != 0) || ppr) {
				zram_set_flag(zram, index, ZRAM_READ_BDEV);
				zram_inc_wb_table(zram, blk_idx);
				bvec.bv_page = page;
				bvec.bv_len = PAGE_SIZE;
				bvec.bv_offset = 0;
				ret = read_comp_from_bdev(zram, &bvec, lru_handle, parent, ppr);
				if (ret < 0)
					zram_dec_wb_table(zram, blk_idx, ppr);
			} else {
				ret = read_from_bdev(zram, page, blk_idx, parent);
			}
		}
#else
		ret = read_from_bdev(zram, page, zram_get_element(zram, index),
				     parent);
#endif
	}

	/* Should NEVER happen. Return bio error if it does. */
	if (WARN_ON(ret < 0))
		pr_err("Decompression failed! err=%d, page=%u\n", ret, index);

	return ret;
}

/*
 * Use a temporary buffer to decompress the page, as the decompressor
 * always expects a full page for the output.
 */
static int zram_bvec_read_partial(struct zram *zram, struct bio_vec *bvec,
				  u32 index, int offset)
{
	struct page *page = alloc_page(GFP_NOIO);
	int ret;

	if (!page)
		return -ENOMEM;
	ret = zram_read_page(zram, page, index, NULL);
	if (likely(!ret))
		memcpy_to_bvec(bvec, page_address(page) + offset);
	__free_page(page);
	return ret;
}

static int zram_bvec_read(struct zram *zram, struct bio_vec *bvec,
			  u32 index, int offset, struct bio *bio)
{
	if (is_partial_io(bvec))
		return zram_bvec_read_partial(zram, bvec, index, offset);
	return zram_read_page(zram, bvec->bv_page, index, bio);
}

static int zram_write_page(struct zram *zram, struct page *page, u32 index)
{
	int ret = 0;
	unsigned long alloced_pages;
	unsigned long handle = -ENOMEM;
	unsigned int comp_len = 0;
	void *src, *dst, *mem;
	struct zcomp_strm *zstrm;
	unsigned long element = 0;
	enum zram_pageflags flags = 0;
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	unsigned long irq_flags;
#endif

	mem = kmap_local_page(page);
	if (page_same_filled(mem, &element)) {
		kunmap_local(mem);
		/* Free memory associated with this sector now. */
		flags = ZRAM_SAME;
		atomic64_inc(&zram->stats.same_pages);
		goto out;
	}
	kunmap_local(mem);

compress_again:
	zstrm = zcomp_stream_get(zram->comps[ZRAM_PRIMARY_COMP]);
	src = kmap_local_page(page);
	ret = zcomp_compress(zram->comps[ZRAM_PRIMARY_COMP], zstrm,
			     src, &comp_len);
	kunmap_local(src);

	if (unlikely(ret)) {
		zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
		pr_err("Compression failed! err=%d\n", ret);
		zs_free(zram->mem_pool, handle);
		return ret;
	}

	if (comp_len >= huge_class_size)
		comp_len = PAGE_SIZE;
	/*
	 * handle allocation has 2 paths:
	 * a) fast path is executed with preemption disabled (for
	 *  per-cpu streams) and has __GFP_DIRECT_RECLAIM bit clear,
	 *  since we can't sleep;
	 * b) slow path enables preemption and attempts to allocate
	 *  the page with __GFP_DIRECT_RECLAIM bit set. we have to
	 *  put per-cpu compression stream and, thus, to re-do
	 *  the compression once handle is allocated.
	 *
	 * if we have a 'non-null' handle here then we are coming
	 * from the slow path and handle has already been allocated.
	 */
	if (IS_ERR_VALUE(handle))
		handle = zs_malloc(zram->mem_pool, comp_len,
				__GFP_KSWAPD_RECLAIM |
				__GFP_NOWARN |
				__GFP_HIGHMEM |
				__GFP_MOVABLE |
				__GFP_CMA
#if defined(__GFP_OFFLINABLE)
				| __GFP_OFFLINABLE
#endif
				);
	if (IS_ERR_VALUE(handle)) {
		zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
		atomic64_inc(&zram->stats.writestall);
		handle = zs_malloc(zram->mem_pool, comp_len,
				GFP_NOIO | __GFP_HIGHMEM |
				__GFP_MOVABLE | __GFP_CMA
#if defined(__GFP_OFFLINABLE)
				| __GFP_OFFLINABLE
#endif
				);
		if (IS_ERR_VALUE(handle))
			return PTR_ERR((void *)handle);

		if (comp_len != PAGE_SIZE)
			goto compress_again;
		/*
		 * If the page is not compressible, you need to acquire the
		 * lock and execute the code below. The zcomp_stream_get()
		 * call is needed to disable the cpu hotplug and grab the
		 * zstrm buffer back. It is necessary that the dereferencing
		 * of the zstrm variable below occurs correctly.
		 */
		zstrm = zcomp_stream_get(zram->comps[ZRAM_PRIMARY_COMP]);
	}

	alloced_pages = zs_get_total_pages(zram->mem_pool);
	update_used_max(zram, alloced_pages);

	if (zram->limit_pages && alloced_pages > zram->limit_pages) {
		zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
		zs_free(zram->mem_pool, handle);
		return -ENOMEM;
	}

	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);

	src = zstrm->buffer;
	if (comp_len == PAGE_SIZE)
		src = kmap_local_page(page);
	memcpy(dst, src, comp_len);
	if (comp_len == PAGE_SIZE)
		kunmap_local(src);

	zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
	zs_unmap_object(zram->mem_pool, handle);
	atomic64_add(comp_len, &zram->stats.compr_data_size);
out:
	/*
	 * Free memory associated with this sector
	 * before overwriting unused sectors.
	 */
	zram_slot_lock(zram, index);
	zram_free_page(zram, index);

	if (comp_len == PAGE_SIZE) {
		zram_set_flag(zram, index, ZRAM_HUGE);
		atomic64_inc(&zram->stats.huge_pages);
		atomic64_inc(&zram->stats.huge_pages_since);
	}

	if (flags) {
		zram_set_flag(zram, index, flags);
		zram_set_element(zram, index, element);
	} else {
		zram_set_handle(zram, index, handle);
		zram_set_obj_size(zram, index, comp_len);
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
		if (!page->mem_cgroup ||
		    page->mem_cgroup->swappiness != NON_LRU_SWAPPINESS) {
			spin_lock_irqsave(&zram->list_lock, irq_flags);
			list_add_tail(&zram->table[index].lru_list, &zram->list);
			spin_unlock_irqrestore(&zram->list_lock, irq_flags);
			zram_set_flag(zram, index, ZRAM_LRU);
			atomic64_inc(&zram->stats.lru_pages);
		}
#endif
	}
	zram_slot_unlock(zram, index);

	/* Update stats */
	atomic64_inc(&zram->stats.pages_stored);
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	if (!flags)
		try_wakeup_zram_wbd(zram);
#endif
	return ret;
}

/*
 * This is a partial IO. Read the full page before writing the changes.
 */
static int zram_bvec_write_partial(struct zram *zram, struct bio_vec *bvec,
				   u32 index, int offset, struct bio *bio)
{
	struct page *page = alloc_page(GFP_NOIO);
	int ret;

	if (!page)
		return -ENOMEM;

	ret = zram_read_page(zram, page, index, bio);
	if (!ret) {
		memcpy_from_bvec(page_address(page) + offset, bvec);
		ret = zram_write_page(zram, page, index);
	}
	__free_page(page);
	return ret;
}

static int zram_bvec_write(struct zram *zram, struct bio_vec *bvec,
			   u32 index, int offset, struct bio *bio)
{
	if (is_partial_io(bvec))
		return zram_bvec_write_partial(zram, bvec, index, offset, bio);
	return zram_write_page(zram, bvec->bv_page, index);
}

#ifdef CONFIG_ZRAM_MULTI_COMP
/*
 * This function will decompress (unless it's ZRAM_HUGE) the page and then
 * attempt to compress it using provided compression algorithm priority
 * (which is potentially more effective).
 *
 * Corresponding ZRAM slot should be locked.
 */
static int zram_recompress(struct zram *zram, u32 index, struct page *page,
			   u64 *num_recomp_pages, u32 threshold, u32 prio,
			   u32 prio_max)
{
	struct zcomp_strm *zstrm = NULL;
	unsigned long handle_old;
	unsigned long handle_new;
	unsigned int comp_len_old;
	unsigned int comp_len_new;
	unsigned int class_index_old;
	unsigned int class_index_new;
	u32 num_recomps = 0;
	void *src, *dst;
	int ret;

	handle_old = zram_get_handle(zram, index);
	if (!handle_old)
		return -EINVAL;

	comp_len_old = zram_get_obj_size(zram, index);
	/*
	 * Do not recompress objects that are already "small enough".
	 */
	if (comp_len_old < threshold)
		return 0;

	ret = zram_read_from_zspool(zram, page, index);
	if (ret)
		return ret;

	class_index_old = zs_lookup_class_index(zram->mem_pool, comp_len_old);
	/*
	 * Iterate the secondary comp algorithms list (in order of priority)
	 * and try to recompress the page.
	 */
	for (; prio < prio_max; prio++) {
		if (!zram->comps[prio])
			continue;

		/*
		 * Skip if the object is already re-compressed with a higher
		 * priority algorithm (or same algorithm).
		 */
		if (prio <= zram_get_priority(zram, index))
			continue;

		num_recomps++;
		zstrm = zcomp_stream_get(zram->comps[prio]);
		src = kmap_local_page(page);
		ret = zcomp_compress(zram->comps[prio], zstrm,
				     src, &comp_len_new);
		kunmap_local(src);

		if (ret) {
			zcomp_stream_put(zram->comps[prio]);
			return ret;
		}

		class_index_new = zs_lookup_class_index(zram->mem_pool,
							comp_len_new);

		/* Continue until we make progress */
		if (class_index_new >= class_index_old ||
		    (threshold && comp_len_new >= threshold)) {
			zcomp_stream_put(zram->comps[prio]);
			continue;
		}

		/* Recompression was successful so break out */
		break;
	}

	/*
	 * We did not try to recompress, e.g. when we have only one
	 * secondary algorithm and the page is already recompressed
	 * using that algorithm
	 */
	if (!zstrm)
		return 0;

	/*
	 * Decrement the limit (if set) on pages we can recompress, even
	 * when current recompression was unsuccessful or did not compress
	 * the page below the threshold, because we still spent resources
	 * on it.
	 */
	if (*num_recomp_pages)
		*num_recomp_pages -= 1;

	if (class_index_new >= class_index_old) {
		/*
		 * Secondary algorithms failed to re-compress the page
		 * in a way that would save memory, mark the object as
		 * incompressible so that we will not try to compress
		 * it again.
		 *
		 * We need to make sure that all secondary algorithms have
		 * failed, so we test if the number of recompressions matches
		 * the number of active secondary algorithms.
		 */
		if (num_recomps == zram->num_active_comps - 1)
			zram_set_flag(zram, index, ZRAM_INCOMPRESSIBLE);
		return 0;
	}

	/* Successful recompression but above threshold */
	if (threshold && comp_len_new >= threshold)
		return 0;

	/*
	 * No direct reclaim (slow path) for handle allocation and no
	 * re-compression attempt (unlike in zram_write_bvec()) since
	 * we already have stored that object in zsmalloc. If we cannot
	 * alloc memory for recompressed object then we bail out and
	 * simply keep the old (existing) object in zsmalloc.
	 */
	handle_new = zs_malloc(zram->mem_pool, comp_len_new,
			       __GFP_KSWAPD_RECLAIM |
			       __GFP_NOWARN |
			       __GFP_HIGHMEM |
			       __GFP_MOVABLE);
	if (IS_ERR_VALUE(handle_new)) {
		zcomp_stream_put(zram->comps[prio]);
		return PTR_ERR((void *)handle_new);
	}

	dst = zs_map_object(zram->mem_pool, handle_new, ZS_MM_WO);
	memcpy(dst, zstrm->buffer, comp_len_new);
	zcomp_stream_put(zram->comps[prio]);

	zs_unmap_object(zram->mem_pool, handle_new);

	zram_free_page(zram, index);
	zram_set_handle(zram, index, handle_new);
	zram_set_obj_size(zram, index, comp_len_new);
	zram_set_priority(zram, index, prio);

	atomic64_add(comp_len_new, &zram->stats.compr_data_size);
	atomic64_inc(&zram->stats.pages_stored);

	return 0;
}

#define RECOMPRESS_IDLE		(1 << 0)
#define RECOMPRESS_HUGE		(1 << 1)

static ssize_t recompress_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	u32 prio = ZRAM_SECONDARY_COMP, prio_max = ZRAM_MAX_COMPS;
	struct zram *zram = dev_to_zram(dev);
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	char *args, *param, *val, *algo = NULL;
	u64 num_recomp_pages = ULLONG_MAX;
	u32 mode = 0, threshold = 0;
	unsigned long index;
	struct page *page;
	ssize_t ret;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "type")) {
			if (!strcmp(val, "idle"))
				mode = RECOMPRESS_IDLE;
			if (!strcmp(val, "huge"))
				mode = RECOMPRESS_HUGE;
			if (!strcmp(val, "huge_idle"))
				mode = RECOMPRESS_IDLE | RECOMPRESS_HUGE;
			continue;
		}

		if (!strcmp(param, "max_pages")) {
			/*
			 * Limit the number of entries (pages) we attempt to
			 * recompress.
			 */
			ret = kstrtoull(val, 10, &num_recomp_pages);
			if (ret)
				return ret;
			continue;
		}

		if (!strcmp(param, "threshold")) {
			/*
			 * We will re-compress only idle objects equal or
			 * greater in size than watermark.
			 */
			ret = kstrtouint(val, 10, &threshold);
			if (ret)
				return ret;
			continue;
		}

		if (!strcmp(param, "algo")) {
			algo = val;
			continue;
		}

		if (!strcmp(param, "priority")) {
			ret = kstrtouint(val, 10, &prio);
			if (ret)
				return ret;

			if (prio == ZRAM_PRIMARY_COMP)
				prio = ZRAM_SECONDARY_COMP;

			prio_max = min(prio + 1, ZRAM_MAX_COMPS);
			continue;
		}
	}

	if (threshold >= huge_class_size)
		return -EINVAL;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		goto release_init_lock;
	}

	if (algo) {
		bool found = false;

		for (; prio < ZRAM_MAX_COMPS; prio++) {
			if (!zram->comp_algs[prio])
				continue;

			if (!strcmp(zram->comp_algs[prio], algo)) {
				prio_max = min(prio + 1, ZRAM_MAX_COMPS);
				found = true;
				break;
			}
		}

		if (!found) {
			ret = -EINVAL;
			goto release_init_lock;
		}
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto release_init_lock;
	}

	ret = len;
	for (index = 0; index < nr_pages; index++) {
		int err = 0;

		if (!num_recomp_pages)
			break;

		zram_slot_lock(zram, index);

		if (!zram_allocated(zram, index))
			goto next;

		if (mode & RECOMPRESS_IDLE &&
		    !zram_test_flag(zram, index, ZRAM_IDLE))
			goto next;

		if (mode & RECOMPRESS_HUGE &&
		    !zram_test_flag(zram, index, ZRAM_HUGE))
			goto next;

		if (zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
		    zram_test_flag(zram, index, ZRAM_SAME) ||
		    zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
			goto next;

		err = zram_recompress(zram, index, page, &num_recomp_pages,
				      threshold, prio, prio_max);
next:
		zram_slot_unlock(zram, index);
		if (err) {
			ret = err;
			break;
		}

		cond_resched();
	}

	__free_page(page);

release_init_lock:
	up_read(&zram->init_lock);
	return ret;
}
#endif

static void zram_bio_discard(struct zram *zram, struct bio *bio)
{
	size_t n = bio->bi_iter.bi_size;
	u32 index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	u32 offset = (bio->bi_iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
			SECTOR_SHIFT;

	/*
	 * zram manages data in physical block size units. Because logical block
	 * size isn't identical with physical block size on some arch, we
	 * could get a discard request pointing to a specific offset within a
	 * certain physical block.  Although we can handle this request by
	 * reading that physiclal block and decompressing and partially zeroing
	 * and re-compressing and then re-storing it, this isn't reasonable
	 * because our intent with a discard request is to save memory.  So
	 * skipping this logical block is appropriate here.
	 */
	if (offset) {
		if (n <= (PAGE_SIZE - offset))
			return;

		n -= (PAGE_SIZE - offset);
		index++;
	}

	while (n >= PAGE_SIZE) {
		zram_slot_lock(zram, index);
		zram_free_page(zram, index);
		zram_slot_unlock(zram, index);
		atomic64_inc(&zram->stats.notify_free);
		index++;
		n -= PAGE_SIZE;
	}

	bio_endio(bio);
}

/*
 * Returns errno if it has some problem. Otherwise return 0 or 1.
 * Returns 0 if IO request was done synchronously
 * Returns 1 if IO request was successfully submitted.
 */
static int zram_bvec_rw(struct zram *zram, struct bio_vec *bvec, u32 index,
			int offset, unsigned int op, struct bio *bio)
{
	unsigned long start_time = jiffies;
	struct request_queue *q = zram->disk->queue;
	int ret;

	generic_start_io_acct(q, op, bvec->bv_len >> SECTOR_SHIFT,
			&zram->disk->part0);

	if (!op_is_write(op)) {
		ret = zram_bvec_read(zram, bvec, index, offset, bio);
		if (unlikely(ret < 0)) {
			atomic64_inc(&zram->stats.failed_reads);
			return ret;
		}
		flush_dcache_page(bvec->bv_page);
	} else {
		ret = zram_bvec_write(zram, bvec, index, offset, bio);
		if (unlikely(ret < 0)) {
			atomic64_inc(&zram->stats.failed_writes);
			return ret;
		}
	}

	generic_end_io_acct(q, op, &zram->disk->part0, start_time);

	zram_slot_lock(zram, index);
	zram_accessed(zram, index);
	zram_slot_unlock(zram, index);
	return 0;
}

static void __zram_make_request(struct zram *zram, struct bio *bio)
{
	int offset;
	u32 index;
	struct bio_vec bvec;
	struct bvec_iter iter;

	index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	offset = (bio->bi_iter.bi_sector &
		  (SECTORS_PER_PAGE - 1)) << SECTOR_SHIFT;

	bio_for_each_segment(bvec, bio, iter) {
		struct bio_vec bv = bvec;
		unsigned int unwritten = bvec.bv_len;

		do {
			bv.bv_len = min_t(unsigned int, PAGE_SIZE - offset,
							unwritten);
			if (zram_bvec_rw(zram, &bv, index, offset,
					 bio_op(bio), bio) < 0)
				goto out;

			bv.bv_offset += bv.bv_len;
			unwritten -= bv.bv_len;

			update_position(&index, &offset, &bv);
		} while (unwritten);
	}

	bio_endio(bio);
	return;

out:
	bio_io_error(bio);
}

/*
 * Handler function for all zram I/O requests.
 */
static blk_qc_t zram_make_request(struct request_queue *queue, struct bio *bio)
{
	struct zram *zram = bio->bi_disk->private_data;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		__zram_make_request(zram, bio);
		break;
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		zram_bio_discard(zram, bio);
		break;
	default:
		WARN_ON_ONCE(1);
		bio_endio(bio);
	}
	return BLK_QC_T_NONE;
}

static void zram_slot_free_notify(struct block_device *bdev,
				unsigned long index)
{
	struct zram *zram;

	zram = bdev->bd_disk->private_data;

	atomic64_inc(&zram->stats.notify_free);
	if (!zram_slot_trylock(zram, index)) {
		atomic64_inc(&zram->stats.miss_free);
		return;
	}

	zram_free_page(zram, index);
	zram_slot_unlock(zram, index);
}

static void zram_comp_params_reset(struct zram *zram)
{
	u32 prio;

	for (prio = ZRAM_PRIMARY_COMP; prio < ZRAM_MAX_COMPS; prio++) {
		comp_params_reset(zram, prio);
	}
}

static void zram_destroy_comps(struct zram *zram)
{
	u32 prio;

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		struct zcomp *comp = zram->comps[prio];

		zram->comps[prio] = NULL;
		if (!comp)
			continue;
		zcomp_destroy(comp);
		zram->num_active_comps--;
	}

	zram_comp_params_reset(zram);
}

static void zram_reset_device(struct zram *zram)
{
	down_write(&zram->init_lock);

	zram->limit_pages = 0;

	if (!init_done(zram)) {
		up_write(&zram->init_lock);
		return;
	}

	set_capacity(zram->disk, 0);
	part_stat_set_all(&zram->disk->part0, 0);

	/* I/O operation under all of CPU are done so let's free */
	zram_meta_free(zram, zram->disksize);
	zram->disksize = 0;
	zram_destroy_comps(zram);
	memset(&zram->stats, 0, sizeof(zram->stats));
	reset_bdev(zram);

	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);
	up_write(&zram->init_lock);
}

#ifdef CONFIG_ZRAM_SIZE_AUTO
/* Only apply auto-sizing once on first boot */
static bool zram_auto_size_applied;
#elif defined(CONFIG_ZRAM_SIZE_OVERRIDE)
/* Only apply size override once on first boot */
static bool zram_size_override_applied;
#endif

static ssize_t disksize_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 disksize;
	struct zcomp *comp;
	struct zram *zram = dev_to_zram(dev);
	int err;
	u32 prio;

	disksize = memparse(buf, NULL);
	if (!disksize)
		return -EINVAL;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Cannot change disksize for initialized device\n");
		err = -EBUSY;
		goto out_unlock;
	}

#ifdef CONFIG_ZRAM_SIZE_AUTO
		/*
		 * Dynamic ZRAM size detection:
		 * totalram_pages() returns usable pages.
		 *
		 * Use fixed sizes for known RAM variants and fall back to
		 * rounded 50% sizing for anything unexpected.
		 */
	if (!zram_auto_size_applied) {
		unsigned long total_ram_mb =
			totalram_pages() * (PAGE_SIZE / 1024) / 1024;
		u64 rounded_half_gb;

		if (total_ram_mb > 14000) {
			disksize = 8ULL * SZ_1G;
			pr_info("Detected 16GB RAM variant (usable: %lu MB), setting ZRAM to 8GB (50%%)",
				total_ram_mb);
		} else if (total_ram_mb > 10000) {
			disksize = 6ULL * SZ_1G;
			pr_info("Detected 12GB RAM variant (usable: %lu MB), setting ZRAM to 6GB (50%%)",
				total_ram_mb);
		} else if (total_ram_mb > 6200) {
			disksize = 4ULL * SZ_1G;
			pr_info("Detected 8GB RAM variant (usable: %lu MB), setting ZRAM to 4GB (50%%)",
				total_ram_mb);
		} else if (total_ram_mb > 4200) {
			disksize = 4ULL * SZ_1G;
			pr_info("Detected 6GB RAM variant (usable: %lu MB), setting ZRAM to 4GB (50%%)",
				total_ram_mb);
		} else {
			rounded_half_gb = DIV_ROUND_CLOSEST_ULL((u64)total_ram_mb, 2048);
			if (!rounded_half_gb)
				rounded_half_gb = 1;
			disksize = rounded_half_gb * SZ_1G;
			pr_info("Detected unknown RAM variant (usable: %lu MB), setting ZRAM to %lluGB (~50%% rounded)",
				total_ram_mb, rounded_half_gb);
		}
		zram_auto_size_applied = true;
	}
#elif defined(CONFIG_ZRAM_SIZE_OVERRIDE)
	/*
	 * Only apply size override on first boot. After that, users
	 * can freely resize ZRAM via sysfs or kernel modules.
	 */
	if (!zram_size_override_applied) {
		disksize = (u64)SZ_1 * CONFIG_ZRAM_SIZE_OVERRIDE;
		pr_info("Overriding zram size to %llu", disksize);
		zram_size_override_applied = true;
	}
#endif
	if (!zram_meta_alloc(zram, disksize)) {
		err = -ENOMEM;
		goto out_unlock;
	}

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		comp = zcomp_create(zram->comp_algs[prio],
				    &zram->params[prio]);
		if (IS_ERR(comp)) {
			pr_err("Cannot initialise %s compressing backend\n",
			       zram->comp_algs[prio]);
			err = PTR_ERR(comp);
			goto out_free_comps;
		}

		zram->comps[prio] = comp;
		zram->num_active_comps++;

		if (prio == ZRAM_PRIMARY_COMP) {
			if (!strncmp(zram->comp_algs[ZRAM_PRIMARY_COMP], "lzo-rle", 7))
				is_lzorle = true;
			else
				is_lzorle = false;
		}
	}
	zram->disksize = disksize;
	set_capacity(zram->disk, zram->disksize >> SECTOR_SHIFT);

	revalidate_disk(zram->disk);
	up_write(&zram->init_lock);

	return len;

out_free_comps:
	zram_destroy_comps(zram);
	zram_meta_free(zram, disksize);
out_unlock:
	up_write(&zram->init_lock);
	return err;
}

static ssize_t reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned short do_reset;
	struct zram *zram;
	struct block_device *bdev;

	ret = kstrtou16(buf, 10, &do_reset);
	if (ret)
		return ret;

	if (!do_reset)
		return -EINVAL;

	zram = dev_to_zram(dev);
	bdev = bdget_disk(zram->disk, 0);
	if (!bdev)
		return -ENOMEM;

	mutex_lock(&bdev->bd_mutex);
	/* Do not reset an active device or claimed device */
	if (bdev->bd_openers || zram->claim) {
		mutex_unlock(&bdev->bd_mutex);
		bdput(bdev);
		return -EBUSY;
	}

	/* From now on, anyone can't open /dev/zram[0-9] */
	zram->claim = true;
	mutex_unlock(&bdev->bd_mutex);
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	stop_lru_writeback(zram);
#endif
	/* Make sure all the pending I/O are finished */
	sync_blockdev(bdev);
	zram_reset_device(zram);
	revalidate_disk(zram->disk);
	bdput(bdev);

	mutex_lock(&bdev->bd_mutex);
	zram->claim = false;
	mutex_unlock(&bdev->bd_mutex);

	return len;
}

static int zram_open(struct block_device *bdev, fmode_t mode)
{
	int ret = 0;
	struct zram *zram;

	WARN_ON(!mutex_is_locked(&bdev->bd_mutex));

	zram = bdev->bd_disk->private_data;
	/* zram was claimed to reset so open request fails */
	if (zram->claim)
		ret = -EBUSY;

	return ret;
}

static const struct block_device_operations zram_devops = {
	.open = zram_open,
	.swap_slot_free_notify = zram_slot_free_notify,
	.owner = THIS_MODULE
};

static DEVICE_ATTR_WO(compact);
static DEVICE_ATTR_RW(disksize);
static DEVICE_ATTR_RO(initstate);
static DEVICE_ATTR_WO(reset);
static DEVICE_ATTR_WO(mem_limit);
static DEVICE_ATTR_WO(mem_used_max);
static DEVICE_ATTR_WO(idle);
static DEVICE_ATTR_RW(max_comp_streams);
static DEVICE_ATTR_RW(comp_algorithm);
#ifdef CONFIG_ZRAM_WRITEBACK
static DEVICE_ATTR_RW(backing_dev);
static DEVICE_ATTR_WO(writeback);
static DEVICE_ATTR_RW(writeback_limit);
static DEVICE_ATTR_RW(writeback_limit_enable);
#endif
#ifdef CONFIG_ZRAM_MULTI_COMP
static DEVICE_ATTR_RW(recomp_algorithm);
static DEVICE_ATTR_WO(recompress);
#endif
static DEVICE_ATTR_WO(algorithm_params);

static struct attribute *zram_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_initstate.attr,
	&dev_attr_reset.attr,
	&dev_attr_compact.attr,
	&dev_attr_mem_limit.attr,
	&dev_attr_mem_used_max.attr,
	&dev_attr_idle.attr,
	&dev_attr_max_comp_streams.attr,
	&dev_attr_comp_algorithm.attr,
#ifdef CONFIG_ZRAM_WRITEBACK
	&dev_attr_backing_dev.attr,
	&dev_attr_writeback.attr,
	&dev_attr_writeback_limit.attr,
	&dev_attr_writeback_limit_enable.attr,
#endif
	&dev_attr_io_stat.attr,
	&dev_attr_mm_stat.attr,
#ifdef CONFIG_ZRAM_WRITEBACK
	&dev_attr_bd_stat.attr,
#endif
	&dev_attr_debug_stat.attr,
#ifdef CONFIG_ZRAM_MULTI_COMP
	&dev_attr_recomp_algorithm.attr,
	&dev_attr_recompress.attr,
#endif
	&dev_attr_algorithm_params.attr,
	NULL,
};

ATTRIBUTE_GROUPS(zram_disk);

/*
 * Allocate and initialize new zram device. the function returns
 * '>= 0' device_id upon success, and negative value otherwise.
 */
static int zram_add(void)
{
	struct zram *zram;
	struct request_queue *queue;
	int ret, device_id;

	zram = kzalloc(sizeof(struct zram), GFP_KERNEL);
	if (!zram)
		return -ENOMEM;

	ret = idr_alloc(&zram_index_idr, zram, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto out_free_dev;
	device_id = ret;

	if (device_id >= 1) {
		ret = -ENOMEM;
		goto out_free_idr;
	}

	init_rwsem(&zram->init_lock);
#ifdef CONFIG_ZRAM_WRITEBACK
	spin_lock_init(&zram->wb_limit_lock);
#endif
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	INIT_LIST_HEAD(&zram->list);
	spin_lock_init(&zram->list_lock);
	spin_lock_init(&zram->wb_table_lock);
	spin_lock_init(&zram->bitmap_lock);
	mutex_init(&zram->blk_bitmap_lock);
#endif
	queue = blk_alloc_queue(GFP_KERNEL);
	if (!queue) {
		pr_err("Error allocating disk queue for device %d\n",
			device_id);
		ret = -ENOMEM;
		goto out_free_idr;
	}

	blk_queue_make_request(queue, zram_make_request);

	/* gendisk structure */
	zram->disk = alloc_disk(1);
	if (!zram->disk) {
		pr_err("Error allocating disk structure for device %d\n",
			device_id);
		ret = -ENOMEM;
		goto out_free_queue;
	}

	zram->disk->major = zram_major;
	zram->disk->first_minor = device_id;
	zram->disk->fops = &zram_devops;
	zram->disk->queue = queue;
	zram->disk->private_data = zram;
	snprintf(zram->disk->disk_name, 16, "zram%d", device_id);

	/* Actual capacity set using sysfs (/sys/block/zram<id>/disksize */
	set_capacity(zram->disk, 0);
	/* zram devices sort of resembles non-rotational disks */
	blk_queue_flag_set(QUEUE_FLAG_NONROT, zram->disk->queue);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, zram->disk->queue);

	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(zram->disk->queue, PAGE_SIZE);
	blk_queue_logical_block_size(zram->disk->queue,
					ZRAM_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(zram->disk->queue, PAGE_SIZE);
	blk_queue_io_opt(zram->disk->queue, PAGE_SIZE);
	zram->disk->queue->limits.discard_granularity = PAGE_SIZE;
	blk_queue_max_discard_sectors(zram->disk->queue, UINT_MAX);
	blk_queue_flag_set(QUEUE_FLAG_DISCARD, zram->disk->queue);

	/*
	 * zram_bio_discard() will clear all logical blocks if logical block
	 * size is identical with physical block size(PAGE_SIZE). But if it is
	 * different, we will skip discarding some parts of logical blocks in
	 * the part of the request range which isn't aligned to physical block
	 * size.  So we can't ensure that all discarded logical blocks are
	 * zeroed.
	 */
	if (ZRAM_LOGICAL_BLOCK_SIZE == PAGE_SIZE)
		blk_queue_max_write_zeroes_sectors(zram->disk->queue, UINT_MAX);

	zram->disk->queue->backing_dev_info->capabilities |= BDI_CAP_STABLE_WRITES;
	device_add_disk(NULL, zram->disk, zram_disk_groups);

	zram_comp_params_reset(zram);
	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);

#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	if (!g_zram)
		g_zram = zram;
#endif

	zram_debugfs_register(zram);
	pr_info("Added device: %s\n", zram->disk->disk_name);
	return device_id;

out_free_queue:
	blk_cleanup_queue(queue);
out_free_idr:
	idr_remove(&zram_index_idr, device_id);
out_free_dev:
	kfree(zram);
	return ret;
}

static int zram_remove(struct zram *zram)
{
	struct block_device *bdev;
	bool claimed;

	bdev = bdget_disk(zram->disk, 0);
	if (!bdev)
		return -ENOMEM;

	mutex_lock(&bdev->bd_mutex);
	if (bdev->bd_openers) {
		mutex_unlock(&bdev->bd_mutex);
		bdput(bdev);
		return -EBUSY;
	}

	claimed = zram->claim;
	if (!claimed)
		zram->claim = true;
	mutex_unlock(&bdev->bd_mutex);

#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	stop_lru_writeback(zram);
	if (g_zram == zram)
		g_zram = NULL;
#endif
	zram_debugfs_unregister(zram);

	if (claimed) {
		/*
		 * If we were claimed by reset_store(), del_gendisk() will
		 * wait until reset_store() is done, so nothing need to do.
		 */
		;
	} else {
		/* Make sure all the pending I/O are finished */
		sync_blockdev(bdev);
		zram_reset_device(zram);
	}
	bdput(bdev);

	pr_info("Removed device: %s\n", zram->disk->disk_name);

	del_gendisk(zram->disk);
	blk_cleanup_queue(zram->disk->queue);

	/* del_gendisk drains pending reset_store */
	WARN_ON_ONCE(claimed && zram->claim);

	/*
	 * disksize_store() may be called in between zram_reset_device()
	 * and del_gendisk(), so run the last reset to avoid leaking
	 * anything allocated with disksize_store()
	 */
	zram_reset_device(zram);

	put_disk(zram->disk);
	kfree(zram);
	return 0;
}

/* zram-control sysfs attributes */

/*
 * NOTE: hot_add attribute is not the usual read-only sysfs attribute. In a
 * sense that reading from this file does alter the state of your system -- it
 * creates a new un-initialized zram device and returns back this device's
 * device_id (or an error code if it fails to create a new device).
 */
static ssize_t hot_add_show(struct class *class,
			struct class_attribute *attr,
			char *buf)
{
	int ret;

	mutex_lock(&zram_index_mutex);
	ret = zram_add();
	mutex_unlock(&zram_index_mutex);

	if (ret < 0)
		return ret;
	return scnprintf(buf, PAGE_SIZE, "%d\n", ret);
}
static struct class_attribute class_attr_hot_add =
	__ATTR(hot_add, 0400, hot_add_show, NULL);

static ssize_t hot_remove_store(struct class *class,
			struct class_attribute *attr,
			const char *buf,
			size_t count)
{
	struct zram *zram;
	int ret, dev_id;

	/* dev_id is gendisk->first_minor, which is `int' */
	ret = kstrtoint(buf, 10, &dev_id);
	if (ret)
		return ret;
	if (dev_id < 0)
		return -EINVAL;

	mutex_lock(&zram_index_mutex);

	zram = idr_find(&zram_index_idr, dev_id);
	if (zram) {
		ret = zram_remove(zram);
		if (!ret)
			idr_remove(&zram_index_idr, dev_id);
	} else {
		ret = -ENODEV;
	}

	mutex_unlock(&zram_index_mutex);
	return ret ? ret : count;
}
static CLASS_ATTR_WO(hot_remove);

static struct attribute *zram_control_class_attrs[] = {
	&class_attr_hot_add.attr,
	&class_attr_hot_remove.attr,
	NULL,
};
ATTRIBUTE_GROUPS(zram_control_class);

static struct class zram_control_class = {
	.name		= "zram-control",
	.class_groups	= zram_control_class_groups,
};

static int zram_remove_cb(int id, void *ptr, void *data)
{
	WARN_ON_ONCE(zram_remove(ptr));
	return 0;
}

static void destroy_devices(void)
{
	class_unregister(&zram_control_class);
	idr_for_each(&zram_index_idr, &zram_remove_cb, NULL);
	zram_debugfs_destroy();
	idr_destroy(&zram_index_idr);
	unregister_blkdev(zram_major, "zram");
	cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
}

static int __init zram_init(void)
{
	struct zram_table_entry zram_te;
	int ret;

	BUILD_BUG_ON(__NR_ZRAM_PAGEFLAGS > sizeof(zram_te.flags) * 8);

	ret = cpuhp_setup_state_multi(CPUHP_ZCOMP_PREPARE, "block/zram:prepare",
				      zcomp_cpu_up_prepare, zcomp_cpu_dead);
	if (ret < 0)
		return ret;

	ret = class_register(&zram_control_class);
	if (ret) {
		pr_err("Unable to register zram-control class\n");
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return ret;
	}

	zram_debugfs_create();
	zram_major = register_blkdev(0, "zram");
	if (zram_major <= 0) {
		pr_err("Unable to get major number\n");
		class_unregister(&zram_control_class);
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return -EBUSY;
	}

	while (num_devices != 0) {
		mutex_lock(&zram_index_mutex);
		ret = zram_add();
		mutex_unlock(&zram_index_mutex);
		if (ret < 0)
			goto out_error;
		num_devices--;
	}

#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	am_app_launch_notifier_register(&zram_app_launch_nb);
#endif
	return 0;

out_error:
	destroy_devices();
	return ret;
}

static void __exit zram_exit(void)
{
	destroy_devices();
#ifdef CONFIG_ZRAM_LRU_WRITEBACK
	am_app_launch_notifier_unregister(&zram_app_launch_nb);
#endif
}

module_init(zram_init);
module_exit(zram_exit);

module_param(num_devices, uint, 0);
MODULE_PARM_DESC(num_devices, "Number of pre-created zram devices");

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Nitin Gupta <ngupta@vflare.org>");
MODULE_DESCRIPTION("Compressed RAM Block Device");
