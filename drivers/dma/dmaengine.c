/*
 * Copyright(c) 2004 - 2006 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in the
 * file called COPYING.
 */

/*
 * This code implements the DMA subsystem. It provides a HW-neutral interface
 * for other kernel code to use asynchronous memory copy capabilities,
 * if present, and allows different HW DMA drivers to register as providing
 * this capability.
 *
 * Due to the fact we are accelerating what is already a relatively fast
 * operation, the code goes to great lengths to avoid additional overhead,
 * such as locking.
 *
 * LOCKING:
 *
 * The subsystem keeps a global list of dma_device structs it is protected by a
 * mutex, dma_list_mutex.
 *
 * A subsystem can get access to a channel by calling dmaengine_get() followed
 * by dma_find_channel(), or if it has need for an exclusive channel it can call
 * dma_request_channel().  Once a channel is allocated a reference is taken
 * against its corresponding driver to disable removal.
 *
 * Each device has a channels list, which runs unlocked but is never modified
 * once the device is registered, it's just setup by the driver.
 *
 * See Documentation/dmaengine.txt for more details
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/hardirq.h>
#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/rcupdate.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/rculist.h>
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/acpi.h>
#include <linux/acpi_dma.h>
#include <linux/of_dma.h>
#include <linux/mempool.h>

static DEFINE_MUTEX(dma_list_mutex);
static DEFINE_IDR(dma_idr);
static LIST_HEAD(dma_device_list);

/* --- sysfs implementation --- */

/**
 * dev_to_dma_chan - convert a device pointer to the its sysfs container object
 * @dev - device node
 *
 * Must be called under dma_list_mutex
 */
static struct dma_chan *dev_to_dma_chan(struct device *dev)
{
	struct dma_chan_dev *chan_dev;

	chan_dev = container_of(dev, typeof(*chan_dev), device);
	return chan_dev->chan;
}

static ssize_t in_use_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct dma_chan *chan;
	int err;

	mutex_lock(&dma_list_mutex);
	chan = dev_to_dma_chan(dev);
	if (chan)
		err = sprintf(buf, "%d\n", chan->client_count);
	else
		err = -ENODEV;
	mutex_unlock(&dma_list_mutex);

	return err;
}
static DEVICE_ATTR_RO(in_use);

static struct attribute *dma_dev_attrs[] = {
	&dev_attr_in_use.attr,
	NULL,
};
ATTRIBUTE_GROUPS(dma_dev);

static void chan_dev_release(struct device *dev)
{
	struct dma_chan_dev *chan_dev;

	chan_dev = container_of(dev, typeof(*chan_dev), device);
	if (atomic_dec_and_test(chan_dev->idr_ref)) {
		mutex_lock(&dma_list_mutex);
		idr_remove(&dma_idr, chan_dev->dev_id);
		mutex_unlock(&dma_list_mutex);
		kfree(chan_dev->idr_ref);
	}
	kfree(chan_dev);
}

static struct class dma_devclass = {
	.name		= "dma",
	.dev_groups	= dma_dev_groups,
	.dev_release	= chan_dev_release,
};

/* --- client and device registration --- */
static int
__dma_device_satisfies_mask(struct dma_device *device,
			    const dma_cap_mask_t *want)
{
	dma_cap_mask_t has;

	bitmap_and(has.bits, want->bits, device->cap_mask.bits,
		DMA_TX_TYPE_END);
	return bitmap_equal(want->bits, has.bits, DMA_TX_TYPE_END);
}

static struct module *dma_chan_to_owner(struct dma_chan *chan)
{
	return chan->device->dev->driver->owner;
}

/**
 * dma_chan_get - try to grab a dma channel's parent driver module
 * @chan - channel to grab
 *
 * Must be called under dma_list_mutex
 */
static int dma_chan_get(struct dma_chan *chan)
{
	struct module *owner = dma_chan_to_owner(chan);
	int ret;

	/* The channel is already in use, update client count */
	if (chan->client_count) {
		__module_get(owner);
		goto out;
	}

	if (!try_module_get(owner))
		return -ENODEV;

	/* allocate upon first client reference */
	if (chan->device->device_alloc_chan_resources) {
		ret = chan->device->device_alloc_chan_resources(chan);
		if (ret < 0)
			goto err_out;
	}

out:
	chan->client_count++;
	return 0;

err_out:
	module_put(owner);
	return ret;
}

