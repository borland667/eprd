/*
 * eprd : An ramdisk that is eventually persistent on disk
 *
 * Partly based up-on sbd and the loop driver.
 * Redistributable under the terms of the GNU GPL.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/crypto.h>
#include <linux/err.h>
#include <linux/scatterlist.h>
#include <linux/workqueue.h>
#include <linux/rbtree.h>
#include <linux/miscdevice.h>
#include <linux/delay.h>
#include <linux/falloc.h>
#include <linux/kthread.h>
#include <linux/version.h>

#include "eprd.h"

#define TRUE 1
#define FALSE 0
#define EPRD_VERSION "0.4.2"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mark Ruijter");

LIST_HEAD(device_list);
static char *zeroblock;

#define LOCK 1
#define NOLOCK 0

/*
 * The internal representation of our device.
 */
static struct eprd_device *device = NULL;
static char *devicenames;
static struct mutex ioctl_mutex;

/*
 * The device operations structure.
 */
static struct block_device_operations eprd_ops = {
	.owner = THIS_MODULE,
};

struct rbdata *eprd_search(struct rbtree *tree, unsigned long blocknr)
{
	struct rb_node *node = tree->rbtree_root.rb_node;
	unsigned long flags;

	spin_lock_irqsave(&tree->rbtree_lock, flags);
	while (node) {
		struct rbdata *data = container_of(node, struct rbdata, node);
		if (blocknr < data->blocknr)
			node = node->rb_left;
		else if (blocknr > data->blocknr)
			node = node->rb_right;
		else {
			spin_unlock_irqrestore(&tree->rbtree_lock, flags);
			return data;
		}
	}
	spin_unlock_irqrestore(&tree->rbtree_lock, flags);
	return NULL;
}

int eprd_insert(struct rbtree *tree, struct rbdata *data)
{
	int ret = TRUE;
	unsigned long flags;
	struct rb_node **new = &(tree->rbtree_root.rb_node), *parent = NULL;

	spin_lock_irqsave(&tree->rbtree_lock, flags);
/* Figure out where to put new node */
	while (*new) {
		struct rbdata *this = container_of(*new, struct rbdata, node);
		parent = *new;
		if (data->blocknr < this->blocknr)
			new = &((*new)->rb_left);
		else if (data->blocknr > this->blocknr)
			new = &((*new)->rb_right);
		else {
			ret = FALSE;
			break;
		}
	}
	if (ret == TRUE) {
/* Add new node and rebalance tree. */
		rb_link_node(&data->node, parent, new);
		rb_insert_color(&data->node, &tree->rbtree_root);
	}
	spin_unlock_irqrestore(&tree->rbtree_lock, flags);
	return ret;
}

void eprd_erase(struct rbtree *tree, unsigned long blocknr, bool lock)
{
	struct rbdata *data = eprd_search(tree, blocknr);
	unsigned long flags = 0;

	if (lock)
		spin_lock_irqsave(&tree->rbtree_lock, flags);
	if (data) {
		rb_erase(&data->node, &tree->rbtree_root);
		kfree(data);
	}
	if (lock)
		spin_unlock_irqrestore(&tree->rbtree_lock, flags);
}

int clean_cache(struct eprd_device *dev)
{
	struct rbdata *data;
	struct rb_node *cur;
	struct rb_node *next;
	unsigned int try = 1;
	unsigned long low_water;
	unsigned long high_water;
	int ret = 0;
	unsigned long threshold;

	low_water = dev->cachesize / BLKSIZE / 3 * 2;
	high_water = dev->cachesize / BLKSIZE;

/* Require that clean_cache purges at least 10% of the entries in
   cache. Whenever we fail to purge 10% we return -1.
   As a result the cache is then completely written and erased. */

	threshold = high_water / 20;

	while (dev->cacheentries > low_water && try < 32) {
		cur = rb_first(&dev->rb_tree.rbtree_root);
		while (cur) {
			data = rb_entry(cur, struct rbdata, node);
			next = rb_next(cur);
			if (data->hitcount < try) {
				eprd_erase(&dev->rb_tree, data->blocknr, LOCK);
				dev->cacheentries--;
				if (1 == atomic_read(&dev->wcachelock)
				    && dev->cacheentries + threshold <
				    high_water)
					goto end_exit;
			}
			cur = next;
		}
		try *= 2;
	}
      end_exit:
	if (dev->cacheentries > high_water - threshold)
		ret = -1;
	return ret;
}

