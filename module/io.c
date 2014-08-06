/**
 * io.c - IO processing core of WalB.
 *
 * Copyright(C) 2012, Cybozu Labs, Inc.
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/ratelimit.h>
#include <linux/printk.h>
#include <linux/time.h>
#include <linux/kmod.h>
#include "walb/logger.h"
#include "kern.h"
#include "io.h"
#include "bio_wrapper.h"
#include "bio_entry.h"
#include "treemap.h"
#include "worker.h"
#include "bio_util.h"
#include "pack_work.h"
#include "logpack.h"
#include "super.h"
#include "sysfs.h"
#include "pending_io.h"
#include "overlapped_io.h"

/*******************************************************************************
 * Static data definition.
 *******************************************************************************/

/**
 * A write pack.
 */
struct pack
{
	struct list_head list; /* list entry. */
	struct list_head biow_list; /* list head of bio_wrapper. */

	struct sector_data *logpack_header_sector;

	/* zero_flush or logpack header IO. */
	struct bio_entry *header_bioe;

	/* true if req_ent_list contains only a zero-size flush. */
	bool is_zero_flush_only;

	/* true if one or more bio(s) are flush request. */
	bool is_flush_contained;

	/* true if the header IO must flush request. */
	bool is_flush_header;

	/* true if submittion failed. */
	bool is_logpack_failed;
};

static atomic_t n_users_of_pack_cache_ = ATOMIC_INIT(0);
#define KMEM_CACHE_PACK_NAME "pack_cache"
struct kmem_cache *pack_cache_ = NULL;

/* All treemap(s) in this module will share a treemap memory manager. */
static atomic_t n_users_of_memory_manager_ = ATOMIC_INIT(0);
static struct treemap_memory_manager mmgr_;
#define TREE_NODE_CACHE_NAME "walb_iocore_bio_node_cache"
#define TREE_CELL_HEAD_CACHE_NAME "walb_iocore_bio_cell_head_cache"
#define TREE_CELL_CACHE_NAME "walb_iocore_bio_cell_cache"
#define N_ITEMS_IN_MEMPOOL (128 * 2) /* for pending data and overlapped data. */

/*******************************************************************************
 * Macros definition.
 *******************************************************************************/

#define WORKER_NAME_GC "walb_gc"

/*******************************************************************************
 * Static functions definition.
 *******************************************************************************/

/**
 * Check read-only mode.
 */
static inline bool is_read_only_mode(const struct iocore_data *iocored)
{
	ASSERT(iocored);
	return test_bit(IOCORE_STATE_READ_ONLY, &iocored->flags);
}

/**
 * Set read-only mode.
 */
static inline void set_read_only_mode(struct iocore_data *iocored)
{
	ASSERT(iocored);
	set_bit(IOCORE_STATE_READ_ONLY, &iocored->flags);
}

/*******************************************************************************
 * Static functions definition.
 *******************************************************************************/

/* bio_entry related. */
static void bio_entry_end_io(struct bio *bio, int error);
static struct bio_entry* create_bio_entry_by_clone(
	struct bio *bio, struct block_device *bdev,
	gfp_t gfp_mask, bool is_deep);
static struct bio_entry* create_bio_entry_by_clone_never_giveup(
	struct bio *bio, struct block_device *bdev,
	gfp_t gfp_mask, bool is_deep);
static void wait_for_bio_entry(struct bio_entry *bioe);

/* pack related. */
static struct pack* create_pack(gfp_t gfp_mask);
static struct pack* create_writepack(gfp_t gfp_mask, unsigned int pbs, u64 logpack_lsid);
static void destroy_pack(struct pack *pack);
static bool is_zero_flush_only(const struct pack *pack);
static bool is_pack_size_too_large(
	struct walb_logpack_header *lhead,
	unsigned int pbs, unsigned int max_logpack_pb,
	struct bio_wrapper *biow);
UNUSED static void print_pack(
	const char *level, struct pack *pack);
UNUSED static void print_pack_list(
	const char *level, struct list_head *wpack_list);
UNUSED static bool pack_contains_flush(const struct pack *pack);
static void get_wdev_and_iocored_from_work(
	struct walb_dev **pwdev, struct iocore_data **piocored,
	struct work_struct *work);

/* Workqueue tasks. */
static void task_submit_logpack_list(struct work_struct *work);
static void task_wait_for_logpack_list(struct work_struct *work);
static void task_wait_and_gc_read_bio_wrapper(struct work_struct *work);
static void task_submit_bio_wrapper_list(struct work_struct *work);
static void task_wait_for_bio_wrapper_list(struct work_struct *work);

/* Logpack GC */
static void run_gc_logpack_list(void *data);

/* Logpack related functions. */
static bool create_logpack_list(
	struct walb_dev *wdev, struct list_head *biow_list,
	struct list_head *pack_list);
static void submit_logpack_list(
	struct walb_dev *wdev, struct list_head *wpack_list);
static void logpack_calc_checksum(
	struct walb_logpack_header *lhead,
	unsigned int pbs, u32 salt, struct list_head *biow_list);
static void submit_logpack(
	struct walb_logpack_header *logh,
	struct list_head *biow_list, struct bio_entry **bioe_p,
	unsigned int pbs, bool is_flush, struct block_device *ldev,
	u64 ring_buffer_off, u64 ring_buffer_size,
	unsigned int chunk_sectors);
static void logpack_submit_header(
	struct walb_logpack_header *logh, struct bio_entry **bioe_p,
	unsigned int pbs, bool is_flush, struct block_device *ldev,
	u64 ring_buffer_off, u64 ring_buffer_size,
	unsigned int chunk_sectors);
static void logpack_submit_bio_wrapper_zero(
	struct bio_wrapper *biow, unsigned int pbs, struct block_device *ldev);
static void logpack_submit_bio_wrapper(
	struct bio_wrapper *biow, u64 lsid,
	unsigned int pbs, struct block_device *ldev,
	u64 ring_buffer_off, u64 ring_buffer_size,
	unsigned int chunk_sectors);
static struct bio* logpack_create_bio(
	struct bio *bio, uint pbs, struct block_device *ldev,
	u64 ldev_off_pb, uint bio_off_lb);
static struct bio_entry* logpack_create_bio_entry(
	struct bio *bio, unsigned int pbs,
	struct block_device *ldev,
	u64 ldev_off_pb, unsigned int bio_off_lb);
static void logpack_submit_flush(struct block_device *bdev, struct pack *pack);
static void wait_for_bio_wrapper_done(struct bio_wrapper *biow);
static void gc_logpack_list(struct walb_dev *wdev, struct list_head *wpack_list);
static void dequeue_and_gc_logpack_list(struct walb_dev *wdev);

/* Validator for debug. */
static bool is_prepared_pack_valid(struct pack *pack);
UNUSED static bool is_pack_list_valid(struct list_head *pack_list);

/* IOcore data related. */
static struct iocore_data* create_iocore_data(gfp_t gfp_mask);
static void destroy_iocore_data(struct iocore_data *iocored);

/* Other helper functions. */
static bool writepack_add_bio_wrapper(
	struct list_head *wpack_list, struct pack **wpackp,
	struct bio_wrapper *biow,
	u64 ring_buffer_size, unsigned int max_logpack_pb,
	u64 *latest_lsidp, u64 *flush_lsidp, unsigned long *log_flush_jiffiesp,
	struct walb_dev *wdev, gfp_t gfp_mask);
static void insert_to_sorted_bio_wrapper_list_by_pos(
	struct bio_wrapper *biow, struct list_head *biow_list);
static void writepack_check_and_set_flush(struct pack *wpack);
static bool wait_for_logpack_header(struct pack *wpack);
static void wait_for_logpack_and_submit_datapack(
	struct walb_dev *wdev, struct pack *wpack);
static void wait_for_write_bio_wrapper(
	struct walb_dev *wdev, struct bio_wrapper *biow);
static void wait_for_bio_wrapper(
	struct bio_wrapper *biow, bool is_endio, bool is_delete);
static void submit_write_bio_wrapper(
	struct bio_wrapper *biow, bool is_plugging);
static void submit_read_bio_wrapper(
	struct walb_dev *wdev, struct bio_wrapper *biow);
static struct bio_entry* submit_flush(struct block_device *bdev);
static void enqueue_submit_task_if_necessary(struct walb_dev *wdev);
static void enqueue_wait_task_if_necessary(struct walb_dev *wdev);
static void enqueue_submit_data_task_if_necessary(struct walb_dev *wdev);
static void enqueue_wait_data_task_if_necessary(struct walb_dev *wdev);
static void start_write_bio_wrapper(
	struct walb_dev *wdev, struct bio_wrapper *biow);
static void wait_for_all_started_write_io_done(struct walb_dev *wdev);
static void wait_for_all_pending_gc_done(struct walb_dev *wdev);
static void wait_for_log_permanent(struct walb_dev *wdev, u64 lsid);
static void flush_all_wq(void);
static void clear_working_flag(int working_bit, unsigned long *flag_p);
static void invoke_userland_exec(struct walb_dev *wdev, const char *event);
static void fail_and_destroy_bio_wrapper_list(
	struct walb_dev *wdev, struct list_head *biow_list);

/* Stop/start queue for fast algorithm. */
static bool should_stop_queue(
	struct walb_dev *wdev, struct bio_wrapper *biow);
static bool should_start_queue(
	struct walb_dev *wdev, struct bio_wrapper *biow);

/* For treemap memory manager. */
static bool treemap_memory_manager_get(void);
static void treemap_memory_manager_put(void);

/* For pack_cache. */
static bool pack_cache_get(void);
static void pack_cache_put(void);

/* For diskstats. */
static void io_acct_start(struct bio_wrapper *biow);
static void io_acct_end(struct bio_wrapper *biow);

/*******************************************************************************
 * Static functions implementation.
 *******************************************************************************/

/**
 * endio callback for bio_entry.
 */
static void bio_entry_end_io(struct bio *bio, int error)
{
	struct bio_entry *bioe = bio->bi_private;
	int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	int bi_cnt;
	ASSERT(bioe);
	ASSERT(bio->bi_bdev);
	ASSERT(bioe->bio == bio);

	if (!uptodate) {
		const unsigned int devt = bio->bi_bdev->bd_dev;
		LOGn("BIO_UPTODATE is false"
			" (dev %u:%u rw %lu pos %" PRIu64 " len %u).\n"
			, MAJOR(devt), MINOR(devt)
			, bio->bi_rw
			, (u64)bio_entry_pos(bioe), bio_entry_len(bioe));
	}

	/* LOGd("bio_entry_end_io() begin.\n"); */
	bioe->error = error;
	bi_cnt = atomic_read(&bio->bi_cnt);
	if (bio->bi_rw & REQ_WRITE) {
		/* 2 for data, 1 for log. */
#ifdef WALB_DEBUG
		if (bi_cnt != 2 && bi_cnt != 1) {
			const unsigned int devt = bio->bi_bdev->bd_dev;
			LOGe("pos %" PRIu64 " len %u dev %u:%u bi_cnt %d\n"
				, (u64)bio_entry_pos(bioe)
				, bio_entry_len(bioe)
				, MAJOR(devt), MINOR(devt), bi_cnt);
		}
#else
		ASSERT(bi_cnt == 2 || bi_cnt == 1);
#endif
	} else {
		ASSERT(bi_cnt == 1);
	}
	LOG_("complete bioe %p pos %" PRIu64 " len %u\n"
		, bioe, (u64)bio_entry_pos(bioe), bio_entry_len(bioe));
	if (bi_cnt == 1)
		bioe->bio = NULL;

	bio_put(bio);
	complete(&bioe->done);
	/* LOGd("bio_entry_end_io() end.\n"); */
}

/**
 * Create a bio_entry by clone.
 *
 * @bio original bio.
 * @bdev block device to forward bio.
 */
static struct bio_entry* create_bio_entry_by_clone(
	struct bio *bio, struct block_device *bdev,
	gfp_t gfp_mask, bool is_deep)
{
	struct bio_entry *bioe;
	struct bio *biotmp;

	bioe = alloc_bio_entry(gfp_mask);
	if (!bioe)
		return NULL;

	if (is_deep)
		biotmp = bio_deep_clone(bio, gfp_mask);
	else
		biotmp = bio_clone(bio, gfp_mask);

	if (!biotmp)
		goto error;

	biotmp->bi_bdev = bdev;
	biotmp->bi_end_io = bio_entry_end_io;
	biotmp->bi_private = bioe;

	if (is_deep)
		init_copied_bio_entry(bioe, biotmp);
	else
		init_bio_entry(bioe, biotmp);

	return bioe;

error:
	destroy_bio_entry(bioe);
	return NULL;
}

static struct bio_entry* create_bio_entry_by_clone_never_giveup(
	struct bio *bio, struct block_device *bdev,
	gfp_t gfp_mask, bool is_deep)
{
	struct bio_entry *bioe;
	for (;;) {
		bioe = create_bio_entry_by_clone(bio, bdev, gfp_mask, is_deep);
		if (bioe)
			break;
		LOGd_("clone bio copy failed %p.\n", bio);
		schedule();
	}
	return bioe;
}

/**
 * Wait for an bio completion in a bio_entry.
 */