/**
 * dma_chan_put - drop a reference to a dma channel's parent driver module
 * @chan - channel to release
 *
 * Must be called under dma_list_mutex
 */
static void dma_chan_put(struct dma_chan *chan)
{
	/* This channel is not in use, bail out */
	if (!chan->client_count)
		return;

	chan->client_count--;
	module_put(dma_chan_to_owner(chan));

	/* This channel is not in use anymore, free it */
	if (!chan->client_count && chan->device->device_free_chan_resources)
		chan->device->device_free_chan_resources(chan);
}

enum dma_status dma_sync_wait(struct dma_chan *chan, dma_cookie_t cookie)
{
	enum dma_status status;
	unsigned long dma_sync_wait_timeout = jiffies + msecs_to_jiffies(5000);

	dma_async_issue_pending(chan);
	do {
		status = dma_async_is_tx_complete(chan, cookie, NULL, NULL);
		if (time_after_eq(jiffies, dma_sync_wait_timeout)) {
			pr_err("%s: timeout!\n", __func__);
			return DMA_ERROR;
		}
		if (status != DMA_IN_PROGRESS)
			break;
		cpu_relax();
	} while (1);

	return status;
}
EXPORT_SYMBOL(dma_sync_wait);

/**
 * dma_issue_pending_all - flush all pending operations across all channels
 */
void dma_issue_pending_all(void)
{
	struct dma_device *device;
	struct dma_chan *chan;

	rcu_read_lock();
	list_for_each_entry_rcu(device, &dma_device_list, global_node) {
		list_for_each_entry(chan, &device->channels, device_node)
			if (chan->client_count)
				device->device_issue_pending(chan);
	}
	rcu_read_unlock();
}
EXPORT_SYMBOL(dma_issue_pending_all);

int dma_get_slave_caps(struct dma_chan *chan, struct dma_slave_caps *caps)
{
	struct dma_device *device;

	if (!chan || !caps)
		return -EINVAL;

	device = chan->device;

	/* check if the channel supports slave transactions */
	if (!test_bit(DMA_SLAVE, device->cap_mask.bits))
		return -ENXIO;

	/*
	 * Check whether it reports it uses the generic slave
	 * capabilities, if not, that means it doesn't support any
	 * kind of slave capabilities reporting.
	 */
	if (!device->directions)
		return -ENXIO;

	caps->src_addr_widths = device->src_addr_widths;
	caps->dst_addr_widths = device->dst_addr_widths;
	caps->directions = device->directions;
	caps->residue_granularity = device->residue_granularity;

	caps->cmd_pause = !!device->device_pause;
	caps->cmd_terminate = !!device->device_terminate_all;

	return 0;
}
EXPORT_SYMBOL_GPL(dma_get_slave_caps);

static struct dma_chan *private_candidate(const dma_cap_mask_t *mask,
					  struct dma_device *dev,
					  dma_filter_fn fn, void *fn_param)
{
	struct dma_chan *chan;

	if (!__dma_device_satisfies_mask(dev, mask)) {
		pr_debug("%s: wrong capabilities\n", __func__);
		return NULL;
	}

	list_for_each_entry(chan, &dev->channels, device_node) {
		if (chan->client_count) {
			pr_debug("%s: %s busy\n",
				 __func__, dma_chan_name(chan));
			continue;
		}
		if (fn && !fn(chan, fn_param)) {
			pr_debug("%s: %s filter said false\n",
				 __func__, dma_chan_name(chan));
			continue;
		}
		return chan;
	}

	return NULL;
}

/**
 * dma_request_slave_channel - try to get specific channel exclusively
 * @chan: target channel
 */
struct dma_chan *dma_get_slave_channel(struct dma_chan *chan)
{
	int err = -EBUSY;

	/* lock against __dma_request_channel */
	mutex_lock(&dma_list_mutex);

	if (chan->client_count == 0) {
		err = dma_chan_get(chan);
		if (err)
			pr_debug("%s: failed to get %s: (%d)\n",
				__func__, dma_chan_name(chan), err);
	} else
		chan = NULL;

	mutex_unlock(&dma_list_mutex);


	return chan;
}
EXPORT_SYMBOL_GPL(dma_get_slave_channel);