void write_valid(struct eprd_device *dev, struct rbdata *data)
{
	int block_start;
	int block_end;
	int bits = 1;
	int bitscount = 0;

	if (is_continues((data->valid & 255), &block_start, &block_end)) {
		eprd_file_write(dev->eprd_dta_file, data->data + block_start,
				block_end - block_start,
				(data->blocknr * BLKSIZE) + block_start);
		return;
	}
	for (bitscount = 0; bitscount < 8; bitscount++) {
		block_start = 512 * bitscount;
		block_end = block_start + 512;
		if (0 != ((data->valid & 255) & bits)) {
			eprd_file_write(dev->eprd_dta_file,
					data->data + block_start,
					block_end - block_start,
					(data->blocknr * BLKSIZE) +
					block_start);
		}
		bits <<= 1;
	}
}

void write_cache(struct eprd_device *dev)
{
	struct rbdata *data;
	struct rb_node *n;
	n = rb_first(&dev->rb_tree.rbtree_root);
	while (n) {
		data = rb_entry(n, struct rbdata, node);
		if (data->dirty) {
			write_valid(dev, data);
			data->dirty = 0;
		}
		n = rb_next(n);
	}
}

void erase_cache(struct eprd_device *dev)
{
	struct rbdata *data;
	struct rb_node *cur;

	write_cache(dev);
	cur = rb_first(&dev->rb_tree.rbtree_root);
	if (!cur)
		return;
	do {
		data = rb_entry(cur, struct rbdata, node);
		eprd_erase(&dev->rb_tree, data->blocknr, LOCK);
		// When you erase your first, goto new first
		cur = rb_first(&dev->rb_tree.rbtree_root);
	} while (cur);
	dev->cacheentries = 0;
}

file_info_t *open_file(char *filename, int flags, mode_t mode)
{
	file_info_t *fileinfo;
	fileinfo = kmalloc(sizeof(file_info_t), GFP_KERNEL);
	fileinfo->fp = filp_open(filename, flags, mode);
	if (fileinfo->fp == NULL) {
		printk(KERN_ALERT "filp_open error!!.\n");
		kfree(fileinfo);
		return NULL;
	}
	fileinfo->fs = get_ds();
	return fileinfo;
}

void close_file(file_info_t * fileinfo)
{
	filp_close(fileinfo->fp, NULL);
	kfree(fileinfo);
}

void write_file(file_info_t * fileinfo, char *buf, int buflen, loff_t offset)
{
	mm_segment_t old_fs;
	int p;

	old_fs = get_fs();
	set_fs(fileinfo->fs);
	fileinfo->fp->f_pos = offset;
	p = fileinfo->fp->f_op->write(fileinfo->fp, buf, buflen,
				      &fileinfo->fp->f_pos);
	set_fs(old_fs);
}

void read_file(file_info_t * fileinfo, char *buf, int buflen, loff_t offset)
{
	mm_segment_t old_fs;
	int p;

	old_fs = get_fs();
	set_fs(fileinfo->fs);
	p = fileinfo->fp->f_op->read(fileinfo->fp, buf, buflen, &offset);
	set_fs(old_fs);
}

/**
 * eprd_file_write - helper for writing data
 */
static int eprd_file_write(struct file *file,
			   u8 * buf, const int len, loff_t pos)
{
	ssize_t bw;
	mm_segment_t old_fs = get_fs();

	set_fs(get_ds());
	bw = file->f_op->write(file, buf, len, &pos);
	set_fs(old_fs);
	if (likely(bw == len))
		return 0;
	EPRDERR("Write error at byte offset %llu, length %i.\n",
		(unsigned long long)pos, len);
	if (bw >= 0)
		bw = -EIO;
	return bw;
}

/**
 * eprd_file_read - helper for reading data
 */
static int eprd_file_read(struct file *file,
			  u8 * buf, const int len, loff_t pos)
{
	ssize_t bw;
	mm_segment_t old_fs = get_fs();

	set_fs(get_ds());
	bw = file->f_op->read(file, buf, len, &pos);
	set_fs(old_fs);
	if (likely(bw == len))
		return 0;
	EPRDERR("Read error at byte offset %llu, length %i.\n",
		(unsigned long long)pos, len);
	if (bw >= 0)
		bw = -EIO;
	return bw;
}

int eprd_sync(struct eprd_device *dev)
{
	int ret;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,0)
	ret = vfs_fsync(dev->eprd_dta_file, 0);
#else
	ret =
	    vfs_fsync(dev->eprd_dta_file, dev->eprd_dta_file->f_path.dentry, 0);
#endif
	return ret;
}

void *as_sprintf(const char *fmt, ...)
{
	/* Guess we need no more than 100 bytes. */
	int n, size = 100;
	void *p;
	va_list ap;
	p = kmalloc(size, GFP_ATOMIC);
	while (1) {
		/* Try to print in the allocated space. */
		va_start(ap, fmt);
		n = vsnprintf(p, size, fmt, ap);
		va_end(ap);
		/* If that worked, return the string. */
		if (n > -1 && n < size)
			return p;
		/* Else try again with more space. */
		if (n > -1)	/* glibc 2.1 */
			size = n + 1;	/* precisely what is needed */
		else		/* glibc 2.0 */
			size *= 2;	/* twice the old size */
		p = krealloc(p, size, GFP_ATOMIC);
	}
}