static void wait_for_bio_entry(struct bio_entry *bioe)
{
	const ulong timeo = msecs_to_jiffies(completion_timeo_ms_);
	ulong rtimeo;
	int c = 0;

retry:
	rtimeo = wait_for_completion_timeout(&bioe->done, timeo);
	if (rtimeo == 0) {
		LOGn("timeout(%d): bioe %p bio %p pos %" PRIu64 " len %u\n"
			, c, bioe, bioe->bio
			, (u64)bio_entry_pos(bioe), bio_entry_len(bioe));
		c++;
		goto retry;
	}
}

/**
 * Create a pack.
 */
static struct pack* create_pack(gfp_t gfp_mask)
{
	struct pack *pack;

	pack = kmem_cache_alloc(pack_cache_, gfp_mask);
	if (!pack) {
		LOGd("kmem_cache_alloc() failed.");
		goto error0;
	}
	INIT_LIST_HEAD(&pack->list);
	INIT_LIST_HEAD(&pack->biow_list);
	pack->header_bioe = NULL;
	pack->is_zero_flush_only = false;
	pack->is_flush_contained = false;
	pack->is_flush_header = false;
	pack->is_logpack_failed = false;

	return pack;
#if 0
error1:
	destory_pack(pack);
#endif
error0:
	LOGe("create_pack() end with error.\n");
	return NULL;
}

/**
 * Create a writepack.
 *
 * @gfp_mask allocation mask.
 * @pbs physical block size in bytes.
 * @logpack_lsid logpack lsid.
 *
 * RETURN:
 *   Allocated and initialized writepack in success, or NULL.
 */
static struct pack* create_writepack(
	gfp_t gfp_mask, unsigned int pbs, u64 logpack_lsid)
{
	struct pack *pack;
	struct walb_logpack_header *lhead;

	ASSERT(logpack_lsid != INVALID_LSID);
	pack = create_pack(gfp_mask);
	if (!pack) { goto error0; }
	pack->logpack_header_sector = sector_alloc(pbs, gfp_mask | __GFP_ZERO);
	if (!pack->logpack_header_sector) { goto error1; }

	lhead = get_logpack_header(pack->logpack_header_sector);
	lhead->sector_type = SECTOR_TYPE_LOGPACK;
	lhead->logpack_lsid = logpack_lsid;
	/* lhead->total_io_size = 0; */
	/* lhead->n_records = 0; */
	/* lhead->n_padding = 0; */

	return pack;
error1:
	destroy_pack(pack);
error0:
	return NULL;
}

/**
 * Destory a pack.
 */
static void destroy_pack(struct pack *pack)
{
	struct bio_wrapper *biow, *biow_next;

	if (!pack) { return; }

	list_for_each_entry_safe(biow, biow_next, &pack->biow_list, list) {
		list_del(&biow->list);
		destroy_bio_wrapper_dec((struct walb_dev *)biow->private_data, biow);
	}
	if (pack->logpack_header_sector) {
		sector_free(pack->logpack_header_sector);
		pack->logpack_header_sector = NULL;
	}
#ifdef WALB_DEBUG
	INIT_LIST_HEAD(&pack->biow_list);
#endif
	kmem_cache_free(pack_cache_, pack);
}

/**
 * Check the pack contains zero-size flush only.
 *
 * RETURN:
 *   true if pack contains only one request and it is zero-size flush, or false.
 */
static bool is_zero_flush_only(const struct pack *pack)
{
	struct walb_logpack_header *logh;
	bool ret;

	ASSERT(pack);
	ASSERT(pack->logpack_header_sector);
	logh = get_logpack_header(pack->logpack_header_sector);
	ASSERT(logh);

	ret = logh->n_records == 0 && !list_empty(&pack->biow_list);
#ifdef WALB_DEBUG
	if (ret) {
		struct bio_wrapper *biow;
		int i = 0;
		list_for_each_entry(biow, &pack->biow_list, list) {
			ASSERT(biow->bio);
			ASSERT(biow->bio->bi_rw & REQ_FLUSH);
			ASSERT(biow->len == 0);
			i++;
		}
		ASSERT(i == 1);
	}
#endif
	return ret;
}

/**
 * Check the pack size exceeds max_logpack_pb or not.
 *
 * RETURN:
 *   true if pack is already exceeds or will be exceeds.
 */
static bool is_pack_size_too_large(
	struct walb_logpack_header *lhead,
	unsigned int pbs, unsigned int max_logpack_pb,
	struct bio_wrapper *biow)
{
	unsigned int pb;
	ASSERT(lhead);
	ASSERT(pbs);
	ASSERT_PBS(pbs);
	ASSERT(biow);

	if (max_logpack_pb == 0) {
		return false;
	}

	pb = (unsigned int)capacity_pb(pbs, biow->len);
	return pb + lhead->total_io_size > max_logpack_pb;
}

/**
 * Print a pack data for debug.
 */
static void print_pack(const char *level, struct pack *pack)
{
	struct walb_logpack_header *lhead;
	struct bio_wrapper *biow;
	unsigned int i;
	ASSERT(level);
	ASSERT(pack);

	printk("%s""print_pack %p begin\n", level, pack);

	i = 0;
	list_for_each_entry(biow, &pack->biow_list, list) {
		i++;
		print_bio_wrapper(level, biow);
	}
	printk("%s""number of bio_wrapper in biow_list: %u.\n", level, i);

	printk("%s""header_bioe: ", level);
	print_bio_entry(level, pack->header_bioe);

	/* logpack header */
	if (pack->logpack_header_sector) {
		lhead = get_logpack_header(pack->logpack_header_sector);
		walb_logpack_header_print(level, lhead);
	} else {
		printk("%s""logpack_header_sector is NULL.\n", level);
	}

	printk("%s""is_logpack_failed: %u\n",
		level, pack->is_logpack_failed);

	printk("%s""print_pack %p end\n", level, pack);
}

/**
 * Print a list of pack data for debug.
 */
static void print_pack_list(const char *level, struct list_head *wpack_list)
{
	struct pack *pack;
	unsigned int i = 0;
	ASSERT(level);
	ASSERT(wpack_list);

	printk("%s""print_pack_list %p begin.\n", level, wpack_list);
	list_for_each_entry(pack, wpack_list, list) {
		LOGd("%u: ", i);
		print_pack(level, pack);
		i++;
	}
	printk("%s""print_pack_list %p end.\n", level, wpack_list);
}

/**
 * RETURN:
 *   true if pack contains one or more flush requests (for log device).
 */
static bool pack_contains_flush(const struct pack *pack)
{
	return pack->is_zero_flush_only ||
		pack->is_flush_contained ||
		pack->is_flush_header;
}

/**
 * Get pointer of wdev and iocored from the work struct in a pwork.
 * The pwork will be destroyed.
 */
static void get_wdev_and_iocored_from_work(
	struct walb_dev **pwdev, struct iocore_data **piocored,
	struct work_struct *work)
{
	struct pack_work *pwork = container_of(work, struct pack_work, work);
	*pwdev = pwork->data;
	*piocored = get_iocored_from_wdev(*pwdev);
	destroy_pack_work(pwork);
}
/**
 * Submit all logpacks generated from bio_wrapper list.
 *
 * (1) Create logpack list.
 * (2) Submit all logpack-related bio(s).
 * (3) Enqueue task_wait_for_logpack_list.
 *
 * If an error (memory allocation failure) occurred inside this,
 * allocator will retry allocation after calling scheule() infinitely.
 *
 * @work work in a pack_work.
 *
 * CONTEXT:
 *   Workqueue task.
 *   The same task is not executed concurrently.
 */
static void task_submit_logpack_list(struct work_struct *work)
{
	struct walb_dev *wdev;
	struct iocore_data *iocored;
	struct list_head wpack_list;
	struct list_head biow_list;

	get_wdev_and_iocored_from_work(&wdev, &iocored, work);
	LOG_("begin\n");

	INIT_LIST_HEAD(&biow_list);
	INIT_LIST_HEAD(&wpack_list);
	while (true) {
		struct pack *wpack, *wpack_next;
		struct bio_wrapper *biow, *biow_next;
		bool is_empty;
		unsigned int n_io = 0;

		ASSERT(list_empty(&biow_list));
		ASSERT(list_empty(&wpack_list));

		/* Dequeue all bio wrappers from the submit queue. */
		spin_lock(&iocored->logpack_submit_queue_lock);
		is_empty = list_empty(&iocored->logpack_submit_queue);
		if (is_empty) {
			clear_working_flag(
				IOCORE_STATE_SUBMIT_LOG_TASK_WORKING,
				&iocored->flags);
		}
		list_for_each_entry_safe(biow, biow_next,
					&iocored->logpack_submit_queue, list) {
			list_move_tail(&biow->list, &biow_list);
			start_write_bio_wrapper(wdev, biow);
			n_io++;
			if (n_io >= wdev->n_io_bulk) { break; }
		}
		spin_unlock(&iocored->logpack_submit_queue_lock);
		if (is_empty) { break; }

		/* Failure mode. */
		if (test_bit(IOCORE_STATE_READ_ONLY, &iocored->flags)) {
			fail_and_destroy_bio_wrapper_list(wdev, &biow_list);
			continue;
		}

		/* Create and submit. */
		if (!create_logpack_list(wdev, &biow_list, &wpack_list)) {
			continue;
		}
		submit_logpack_list(wdev, &wpack_list);

		/* Enqueue logpack list to the wait queue. */
		spin_lock(&iocored->logpack_wait_queue_lock);
		list_for_each_entry_safe(wpack, wpack_next, &wpack_list, list) {
			list_move_tail(&wpack->list, &iocored->logpack_wait_queue);
		}
		spin_unlock(&iocored->logpack_wait_queue_lock);

		/* Enqueue wait task. */
		enqueue_wait_task_if_necessary(wdev);
	}

	LOG_("end\n");
}

/**
 * Wait for completion of all logpacks related to a call of request_fn.
 *
 * If submission a logpack is partially failed,
 * this function will end all requests related to the logpack and the followings.
 *
 * All failed (and end_request called) reqe(s) will be destroyed.
 *
 * @work work in a logpack_work.
 *
 * CONTEXT:
 *   Workqueue task.
 *   Works are serialized by singlethread workqueue.
 */
static void task_wait_for_logpack_list(struct work_struct *work)
{
	struct walb_dev *wdev;
	struct iocore_data *iocored;
	struct list_head wpack_list;

	get_wdev_and_iocored_from_work(&wdev, &iocored, work);
	LOG_("begin\n");

	INIT_LIST_HEAD(&wpack_list);
	while (true) {
		struct pack *wpack, *wpack_next;
		bool is_empty;
		unsigned int n_pack = 0;
		ASSERT(list_empty(&wpack_list));

		/* Dequeue logpack list from the submit queue. */
		spin_lock(&iocored->logpack_wait_queue_lock);
		is_empty = list_empty(&iocored->logpack_wait_queue);
		if (is_empty) {
			clear_working_flag(
				IOCORE_STATE_WAIT_LOG_TASK_WORKING,
				&iocored->flags);
		}
		list_for_each_entry_safe(wpack, wpack_next,
					&iocored->logpack_wait_queue, list) {
			list_move_tail(&wpack->list, &wpack_list);
			n_pack++;
			if (n_pack >= wdev->n_pack_bulk) { break; }
		}
		spin_unlock(&iocored->logpack_wait_queue_lock);
		if (is_empty) { break; }

		/* Wait logpack completion and submit datapacks. */
		list_for_each_entry_safe(wpack, wpack_next, &wpack_list, list) {
			wait_for_logpack_and_submit_datapack(wdev, wpack);
		}
		enqueue_submit_data_task_if_necessary(wdev);

		/* Put packs into the gc queue. */
		atomic_add(n_pack, &iocored->n_pending_gc);
		spin_lock(&iocored->logpack_gc_queue_lock);
		list_for_each_entry_safe(wpack, wpack_next, &wpack_list, list) {
			list_move_tail(&wpack->list, &iocored->logpack_gc_queue);
		}
		spin_unlock(&iocored->logpack_gc_queue_lock);

		/* Wakeup the gc task. */
		wakeup_worker(&iocored->gc_worker_data);
	}

	LOG_("end\n");
}

/**
 * Wait for all related bio(s) for a bio_wrapper and gc it.
 */
static void task_wait_and_gc_read_bio_wrapper(struct work_struct *work)
{
	struct bio_wrapper *biow = container_of(work, struct bio_wrapper, work);
	struct walb_dev *wdev = (struct walb_dev *)biow->private_data;

	wait_for_bio_wrapper(biow, true, true);
	destroy_bio_wrapper_dec(wdev, biow);
}

/**
 * Submit bio wrapper list for data device.
 */