struct dma_chan *dma_get_any_slave_channel(struct dma_device *device)
{
	dma_cap_mask_t mask;
	struct dma_chan *chan;
	int err;

	dma_cap_zero(mask);
	dma_cap_set(DMA_SLAVE, mask);

	/* lock against __dma_request_channel */
	mutex_lock(&dma_list_mutex);

	chan = private_candidate(&mask, device, NULL, NULL);
	if (chan) {
		err = dma_chan_get(chan);
		if (err) {
			pr_debug("%s: failed to get %s: (%d)\n",
				__func__, dma_chan_name(chan), err);
			chan = NULL;
		}
	}

	mutex_unlock(&dma_list_mutex);

	return chan;
}
EXPORT_SYMBOL_GPL(dma_get_any_slave_channel);

/**
 * __dma_request_channel - try to allocate an exclusive channel
 * @mask: capabilities that the channel must satisfy
 * @fn: optional callback to disposition available channels
 * @fn_param: opaque parameter to pass to dma_filter_fn
 *
 * Returns pointer to appropriate DMA channel on success or NULL.
 */
struct dma_chan *__dma_request_channel(const dma_cap_mask_t *mask,
				       dma_filter_fn fn, void *fn_param)
{
	struct dma_device *device, *_d;
	struct dma_chan *chan = NULL;
	int err;

	/* Find a channel */
	mutex_lock(&dma_list_mutex);
	list_for_each_entry_safe(device, _d, &dma_device_list, global_node) {
		chan = private_candidate(mask, device, fn, fn_param);
		if (chan) {
			err = dma_chan_get(chan);

			if (err == -ENODEV) {
				pr_debug("%s: %s module removed\n",
					 __func__, dma_chan_name(chan));
				list_del_rcu(&device->global_node);
			} else if (err)
				pr_debug("%s: failed to get %s: (%d)\n",
					 __func__, dma_chan_name(chan), err);
			else
				break;
			chan = NULL;
		}
	}
	mutex_unlock(&dma_list_mutex);

	pr_debug("%s: %s (%s)\n",
		 __func__,
		 chan ? "success" : "fail",
		 chan ? dma_chan_name(chan) : NULL);

	return chan;
}
EXPORT_SYMBOL_GPL(__dma_request_channel);

/**
 * dma_request_slave_channel - try to allocate an exclusive slave channel
 * @dev:	pointer to client device structure
 * @name:	slave channel name
 *
 * Returns pointer to appropriate DMA channel on success or an error pointer.
 */
struct dma_chan *dma_request_slave_channel_reason(struct device *dev,
						  const char *name)
{
	/* If device-tree is present get slave info from here */
	if (dev->of_node)
		return of_dma_request_slave_channel(dev->of_node, name);

	/* If device was enumerated by ACPI get slave info from here */
	if (ACPI_HANDLE(dev))
		return acpi_dma_request_slave_chan_by_name(dev, name);

	return ERR_PTR(-ENODEV);
}
EXPORT_SYMBOL_GPL(dma_request_slave_channel_reason);

/**
 * dma_request_slave_channel - try to allocate an exclusive slave channel
 * @dev:	pointer to client device structure
 * @name:	slave channel name
 *
 * Returns pointer to appropriate DMA channel on success or NULL.
 */
struct dma_chan *dma_request_slave_channel(struct device *dev,
					   const char *name)
{
	struct dma_chan *ch = dma_request_slave_channel_reason(dev, name);
	if (IS_ERR(ch))
		return NULL;
	return ch;
}
EXPORT_SYMBOL_GPL(dma_request_slave_channel);

void dma_release_channel(struct dma_chan *chan)
{
	mutex_lock(&dma_list_mutex);
	WARN_ONCE(chan->client_count != 1,
		  "chan reference count %d != 1\n", chan->client_count);
	dma_chan_put(chan);
	mutex_unlock(&dma_list_mutex);
}
EXPORT_SYMBOL_GPL(dma_release_channel);