void update_cacheentries(struct eprd_device *dev)
{
	unsigned long high_water;

	high_water = dev->cachesize / BLKSIZE;

	if (0 == dev->cachesize)
		return;
	dev->cacheentries++;
	if (dev->cacheentries >= high_water) {
		if (0 == atomic_read(&dev->flush)) {
			atomic_set(&dev->flush, 1);
			wake_up(&dev->cache_event);
		}
	}
}

/* return 1 is the block is continues
   When the block is continues :
   set block_start to the offset at which data starts
   set block_end to the end of valid data */
int is_continues(u_char valid, int *block_start, int *block_end)
{
	int bits = 1;
	int bitscount;
	int result;

	for (bitscount = 0; bitscount < 8; bitscount++) {
		result = valid & bits;
		if (result != 0)
			break;
		bits <<= 1;
	}
	*block_start = (bitscount) * 512;
	for (; bitscount < 8; bitscount++) {
		result = valid & bits;
		if (result == 0)
			break;
		bits <<= 1;
	}
	bits >>= 1;
	*block_end = (bitscount) * 512;
	if (bitscount == 8)
		return 1;

	bits <<= 1;
	for (; bitscount < 9; bitscount++) {
		result = valid & bits;
		if (result != 0)
			break;
		bits <<= 1;
	}
	if (bitscount == 9)
		return 1;
	return 0;
}

/* Voodoo to determine if a block of 512 bytes is valid in the cache
   Only valid blocks should be written or read
   char rbdata->valid contains this information
   A bit is set for each block that is valid
   4k/512 = 8 blocks and we therefore use 8 bits
   For example : 
   block_offset = 0 and nbytes = 4096 will return 1111 1111
   block_offset = 1 and nbytes = 1 with return 0000 0001
   block_offset = 1 and nbytes = 512 with return 0000 0011
   block_offset = 0 and nbytes = 512 with return 0000 0001
   block_offset = 0 and nbytes = 1024 with return 0000 0011
*/
void set_valid(u_char * valid, unsigned int block_offset, unsigned long nbytes,
	       int caller)
{
	int res, max, start, stop;

	if ((nbytes > BLKSIZE) || (block_offset > BLKSIZE - 1) || (nbytes == 0)) {
		EPRDCRIT
		    ("set_valid wrong input : nbytes %lu, block_offset %u caller %u",
		     nbytes, block_offset, caller);
		return;
	}
	start = block_offset / KERNEL_SECTORSIZE;
	max = nbytes + block_offset;
	if (max > BLKSIZE)
		max = BLKSIZE;
	stop = max;
	stop = stop / KERNEL_SECTORSIZE;
	if (stop * KERNEL_SECTORSIZE < max)
		stop++;
	if (stop == 0)
		stop = 1;
	res = ((2 << (stop - 1)) - 1) ^ ((2 << (start - 1)) - 1);
	if (res > 255)
		res ^= 511;
	if (res < 0) {
		res += 256;
		res ^= 255;
	}
	*valid = res & 255;
}

static int eprd_write(struct eprd_device *dev, char *buffer,
		      unsigned long nbytes, unsigned long blocknr,
		      unsigned int block_offset, loff_t offset)
{
	struct rbdata *rbtreedata;
	unsigned long done = 0;
	int exist = 0;
	u_char valid = 0;

	while (done < nbytes) {
		exist = 0;
		rbtreedata = eprd_search(&dev->rb_tree, blocknr);
		if (NULL == rbtreedata) {
			rbtreedata = kzalloc(sizeof(struct rbdata), GFP_KERNEL);
			if (!rbtreedata)
				return -ENOMEM;
			rbtreedata->blocknr = blocknr;
		} else {
			if (rbtreedata->hitcount < 32)
				rbtreedata->hitcount++;
			exist = 1;
		}
		set_valid(&valid, block_offset, nbytes - done, 1);
		rbtreedata->valid |= valid;
		if (nbytes + block_offset <= BLKSIZE) {
			memcpy(rbtreedata->data + block_offset,
			       buffer + done, nbytes - done);
			done = nbytes;
		} else {
			memcpy(rbtreedata->data + block_offset,
			       buffer + done, BLKSIZE - block_offset - done);
			done = BLKSIZE - block_offset;
			blocknr++;
			block_offset = 0;
		}
		rbtreedata->dirty = 1;
		if (!exist) {
			update_cacheentries(dev);
			eprd_insert(&dev->rb_tree, rbtreedata);
		}
/* When we have a cache size and commit_interval == 0 we write everything to disk
   before we leave this routine. Since we have a mutex lock it is save to flag it clean
   at this point and do the write in one pass below */
		if (dev->commit_interval == 0 && dev->cachesize != 0)
			rbtreedata->dirty = 0;
	}
	if (dev->commit_interval == 0 && dev->cachesize != 0) {
		eprd_file_write(dev->eprd_dta_file, buffer, nbytes, offset);
	}
	return 0;
}