static void task_submit_bio_wrapper_list(struct work_struct *work)
{
	struct walb_dev *wdev;
	struct iocore_data *iocored;
	struct list_head biow_list, biow_list_sorted;

	get_wdev_and_iocored_from_work(&wdev, &iocored, work);
	LOG_("begin\n");

	INIT_LIST_HEAD(&biow_list);
	INIT_LIST_HEAD(&biow_list_sorted);
	while (true) {
		struct bio_wrapper *biow, *biow_next;
		bool is_empty;
		u64 lsid = 0;
		unsigned int n_io = 0;
		struct blk_plug plug;
#ifdef WALB_OVERLAPPED_SERIALIZE
		bool ret;
#endif

		ASSERT(list_empty(&biow_list));
		ASSERT(list_empty(&biow_list_sorted));

		/* Dequeue all bio wrappers from the submit queue. */
		spin_lock(&iocored->datapack_submit_queue_lock);
		is_empty = list_empty(&iocored->datapack_submit_queue);
		if (is_empty) {
			clear_working_flag(
				IOCORE_STATE_SUBMIT_DATA_TASK_WORKING,
				&iocored->flags);
		}
		list_for_each_entry_safe(biow, biow_next,
					&iocored->datapack_submit_queue, list2) {
			list_move_tail(&biow->list2, &biow_list);
			n_io++;
			lsid = biow->lsid;
			BIO_WRAPPER_CHANGE_STATE(biow);
			if (n_io >= wdev->n_io_bulk) { break; }
		}
		spin_unlock(&iocored->datapack_submit_queue_lock);
		if (is_empty) { break; }

		/* Wait for all previous log must be permanent
		   before submitting data IO. */
		wait_for_log_permanent(wdev, lsid);

#ifdef WALB_OVERLAPPED_SERIALIZE
		/* Check and insert to overlapped detection data. */
		list_for_each_entry(biow, &biow_list, list2) {
		retry_insert_ol:
			spin_lock(&iocored->overlapped_data_lock);
			ret = overlapped_check_and_insert(
				iocored->overlapped_data,
				&iocored->max_sectors_in_overlapped,
				biow, GFP_ATOMIC
#ifdef WALB_DEBUG
				, &iocored->overlapped_in_id
#endif
				);
			spin_unlock(&iocored->overlapped_data_lock);
			if (!ret) {
				schedule();
				goto retry_insert_ol;
			}
		}
#endif /* WALB_OVERLAPPED_SERIALIZE */

		/* Sort IOs. */
		list_for_each_entry_safe(biow, biow_next, &biow_list, list2) {
			clear_flush_bit(&biow->cloned_bio_list);

#ifdef WALB_OVERLAPPED_SERIALIZE
			if (!bio_wrapper_state_is_delayed(biow)) {
				ASSERT(biow->n_overlapped == 0);
				if (is_sort_data_io_) {
					/* Sort. */
					insert_to_sorted_bio_wrapper_list_by_pos(
						biow, &biow_list_sorted);
				} else {
					list_add_tail(&biow->list4, &biow_list_sorted);
				}
			} else {
				/* Delayed. */
			}
#else /* WALB_OVERLAPPED_SERIALIZE */
			if (is_sort_data_io_) {
				/* Sort. */
				insert_to_sorted_bio_wrapper_list_by_pos(
					biow, &biow_list_sorted);
			} else {
				list_add_tail(&biow->list4, &biow_list_sorted);
			}
#endif /* WALB_OVERLAPPED_SERIALIZE */
		}

		/* Submit. */
		blk_start_plug(&plug);
		list_for_each_entry_safe(biow, biow_next, &biow_list_sorted, list4) {
			const bool is_plugging = false;
			/* Submit bio wrapper. */
			list_del(&biow->list4);
			BIO_WRAPPER_CHANGE_STATE(biow);
			BIO_WRAPPER_PRINT("data0", biow);
			submit_write_bio_wrapper(biow, is_plugging);
		}
		blk_finish_plug(&plug);

		/* Enqueue wait task. */
		spin_lock(&iocored->datapack_wait_queue_lock);
		list_for_each_entry_safe(biow, biow_next, &biow_list, list2) {
			BIO_WRAPPER_CHANGE_STATE(biow);
			list_move_tail(&biow->list2, &iocored->datapack_wait_queue);
		}
		spin_unlock(&iocored->datapack_wait_queue_lock);
		enqueue_wait_data_task_if_necessary(wdev);
	}

	LOG_("end.\n");
}

/**
 * Wait for bio wrapper list for data device.
 */
static void task_wait_for_bio_wrapper_list(struct work_struct *work)
{
	struct walb_dev *wdev;
	struct iocore_data *iocored;
	struct list_head biow_list;

	get_wdev_and_iocored_from_work(&wdev, &iocored, work);
	LOG_("begin.\n");

	INIT_LIST_HEAD(&biow_list);
	for (;;) {
		struct bio_wrapper *biow, *biow_next;
		bool is_empty;
		unsigned int n_io = 0;

		ASSERT(list_empty(&biow_list));

		/* Dequeue all bio wrappers from the submit queue. */
		spin_lock(&iocored->datapack_wait_queue_lock);
		is_empty = list_empty(&iocored->datapack_wait_queue);
		if (is_empty) {
			clear_working_flag(
				IOCORE_STATE_WAIT_DATA_TASK_WORKING,
				&iocored->flags);
		}
		list_for_each_entry_safe(biow, biow_next,
					&iocored->datapack_wait_queue, list2) {
			list_move_tail(&biow->list2, &biow_list);
			n_io++;
			BIO_WRAPPER_CHANGE_STATE(biow);
			if (n_io >= wdev->n_io_bulk) { break; }
		}
		spin_unlock(&iocored->datapack_wait_queue_lock);
		if (is_empty) { break; }
		ASSERT(n_io <= wdev->n_io_bulk);

		/* Wait for write bio wrapper and notify to gc task. */
		list_for_each_entry_safe(biow, biow_next, &biow_list, list2) {
			list_del(&biow->list2);
			wait_for_write_bio_wrapper(wdev, biow);
#ifdef WALB_PERFORMANCE_ANALYSIS
			getnstimeofday(&biow->ts[WALB_TIME_DATA_COMPLETED]);
#endif
			complete(&biow->done);
		}
	}

	LOG_("end.\n");
}

/**
 * Run gc logpack list.
 */
static void run_gc_logpack_list(void *data)
{
	struct walb_dev *wdev = (struct walb_dev *)data;
	ASSERT(wdev);

	dequeue_and_gc_logpack_list(wdev);
}

/**
 * Create logpack list using bio_wrapper(s) in biow_list,
 * and add to wpack_list.
 *
 * @wdev walb device.
 * @biow_list list of bio_wrapper.
 *   When all bio wrappers are successfuly processed,
 *   biow_list will be empty.
 *   When memory allocation errors occur,
 *   biow_list will not be empty.
 * @wpack_list list of pack (must be empty).
 *   Finally all biow(s) in the biow_list will be
 *   moved to pack(s) in the wpack_list.
 */
static bool create_logpack_list(
	struct walb_dev *wdev, struct list_head *biow_list,
	struct list_head *wpack_list)
{
	struct iocore_data *iocored;
	struct bio_wrapper *biow, *biow_next;
	struct pack *wpack = NULL;
	u64 latest_lsid, latest_lsid_old,
		flush_lsid, written_lsid, oldest_lsid;
	unsigned long log_flush_jiffies;
	bool ret;

	ASSERT(wdev);
	iocored = get_iocored_from_wdev(wdev);
	ASSERT(iocored);
	ASSERT(list_empty(wpack_list));
	ASSERT(!list_empty(biow_list));

	/* Load latest_lsid */
	spin_lock(&wdev->lsid_lock);
	latest_lsid = wdev->lsids.latest;
	oldest_lsid = wdev->lsids.oldest;
	written_lsid = wdev->lsids.written;
	flush_lsid = wdev->lsids.flush;
	log_flush_jiffies = iocored->log_flush_jiffies;
	spin_unlock(&wdev->lsid_lock);
	latest_lsid_old = latest_lsid;
	ASSERT(latest_lsid >= written_lsid);

	/* Create logpack(s). */
	list_for_each_entry_safe(biow, biow_next, biow_list, list) {
		list_del(&biow->list);
	retry:
		ret = writepack_add_bio_wrapper(
			wpack_list, &wpack, biow,
			wdev->ring_buffer_size, wdev->max_logpack_pb,
			&latest_lsid, &flush_lsid, &log_flush_jiffies,
			wdev, GFP_NOIO);
		if (!ret) {
			LOGn("writepack_add_bio_wrapper failed.\n");
			schedule();
			goto retry;
		}
	}
	if (wpack) {
		bool is_flush_size, is_flush_period;
		struct walb_logpack_header *logh
			= get_logpack_header(wpack->logpack_header_sector);
		writepack_check_and_set_flush(wpack);
		ASSERT(is_prepared_pack_valid(wpack));
		list_add_tail(&wpack->list, wpack_list);
		latest_lsid = get_next_lsid_unsafe(logh);

		/* Decide to flush the log device or not. */
		ASSERT(latest_lsid >= flush_lsid);
		is_flush_size = wdev->log_flush_interval_pb > 0 &&
			latest_lsid - flush_lsid > wdev->log_flush_interval_pb;
		is_flush_period = wdev->log_flush_interval_jiffies > 0 &&
			log_flush_jiffies < jiffies;
		if (is_flush_size || is_flush_period) {
			wpack->is_flush_header = true;
			flush_lsid = logh->logpack_lsid;
#ifdef WALB_DEBUG
			atomic_inc(&iocored->n_flush_logpack);
#endif
		}
	}

	/* Currently all requests are packed and lsid of all writepacks is defined. */
	ASSERT(is_pack_list_valid(wpack_list));
	ASSERT(!list_empty(wpack_list));
	ASSERT(list_empty(biow_list));

	/* Check whether we must avoid ring buffer overflow. */
	if (is_error_before_overflow_ && wdev->ring_buffer_size < latest_lsid - oldest_lsid) {
		struct pack *wpack_next;
		/* We must fail the IOs to avoid ring buffer overflow. */
		list_for_each_entry_safe(wpack, wpack_next, wpack_list, list) {
			list_del(&wpack->list);
			fail_and_destroy_bio_wrapper_list(wdev, &wpack->biow_list);
#ifdef DEBUG
			if (wpack->is_flush_header) {
				atomic_dec(&iocored->n_flush_logpack);
			}
#endif
			ASSERT(!wpack->header_bioe);
			destroy_pack(wpack);
		}
		ASSERT(list_empty(wpack_list));
		return false;
	}

	/* Store lsids. */
	ASSERT(latest_lsid >= latest_lsid_old);
	spin_lock(&wdev->lsid_lock);
	ASSERT(wdev->lsids.latest == latest_lsid_old);
	wdev->lsids.latest = latest_lsid;
	if (wdev->lsids.flush < flush_lsid) {
		wdev->lsids.flush = flush_lsid;
		iocored->log_flush_jiffies =
			jiffies + wdev->log_flush_interval_jiffies;
	}
	spin_unlock(&wdev->lsid_lock);

	/* Check ring buffer overflow. */
	ASSERT(latest_lsid >= oldest_lsid);
	if (latest_lsid - oldest_lsid > wdev->ring_buffer_size) {
		if (test_and_set_bit(IOCORE_STATE_LOG_OVERFLOW, &iocored->flags) == 0) {
			pr_warn_ratelimited(
				"Ring buffer for log has been overflowed."
				" reset_wal is required.\n");
			invoke_userland_exec(wdev, "overflow");
		}
	}

	/* Check consistency. */
	ASSERT(latest_lsid >= written_lsid);
	if (latest_lsid - written_lsid > wdev->ring_buffer_size) {
		pr_err_ratelimited(
			"Ring buffer size is too small to keep consistency. "
			"!!!PLEASE GROW THE LOG DEVICE SIZE.!!!\n"
			"latest_lsid %" PRIu64 "\n"
			"written_lsid %" PRIu64 "\n"
			"ring_buffer_size %" PRIu64 "\n"
			, latest_lsid, written_lsid, wdev->ring_buffer_size);
	}

	return true;
}

/**
 * Submit all write packs in a list to the log device.
 */
static void submit_logpack_list(
	struct walb_dev *wdev, struct list_head *wpack_list)
{
	struct iocore_data *iocored;
	struct pack *wpack;
	struct blk_plug plug;
	ASSERT(wpack_list);
	ASSERT(wdev);
	iocored = get_iocored_from_wdev(wdev);
	ASSERT(iocored);

	blk_start_plug(&plug);
	list_for_each_entry(wpack, wpack_list, list) {
		struct walb_logpack_header *logh;

		ASSERT_SECTOR_DATA(wpack->logpack_header_sector);
		logh = get_logpack_header(wpack->logpack_header_sector);

		if (wpack->is_zero_flush_only) {
			ASSERT(logh->n_records == 0);
			LOG_("is_zero_flush_only\n");
			logpack_submit_flush(wdev->ldev, wpack);
		} else {
			ASSERT(logh->n_records > 0);
			logpack_calc_checksum(logh, wdev->physical_bs,
					wdev->log_checksum_salt, &wpack->biow_list);
			submit_logpack(
				logh, &wpack->biow_list, &wpack->header_bioe,
				wdev->physical_bs, wpack->is_flush_header,
				wdev->ldev, wdev->ring_buffer_off,
				wdev->ring_buffer_size, wdev->ldev_chunk_sectors);
		}
	}
	blk_finish_plug(&plug);
}

/**
 * Set checksum of each bio and calc/set log header checksum.
 *
 * @logh log pack header.
 * @pbs physical sector size (allocated size as logh).
 * @biow_list list of biow.
 *   checksum of each bio has already been calculated as biow->csum.
 */
static void logpack_calc_checksum(
	struct walb_logpack_header *logh,
	unsigned int pbs, u32 salt, struct list_head *biow_list)
{
	int i;
	struct bio_wrapper *biow;
	int n_padding;

	ASSERT(logh);
	ASSERT(logh->n_records > 0);
	ASSERT(logh->n_records > logh->n_padding);

	n_padding = 0;
	i = 0;
	list_for_each_entry(biow, biow_list, list) {
		if (test_bit_u32(LOG_RECORD_PADDING, &logh->record[i].flags)) {
			n_padding++;
			i++;
			/* The corresponding record of the biow must be the next. */
			ASSERT(i < logh->n_records);
		}

		ASSERT(biow);
		ASSERT(biow->bio);
		ASSERT(biow->bio->bi_rw & REQ_WRITE);

		if (biow->len == 0) {
			ASSERT(biow->bio->bi_rw & REQ_FLUSH);
			continue;
		}

		logh->record[i].checksum = biow->csum;
		i++;
	}

	ASSERT(n_padding <= 1);
	ASSERT(n_padding == logh->n_padding);
	ASSERT(i == logh->n_records);
	ASSERT(logh->checksum == 0);
	logh->checksum = checksum((u8 *)logh, pbs, salt);
	ASSERT(checksum((u8 *)logh, pbs, salt) == 0);
}