static bool device_has_all_tx_types(struct dma_device *device)
{
	/* A device that satisfies this test has channels that will never cause
	 * an async_tx channel switch event as all possible operation types can
	 * be handled.
	 */
	#ifdef CONFIG_ASYNC_TX_DMA
	if (!dma_has_cap(DMA_INTERRUPT, device->cap_mask))
		return false;
	#endif

	#if defined(CONFIG_ASYNC_MEMCPY) || defined(CONFIG_ASYNC_MEMCPY_MODULE)
	if (!dma_has_cap(DMA_MEMCPY, device->cap_mask))
		return false;
	#endif

	#if defined(CONFIG_ASYNC_XOR) || defined(CONFIG_ASYNC_XOR_MODULE)
	if (!dma_has_cap(DMA_XOR, device->cap_mask))
		return false;

	#ifndef CONFIG_ASYNC_TX_DISABLE_XOR_VAL_DMA
	if (!dma_has_cap(DMA_XOR_VAL, device->cap_mask))
		return false;
	#endif
	#endif

	#if defined(CONFIG_ASYNC_PQ) || defined(CONFIG_ASYNC_PQ_MODULE)
	if (!dma_has_cap(DMA_PQ, device->cap_mask))
		return false;

	#ifndef CONFIG_ASYNC_TX_DISABLE_PQ_VAL_DMA
	if (!dma_has_cap(DMA_PQ_VAL, device->cap_mask))
		return false;
	#endif
	#endif

	return true;
}

static int get_dma_id(struct dma_device *device)
{
	int rc;

	mutex_lock(&dma_list_mutex);

	rc = idr_alloc(&dma_idr, NULL, 0, 0, GFP_KERNEL);
	if (rc >= 0)
		device->dev_id = rc;

	mutex_unlock(&dma_list_mutex);
	return rc < 0 ? rc : 0;
}

/**
 * dma_async_device_register - registers DMA devices found
 * @device: &dma_device
 */
int dma_async_device_register(struct dma_device *device)
{
	int chancnt = 0, rc;
	struct dma_chan* chan;
	atomic_t *idr_ref;

	if (!device)
		return -ENODEV;

	/* validate device routines */
	BUG_ON(dma_has_cap(DMA_MEMCPY, device->cap_mask) &&
		!device->device_prep_dma_memcpy);
	BUG_ON(dma_has_cap(DMA_XOR, device->cap_mask) &&
		!device->device_prep_dma_xor);
	BUG_ON(dma_has_cap(DMA_XOR_VAL, device->cap_mask) &&
		!device->device_prep_dma_xor_val);
	BUG_ON(dma_has_cap(DMA_PQ, device->cap_mask) &&
		!device->device_prep_dma_pq);
	BUG_ON(dma_has_cap(DMA_PQ_VAL, device->cap_mask) &&
		!device->device_prep_dma_pq_val);
	BUG_ON(dma_has_cap(DMA_INTERRUPT, device->cap_mask) &&
		!device->device_prep_dma_interrupt);
	BUG_ON(dma_has_cap(DMA_SG, device->cap_mask) &&
		!device->device_prep_dma_sg);
	BUG_ON(dma_has_cap(DMA_CYCLIC, device->cap_mask) &&
		!device->device_prep_dma_cyclic);
	BUG_ON(dma_has_cap(DMA_INTERLEAVE, device->cap_mask) &&
		!device->device_prep_interleaved_dma);

	BUG_ON(!device->device_tx_status);
	BUG_ON(!device->device_issue_pending);
	BUG_ON(!device->dev);

	WARN(dma_has_cap(DMA_SLAVE, device->cap_mask) && !device->directions,
	     "this driver doesn't support generic slave capabilities reporting\n");

	/* note: this only matters in the
	 * CONFIG_ASYNC_TX_ENABLE_CHANNEL_SWITCH=n case
	 */
	if (device_has_all_tx_types(device))
		dma_cap_set(DMA_ASYNC_TX, device->cap_mask);

	idr_ref = kmalloc(sizeof(*idr_ref), GFP_KERNEL);
	if (!idr_ref)
		return -ENOMEM;
	rc = get_dma_id(device);
	if (rc != 0) {
		kfree(idr_ref);
		return rc;
	}

	atomic_set(idr_ref, 0);

	/* represent channels in sysfs. Probably want devs too */
	list_for_each_entry(chan, &device->channels, device_node) {
		chan->dev = kzalloc(sizeof(*chan->dev), GFP_KERNEL);
		if (chan->dev == NULL) {
			rc = -ENOMEM;
			goto err_out;
		}

		chan->chan_id = chancnt++;
		chan->dev->device.class = &dma_devclass;
		chan->dev->device.parent = device->dev;
		chan->dev->chan = chan;
		chan->dev->idr_ref = idr_ref;
		chan->dev->dev_id = device->dev_id;
		atomic_inc(idr_ref);
		dev_set_name(&chan->dev->device, "dma%dchan%d",
			     device->dev_id, chan->chan_id);

		rc = device_register(&chan->dev->device);
		if (rc) {
			kfree(chan->dev);
			atomic_dec(idr_ref);
			goto err_out;
		}
		chan->client_count = 0;
	}
	device->chancnt = chancnt;
	list_add_tail_rcu(&device->global_node, &dma_device_list);

	return 0;

err_out:
	/* if we never registered a channel just release the idr */
	if (atomic_read(idr_ref) == 0) {
		mutex_lock(&dma_list_mutex);
		idr_remove(&dma_idr, device->dev_id);
		mutex_unlock(&dma_list_mutex);
		kfree(idr_ref);
		return rc;
	}

	list_for_each_entry(chan, &device->channels, device_node) {
		mutex_lock(&dma_list_mutex);
		chan->dev->chan = NULL;
		mutex_unlock(&dma_list_mutex);
		device_unregister(&chan->dev->device);
	}
	return rc;
}
EXPORT_SYMBOL(dma_async_device_register);