static int eprd_read(struct eprd_device *dev, char *buffer,
		     unsigned long nbytes, unsigned long blocknr,
		     unsigned int block_offset)
{
	struct rbdata *rbtreedata;
	unsigned long done = 0;
	int block_start = 0;
	int block_end = 0;

	while (done < nbytes) {
		rbtreedata = eprd_search(&dev->rb_tree, blocknr);
		if (0 != dev->cachesize && NULL == rbtreedata) {
			update_cacheentries(dev);
			rbtreedata = kzalloc(sizeof(struct rbdata), GFP_KERNEL);
			if (!rbtreedata)
				return -ENOMEM;
			rbtreedata->blocknr = blocknr;
		      do_read:
			rbtreedata->valid = 255;	/* We will read a full 4k block */
			eprd_file_read(dev->eprd_dta_file,
				       rbtreedata->data, BLKSIZE,
				       blocknr * BLKSIZE);
			if (nbytes + block_offset <= BLKSIZE) {
				memcpy(buffer,
				       rbtreedata->data + block_offset, nbytes);
				done = nbytes;
			} else {
				memcpy(buffer,
				       rbtreedata->data + block_offset,
				       BLKSIZE - block_offset);
				done += BLKSIZE - block_offset;
				blocknr++;
				block_offset = 0;
			}
			if (rbtreedata->hitcount == 0)
				eprd_insert(&dev->rb_tree, rbtreedata);
			continue;
		}
		if (NULL != rbtreedata) {
			if (rbtreedata->hitcount < 32)
				rbtreedata->hitcount++;
			if (0 != dev->cachesize) {
				if (!is_continues
				    (rbtreedata->valid, &block_start,
				     &block_end)) {
					if (rbtreedata->dirty) {
						write_valid(dev, rbtreedata);
						rbtreedata->dirty = 0;
					}
					goto do_read;
				}
				if (block_offset < block_start) {
					if (rbtreedata->dirty) {
						write_valid(dev, rbtreedata);
						rbtreedata->dirty = 0;
					}
					goto do_read;
				}
			}
			if (nbytes + block_offset <= BLKSIZE) {
				if (0 != dev->cachesize
				    && nbytes + block_offset > block_end) {
					if (rbtreedata->dirty) {
						write_valid(dev, rbtreedata);
						rbtreedata->dirty = 0;
					}
					goto do_read;
				}
				memcpy(buffer + done,
				       rbtreedata->data + block_offset,
				       nbytes - done);
				done = nbytes;
			} else {
				if (0 != dev->cachesize
				    && BLKSIZE - block_offset - done >
				    block_end) {
					if (rbtreedata->dirty) {
						write_valid(dev, rbtreedata);
						rbtreedata->dirty = 0;
					}
					goto do_read;
				}
				memcpy(buffer + done,
				       rbtreedata->data + block_offset,
				       BLKSIZE - block_offset - done);
				done = BLKSIZE - block_offset;
				blocknr++;
			}
		} else {
			if (nbytes + block_offset <= BLKSIZE) {
				memset(buffer + done, 0, nbytes - done);
				done = nbytes;
			} else {
				memset(buffer + done, 0,
				       BLKSIZE - block_offset - done);
				done = BLKSIZE - block_offset;
				blocknr++;
				block_offset = 0;
			}
		}
	}
	return 0;
}

/*
 * Grab first pending buffer
 */
static struct bio *eprd_get_bio(struct eprd_device *dev)
{
	return bio_list_pop(&dev->eprd_bio_list);
}

void determine_iotype(struct eprd_device *dev, unsigned long blocknr)
{
	int ioswitch = 0;
	int old_iotype = dev->iotype;

	if (dev->cachesize == 0)
		return;
	if (blocknr >= dev->lastblocknr - 1 && blocknr <= dev->lastblocknr + 1) {
		ioswitch = 1;
	}
	if (ioswitch && dev->insequence < 10)
		dev->insequence++;
	else {
		if (dev->insequence > 0)
			dev->insequence--;
	}
	if (dev->insequence > 5)
		dev->iotype = SEQUENTIAL;
	else
		dev->iotype = RANDOM;
	if (old_iotype != dev->iotype)
		erase_cache(dev);
	dev->lastblocknr = blocknr;
}

