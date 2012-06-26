/**
 * simple_blk_bio.c - Simple block device with bio interface.
 *
 * Copyright(C) 2012, Cybozu Labs, Inc.
 * @author HOSHINO Takashi <hoshino@labs.cybozu.co.jp>
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include "walb/block_size.h"
#include "size_list.h"
#include "simple_blk.h"
#include "simple_blk_bio.h"

/*******************************************************************************
 * Module variables definition.
 *******************************************************************************/

/* Device size list string. The unit of each size is bytes. */
char *device_size_list_str_ = "1m";
/* Minor id start. */
int start_minor_ = 0;

/* Logical block size is 512. */
#define LOGICAL_BLOCK_SIZE 512
/* Physical block size. */
int physical_block_size_ = 4096;

/* Number of devices. */
unsigned int n_devices_ = 0;

/**
 * IO workqueue type.
 *
 * "normal", "single", "unbound"
 *
 **/
char *wq_io_type_str_ = "normal";

/*******************************************************************************
 * Module parameters definition.
 *******************************************************************************/

module_param_named(device_size_list, device_size_list_str_, charp, S_IRUGO);
module_param_named(start_minor, start_minor_, int, S_IRUGO);
module_param_named(pbs, physical_block_size_, int, S_IRUGO);
module_param_named(wq_io_type, wq_io_type_str_, charp, S_IRUGO);

/*******************************************************************************
 * Static data definition.
 *******************************************************************************/

static enum workqueue_type wq_io_type_;

/*******************************************************************************
 * Static functions prototype.
 *******************************************************************************/

static unsigned int get_minor(unsigned int id);
static bool register_alldevs(void);
static void unregister_alldevs(void);
static bool start_alldevs(void);
static void stop_alldevs(void);
static void set_workqueue_type(void);

/*******************************************************************************
 * Static functions definition.
 *******************************************************************************/

static unsigned int get_minor(unsigned int id)
{
        return (unsigned int)start_minor_ + id;
}

static bool register_alldevs(void)
{
        unsigned int i;
        u64 capacity;
        bool ret;
        struct simple_blk_dev *sdev;

        for (i = 0; i < n_devices_; i ++) {
                capacity = sizlist_nth_size(device_size_list_str_, i)
                        / LOGICAL_BLOCK_SIZE;
                ASSERT(capacity > 0);

                ret = sdev_register_with_bio(get_minor(i), capacity, physical_block_size_,
					simple_blk_bio_make_request);

                if (!ret) {
                        goto error;
                }
                sdev = sdev_get(get_minor(i));
                if (!create_private_data(sdev)) {
                        goto error;
                }
                customize_sdev(sdev);
        }
        return true;
error:
        unregister_alldevs();
        return false;
}

static void unregister_alldevs(void)
{
        unsigned int i;
        struct simple_blk_dev *sdev;
        
        ASSERT(n_devices_ > 0);
        
        for (i = 0; i < n_devices_; i ++) {

                sdev = sdev_get(get_minor(i));
                if (sdev) {
                        destroy_private_data(sdev);
                }
                sdev_unregister(get_minor(i));
        }
}

static bool start_alldevs(void)
{
        unsigned int i;

        ASSERT(n_devices_ > 0);
        for (i = 0; i < n_devices_; i ++) {
                if (!sdev_start(get_minor(i))) {
                        goto error;
                }
        }
        return true;
error:
        stop_alldevs();
        return false;
}

static void stop_alldevs(void)
{
        unsigned int i;
        ASSERT(n_devices_ > 0);
        
        for (i = 0; i < n_devices_; i ++) {
                sdev_stop(get_minor(i));
        }
}

/**
 * Work struct for finalizer.
 */
struct fin_work
{
	struct work_struct work;
	unsigned long done;
};

/**
 * Finalization Task.
 */
static void stop_alldevs_task(struct work_struct *work)
{
	struct fin_work *fwork = container_of(work, struct fin_work, work);
	stop_alldevs();
	set_bit(0, &fwork->done);
}

/**
 * Finalizer in atomic context.
 */
static void stop_alldevs_atomic(void)
{
	struct fin_work fwork;
	INIT_WORK(&fwork.work, stop_alldevs_task);
	fwork.done = 0;
	schedule_work(&fwork.work);

	while (!test_bit(0, &fwork.done)); /* busy loop wait. */
}


static void set_workqueue_type(void)
{
	if (strcmp(wq_io_type_str_, "single") == 0) {
		wq_io_type_ = WQ_TYPE_SINGLE;
		LOGn("wq_io_type: single\n");
	} else if (strcmp(wq_io_type_str_, "unbound") == 0) {
		wq_io_type_ = WQ_TYPE_UNBOUND;
		LOGn("wq_io_type: unbound\n");
	} else {
		wq_io_type_ = WQ_TYPE_NORMAL;
		LOGn("wq_io_type: normal\n");
	}
}

/*******************************************************************************
 * Global functions definition.
 *******************************************************************************/

enum workqueue_type get_workqueue_type(void)
{
	return wq_io_type_;
}

/*******************************************************************************
 * Init/exit definition.
 *******************************************************************************/

static int __init simple_blk_init(void)
{
	set_workqueue_type();

	if (!is_valid_pbs(physical_block_size_)) {
		goto error0;
	}
	
        n_devices_ = sizlist_length(device_size_list_str_);
        ASSERT(n_devices_ > 0);
        ASSERT(start_minor_ >= 0);

        pre_register();
        
        if (!register_alldevs()) {
                goto error0;
        }
        if (!start_alldevs()) {
                goto error1;
        }

        return 0;
#if 0
error2:
        stop_alldevs();
#endif
error1:
        unregister_alldevs();
error0:
        return -1;
}

static void simple_blk_exit(void)
{
        stop_alldevs_atomic();
        unregister_alldevs();
        post_unregister();
}

module_init(simple_blk_init);
module_exit(simple_blk_exit);
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("Simple block bio device for Test");
MODULE_ALIAS("simple_blk_bio");