/**
 * dma_async_device_unregister - unregister a DMA device
 * @device: &dma_device
 *
 * This routine is called by dma driver exit routines, dmaengine holds module
 * references to prevent it being called while channels are in use.
 */
void dma_async_device_unregister(struct dma_device *device)
{
	struct dma_chan *chan;

	list_del_rcu(&device->global_node);

	list_for_each_entry(chan, &device->channels, device_node) {
		WARN_ONCE(chan->client_count,
			  "%s called while %d clients hold a reference\n",
			  __func__, chan->client_count);
		mutex_lock(&dma_list_mutex);
		chan->dev->chan = NULL;
		mutex_unlock(&dma_list_mutex);
		device_unregister(&chan->dev->device);
	}
}
EXPORT_SYMBOL(dma_async_device_unregister);

struct dmaengine_unmap_pool {
	struct kmem_cache *cache;
	const char *name;
	mempool_t *pool;
	size_t size;
};

#define __UNMAP_POOL(x) { .size = x, .name = "dmaengine-unmap-" __stringify(x) }
static struct dmaengine_unmap_pool unmap_pool[] = {
	__UNMAP_POOL(2),
	#if IS_ENABLED(CONFIG_DMA_ENGINE_RAID)
	__UNMAP_POOL(16),
	__UNMAP_POOL(128),
	__UNMAP_POOL(256),
	#endif
};

static struct dmaengine_unmap_pool *__get_unmap_pool(int nr)
{
	int order = get_count_order(nr);

	switch (order) {
	case 0 ... 1:
		return &unmap_pool[0];
	case 2 ... 4:
		return &unmap_pool[1];
	case 5 ... 7:
		return &unmap_pool[2];
	case 8:
		return &unmap_pool[3];
	default:
		BUG();
		return NULL;
	}
}

static void dmaengine_unmap(struct kref *kref)
{
	struct dmaengine_unmap_data *unmap = container_of(kref, typeof(*unmap), kref);
	struct device *dev = unmap->dev;
	int cnt, i;

	cnt = unmap->to_cnt;
	for (i = 0; i < cnt; i++)
		dma_unmap_page(dev, unmap->addr[i], unmap->len,
			       DMA_TO_DEVICE);
	cnt += unmap->from_cnt;
	for (; i < cnt; i++)
		dma_unmap_page(dev, unmap->addr[i], unmap->len,
			       DMA_FROM_DEVICE);
	cnt += unmap->bidi_cnt;
	for (; i < cnt; i++) {
		if (unmap->addr[i] == 0)
			continue;
		dma_unmap_page(dev, unmap->addr[i], unmap->len,
			       DMA_BIDIRECTIONAL);
	}
	cnt = unmap->map_cnt;
	mempool_free(unmap, __get_unmap_pool(cnt)->pool);
}

void dmaengine_unmap_put(struct dmaengine_unmap_data *unmap)
{
	if (unmap)
		kref_put(&unmap->kref, dmaengine_unmap);
}
EXPORT_SYMBOL_GPL(dmaengine_unmap_put);