static int eprd_do_bio(struct eprd_device *dev, struct bio *bio)
{
	loff_t offset;
	struct bio_vec *bvec;
	int i, ret = 0;
	unsigned long blocknr;
	unsigned int block_offset;
	struct file *file;
	char *buffer;

	atomic_set(&dev->wcachelock, 1);
	mutex_lock(&dev->cachelock);
	atomic_set(&dev->wcachelock, 0);
	offset = ((loff_t) bio->bi_sector << 9);
	blocknr = offset / BLKSIZE;
	block_offset = (offset - (blocknr * BLKSIZE));
	file = dev->eprd_dta_file;

	if (0 != dev->cachesize) {

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,0,0)
		bool barrier = bio_rw_flagged(bio, BIO_RW_BARRIER);
#endif
		if (bio_rw(bio) == WRITE) {
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,0,0)
			if (barrier) {
#else
			if (bio->bi_rw & REQ_FLUSH) {
#endif
				if (!dev->barrier) {
					ret = -EOPNOTSUPP;
					goto out;
				}
				write_cache(dev);
				ret = eprd_sync(dev);
				if (unlikely(ret && ret != -EINVAL)) {
					ret = -EIO;
					goto out;
				}
			}
		}
	}

	bio_for_each_segment(bvec, bio, i) {
		determine_iotype(dev, blocknr);
		buffer = kmap(bvec->bv_page);
		if (bio_rw(bio) == WRITE) {
			if (dev->iotype == RANDOM) {
				ret =
				    eprd_write(dev,
					       buffer + bvec->bv_offset,
					       bvec->bv_len, blocknr,
					       block_offset, offset);
			} else {
				ret =
				    eprd_file_write(dev->eprd_dta_file,
						    buffer + bvec->bv_offset,
						    bvec->bv_len, offset);
			}
		} else {
			if (dev->iotype == RANDOM) {
				ret =
				    eprd_read(dev,
					      buffer + bvec->bv_offset,
					      bvec->bv_len, blocknr,
					      block_offset);
			} else {
				ret =
				    eprd_file_read(dev->eprd_dta_file,
						   buffer + bvec->bv_offset,
						   bvec->bv_len, offset);
			}
		}
		kunmap(bvec->bv_page);
		if (ret < 0)
			break;
		offset += bvec->bv_len;
		blocknr = offset / BLKSIZE;
		block_offset = (offset - (blocknr * BLKSIZE));
	}
      out:
	mutex_unlock(&dev->cachelock);
	return ret;
}

static inline void eprd_handle_bio(struct eprd_device *dev, struct bio *bio)
{
	int ret;
	ret = eprd_do_bio(dev, bio);
	bio_endio(bio, ret);
}

void cache_manager(struct work_struct *work)
{
	cache_worker_t *cache;
	unsigned long high_water;
	int ret = 0;

	cache = (cache_worker_t *) work;
	high_water = cache->device->cachesize / BLKSIZE;
	while (1) {
		wait_event_interruptible(cache->device->cache_event,
					 1 ==
					 atomic_read(&cache->device->flush));
		if (cache->device->stop)
			break;
		mutex_lock(&cache->device->cachelock);
		if (0 != cache->device->cachesize) {
			if (1 == atomic_read(&cache->device->commit)) {
				write_cache(cache->device);
				atomic_set(&cache->device->commit, 0);
			}
			if (cache->device->cacheentries > high_water) {
				write_cache(cache->device);
				ret = clean_cache(cache->device);
			}
			if (ret != 0)
				erase_cache(cache->device);
		}
		atomic_set(&cache->device->flush, 0);
		mutex_unlock(&cache->device->cachelock);
	}
}

static int eprd_thread(void *data)
{
	struct eprd_device *dev = data;
	struct bio *bio;

	set_user_nice(current, -20);

	while (!kthread_should_stop() || !bio_list_empty(&dev->eprd_bio_list)) {

		wait_event_interruptible(dev->eprd_event,
					 !bio_list_empty(&dev->eprd_bio_list) ||
					 kthread_should_stop());
		if (bio_list_empty(&dev->eprd_bio_list))
			continue;
		spin_lock_irq(&dev->lock);
		bio = eprd_get_bio(dev);
		spin_unlock_irq(&dev->lock);

		BUG_ON(!bio);
		eprd_handle_bio(dev, bio);
	}

	return 0;
}

static void eprd_add_bio(struct eprd_device *dev, struct bio *bio)
{
	bio_list_add(&dev->eprd_bio_list, bio);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
static int eprd_make_request(struct request_queue *q, struct bio *old_bio)
#else
static void eprd_make_request(struct request_queue *q, struct bio *old_bio)
#endif
{
	struct eprd_device *dev = q->queuedata;
	int rw = bio_rw(old_bio);

	if (rw == READA)
		rw = READ;

	BUG_ON(!dev || (rw != READ && rw != WRITE));
	spin_lock_irq(&dev->lock);
	if (!dev->active)
		goto out;
	eprd_add_bio(dev, old_bio);
	wake_up(&dev->eprd_event);
	spin_unlock_irq(&dev->lock);
	goto end_return;

      out:
	spin_unlock_irq(&dev->lock);
	bio_io_error(old_bio);

      end_return:
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,3,0)
	return 0;
#else
	return;
#endif
}