/**
 * Submit logpack entry.
 *
 * @logh logpack header.
 * @biow_list bio wrapper list. must not be empty.
 * @bioe_p pointer to bio entry. *bioe_p must be NULL.
 *   submitted bio for logpack header will be set.
 * @pbs physical block size.
 * @is_flush true if the logpack header's REQ_FLUSH flag must be on.
 * @ldev log block device.
 * @ring_buffer_off ring buffer offset.
 * @ring_buffer_size ring buffer size.
 * @chunk_sectors chunk_sectors for bio alignment.
 *
 * CONTEXT:
 *     Non-IRQ. Non-atomic.
 */
static void submit_logpack(
	struct walb_logpack_header *logh,
	struct list_head *biow_list, struct bio_entry **bioe_p,
	unsigned int pbs, bool is_flush, struct block_device *ldev,
	u64 ring_buffer_off, u64 ring_buffer_size,
	unsigned int chunk_sectors)
{
	struct bio_wrapper *biow;
	int i;

	ASSERT(!list_empty(biow_list));

	/* Submit logpack header block. */
	logpack_submit_header(
		logh, bioe_p, pbs, is_flush, ldev,
		ring_buffer_off, ring_buffer_size,
		chunk_sectors);

	/* Submit logpack contents for each request. */
	i = 0;
	list_for_each_entry(biow, biow_list, list) {
		struct walb_log_record *rec = &logh->record[i];
		if (test_bit_u32(LOG_RECORD_PADDING, &rec->flags)) {
			i++;
			rec = &logh->record[i];
			/* The biow must be for the next record. */
		}
#ifdef WALB_PERFORMANCE_ANALYSIS
		getnstimeofday(&biow->ts[WALB_TIME_LOG_SUBMITTED]);
#endif
		if (test_bit_u32(LOG_RECORD_DISCARD, &rec->flags)) {
			/* No need to execute IO to the log device. */
			ASSERT(bio_wrapper_state_is_discard(biow));
			ASSERT(biow->bio->bi_rw & REQ_DISCARD);
			ASSERT(biow->len > 0);
		} else if (biow->len == 0) {
			/* Zero-sized IO will not be stored in logpack header.
			   We just submit it and will wait for it. */

			/* such bio must be flush. */
			ASSERT(biow->bio->bi_rw & REQ_FLUSH);
			/* such bio must be permitted at first only. */
			ASSERT(i == 0);

			BIO_WRAPPER_PRINT("logF", biow);
			logpack_submit_bio_wrapper_zero(biow, pbs, ldev);
		} else {
			/* Normal IO. */
			ASSERT(i < logh->n_records);

			BIO_WRAPPER_PRINT("log0", biow);
			/* submit bio(s) for the biow. */
			logpack_submit_bio_wrapper(
				biow, rec->lsid, pbs, ldev, ring_buffer_off,
				ring_buffer_size, chunk_sectors);
		}
		i++;
	}
}

/**
 * Submit bio of header block.
 *
 * @lhead logpack header data.
 * @bioe_p pointer to bio_entry pointer.
 *     submitted lhead bio will be stored.
 * @pbs physical block size [bytes].
 * @is_flush if true, REQ_FLUSH must be added.
 * @ldev log device.
 * @ring_buffer_off ring buffer offset [physical blocks].
 * @ring_buffer_size ring buffer size [physical blocks].
 */
static void logpack_submit_header(
	struct walb_logpack_header *lhead, struct bio_entry **bioe_p,
	unsigned int pbs, bool is_flush, struct block_device *ldev,
	u64 ring_buffer_off, u64 ring_buffer_size,
	unsigned int chunk_sectors)
{
	struct bio *bio;
	struct bio_entry *bioe;
	struct page *page;
	u64 off_pb, off_lb;
	int len;
#ifdef WALB_DEBUG
	struct page *page2;
#endif

retry_bio_entry:
	bioe = alloc_bio_entry(GFP_NOIO);
	if (!bioe) {
		schedule();
		goto retry_bio_entry;
	}
retry_bio:
	bio = bio_alloc(GFP_NOIO, 1);
	if (!bio) {
		schedule();
		goto retry_bio;
	}

	page = virt_to_page(lhead);
#ifdef WALB_DEBUG
	page2 = virt_to_page((unsigned long)lhead + pbs - 1);
	ASSERT(page == page2);
#endif
	bio->bi_bdev = ldev;
	off_pb = lhead->logpack_lsid % ring_buffer_size + ring_buffer_off;
	off_lb = addr_lb(pbs, off_pb);
	bio->bi_iter.bi_sector = off_lb;
	if (is_flush) {
		bio->bi_rw = WRITE_FLUSH;
	} else {
		bio->bi_rw = WRITE;
	}
	bio->bi_end_io = bio_entry_end_io;
	bio->bi_private = bioe;
	len = bio_add_page(bio, page, pbs, offset_in_page(lhead));
	ASSERT(len == pbs);

	init_bio_entry(bioe, bio);
	ASSERT((bio_entry_len(bioe) << 9) == pbs);
	*bioe_p = bioe;

	ASSERT(!should_split_bio_for_chunk(bioe->bio, chunk_sectors));
	generic_make_request(bioe->bio);

	return;
#if 0
error2:
	bio_put(bio);
	bioe->bio = NULL;
error1:
	destroy_bio_entry(bioe);
error0:
	return;
#endif
}

/**
 * Submit a logpack bio for a flush request.
 *
 * @biow bio wrapper(which contains original bio).
 *       The bio->size must be 0.
 * @bioe_list list of bio_entry. must be empty.
 *   successfully submitted bioe(s) must be added to the tail of this.
 * @pbs physical block size [bytes]
 * @ldev log device.
 */
static void logpack_submit_bio_wrapper_zero(
	struct bio_wrapper *biow, unsigned int pbs, struct block_device *ldev)
{
	struct bio_entry *bioe;

	ASSERT(biow->len == 0);
	ASSERT(biow->bio);
	ASSERT(!biow->cloned_bioe);

retry:
	bioe = logpack_create_bio_entry(biow->bio, pbs, ldev, 0, 0);
	if (!bioe) {
		schedule();
		goto retry;
	}
	biow->cloned_bioe = bioe;
	LOG_("submit_lr: bioe %p pos %" PRIu64 " len %u\n"
		, bioe, (u64)bioe->pos, bioe->len);
	generic_make_request(bioe->bio);
}

/**
 * Submit all logpack bio(s) for a request.
 *
 * @biow bio wrapper(which contains original bio).
 * @lsid lsid of the bio in the logpack.
 * @pbs physical block size [bytes]
 * @ldev log device.
 * @ring_buffer_off ring buffer offset [physical block].
 * @ring_buffer_size ring buffer size [physical block].
 */
static void logpack_submit_bio_wrapper(
	struct bio_wrapper *biow, u64 lsid,
	unsigned int pbs, struct block_device *ldev,
	u64 ring_buffer_off, u64 ring_buffer_size,
	unsigned int chunk_sectors)
{
	struct bio_entry *bioe;
	const u64 ldev_off_pb = lsid % ring_buffer_size + ring_buffer_off;
	struct list_head tmp_list;
	struct bio_list bio_list;

	INIT_LIST_HEAD(&tmp_list);
	ASSERT(biow);
	ASSERT(biow->bio);
	ASSERT(!bio_wrapper_state_is_discard(biow));
	ASSERT((biow->bio->bi_rw & REQ_DISCARD) == 0);

retry_bio_entry:
	bioe = logpack_create_bio_entry(
		biow->bio, pbs, ldev, ldev_off_pb, 0);
	if (!bioe) {
		schedule();
		goto retry_bio_entry;
	}
	biow->cloned_bioe = bioe;

	/* split if required. */
	bio_list = split_bio_for_chunk_never_giveup(
		bioe->bio, chunk_sectors, GFP_NOIO);
	/* No need to set biow->cloned_bio_list. */

	/* really submit */
	LOG_("submit_lr: bioe %p pos %" PRIu64 " len %u\n"
		, bioe, bioe->pos, bioe->len);
	submit_all_bio_list(&bio_list);
}

/**
 * Create a bio which is for logpack.
 */
static struct bio* logpack_create_bio(
	struct bio *bio, uint pbs, struct block_device *ldev,
	u64 ldev_off_pb, uint bio_off_lb)
{
	struct bio *cbio;
	cbio = bio_clone(bio, GFP_NOIO);
	if (!cbio)
		return NULL;

	cbio->bi_bdev = ldev;
	cbio->bi_iter.bi_sector = addr_lb(pbs, ldev_off_pb) + bio_off_lb;
	/* cbio->bi_end_io = NULL; */
	/* cbio->bi_private = NULL; */

	/* An IO persistence requires all previous log IO(s) persistence. */
	if (cbio->bi_rw & REQ_FUA)
		cbio->bi_rw |= REQ_FLUSH;

	return cbio;
}

/**
 * Create a bio_entry which is a part of logpack.
 *
 * @bio original bio to clone.
 * @pbs physical block device [bytes].
 * @ldev_off_pb log device offset for the request [physical block].
 * @bio_off_lb offset of the bio inside the whole request [logical block].
 *
 * RETURN:
 *   bio_entry in success which bio is submitted, or NULL.
 */
static struct bio_entry* logpack_create_bio_entry(
	struct bio *bio, unsigned int pbs,
	struct block_device *ldev,
	u64 ldev_off_pb, unsigned int bio_off_lb)
{
	struct bio_entry *bioe;
	struct bio *cbio;

	bioe = alloc_bio_entry(GFP_NOIO);
	if (!bioe)
		return NULL;

	cbio = logpack_create_bio(bio, pbs, ldev, ldev_off_pb, bio_off_lb);
	if (!cbio)
		goto error;

	cbio->bi_end_io = bio_entry_end_io;
	cbio->bi_private = bioe;
	init_bio_entry(bioe, cbio);
	return bioe;

error:
	destroy_bio_entry(bioe);
	return NULL;
}

/**
 * Submit flush for logpack.
 */
static void logpack_submit_flush(struct block_device *bdev, struct pack *pack)
{
	struct bio_entry *bioe;
	ASSERT(bdev);
	ASSERT(pack);

retry:
	bioe = submit_flush(bdev);
	if (!bioe) {
		schedule();
		goto retry;
	}
	pack->header_bioe = bioe;
}

static void wait_for_bio_wrapper_done(struct bio_wrapper *biow)
{
	const unsigned long timeo = msecs_to_jiffies(completion_timeo_ms_);
	int c = 0;

	for (;;) {
		ulong rtimeo = wait_for_completion_timeout(&biow->done, timeo);
		if (rtimeo)
			break;
		LOGn("timeout(%d): "
			"biow %p pos %" PRIu64 " len %u csum %08x "
			"error %d discard %u\n"
			, c, biow, (u64)biow->pos, biow->len
			, biow->csum, biow->error
			, bio_wrapper_state_is_discard(biow) ? 1 : 0);
		c++;
	}
}

/**
 * Gc logpack list.
 */
static void gc_logpack_list(struct walb_dev *wdev, struct list_head *wpack_list)
{
	struct pack *wpack, *wpack_next;
	u64 written_lsid = INVALID_LSID;
	ASSERT(!list_empty(wpack_list));

	list_for_each_entry_safe(wpack, wpack_next, wpack_list, list) {
		struct bio_wrapper *biow, *biow_next;
		list_del(&wpack->list);
		list_for_each_entry_safe(biow, biow_next, &wpack->biow_list, list) {
			list_del(&biow->list);
			ASSERT(bio_wrapper_state_is_prepared(biow));
			wait_for_bio_wrapper_done(biow);
			ASSERT(bio_wrapper_state_is_submitted(biow));
			ASSERT(bio_wrapper_state_is_completed(biow));
			if (biow->error) {
				LOGe("data IO error. to be read-only mode.\n");
				set_read_only_mode(get_iocored_from_wdev(wdev));
			}
#ifdef WALB_PERFORMANCE_ANALYSIS
			getnstimeofday(&biow->ts[WALB_TIME_END]);
			print_bio_wrapper_performance(KERN_NOTICE, biow);
#endif
			destroy_bio_wrapper_dec(wdev, biow);
		}
		ASSERT(list_empty(&wpack->biow_list));
		ASSERT(!wpack->header_bioe);

		written_lsid = get_next_lsid_unsafe(
			get_logpack_header(wpack->logpack_header_sector));

		destroy_pack(wpack);
	}
	ASSERT(list_empty(wpack_list));

	/* Update written_lsid. */
	ASSERT(written_lsid != INVALID_LSID);
	spin_lock(&wdev->lsid_lock);
	wdev->lsids.written = written_lsid;
	spin_unlock(&wdev->lsid_lock);
}

/**
 * Get logpack(s) from the gc queue and execute gc for them.
 */