static void dmaengine_destroy_unmap_pool(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(unmap_pool); i++) {
		struct dmaengine_unmap_pool *p = &unmap_pool[i];

		if (p->pool)
			mempool_destroy(p->pool);
		p->pool = NULL;
		if (p->cache)
			kmem_cache_destroy(p->cache);
		p->cache = NULL;
	}
}

static int __init dmaengine_init_unmap_pool(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(unmap_pool); i++) {
		struct dmaengine_unmap_pool *p = &unmap_pool[i];
		size_t size;

		size = sizeof(struct dmaengine_unmap_data) +
		       sizeof(dma_addr_t) * p->size;

		p->cache = kmem_cache_create(p->name, size, 0,
					     SLAB_HWCACHE_ALIGN, NULL);
		if (!p->cache)
			break;
		p->pool = mempool_create_slab_pool(1, p->cache);
		if (!p->pool)
			break;
	}

	if (i == ARRAY_SIZE(unmap_pool))
		return 0;

	dmaengine_destroy_unmap_pool();
	return -ENOMEM;
}

struct dmaengine_unmap_data *
dmaengine_get_unmap_data(struct device *dev, int nr, gfp_t flags)
{
	struct dmaengine_unmap_data *unmap;

	unmap = mempool_alloc(__get_unmap_pool(nr)->pool, flags);
	if (!unmap)
		return NULL;

	memset(unmap, 0, sizeof(*unmap));
	kref_init(&unmap->kref);
	unmap->dev = dev;
	unmap->map_cnt = nr;

	return unmap;
}
EXPORT_SYMBOL(dmaengine_get_unmap_data);

void dma_async_tx_descriptor_init(struct dma_async_tx_descriptor *tx,
	struct dma_chan *chan)
{
	tx->chan = chan;
	#ifdef CONFIG_ASYNC_TX_ENABLE_CHANNEL_SWITCH
	spin_lock_init(&tx->lock);
	#endif
}
EXPORT_SYMBOL(dma_async_tx_descriptor_init);

/* dma_wait_for_async_tx - spin wait for a transaction to complete
 * @tx: in-flight transaction to wait on
 */
enum dma_status
dma_wait_for_async_tx(struct dma_async_tx_descriptor *tx)
{
	unsigned long dma_sync_wait_timeout = jiffies + msecs_to_jiffies(5000);

	if (!tx)
		return DMA_COMPLETE;

	while (tx->cookie == -EBUSY) {
		if (time_after_eq(jiffies, dma_sync_wait_timeout)) {
			pr_err("%s timeout waiting for descriptor submission\n",
			       __func__);
			return DMA_ERROR;
		}
		cpu_relax();
	}
	return dma_sync_wait(tx->chan, tx->cookie);
}
EXPORT_SYMBOL_GPL(dma_wait_for_async_tx);

/* dma_run_dependencies - helper routine for dma drivers to process
 *	(start) dependent operations on their target channel
 * @tx: transaction with dependencies
 */
void dma_run_dependencies(struct dma_async_tx_descriptor *tx)
{
	struct dma_async_tx_descriptor *dep = txd_next(tx);
	struct dma_async_tx_descriptor *dep_next;
	struct dma_chan *chan;

	if (!dep)
		return;

	/* we'll submit tx->next now, so clear the link */
	txd_clear_next(tx);
	chan = dep->chan;

	/* keep submitting up until a channel switch is detected
	 * in that case we will be called again as a result of
	 * processing the interrupt from async_tx_channel_switch
	 */
	for (; dep; dep = dep_next) {
		txd_lock(dep);
		txd_clear_parent(dep);
		dep_next = txd_next(dep);
		if (dep_next && dep_next->chan == chan)
			txd_clear_next(dep); /* ->next will be submitted */
		else
			dep_next = NULL; /* submit current dep and terminate */
		txd_unlock(dep);

		dep->tx_submit(dep);
	}

	chan->device->device_issue_pending(chan);
}
EXPORT_SYMBOL_GPL(dma_run_dependencies);

static int __init dma_bus_init(void)
{
	int err = dmaengine_init_unmap_pool();

	if (err)
		return err;
	return class_register(&dma_devclass);
}
arch_initcall(dma_bus_init);