void init_devicenames(void)
{
	int count;
/* Allow max 24 devices to be configured */
	devicenames = kmalloc(sizeof(char) * 26, GFP_KERNEL);

	for (count = 0; count <= 25; count++) {
		devicenames[count] = 97 + count;
	}
}

void release_devicename(char *devicename)
{
	int pos;
	char d;

	if (NULL == devicename)
		return;
	d = devicename[4];	/*eprdN */
/* Restore the char in devicenames */
	pos = d - 97;
	devicenames[pos] = d;
	kfree(devicename);
}

char *reserve_devicename(unsigned int *devnr)
{
	char devicenr;
	char *retname;
	int count;
	for (count = 0; count <= 25; count++) {
		devicenr = devicenames[count];
		if (devicenr != 0)
			break;
	}
	if (0 == devicenr) {
		EPRDERR("Maximum number of devices exceeded");
		return NULL;
	}
	retname = as_sprintf("eprd%c", devicenr);
	*devnr = count;
	devicenames[count] = 0;
	return retname;
}

static int load_data(struct eprd_device *dev)
{
	int ret = -1;
	unsigned long backfilesize;
	unsigned long curblock;
	struct rbdata *rbtreedata;

	backfilesize =
	    KERNEL_SECTORSIZE * eprd_get_size(0, 0, dev->eprd_dta_file);
	if (dev->size != backfilesize) {
		EPRDERR("ABORT dev->size = %lu, backfilesize = %lu", dev->size,
			backfilesize);
		return ret;
	}
	for (curblock = 0; curblock < dev->size; curblock += BLKSIZE) {
		rbtreedata = kzalloc(sizeof(struct rbdata), GFP_KERNEL);
		if (!rbtreedata)
			return -ENOMEM;
		rbtreedata->blocknr = curblock / BLKSIZE;
		eprd_file_read(dev->eprd_dta_file, rbtreedata->data,
			       BLKSIZE, curblock);
		if (0 != memcmp(zeroblock, rbtreedata->data, BLKSIZE))
			eprd_insert(&dev->rb_tree, rbtreedata);
		else {
			kfree(rbtreedata);
		}
	}
	ret = 0;
	return ret;
}

void flush_timer_expired(unsigned long q)
{
	struct eprd_device *dev = (struct eprd_device *)q;

	if (0 == atomic_read(&dev->flush)) {
		atomic_set(&dev->flush, 1);
		atomic_set(&dev->commit, 1);
		wake_up(&dev->cache_event);
	}
	dev->flush_timer.expires =
	    jiffies + msecs_to_jiffies(dev->commit_interval * 1000);
	add_timer(&dev->flush_timer);
}

static int eprd_register(struct eprd_device *dev)
{
	int devnr;
	int ret = 0;
	cache_worker_t *cacheworker;

	dev->devname = reserve_devicename(&devnr);
	if (NULL == dev->devname)
		return -1;
	dev->active = 1;
/* Barriers can not be used when we work in ram only */
	if (dev->cachesize == 0)
		dev->barrier = 0;
	if (0 == dev->logical_block_size)
		dev->logical_block_size = 512;
	dev->nsectors = dev->size / dev->logical_block_size;
	dev->size = dev->nsectors * dev->logical_block_size;
	EPRDINFO("%s size : %lu", dev->devname, dev->size);
	spin_lock_init(&dev->lock);
	bio_list_init(&dev->eprd_bio_list);
	dev->rqueue = blk_alloc_queue(GFP_KERNEL);
	if (!dev->rqueue) {
		ret = -ENOMEM;
		goto out;
	}
	init_waitqueue_head(&dev->eprd_event);
	init_waitqueue_head(&dev->cache_event);
	dev->cacheentries = 0;
	dev->stop = 0;
	dev->iotype = RANDOM;
	dev->rb_tree.rbtree_root = RB_ROOT;
	atomic_set(&dev->flush, 0);
	atomic_set(&dev->commit, 0);
	atomic_set(&dev->wcachelock, 0);
	spin_lock_init(&dev->rb_tree.rbtree_lock);
	EPRDINFO("cache size : %lu", dev->cachesize);
	if (0 == dev->cachesize) {
		ret = load_data(dev);
		if (ret != 0)
			goto out;
	}
	/*
	 * Get a request queue.
	 */
	mutex_init(&dev->eprd_ctl_mutex);
	mutex_init(&dev->cachelock);
	/*
	 * set queue make_request_fn, and add limits based on lower level
	 * device
	 */
	blk_queue_make_request(dev->rqueue, eprd_make_request);
	dev->rqueue->queuedata = (void *)dev;

	/* Tell the block layer that we are not a rotational device
	 */
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, dev->rqueue);
	blk_queue_logical_block_size(dev->rqueue, dev->logical_block_size);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,0,0)
	if (dev->barrier)
		blk_queue_flush(dev->rqueue, REQ_FLUSH);