static void dequeue_and_gc_logpack_list(struct walb_dev *wdev)
{
	struct pack *wpack, *wpack_next;
	struct list_head wpack_list;
	struct iocore_data *iocored;

	ASSERT(wdev);
	iocored = get_iocored_from_wdev(wdev);
	ASSERT(iocored);

	INIT_LIST_HEAD(&wpack_list);
	while (true) {
		bool is_empty;
		int n_pack = 0;
		/* Dequeue logpack list */
		spin_lock(&iocored->logpack_gc_queue_lock);
		is_empty = list_empty(&iocored->logpack_gc_queue);
		list_for_each_entry_safe(wpack, wpack_next,
					&iocored->logpack_gc_queue, list) {
			list_move_tail(&wpack->list, &wpack_list);
			n_pack++;
			if (n_pack >= wdev->n_pack_bulk) { break; }
		}
		spin_unlock(&iocored->logpack_gc_queue_lock);
		if (is_empty) { break; }

		/* Gc */
		gc_logpack_list(wdev, &wpack_list);
		ASSERT(list_empty(&wpack_list));
		atomic_sub(n_pack, &iocored->n_pending_gc);
	}
}

/**
 * Check whether pack is valid.
 *   Assume just created and filled. checksum is not calculated at all.
 *
 * RETURN:
 *   true if valid, or false.
 */
static bool is_prepared_pack_valid(struct pack *pack)
{
	struct walb_logpack_header *lhead;
	unsigned int pbs;
	unsigned int i;
	struct bio_wrapper *biow;
	u64 total_pb; /* total io size in physical block. */
	unsigned int n_padding = 0;

	LOG_("is_prepared_pack_valid begin.\n");

	CHECKd(pack);
	CHECKd(pack->logpack_header_sector);

	lhead = get_logpack_header(pack->logpack_header_sector);
	pbs = pack->logpack_header_sector->size;
	ASSERT_PBS(pbs);
	CHECKd(lhead);
	CHECKd(is_valid_logpack_header(lhead));

	CHECKd(!list_empty(&pack->biow_list));

	i = 0;
	total_pb = 0;
	list_for_each_entry(biow, &pack->biow_list, list) {
		struct walb_log_record *lrec;

		CHECKd(biow->bio);
		if (biow->len == 0) {
			CHECKd(biow->bio->bi_rw & REQ_FLUSH);
			CHECKd(i == 0);
			CHECKd(lhead->n_records == 0);
			CHECKd(lhead->total_io_size == 0);
			continue;
		}

		CHECKd(i < lhead->n_records);
		lrec = &lhead->record[i];
		CHECKd(test_bit_u32(LOG_RECORD_EXIST, &lrec->flags));

		if (test_bit_u32(LOG_RECORD_PADDING, &lrec->flags)) {
			LOG_("padding found.\n"); /* debug */
			total_pb += capacity_pb(pbs, lrec->io_size);
			n_padding++;
			i++;
			/* The corresponding record of the biow must be the next. */
			CHECKd(i < lhead->n_records);
			lrec = &lhead->record[i];
			CHECKd(test_bit_u32(LOG_RECORD_EXIST, &lrec->flags));
		}

		/* Normal record. */
		CHECKd(biow->bio);
		CHECKd(biow->bio->bi_rw & REQ_WRITE);
		CHECKd(biow->pos == (sector_t)lrec->offset);
		CHECKd(lhead->logpack_lsid == lrec->lsid - lrec->lsid_local);
		CHECKd(biow->len == lrec->io_size);
		if (test_bit_u32(LOG_RECORD_DISCARD, &lrec->flags)) {
			CHECKd(bio_wrapper_state_is_discard(biow));
		} else {
			CHECKd(!bio_wrapper_state_is_discard(biow));
			total_pb += capacity_pb(pbs, lrec->io_size);
		}
		i++;
	}
	CHECKd(i == lhead->n_records);
	CHECKd(total_pb == lhead->total_io_size);
	CHECKd(n_padding == lhead->n_padding);
	if (lhead->n_records == 0) {
		CHECKd(pack->is_zero_flush_only);
	}
	LOG_("valid.\n");
	return true;
error:
	LOG_("not valid.\n");
	return false;
}

/**
 * Check whether pack list is valid.
 * This is just for debug.
 *
 * @listh list of struct pack.
 *
 * RETURN:
 *   true if valid, or false.
 */
static bool is_pack_list_valid(struct list_head *pack_list)
{
	struct pack *pack;

	list_for_each_entry(pack, pack_list, list) {
		CHECKd(is_prepared_pack_valid(pack));
	}
	return true;
error:
	return false;
}

/**
 * Create iocore data.
 * GC worker will not be started inside this function.
 */
static struct iocore_data* create_iocore_data(gfp_t gfp_mask)
{
	struct iocore_data *iocored;

	iocored = kmalloc(sizeof(struct iocore_data), gfp_mask);
	if (!iocored) {
		LOGe("memory allocation failure.\n");
		goto error0;
	}

	/* Flags. */
	iocored->flags = 0;

	/* Stoppers */
	atomic_set(&iocored->n_stoppers, 0);

	/* Queues and their locks. */
	INIT_LIST_HEAD(&iocored->logpack_submit_queue);
	INIT_LIST_HEAD(&iocored->logpack_wait_queue);
	INIT_LIST_HEAD(&iocored->datapack_submit_queue);
	INIT_LIST_HEAD(&iocored->datapack_wait_queue);
	INIT_LIST_HEAD(&iocored->logpack_gc_queue);
	spin_lock_init(&iocored->logpack_submit_queue_lock);
	spin_lock_init(&iocored->logpack_wait_queue_lock);
	spin_lock_init(&iocored->datapack_submit_queue_lock);
	spin_lock_init(&iocored->datapack_wait_queue_lock);
	spin_lock_init(&iocored->logpack_gc_queue_lock);

	/* To wait all IO for underlying devices done. */
	atomic_set(&iocored->n_started_write_bio, 0);
	atomic_set(&iocored->n_pending_bio, 0);
	atomic_set(&iocored->n_pending_gc, 0);

	/* Log flush time. */
	iocored->log_flush_jiffies = jiffies;

#ifdef WALB_OVERLAPPED_SERIALIZE
	spin_lock_init(&iocored->overlapped_data_lock);
	iocored->overlapped_data = multimap_create(gfp_mask, &mmgr_);
	if (!iocored->overlapped_data) {
		LOGe();
		goto error1;
	}
	iocored->max_sectors_in_overlapped = 0;
#ifdef WALB_DEBUG
	iocored->overlapped_in_id = 0;
	iocored->overlapped_out_id = 0;
#endif
#endif

	spin_lock_init(&iocored->pending_data_lock);
	iocored->pending_data = multimap_create(gfp_mask, &mmgr_);
	if (!iocored->pending_data) {
		LOGe();
		goto error2;
	}
	iocored->pending_sectors = 0;
	iocored->queue_restart_jiffies = jiffies;
	iocored->is_under_throttling = false;
	iocored->max_sectors_in_pending = 0;

#ifdef WALB_DEBUG
	atomic_set(&iocored->n_flush_io, 0);
	atomic_set(&iocored->n_flush_logpack, 0);
	atomic_set(&iocored->n_flush_force, 0);
#endif
	return iocored;

error2:
	multimap_destroy(iocored->pending_data);

#ifdef WALB_OVERLAPPED_SERIALIZE
error1:
	multimap_destroy(iocored->overlapped_data);
#endif
	kfree(iocored);
error0:
	return NULL;
}

/**
 * Destroy iocore data.
 */
static void destroy_iocore_data(struct iocore_data *iocored)
{
	ASSERT(iocored);

	multimap_destroy(iocored->pending_data);
#ifdef WALB_OVERLAPPED_SERIALIZE
	multimap_destroy(iocored->overlapped_data);
#endif
	kfree(iocored);
}

/**
 * Add a bio_wrapper to a writepack.
 *
 * @wpack_list wpack list.
 * @wpackp pointer to a wpack pointer. *wpackp can be NULL.
 * @biow bio_wrapper to add.
 * @ring_buffer_size ring buffer size [physical block]
 * @latest_lsidp pointer to the latest_lsid value.
 *   *latest_lsidp must be always (*wpackp)->logpack_lsid.
 * @flush_lsidp pointer to the flush_lsid value.
 *   *flush_lsidp will be updated if the bio is flush request.
 * @log_flush_jiffiesp pointer to the log_flush_jiffies value.
 *   *log_flush_jiffiesp will be updated if the bio is flush request.
 * @wdev wrapper block device.
 * @gfp_mask memory allocation mask.
 *
 * RETURN:
 *   true if successfuly added, or false (due to memory allocation failure).
 * CONTEXT:
 *   serialized.
 */
static bool writepack_add_bio_wrapper(
	struct list_head *wpack_list, struct pack **wpackp,
	struct bio_wrapper *biow,
	u64 ring_buffer_size, unsigned int max_logpack_pb,
	u64 *latest_lsidp, u64 *flush_lsidp, unsigned long *log_flush_jiffiesp,
	struct walb_dev *wdev, gfp_t gfp_mask)
{
	struct pack *pack;
	bool ret;
	unsigned int pbs;
	struct walb_logpack_header *lhead = NULL;

	LOG_("begin\n");

	ASSERT(wpack_list);
	ASSERT(wpackp);
	ASSERT(biow);
	ASSERT(biow->bio);
	ASSERT(biow->bio->bi_rw & REQ_WRITE);
	ASSERT(wdev);
	pbs = wdev->physical_bs;
	ASSERT_PBS(pbs);

	pack = *wpackp;

	if (!pack) {
		goto newpack;
	}

	ASSERT(pack);
	ASSERT(pack->logpack_header_sector);
	ASSERT(pbs == pack->logpack_header_sector->size);
	lhead = get_logpack_header(pack->logpack_header_sector);
	ASSERT(*latest_lsidp == lhead->logpack_lsid);

	if (is_zero_flush_only(pack)) {
		goto newpack;
	}
	if (lhead->n_records > 0 &&
		((biow->bio->bi_rw & REQ_FLUSH)
			|| is_pack_size_too_large(lhead, pbs, max_logpack_pb, biow))) {
		/* Flush request must be the first of the pack. */
		goto newpack;
	}
	if (!walb_logpack_header_add_bio(lhead, biow->bio, pbs, ring_buffer_size)) {
		/* logpack header capacity full so create a new pack. */
		goto newpack;
	}
	if (lhead->n_records > 0) {
		struct walb_log_record *rec = &lhead->record[lhead->n_records - 1];
		ASSERT(rec->offset == biow->pos);
		ASSERT(rec->io_size == biow->len);
		biow->lsid = rec->lsid;
	}
	goto fin;

newpack:
	if (lhead) {
		writepack_check_and_set_flush(pack);
		ASSERT(is_prepared_pack_valid(pack));
		list_add_tail(&pack->list, wpack_list);
		*latest_lsidp = get_next_lsid_unsafe(lhead);
	}
	pack = create_writepack(gfp_mask, pbs, *latest_lsidp);
	if (!pack) { goto error0; }
	*wpackp = pack;
	lhead = get_logpack_header(pack->logpack_header_sector);
	ret = walb_logpack_header_add_bio(lhead, biow->bio, pbs, ring_buffer_size);
	ASSERT(ret);
	if (lhead->n_records > 0) {
		struct walb_log_record *rec = &lhead->record[lhead->n_records - 1];
		ASSERT(rec->offset == biow->pos);
		ASSERT(rec->io_size == biow->len);
		biow->lsid = rec->lsid;
	}
fin:
	/* The request is just added to the pack. */
	list_add_tail(&biow->list, &pack->biow_list);
	if (biow->bio->bi_rw & REQ_FLUSH) {
		pack->is_flush_contained = true;
		if (lhead->n_records > 0 && !bio_wrapper_state_is_discard(biow)) {
			*flush_lsidp = biow->lsid;
		} else {
			*flush_lsidp = *latest_lsidp;
		}
		*log_flush_jiffiesp = jiffies + wdev->log_flush_interval_jiffies;

		/* debug */
		if (bio_wrapper_state_is_discard(biow)) {
			LOGw("The bio has both REQ_FLUSH and REQ_DISCARD.\n");
		}
#ifdef WALB_DEBUG
		atomic_inc(&get_iocored_from_wdev(wdev)->n_flush_io);
#endif
	}
	LOG_("normal end\n");
	return true;
error0:
	LOG_("failure end\n");
	return false;
}

/**
 * Insert a bio wrapper to a sorted bio wrapper list.
 * using insertion sort.
 *
 * They are sorted by biow->pos.
 * Use biow->list4 for list operations.
 *
 * Sort cost is O(n^2) in a worst case,
 * while the cost is O(1) in sequential write.
 *
 * @biow (struct bio_wrapper *)
 * @biow_list (struct list_head *)
 */
static void insert_to_sorted_bio_wrapper_list_by_pos(
	struct bio_wrapper *biow, struct list_head *biow_list)
{
	struct bio_wrapper *biow_tmp, *biow_next;
	bool moved;
#ifdef WALB_DEBUG
	sector_t pos;
#endif

	ASSERT(biow);
	ASSERT(biow_list);

	if (!list_empty(biow_list)) {
		/* last entry. */
		biow_tmp = list_last_entry(biow_list, struct bio_wrapper, list4);
		ASSERT(biow_tmp);
		if (biow->pos > biow_tmp->pos) {
			list_add_tail(&biow->list4, biow_list);
			return;
		}
	}
	moved = false;
	list_for_each_entry_safe_reverse(biow_tmp, biow_next, biow_list, list4) {
		if (biow->pos > biow_tmp->pos) {
			list_add(&biow->list4, &biow_tmp->list4);
			moved = true;
			break;
		}
	}
	if (!moved) {
		list_add(&biow->list4, biow_list);
	}