#endif
	/*
	 * Get registered.
	 */
	dev->major_num = register_blkdev(0, dev->devname);
	if (dev->major_num <= 0) {
		printk(KERN_WARNING "eprd: unable to get major number\n");
		goto out;
	}
	/*
	 * And the gendisk structure.
	 */
	dev->gd = alloc_disk(16);
	if (!dev->gd)
		goto out_unregister;
	dev->gd->major = dev->major_num;
	dev->gd->first_minor = 0;
	dev->gd->fops = &eprd_ops;
	dev->gd->private_data = &dev;
	strcpy(dev->gd->disk_name, dev->devname);
	set_capacity(dev->gd, dev->nsectors * (dev->logical_block_size / 512));
	dev->gd->queue = dev->rqueue;
	dev->eprd_thread = kthread_create(eprd_thread, dev, dev->devname);
	if (IS_ERR(dev->eprd_thread)) {
		EPRDERR("Failed to create kernel thread");
		ret = PTR_ERR(dev->eprd_thread);
		goto out_unregister;
	}
	wake_up_process(dev->eprd_thread);
	cacheworker = kmalloc(sizeof(cache_worker_t), GFP_KERNEL);
	if (!cacheworker) {
		EPRDERR("Failed to allocate memory for cacheworker");
		goto out_unregister;
	}
	cacheworker->device = dev;
	dev->wqueue = create_workqueue(dev->devname);
	INIT_WORK((struct work_struct *)cacheworker, cache_manager);
	queue_work(dev->wqueue, (struct work_struct *)cacheworker);
	init_timer(&dev->flush_timer);
	dev->flush_timer.data = (unsigned long)dev;
	dev->flush_timer.function = flush_timer_expired;
	if (0 != dev->commit_interval && 0 != dev->cachesize) {
		dev->flush_timer.expires =
		    jiffies + msecs_to_jiffies(dev->commit_interval * 1000);
		add_timer(&dev->flush_timer);
	}
	add_disk(dev->gd);
	return ret;

      out_unregister:
	unregister_blkdev(dev->major_num, dev->devname);
      out:
	return ret;
}

static loff_t eprd_get_size(loff_t offset, loff_t sizelimit, struct file *file)
{
	loff_t size, loopsize;

	/* Compute loopsize in bytes */
	size = i_size_read(file->f_mapping->host);
	loopsize = size - offset;
	/* offset is beyond i_size, wierd but possible */
	if (loopsize < 0)
		return 0;

	if (sizelimit > 0 && sizelimit < loopsize)
		loopsize = sizelimit;
	/*
	 * Unfortunately, if we want to do I/O on the device,
	 * the number of 512-byte sectors has to fit into a sector_t.
	 */
	return loopsize >> 9;
}

static int eprd_set_dtafd(struct eprd_device *eprd, unsigned int arg)
{
	int error = -EBADF;
	struct file *file;

	file = fget(arg);
	if (!file)
		goto out;
	if (!(file->f_mode & FMODE_WRITE)) {
		error = -EPERM;
		goto out;
	}
	error = 0;
	eprd->eprd_dta_file = file;
      out:
	return error;
}

/* Return the number of devices in nr
   and return the last eprd_device */
struct eprd_device *device_nr(int *nr)
{
	struct list_head *pos, *q;
	struct eprd_device *ret = NULL;

	*nr = 0;
	list_for_each_safe(pos, q, &device_list) {
		ret = list_entry(pos, struct eprd_device, list);
		*nr += 1;
	}
	return ret;
}

static int eprd_device_count(void)
{
	struct list_head *pos, *q;
	int count = 0;

	list_for_each_safe(pos, q, &device_list) {
		count++;
	}
	return count;
}

void eprd_deregister(struct eprd_device *eprd)
{
	if (eprd->active) {
		EPRDINFO("Deregister device %s", eprd->devname);
		eprd->stop = 1;
		atomic_set(&eprd->flush, 1);
		wake_up(&eprd->cache_event);
		destroy_workqueue(eprd->wqueue);
		kthread_stop(eprd->eprd_thread);
		erase_cache(eprd);
		mutex_destroy(&eprd->eprd_ctl_mutex);
		mutex_destroy(&eprd->cachelock);
		del_timer_sync(&eprd->flush_timer);
		del_gendisk(eprd->gd);
		put_disk(eprd->gd);
		blk_cleanup_queue(eprd->rqueue);
		unregister_blkdev(eprd->major_num, eprd->devname);
		list_del(&eprd->list);
		release_devicename(eprd->devname);
		eprd_sync(eprd);
		if (NULL != eprd->eprd_dta_file)
			filp_close(eprd->eprd_dta_file, NULL);
		kfree(eprd);
		eprd = NULL;
	}
}

void del_eprd_device(char *devicename)
{
	struct eprd_device *eprd, *next;

	list_for_each_entry_safe(eprd, next, &device_list, list) {
		if (NULL != eprd->devname) {
			if (NULL != strstr(devicename, eprd->devname)) {
				eprd_deregister(eprd);
			}
		}
	}
}

static long eprd_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct eprd_device *eprd = NULL;
	struct eprd_device *eprdnew;
	int current_device_nr;
	int err = 0;
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	mutex_lock(&ioctl_mutex);
	if (cmd != EPRD_INIT)
		eprd = device_nr(&current_device_nr);
	if (NULL == eprd && cmd != EPRD_INIT) {
		err = -EBADSLT;
		goto end_error;
	}
	switch (cmd) {
	case EPRD_INIT:
		eprdnew = kzalloc(sizeof(struct eprd_device), GFP_KERNEL);
		if (NULL == eprdnew)
			return -ENOMEM;
		if (0 == eprd_device_count()) {
			device = eprdnew;
		}
		list_add_tail(&eprdnew->list, &device_list);
		break;
	case EPRD_SET_DTAFD:
		err = -EEXIST;
		if (0 != eprd->eprd_device_number)
			break;
		err = eprd_set_dtafd(eprd, arg);
		break;
	case EPRD_SET_DEVSZ:
		err = -EEXIST;
		if (0 != eprd->eprd_device_number)
			break;
		err = 0;
		eprd->size = arg;
		break;
	case EPRD_CACHESIZE:
		err = -EEXIST;
		if (0 != eprd->eprd_device_number)
			break;
		err = 0;
		eprd->cachesize = arg;
		break;
	case EPRD_SET_SECTORSIZE:
		err = -EEXIST;
		if (0 != eprd->eprd_device_number)
			break;
		err = 0;
		eprd->logical_block_size = arg;
		EPRDINFO("sectorsize : %d", eprd->logical_block_size);
		break;
	case EPRD_SET_COMMIT_INTERVAL:
		err = -EEXIST;
		if (0 != eprd->eprd_device_number)
			break;
		err = 0;
		eprd->commit_interval = arg;
		break;
	case EPRD_BARRIER:
		err = -EEXIST;
		if (0 != eprd->eprd_device_number)
			break;
		err = 0;
		eprd->barrier = arg;
		if (eprd->barrier)
			EPRDINFO("barriers   : enabled");
		else
			EPRDINFO("barriers   : disabled");
		break;
	case EPRD_REGISTER:
		err = -EEXIST;
		if (0 != eprd->eprd_device_number)
			break;
		if (0 == eprd->size || 0 == eprd->eprd_dta_file) {
			EPRDERR("Insufficient parameters entered");
		} else {
			eprd->eprd_device_number = current_device_nr;
			err = eprd_register(eprd);
		}
		break;
	case EPRD_DEREGISTER:
		err = eprd_device_count();
		del_eprd_device((char *)arg);
		if (1 == err)
			device = NULL;
		err = 0;
		break;
	default:
		err = eprd->ioctl ? eprd->ioctl(eprd, cmd, arg) : -EINVAL;
	}
      end_error:
	mutex_unlock(&ioctl_mutex);
	return err;
}

static const struct file_operations _eprd_ctl_fops = {
	.open = nonseekable_open,
	.unlocked_ioctl = eprd_ioctl,
	.owner = THIS_MODULE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,3,0)
	.llseek = noop_llseek
#else
	.llseek = no_llseek
#endif
};

static struct miscdevice _eprd_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "eprdcontrol",
	.nodename = "/dev/eprdcontrol",
	.fops = &_eprd_ctl_fops
};

static int __init eprd_init(void)
{
	int r;
	/* First register out control device */

	EPRDINFO("version    : %s", EPRD_VERSION);
	r = misc_register(&_eprd_misc);
	if (r) {
		EPRDERR("misc_register failed for control device");
		return r;
	}
	/*
	 * Alloc our device names
	 */
	init_devicenames();
	mutex_init(&ioctl_mutex);
	zeroblock = kmalloc(BLKSIZE, GFP_KERNEL);
	memset(zeroblock, 0, BLKSIZE);
	return 0;
}

static void __exit eprd_exit(void)
{
	struct eprd_device *eprd, *next;

	list_for_each_entry_safe(eprd, next, &device_list, list)
	    eprd_deregister(eprd);

	if (misc_deregister(&_eprd_misc) < 0)
		EPRDERR("misc_deregister failed for eprd control device");
	kfree(devicenames);
	kfree(zeroblock);
	mutex_destroy(&ioctl_mutex);
}

module_init(eprd_init);
module_exit(eprd_exit);