	/* debug */
#ifdef WALB_DEBUG
	pos = 0;
	/* LOGn("begin\n"); */
	list_for_each_entry_safe(biow_tmp, biow_next, biow_list, list4) {
		/* LOGn("%" PRIu64 "\n", (u64)biow_tmp->pos); */
		ASSERT(pos <= biow_tmp->pos);
		pos = biow_tmp->pos;
	}
	/* LOGn("end\n"); */
#endif
}

/**
 * Check whether wpack is zero-flush-only and set the flag.
 */
static void writepack_check_and_set_flush(struct pack *wpack)
{
	struct walb_logpack_header *logh =
		get_logpack_header(wpack->logpack_header_sector);

	/* Check whether zero-flush-only or not. */
	if (logh->n_records == 0) {
		ASSERT(is_zero_flush_only(wpack));
		wpack->is_zero_flush_only = true;
	}
}

static bool wait_for_logpack_header(struct pack *wpack)
{
	bool success = true;
	struct bio_entry *bioe = wpack->header_bioe;

	wait_for_bio_entry(bioe);
	if (bioe->error)
		success = false;

	destroy_bio_entry(bioe);
	wpack->header_bioe = NULL;
	return success;
}

/**
 * Wait for completion of all bio(s) and enqueue datapack tasks.
 *
 * Request success -> enqueue datapack.
 * Request failure -> all subsequent requests must fail.
 *
 * If any write failed, wdev will be read-only mode.
 */
/* TODO: refactor */
static void wait_for_logpack_and_submit_datapack(
	struct walb_dev *wdev, struct pack *wpack)
{
	struct bio_wrapper *biow, *biow_next;
	bool is_failed = false;
	struct iocore_data *iocored;
	bool is_pending_insert_succeeded;
	bool is_stop_queue = false;

	ASSERT(wpack);
	ASSERT(wdev);

	/* Check read only mode. */
	iocored = get_iocored_from_wdev(wdev);
	if (is_read_only_mode(iocored))
		is_failed = true;

	/* Wait for logpack header or flush IO. */
	is_failed = !wait_for_logpack_header(wpack);

	/* Update permanent_lsid if necessary. */
	if (!is_failed && wpack->is_flush_header) {
		struct walb_logpack_header *logh =
			get_logpack_header(wpack->logpack_header_sector);
		bool should_notice = false;
		spin_lock(&wdev->lsid_lock);
		if (wdev->lsids.permanent < logh->logpack_lsid) {
			should_notice = is_permanent_log_empty(&wdev->lsids);
			wdev->lsids.permanent = logh->logpack_lsid;
			LOG_("log_flush_completed_header\n");
		}
		spin_unlock(&wdev->lsid_lock);
		if (should_notice)
			walb_sysfs_notify(wdev, "lsids");
	}

	/*
	 * For each biow,
	 *   (1) Wait for each log IOs corresponding to the biow.
	 *   (2) Flush request with size zero will be destoroyed.
	 *   (3) Clone the bio and split if necessary.
	 *   (4) Insert cloned bio to the pending data.
	 */
	list_for_each_entry_safe(biow, biow_next, &wpack->biow_list, list) {
		ASSERT(biow->bio);
		wait_for_bio_wrapper(biow, false, true);
		if (is_failed || biow->error) goto error_io;

#ifdef WALB_PERFORMANCE_ANALYSIS
		getnstimeofday(&biow->ts[WALB_TIME_LOG_COMPLETED]);
#endif
		if (biow->len == 0) {
			/* Zero-flush. */
			ASSERT(wpack->is_zero_flush_only);
			ASSERT(biow->bio->bi_rw & REQ_FLUSH);
			list_del(&biow->list);
			set_bit(BIO_UPTODATE, &biow->bio->bi_flags);
			bio_endio(biow->bio, 0);
			destroy_bio_wrapper_dec(wdev, biow);
		} else {
			const bool is_discard = bio_wrapper_state_is_discard(biow);
			if (!is_discard || blk_queue_discard(bdev_get_queue(wdev->ddev))) {
				/* Create all related bio(s) by copying IO data. */
				biow->cloned_bioe = create_bio_entry_by_clone_never_giveup(
					biow->bio, wdev->ddev, GFP_NOIO, true);
			} else {
				/* Do nothing.
				   TODO: should do write zero? */
			}

			if (biow->cloned_bioe) {
				/* Split if required due to chunk limitations. */
				biow->cloned_bio_list =
					split_bio_for_chunk_never_giveup(
						biow->cloned_bioe->bio,
						wdev->ddev_chunk_sectors,
						GFP_NOIO);
			}

			/* Try to insert pending data. */
		retry_insert_pending:
			spin_lock(&iocored->pending_data_lock);
			LOG_("pending_sectors %u\n", iocored->pending_sectors);
			is_stop_queue = should_stop_queue(wdev, biow);
			if (is_discard) {
				/* Discard IO does not have buffer of biow->len bytes.
				   We consider its metadata only. */
				iocored->pending_sectors++;
				is_pending_insert_succeeded = true;
			} else {
				iocored->pending_sectors += biow->len;
				is_pending_insert_succeeded =
					pending_insert_and_delete_fully_overwritten(
						iocored->pending_data,
						&iocored->max_sectors_in_pending,
						biow, GFP_ATOMIC);
			}
			spin_unlock(&iocored->pending_data_lock);
			if (!is_pending_insert_succeeded) {
				spin_lock(&iocored->pending_data_lock);
				if (bio_wrapper_state_is_discard(biow)) {
					iocored->pending_sectors--;
				} else {
					iocored->pending_sectors -= biow->len;
				}
				spin_unlock(&iocored->pending_data_lock);
				schedule();
				goto retry_insert_pending;
			}

			/* Check pending data size and stop the queue if needed. */
			if (is_stop_queue) {
				if (atomic_inc_return(&iocored->n_stoppers) == 1) {
					LOG_("iocore frozen.\n");
				}
			}

			/* call endio here in fast algorithm,
			   while easy algorithm call it after data device IO. */
			io_acct_end(biow);
			set_bit(BIO_UPTODATE, &biow->bio->bi_flags);
			BIO_WRAPPER_PRINT("log1", biow);
			bio_endio(biow->bio, 0);
			biow->bio = NULL;

			bio_wrapper_state_set_prepared(biow);
			BIO_WRAPPER_CHANGE_STATE(biow);

			/* Enqueue submit datapack task. */
			spin_lock(&iocored->datapack_submit_queue_lock);
			list_add_tail(&biow->list2, &iocored->datapack_submit_queue);
			spin_unlock(&iocored->datapack_submit_queue_lock);
		}
		continue;
	error_io:
		is_failed = true;
		set_read_only_mode(iocored);
		LOGe("WalB changes device minor:%u to read-only mode.\n",
			MINOR(wdev->devt));
		bio_endio(biow->bio, -EIO);
		list_del(&biow->list);
		destroy_bio_wrapper_dec(wdev, biow);
	}


	/* Update completed_lsid/permanent_lsid. */
	if (!is_failed) {
		struct walb_logpack_header *logh =
			get_logpack_header(wpack->logpack_header_sector);
		bool should_notice = false;
		spin_lock(&wdev->lsid_lock);
		wdev->lsids.completed = get_next_lsid(logh);
		if (wpack->is_flush_contained &&
			wdev->lsids.permanent < logh->logpack_lsid) {
			should_notice = is_permanent_log_empty(&wdev->lsids);
			wdev->lsids.permanent = logh->logpack_lsid;
			LOG_("log_flush_completed_io\n");
		}
		if (!(wdev->queue->flush_flags & REQ_FLUSH)) {
			/* For flush-not-supportted device. */
			should_notice = is_permanent_log_empty(&wdev->lsids);
			wdev->lsids.flush = get_next_lsid(logh);
			wdev->lsids.permanent = wdev->lsids.flush;
		}
		spin_unlock(&wdev->lsid_lock);
		if (should_notice) {
			walb_sysfs_notify(wdev, "lsids");
		}
	}
}

/**
 * Wait for completion of datapack IO.
 */
static void wait_for_write_bio_wrapper(
	struct walb_dev *wdev, struct bio_wrapper *biow)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);
	bool is_start_queue;
#ifdef WALB_OVERLAPPED_SERIALIZE
	struct bio_wrapper *biow_tmp, *biow_tmp_next;
	unsigned int n_should_submit;
	struct list_head should_submit_list;
	unsigned int c = 0;
	struct blk_plug plug;
#endif
	ASSERT(bio_wrapper_state_is_prepared(biow));
	ASSERT(bio_wrapper_state_is_submitted(biow));
#ifdef WALB_OVERLAPPED_SERIALIZE
	ASSERT(biow->n_overlapped == 0);
#endif

	/* Wait for completion and call end_request. */
	wait_for_bio_wrapper(biow, false, false);

	ASSERT(bio_wrapper_state_is_submitted(biow));
	bio_wrapper_state_set_completed(biow);
	BIO_WRAPPER_PRINT("done", biow);

#ifdef WALB_OVERLAPPED_SERIALIZE
	/* Delete from overlapped detection data. */
	INIT_LIST_HEAD(&should_submit_list);
	spin_lock(&iocored->overlapped_data_lock);
	n_should_submit = overlapped_delete_and_notify(
		iocored->overlapped_data,
		&iocored->max_sectors_in_overlapped,
		&should_submit_list, biow
#ifdef WALB_DEBUG
		, &iocored->overlapped_out_id
#endif
		);
	spin_unlock(&iocored->overlapped_data_lock);

	/* Submit bio wrapper(s) which n_overlapped became 0. */
	if (n_should_submit > 0) {
		blk_start_plug(&plug);
		list_for_each_entry_safe(biow_tmp, biow_tmp_next,
					&should_submit_list, list4) {
			const bool is_plug = false;
			ASSERT(biow_tmp->n_overlapped == 0);
			ASSERT(bio_wrapper_state_is_delayed(biow_tmp));
			ASSERT(biow_tmp != biow);
			list_del(&biow_tmp->list4);
#if 0
			LOGn("submit overlapped biow %p pos %" PRIu64 " len %u\n",
				biow_tmp, (u64)biow_tmp->pos, biow_tmp->len); /* debug */
#endif
			c++;
			BIO_WRAPPER_PRINT("data1", biow);
			submit_write_bio_wrapper(biow_tmp, is_plug);
		}
		blk_finish_plug(&plug);
	}
	ASSERT(c == n_should_submit);
	ASSERT(list_empty(&should_submit_list));
#endif

	/* Delete from pending data. */
	spin_lock(&iocored->pending_data_lock);
	is_start_queue = should_start_queue(wdev, biow);
	if (bio_wrapper_state_is_discard(biow)) {
		iocored->pending_sectors--;
	} else {
		iocored->pending_sectors -= biow->len;
		if (!bio_wrapper_state_is_overwritten(biow)) {
			pending_delete(iocored->pending_data,
				&iocored->max_sectors_in_pending, biow);
		}
	}
	spin_unlock(&iocored->pending_data_lock);
	if (is_start_queue)
		iocore_melt(wdev);

	/* Put related bio(s) and free resources. */
	if (biow->cloned_bioe) {
		destroy_bio_entry(biow->cloned_bioe);
		biow->cloned_bioe = NULL;
	} else {
		ASSERT(bio_wrapper_state_is_discard(biow));
		ASSERT(!blk_queue_discard(bdev_get_queue(wdev->ddev)));
	}
}

/**
 * Wait for completion of cloned_bioe related to a bio_wrapper.
 * and call bio_endio()/io_acct_end(), delete cloned_bioe if required.
 *
 * @biow target bio_wrapper.
 *   Do not assume biow->bio is available when is_endio is false.
 * @is_endio
 *   if true, call bio_endio() and io_acct_end() for biow->bio.
 * @is_delete
 *   destroy biow->cloned_bioe.
 *
 * CONTEXT:
 *   non-irq. non-atomic.
 */
static void wait_for_bio_wrapper(struct bio_wrapper *biow, bool is_endio, bool is_delete)
{
	ASSERT(biow);
	ASSERT(biow->error == 0);

	if (biow->cloned_bioe) {
		wait_for_bio_entry(biow->cloned_bioe);
		biow->error = biow->cloned_bioe->error;
	} else {
		ASSERT(biow->len == 0 || bio_wrapper_state_is_discard(biow));
	}

	if (is_endio) {
		ASSERT(biow->bio);
		if (!biow->error)
			set_bit(BIO_UPTODATE, &biow->bio->bi_flags);

		BIO_WRAPPER_PRINT_CSUM("read2", biow);
		io_acct_end(biow);
		bio_endio(biow->bio, biow->error);
		biow->bio = NULL;
	}
	if (is_delete) {
		destroy_bio_entry(biow->cloned_bioe);
		biow->cloned_bioe = NULL;
	}
}

/**
 * Submit data io.
 */
static void submit_write_bio_wrapper(struct bio_wrapper *biow, bool is_plugging)
{
#ifdef WALB_DEBUG
	struct walb_dev *wdev = biow->private_data;
#endif
	struct blk_plug plug;

#ifdef WALB_OVERLAPPED_SERIALIZE
	ASSERT(biow->n_overlapped == 0);
#endif

	ASSERT(bio_wrapper_state_is_prepared(biow));
	bio_wrapper_state_set_submitted(biow);

#ifdef WALB_DEBUG
	if (bio_wrapper_state_is_discard(biow) &&
		!blk_queue_discard(bdev_get_queue(wdev->ddev))) {
		/* Data device does not support REQ_DISCARD. */
		ASSERT(!biow->cloned_bioe);
	} else {
		ASSERT(biow->cloned_bioe);
		ASSERT(!bio_list_empty(&biow->cloned_bio_list));
	}
#endif
	/* Submit all related bio(s). */
	if (is_plugging)
		blk_start_plug(&plug);

	LOG_("submit_lr: bioe %p pos %" PRIu64 " len %u\n"
		, bioe, bioe->pos, bioe->len);
	submit_all_bio_list(&biow->cloned_bio_list);

	if (is_plugging)
		blk_finish_plug(&plug);

#ifdef WALB_PERFORMANCE_ANALYSIS
	getnstimeofday(&biow->ts[WALB_TIME_DATA_SUBMITTED]);
#endif
}

/**
 * Submit bio wrapper for read.
 *
 * @wdev walb device.
 * @biow bio wrapper (read).
 */
static void submit_read_bio_wrapper(
	struct walb_dev *wdev, struct bio_wrapper *biow)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);
	bool ret;
	struct bio_entry *bioe;
	struct bio_list *bio_list = &biow->cloned_bio_list;

	ASSERT(bio_list_empty(bio_list));

	/* Create cloned bio. */
	bioe = create_bio_entry_by_clone(
		biow->bio, wdev->ddev, GFP_NOIO, false);
	if (!bioe)
		goto error0;

	ASSERT(!biow->cloned_bioe);
	biow->cloned_bioe = bioe;

	/* Split if required due to chunk limitations. */
	if (!split_bio_for_chunk(
			bio_list, bioe->bio,
			wdev->ddev_chunk_sectors, GFP_NOIO))
		goto error1;

	/* Check pending data and copy data from executing write requests. */
	BIO_WRAPPER_PRINT_LS("read0", biow, bio_list_size(bio_list));
	spin_lock(&iocored->pending_data_lock);
	ret = pending_check_and_copy(
		iocored->pending_data,
		iocored->max_sectors_in_pending, biow, GFP_ATOMIC);
	spin_unlock(&iocored->pending_data_lock);
	if (!ret)
		goto error1;

	/* Submit all related bio(s). */
	LOG_("submit_lr: bioe %p pos %" PRIu64 " len %u\n"
		, bioe, bioe->pos, bioe->len);
	BIO_WRAPPER_PRINT_LS("read1", biow, bio_list_size(bio_list));
	/* TODO: if bio_list is empty,
	   we need not delay to call bio_endio and gc it. */
	submit_all_bio_list(bio_list);

	/* Enqueue wait/gc task. */
	INIT_WORK(&biow->work, task_wait_and_gc_read_bio_wrapper);
	queue_work(wq_unbound_, &biow->work);
	return;

error1:
	put_all_bio_list(bio_list);
	bioe->bio = NULL;
	destroy_bio_entry(bioe);
	biow->cloned_bioe = NULL;
error0:
	bio_endio(biow->bio, -ENOMEM);
	destroy_bio_wrapper_dec(wdev, biow);
}

/**
 * Submit a flush request.
 *
 * @bdev block device.
 *
 * RETURN:
 *   created bioe containing submitted bio in success, or NULL.
 * CONTEXT:
 *   non-atomic.
 */
static struct bio_entry* submit_flush(struct block_device *bdev)
{
	struct bio_entry *bioe;
	struct bio *bio;

	bioe = alloc_bio_entry(GFP_NOIO);
	if (!bioe) { goto error0; }

	bio = bio_alloc(GFP_NOIO, 0);
	if (!bio) { goto error1; }

	bio->bi_end_io = bio_entry_end_io;
	bio->bi_private = bioe;
	bio->bi_bdev = bdev;
	bio->bi_rw = WRITE_FLUSH;

	init_bio_entry(bioe, bio);
	ASSERT(bio_entry_len(bioe) == 0);

	generic_make_request(bio);

	return bioe;
error1:
	destroy_bio_entry(bioe);
error0:
	return NULL;
}

/**
 * Enqueue logpack submit task if necessary.
 */
static void enqueue_submit_task_if_necessary(struct walb_dev *wdev)
{
	enqueue_task_if_necessary(
		wdev,
		IOCORE_STATE_SUBMIT_LOG_TASK_WORKING,
		&get_iocored_from_wdev(wdev)->flags,
		wq_unbound_,
		task_submit_logpack_list);
}

/**
 * Enqueue wait task if necessary.
 */
static void enqueue_wait_task_if_necessary(struct walb_dev *wdev)
{
	enqueue_task_if_necessary(
		wdev,
		IOCORE_STATE_WAIT_LOG_TASK_WORKING,
		&get_iocored_from_wdev(wdev)->flags,
		wq_unbound_,
		task_wait_for_logpack_list);
}

/**
 * Enqueue datapack submit task if necessary.
 */
static void enqueue_submit_data_task_if_necessary(struct walb_dev *wdev)
{
	enqueue_task_if_necessary(
		wdev,
		IOCORE_STATE_SUBMIT_DATA_TASK_WORKING,
		&get_iocored_from_wdev(wdev)->flags,
		wq_unbound_, /* QQQ: should be normal? */
		task_submit_bio_wrapper_list);
}

/**
 * Enqueue datapack wait task if necessary.
 */
static void enqueue_wait_data_task_if_necessary(struct walb_dev *wdev)
{
	enqueue_task_if_necessary(
		wdev,
		IOCORE_STATE_WAIT_DATA_TASK_WORKING,
		&get_iocored_from_wdev(wdev)->flags,
		wq_unbound_,
		task_wait_for_bio_wrapper_list);
}

/**
 * Start to processing write bio_wrapper.
 */
static void start_write_bio_wrapper(
	struct walb_dev *wdev, struct bio_wrapper *biow)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);
	ASSERT(biow);

	if (test_and_set_bit(BIO_WRAPPER_STARTED, &biow->flags))
		BUG();

	atomic_inc(&iocored->n_started_write_bio);
}

/**
 * Wait for all data write IO(s) done.
 */
static void wait_for_all_started_write_io_done(struct walb_dev *wdev)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);

	while (atomic_read(&iocored->n_started_write_bio) > 0) {
		LOGi("n_started_write_bio %d\n",
			atomic_read(&iocored->n_started_write_bio));
		msleep(100);
	}
	LOGi("n_started_write_bio %d\n",
		atomic_read(&iocored->n_started_write_bio));
}

/**
 * Wait for all gc task done.
 */
static void wait_for_all_pending_gc_done(struct walb_dev *wdev)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);

	while (atomic_read(&iocored->n_pending_gc) > 0) {
		LOGi("n_pending_gc %d\n",
			atomic_read(&iocored->n_pending_gc));
		msleep(100);
	}
	LOGi("n_pending_gc %d\n", atomic_read(&iocored->n_pending_gc));
}

/**
 * Wait for all logs permanent which lsid <= specified 'lsid'.
 *
 * We must confirm the corresponding log has been permanent
 * before submitting data IOs.
 *
 * Do nothing if wdev->log_flush_interval_jiffies is 0,
 * In such case, WalB device concistency is not be kept.
 * Set log_flush_interval_jiffies to 0 for test only.
 *
 * @wdev walb device.
 * @lsid threshold lsid.
 */
static void wait_for_log_permanent(struct walb_dev *wdev, u64 lsid)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);
	u64 permanent_lsid, flush_lsid, latest_lsid;
	unsigned long log_flush_jiffies, timeout_jiffies;
	int err;
	bool should_notice = false;

	if (wdev->log_flush_interval_jiffies == 0) {
		return;
	}
	/* We will wait for log flush at most the given interval period. */
	timeout_jiffies = jiffies + wdev->log_flush_interval_jiffies;
retry:
	spin_lock(&wdev->lsid_lock);
	permanent_lsid = wdev->lsids.permanent;
	flush_lsid = wdev->lsids.flush;
	latest_lsid = wdev->lsids.latest;
	log_flush_jiffies = iocored->log_flush_jiffies;
	spin_unlock(&wdev->lsid_lock);
	if (lsid < permanent_lsid) {
		/* No need to wait. */
		return;
	}
	if (time_is_after_jiffies(timeout_jiffies) &&
		lsid < flush_lsid + wdev->log_flush_interval_pb &&
		time_is_after_jiffies(log_flush_jiffies)) {
		/* Too early to force flush log device.
		   Wait for a while. */
		msleep(1);
		goto retry;
	}

	/* We must flush log device. */
	LOG_("lsid %"PRIu64""
		" flush_lsid %"PRIu64""
		" permanent_lsid %"PRIu64"\n",
		lsid, flush_lsid, permanent_lsid);

	/* Update flush_lsid. */
	spin_lock(&wdev->lsid_lock);
	latest_lsid = wdev->lsids.latest;
	if (wdev->lsids.flush < latest_lsid) {
		wdev->lsids.flush = latest_lsid;
		iocored->log_flush_jiffies =
			jiffies + wdev->log_flush_interval_jiffies;
	}
	spin_unlock(&wdev->lsid_lock);

	/* Execute a flush request. */
	err = blkdev_issue_flush(wdev->ldev, GFP_NOIO, NULL);
	if (err) {
		LOGe("log device flush failed. to be read-only mode\n");
		set_read_only_mode(iocored);
	}

#ifdef WALB_DEBUG
	atomic_inc(&get_iocored_from_wdev(wdev)->n_flush_force);
#endif

	/* Update permanent_lsid. */
	spin_lock(&wdev->lsid_lock);
	if (wdev->lsids.permanent < latest_lsid) {
		should_notice = is_permanent_log_empty(&wdev->lsids);
		wdev->lsids.permanent = latest_lsid;
		LOG_("log_flush_completed_data\n");
	}
	ASSERT(lsid <= wdev->lsids.permanent);
	spin_unlock(&wdev->lsid_lock);
	if (should_notice) {
		walb_sysfs_notify(wdev, "lsids");
	}
}

/**
 * Flush all workqueues for IO.
 */
static void flush_all_wq(void)
{
	flush_workqueue(wq_normal_);
	flush_workqueue(wq_unbound_);
}

/**
 * Clear working bit.
 */
static void clear_working_flag(int working_bit, unsigned long *flag_p)
{
	int ret;
	ret = test_and_clear_bit(working_bit, flag_p);
	ASSERT(ret);
}

/**
 * Invoke the userland executable binary.
 * @event_str event string. It must be zero-terminatd.
 */
static void invoke_userland_exec(struct walb_dev *wdev, const char *event_str)
{
	size_t len;
	int ret;
	/* uint max is "4294967295\0" so size 11 is enough. */
	const int UINT_STR_LEN = 11;
	char major_str[UINT_STR_LEN];
	char minor_str[UINT_STR_LEN];
	char *argv[] = { exec_path_on_error_, NULL, NULL, NULL, NULL };
	char *envp[] = { "HOME=/", "TERM=linux", "PATH=/bin:/usr/bin:/sbin:/usr/sbin", NULL };

	len = strnlen(exec_path_on_error_, EXEC_PATH_ON_ERROR_LEN);
	if (len == 0 || len == EXEC_PATH_ON_ERROR_LEN) { return; }

	ASSERT(wdev);
	ret = snprintf(major_str, UINT_STR_LEN, "%u", MAJOR(wdev->devt));
	ASSERT(ret < UINT_STR_LEN);
	ret = snprintf(minor_str, UINT_STR_LEN, "%u", MINOR(wdev->devt));
	ASSERT(ret < UINT_STR_LEN);

	argv[1] = major_str;
	argv[2] = minor_str;
	argv[3] = (char *)event_str;

	ret = call_usermodehelper(exec_path_on_error_, argv, envp, UMH_WAIT_EXEC);
	if (ret) {
		LOGe("Execute userland command failed: %s %s %s %s\n"
			, exec_path_on_error_
			, major_str
			, minor_str
			, event_str);
	}
}

/**
 * Fail all bios in specified bio wrapper list and destroy them.
 */
static void fail_and_destroy_bio_wrapper_list(
	struct walb_dev *wdev, struct list_head *biow_list)
{
	struct bio_wrapper *biow, *biow_next;
	list_for_each_entry_safe(biow, biow_next, biow_list, list) {
		bio_endio(biow->bio, -EIO);
		list_del(&biow->list);
		destroy_bio_wrapper_dec(wdev, biow);
	}
	ASSERT(list_empty(biow_list));
}

/**
 * Check whether walb should stop the queue
 * due to too much pending data.
 *
 * CONTEXT:
 *   pending_data_lock must be held.
 */
static bool should_stop_queue(
	struct walb_dev *wdev, struct bio_wrapper *biow)
{
	bool should_stop;
	struct iocore_data *iocored;

	ASSERT(wdev);
	ASSERT(biow);
	iocored = get_iocored_from_wdev(wdev);
	ASSERT(iocored);

	if (iocored->is_under_throttling) {
		return false;
	}

	should_stop = iocored->pending_sectors + biow->len
		> wdev->max_pending_sectors;

	if (should_stop) {
		iocored->queue_restart_jiffies =
			jiffies + wdev->queue_stop_timeout_jiffies;
		iocored->is_under_throttling = true;
		return true;
	} else {
		return false;
	}
}

/**
 * Check whether walb should restart the queue
 * because pending data is not too much now.
 *
 * CONTEXT:
 *   pending_data_lock must be held.
 */
static bool should_start_queue(
	struct walb_dev *wdev, struct bio_wrapper *biow)
{
	bool is_size;
	bool is_timeout;
	struct iocore_data *iocored;

	ASSERT(wdev);
	ASSERT(biow);
	iocored = get_iocored_from_wdev(wdev);
	ASSERT(iocored);

	if (!iocored->is_under_throttling) {
		return false;
	}

	if (iocored->pending_sectors >= biow->len) {
		is_size = iocored->pending_sectors - biow->len
			< wdev->min_pending_sectors;
	} else {
		is_size = true;
	}

	is_timeout = time_is_before_jiffies(iocored->queue_restart_jiffies);

	if (is_size || is_timeout) {
		iocored->is_under_throttling = false;
		return true;
	} else {
		return false;
	}
}

/**
 * Increment n_users of treemap memory manager and
 * iniitialize mmgr_ if necessary.
 */
static bool treemap_memory_manager_get(void)
{
	bool ret;

	if (atomic_inc_return(&n_users_of_memory_manager_) == 1) {
		ret = initialize_treemap_memory_manager(
			&mmgr_, N_ITEMS_IN_MEMPOOL,
			TREE_NODE_CACHE_NAME,
			TREE_CELL_HEAD_CACHE_NAME,
			TREE_CELL_CACHE_NAME);
		if (!ret) { goto error; }
	}
	return true;
error:
	atomic_dec(&n_users_of_memory_manager_);
	return false;
}

/**
 * Decrement n_users of treemap memory manager and
 * finalize mmgr_ if necessary.
 */
static void treemap_memory_manager_put(void)
{
	if (atomic_dec_return(&n_users_of_memory_manager_) == 0) {
		finalize_treemap_memory_manager(&mmgr_);
	}
}

static bool pack_cache_get(void)
{
	if (atomic_inc_return(&n_users_of_pack_cache_) == 1) {
		pack_cache_ = kmem_cache_create(
			KMEM_CACHE_PACK_NAME,
			sizeof(struct pack), 0, 0, NULL);
		if (!pack_cache_) {
			goto error;
		}
	}
	return true;
error:
	atomic_dec(&n_users_of_pack_cache_);
	return false;
}

static void pack_cache_put(void)
{
	if (atomic_dec_return(&n_users_of_pack_cache_) == 0) {
		kmem_cache_destroy(pack_cache_);
		pack_cache_ = NULL;
	}
}

static void io_acct_start(struct bio_wrapper *biow)
{
	int cpu;
	int rw = bio_data_dir(biow->bio);
	struct walb_dev *wdev = biow->private_data;
	struct hd_struct *part0 = &wdev->gd->part0;

	biow->start_time = jiffies;

	cpu = part_stat_lock();
	part_round_stats(cpu, part0);
	part_stat_inc(cpu, part0, ios[rw]);
	part_stat_add(cpu, part0, sectors[rw], biow->len);
	part_inc_in_flight(part0, rw);
	part_stat_unlock();
}

static void io_acct_end(struct bio_wrapper *biow)
{
	int cpu;
	int rw = bio_data_dir(biow->bio);
	struct walb_dev *wdev = biow->private_data;
	struct hd_struct *part0 = &wdev->gd->part0;
	unsigned long duration = jiffies - biow->start_time;

	cpu = part_stat_lock();
	part_round_stats(cpu, part0);
	part_stat_add(cpu, part0, ticks[rw], duration);
	part_dec_in_flight(part0, rw);
	part_stat_unlock();
}

/*******************************************************************************
 * Global functions implementation.
 *******************************************************************************/

/**
 * Initialize iocore data for a wdev.
 */
bool iocore_initialize(struct walb_dev *wdev)
{
	int ret;
	struct iocore_data *iocored;

	if (!treemap_memory_manager_get()) {
		LOGe("Treemap memory manager inc failed.\n");
		goto error0;
	}

	if (!pack_cache_get()) {
		LOGe("Failed to create a kmem_cache for pack.\n");
		goto error1;
	}

	if (!bio_entry_init()) {
		LOGe("Failed to init bio_entry.\n");
		goto error2;
	}

	if (!bio_wrapper_init()) {
		LOGe("Failed to init bio_wrapper.\n");
		goto error3;
	}

	if (!pack_work_init()) {
		LOGe("Failed to init pack_work.\n");
		goto error4;
	}

	iocored = create_iocore_data(GFP_KERNEL);
	if (!iocored) {
		LOGe("Memory allocation failed.\n");
		goto error5;
	}
	wdev->private_data = iocored;

	/* Decide gc worker name and start it. */
	ret = snprintf(iocored->gc_worker_data.name, WORKER_NAME_MAX_LEN,
		"%s/%u", WORKER_NAME_GC, MINOR(wdev->devt) / 2);
	if (ret >= WORKER_NAME_MAX_LEN) {
		LOGe("Thread name size too long.\n");
		goto error6;
	}
	initialize_worker(&iocored->gc_worker_data,
			run_gc_logpack_list, (void *)wdev);

	return true;

#if 0
error7:
	finalize_worker(&iocored->gc_worker_data);
#endif
error6:
	destroy_iocore_data(iocored);
	wdev->private_data = NULL;
error5:
	pack_work_exit();
error4:
	bio_wrapper_exit();
error3:
	bio_entry_exit();
error2:
	pack_cache_put();
error1:
	treemap_memory_manager_put();
error0:
	return false;
}

/**
 * Finalize iocore data for a wdev.
 */
void iocore_finalize(struct walb_dev *wdev)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);

#ifdef WALB_DEBUG
	int n_flush_io, n_flush_logpack, n_flush_force;
	n_flush_io = atomic_read(&iocored->n_flush_io);
	n_flush_logpack = atomic_read(&iocored->n_flush_logpack);
	n_flush_force = atomic_read(&iocored->n_flush_force);
#endif

	finalize_worker(&iocored->gc_worker_data);
	destroy_iocore_data(iocored);
	wdev->private_data = NULL;

	pack_work_exit();
	bio_wrapper_exit();
	bio_entry_exit();
	pack_cache_put();
	treemap_memory_manager_put();

#ifdef WALB_DEBUG
	LOGn("n_allocated_pages: %u\n"
		"n_flush_io: %d\n"
		"n_flush_logpack: %d\n"
		"n_flush_force: %d\n",
		bio_entry_get_n_allocated_pages(),
		n_flush_io, n_flush_logpack, n_flush_force);
#endif
}

/**
 * Stop (write) IO processing.
 *
 * After stopped, there is no IO pending underlying
 * data/log devices.
 * Upper layer can submit IOs but the walb driver
 * just queues them and does not start processing during stopped.
 */
void iocore_freeze(struct walb_dev *wdev)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);

	might_sleep();

	if (atomic_inc_return(&iocored->n_stoppers) == 1) {
		LOGi("iocore frozen [%u:%u].\n"
			, MAJOR(wdev->devt), MINOR(wdev->devt));
	}

	/* Wait for all started write io done. */
	wait_for_all_started_write_io_done(wdev);

	/* Wait for all pending gc task done
	   which update wdev->written_lsid. */
	wait_for_all_pending_gc_done(wdev);
}

/**
 * (Re)start (write) IO processing.
 */
void iocore_melt(struct walb_dev *wdev)
{
	struct iocore_data *iocored;

	might_sleep();
	iocored = get_iocored_from_wdev(wdev);

	if (atomic_dec_return(&iocored->n_stoppers) == 0) {
		LOGi("iocore melted. [%u:%u]\n"
			, MAJOR(wdev->devt), MINOR(wdev->devt));
		enqueue_submit_task_if_necessary(wdev);
	}
}

/**
 * Make request.
 */
void iocore_make_request(struct walb_dev *wdev, struct bio *bio)
{
	struct bio_wrapper *biow;
	int error = -EIO;
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);
	unsigned long is_write = bio->bi_rw & REQ_WRITE;

	/* Failure/Read-only state check. */
	if (test_bit(IOCORE_STATE_FAILURE, &iocored->flags) ||
		(is_write && is_read_only_mode(iocored))) {
		error = -EIO;
		goto error0;
	}

	/* Create bio wrapper. */
	biow = alloc_bio_wrapper_inc(wdev, GFP_NOIO);
	if (!biow) {
		error = -ENOMEM;
		goto error0;
	}
	init_bio_wrapper(biow, bio);
	biow->private_data = wdev;

	/* IO accounting for diskstats. */
	io_acct_start(biow);

	if (is_write) {
#ifdef WALB_PERFORMANCE_ANALYSIS
		getnstimeofday(&biow->ts[WALB_TIME_BEGIN]);
#endif
		/* Calculate checksum. */
		biow->csum = bio_calc_checksum(
			biow->bio, wdev->log_checksum_salt);

		/* Push into queue. */
		spin_lock(&iocored->logpack_submit_queue_lock);
		list_add_tail(&biow->list, &iocored->logpack_submit_queue);
		spin_unlock(&iocored->logpack_submit_queue_lock);

		/* Enqueue logpack-submit task. */
		if (atomic_read(&iocored->n_stoppers) == 0) {
			enqueue_submit_task_if_necessary(wdev);
		}
	} else {
		submit_read_bio_wrapper(wdev, biow);

		/* TODO: support IOCORE_STATE_QUEUE_STOPPED for read also. */
	}
	return;
#if 0
error1:
	destroy_bio_wrapper_dec(wdev, biow);
#endif
error0:
	bio_endio(bio, error);
}

/**
 * Make request for wrapper log device.
 */
void iocore_log_make_request(struct walb_dev *wdev, struct bio *bio)
{
	if (bio->bi_rw & WRITE) {
		bio_endio(bio, -EIO);
	} else {
		bio->bi_bdev = wdev->ldev;
		generic_make_request(bio);
	}
}

/**
 * Wait for all pending IO(s) for underlying data/log devices.
 */
void iocore_flush(struct walb_dev *wdev)
{
	wait_for_all_pending_io_done(wdev);
	flush_all_wq();
}

/**
 * Set read-only mode.
 */
void iocore_set_readonly(struct walb_dev *wdev)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);

	set_read_only_mode(iocored);
}

/**
 * Check read-only mode.
 */
bool iocore_is_readonly(struct walb_dev *wdev)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);

	return is_read_only_mode(iocored);
}

/**
 * Set failure mode.
 */
void iocore_set_failure(struct walb_dev *wdev)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);

	set_bit(IOCORE_STATE_FAILURE, &iocored->flags);
}

/**
 * Clear ring buffer overflow state bit.
 */
void iocore_clear_log_overflow(struct walb_dev *wdev)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);

	clear_bit(IOCORE_STATE_LOG_OVERFLOW, &iocored->flags);
}

/**
 * Check ring buffer has been overflow.
 */
bool iocore_is_log_overflow(struct walb_dev *wdev)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);

	return test_bit(IOCORE_STATE_LOG_OVERFLOW, &iocored->flags);
}

/**
 * Wait for all pending IO(s) done.
 */
void wait_for_all_pending_io_done(struct walb_dev *wdev)
{
	struct iocore_data *iocored = get_iocored_from_wdev(wdev);

	while (atomic_read(&iocored->n_pending_bio) > 0) {
		LOGn("n_pending_bio %d\n",
			atomic_read(&iocored->n_pending_bio));
		msleep(100);
	}
	LOGn("n_pending_bio %d\n", atomic_read(&iocored->n_pending_bio));
}

/**
 * Allocate a bio wrapper and increment
 * n_pending_read_bio or n_pending_write_bio.
 */
struct bio_wrapper* alloc_bio_wrapper_inc(
	struct walb_dev *wdev, gfp_t gfp_mask)
{
	struct bio_wrapper *biow;
	struct iocore_data *iocored;

	ASSERT(wdev);
	iocored = get_iocored_from_wdev(wdev);
	ASSERT(iocored);

	biow = alloc_bio_wrapper(gfp_mask);
	if (!biow) { return NULL; }

	atomic_inc(&iocored->n_pending_bio);
	clear_bit(BIO_WRAPPER_STARTED, &biow->flags);

	return biow;
}

/**
 * Destroy a bio wrapper and decrement n_pending_bio.
 */
void destroy_bio_wrapper_dec(
	struct walb_dev *wdev, struct bio_wrapper *biow)
{
	struct iocore_data *iocored;
	bool started;

	ASSERT(wdev);
	iocored = get_iocored_from_wdev(wdev);
	ASSERT(iocored);
	ASSERT(biow);

	started = bio_wrapper_state_is_started(biow);
	destroy_bio_wrapper(biow);

	atomic_dec(&iocored->n_pending_bio);
	if (started) {
		atomic_dec(&iocored->n_started_write_bio);
	}
}

/**
 * Make request.
 */
void walb_make_request(struct request_queue *q, struct bio *bio)
{
	struct walb_dev *wdev = get_wdev_from_queue(q);
	iocore_make_request(wdev, bio);
}

/**
 * Walblog device make request.
 *
 * 1. Completion with error if write.
 * 2. Just forward to underlying log device if read.
 *
 * @q request queue.
 * @bio bio.
 */
void walblog_make_request(struct request_queue *q, struct bio *bio)
{
	struct walb_dev *wdev = get_wdev_from_queue(q);
	iocore_log_make_request(wdev, bio);
}

MODULE_LICENSE("Dual BSD/GPL");
