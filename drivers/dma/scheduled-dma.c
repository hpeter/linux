/*
 * Copyright (C) 2015 Maxime Ripard
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/list.h>
#include <linux/slab.h>

#include "scheduled-dma.h"
#include "virt-dma.h"

/**
 * __sdma_elect_chan_by_req() - Elect a new channel for a given request
 * @sdma:	pointer to the SDMA device we want to work on.
 * @sreq:	pointer to the request we want to elect a channel for.
 *
 * This function tries to find an available sdma_channel to be run on
 * a pending sdma_request. This function is expected to be called from
 * an interrupt context.
 *
 * This function should be called with sdma->lock held.
 *
 * Return:	an sdma_channel pointer expected to be used to transfer
 *		the given request. NULL on failure or if no channels are
 *		available.
 */
static struct sdma_channel *__sdma_elect_chan_by_req(struct sdma *sdma,
						     struct sdma_request *sreq)
{
	struct sdma_channel *schan;

	/* Is there an available channel */
	if (list_empty(&sdma->avail_chans))
		return NULL;

	/*
	 * If we don't have any validate_request callback, any request
	 * can be dispatched to any channel.
	 *
	 * Remove the first available channel and return it.
	 */
	if (!sdma->ops->validate_request) {
		schan = list_first_entry(&sdma->avail_chans,
					 struct sdma_channel, node);
		list_del_init(&schan->node);
		return schan;
	}

	list_for_each_entry(schan, &sdma->avail_chans, node) {
		/*
		 * Ask the driver to validate that the request can
		 * happen on the channel.
		 */
		if (sdma->ops->validate_request(schan, sreq)) {
			list_del_init(&schan->node);
			return schan;
		}

		/* Otherwise, just keep looping */
	}

	return NULL;
}

/**
 * __sdma_elect_req_by_chan() - Elect new request for a given transfer
 * @sdma:	pointer to the SDMA device we want to work on.
 * @schan:	pointer to the channel we want to elect a request for.
 *
 * This function tries to elect a pending sdma_request to be run on a
 * newly available sdma_channel. This function is expected to be
 * called from an interrupt context.
 *
 * This function should be called with sdma->lock held.
 *
 * Return:	a sdma_request pointer expected to be run on the channel
 *		given as parameter. NULL on failure or if no pending
 *		request can be run on that channel
 */
static struct sdma_request *__sdma_elect_req_by_chan(struct sdma *sdma,
						     struct sdma_channel *schan)
{
	struct sdma_request *sreq;

	/* No requests are awaiting an available channel */
	if (list_empty(&sdma->pend_reqs))
		return NULL;

	/*
	 * If we don't have any validate_request callback, any request
	 * can be dispatched to any channel.
	 *
	 * Remove the first entry and return it.
	 */
	if (!sdma->ops->validate_request) {
		sreq = list_first_entry(&sdma->pend_reqs,
					struct sdma_request, node);
		list_del_init(&sreq->node);
		return sreq;
	}

	list_for_each_entry(sreq, &sdma->pend_reqs, node) {
		/*
		 * Ask the driver to validate that the request can
		 * happen on the channel.
		 */
		if (sdma->ops->validate_request(schan, sreq)) {
			list_del_init(&sreq->node);
			return sreq;
		}

		/* Otherwise, just keep looping */
	}

	return NULL;
}

/**
 * sdma_report() - Report the status of a transfer
 * @sdma:	Pointer to the SDMA device we want to work on.
 * @schan:	Pointer to the channel we want to report the status for.
 * @status:	Current status to report
 *
 * This function is used to report the status of an ongoing transfer
 * on a given channel. This is expected to be called from an interrupt
 * handler, to let the scheduled DMA framework know about the current
 * state of a transfer running on a given transfer.
 *
 * Return:	a pointer to a new SDMA descriptor to run on the given
 *		channel. NULL on failure or if no new descriptor is
 *		available.
 */
struct sdma_desc *sdma_report(struct sdma *sdma,
			      struct sdma_channel *schan,
			      enum sdma_report_status status)
{
	struct sdma_desc *sdesc = NULL;
	struct virt_dma_desc *vdesc;
	struct sdma_request *sreq;

	switch (status) {
	case SDMA_REPORT_TRANSFER:
		/*
		 * We're done with the current transfer that was in this
		 * physical channel.
		 */
		vchan_cookie_complete(&schan->desc->vdesc);

		/*
		 * Now, try to see if there's any queued transfer
		 * awaiting an available channel.
		 *
		 * If not, just bail out, and mark the pchan as
		 * available.
		 *
		 * If there's such a transfer, validate that the
		 * driver can handle it, and ask it to do the
		 * transfer.
		 */
		spin_lock(&sdma->lock);
		sreq = __sdma_elect_req_by_chan(sdma, schan);
		if (!sreq) {
			list_add_tail(&schan->node, &sdma->avail_chans);
			spin_unlock(&sdma->lock);
			return NULL;
		}
		spin_unlock(&sdma->lock);

		/* Mark the request as assigned to a particular channel */
		sreq->chan = schan;

		/* Retrieve the next transfer descriptor */
		vdesc = vchan_next_desc(&sreq->vchan);
		schan->desc = sdesc = to_sdma_desc(&vdesc->tx);

		break;

	default:
		break;
	}

	return sdesc;
}
EXPORT_SYMBOL_GPL(sdma_report);

static enum dma_status sdma_tx_status(struct dma_chan *chan,
				      dma_cookie_t cookie,
				      struct dma_tx_state *state)
{
	struct sdma_request *sreq = to_sdma_request(chan);
	struct sdma *sdma = to_sdma(chan->device);
	struct sdma_channel *schan = sreq->chan;
	struct virt_dma_desc *vd;
	struct sdma_desc *desc;
	enum dma_status ret;
	unsigned long flags;
	size_t bytes = 0;
	void *lli;

	spin_lock_irqsave(&sreq->vchan.lock, flags);

	ret = dma_cookie_status(chan, cookie, state);
	if (ret == DMA_COMPLETE)
		goto out;

	vd = vchan_find_desc(&sreq->vchan, cookie);
	desc = to_sdma_desc(&vd->tx);

	if (vd) {
		lli = desc->v_lli;
		while (true) {
			bytes += sdma->ops->lli_size(lli);

			if (!sdma->ops->lli_has_next(lli))
				break;

			lli = sdma->ops->lli_next(lli);
		}
	} else if (chan) {
		bytes = sdma->ops->channel_residue(schan);
	}

	dma_set_residue(state, bytes);

out:
	spin_unlock_irqrestore(&sreq->vchan.lock, flags);

	return ret;
};

static int sdma_config(struct dma_chan *chan,
		       struct dma_slave_config *config)
{
	struct sdma_request *sreq = to_sdma_request(chan);
	unsigned long flags;

	spin_lock_irqsave(&sreq->vchan.lock, flags);
	memcpy(&sreq->cfg, config, sizeof(*config));
	spin_unlock_irqrestore(&sreq->vchan.lock, flags);

	return 0;
}

static int sdma_pause(struct dma_chan *chan)
{
	struct sdma_request *sreq = to_sdma_request(chan);
	struct sdma_channel *schan = sreq->chan;
	struct sdma *sdma = to_sdma(chan->device);
	unsigned long flags;

	spin_lock_irqsave(&sreq->vchan.lock, flags);

	/*
	 * If the request is currently scheduled on a channel, just
	 * pause the channel.
	 *
	 * If not, remove the request from the pending list.
	 */
	if (schan) {
		sdma->ops->channel_pause(schan);
	} else {
		spin_lock(&sdma->lock);
		list_del_init(&sreq->node);
		spin_unlock(&sdma->lock);
	}

	spin_unlock_irqrestore(&sreq->vchan.lock, flags);

	return 0;
}

static int sdma_resume(struct dma_chan *chan)
{
	struct sdma_request *sreq = to_sdma_request(chan);
	struct sdma_channel *schan = sreq->chan;
	struct sdma *sdma = to_sdma(chan->device);
	unsigned long flags;

	spin_lock_irqsave(&sreq->vchan.lock, flags);

	/*
	 * If the request is currently scheduled on a channel, just
	 * resume the channel.
	 *
	 * If not, add the request from the pending list.
	 */
	if (schan) {
		sdma->ops->channel_resume(schan);
	} else {
		spin_lock(&sdma->lock);
		list_add_tail(&sreq->node, &sdma->pend_reqs);
		spin_unlock(&sdma->lock);
	}

	spin_unlock_irqrestore(&sreq->vchan.lock, flags);

	return 0;
}

static int sdma_terminate(struct dma_chan *chan)
{
	struct sdma_request *sreq = to_sdma_request(chan);
	struct sdma_channel *schan = sreq->chan;
	struct sdma *sdma = to_sdma(chan->device);
	unsigned long flags;

	spin_lock_irqsave(&sreq->vchan.lock, flags);

	/*
	 * If the request is currently scheduled on a channel,
	 * terminate the channel.
	 *
	 * If not, prevent the request from being scheduled.
	 */
	if (schan) {
		sdma->ops->channel_terminate(schan);
	} else {
		spin_lock(&sdma->lock);
		list_del_init(&sreq->node);
		spin_unlock(&sdma->lock);
	}

	spin_unlock_irqrestore(&sreq->vchan.lock, flags);

	/*
	 * Flush all the pending descriptors from our vchan
	 */
	vchan_free_chan_resources(&sreq->vchan);

	return 0;
}


static struct dma_async_tx_descriptor *sdma_prep_memcpy(struct dma_chan *chan,
							dma_addr_t dest, dma_addr_t src,
							size_t len, unsigned long flags)
{
	struct sdma_request *req = to_sdma_request(chan);
	struct sdma *sdma = to_sdma(chan->device);
	struct sdma_desc *sdesc;
	dma_addr_t p_lli;
	void *v_lli;

	if (!len)
		return NULL;

	/* Allocate our representation of a descriptor */
	sdesc = kzalloc(sizeof(*sdesc), GFP_NOWAIT);
	if (!sdesc)
		return NULL;

	/* Allocate a new LLI */
	v_lli = dma_pool_alloc(sdma->pool, GFP_NOWAIT, &p_lli);
	if (!v_lli)
		goto err_desc_free;

	/* Ask the driver to initialise its hardware descriptor */
	if (sdma->ops->lli_init(v_lli, req->private,
				SDMA_TRANSFER_MEMCPY,
				DMA_MEM_TO_MEM, src, dest, len,
				NULL))
		goto err_lli_free;

	/* Create our single item LLI */
	sdma->ops->lli_queue(NULL, v_lli, p_lli);
	sdesc->p_lli = p_lli;
	sdesc->v_lli = v_lli;

	return vchan_tx_prep(&req->vchan, &sdesc->vdesc, flags);

err_lli_free:
	dma_pool_free(sdma->pool, v_lli, p_lli);
err_desc_free:
	kfree(sdesc);

	return NULL;
}

static struct dma_async_tx_descriptor *sdma_prep_slave_sg(struct dma_chan *chan,
							  struct scatterlist *sgl,
							  unsigned int sg_len,
							  enum dma_transfer_direction dir,
							  unsigned long flags, void *context)
{
	struct sdma_request *req = to_sdma_request(chan);
	struct dma_slave_config *config = &req->cfg;
	struct sdma *sdma = to_sdma(chan->device);
	void *v_lli, *prev_v_lli = NULL;
	struct scatterlist *sg;
	struct sdma_desc *sdesc;
	dma_addr_t p_lli;
	int i;

	if (!sgl || !sg_len)
		return NULL;

	/* Allocate our representation of a descriptor */
	sdesc = kzalloc(sizeof(*sdesc), GFP_NOWAIT);
	if (!sdesc)
		return NULL;

	/*
	 * For each scatter list entry, build up our representation of
	 * the LLI, and ask the driver to create its hardware
	 * descriptor.
	 */
	for_each_sg(sgl, sg, sg_len, i) {
		v_lli = dma_pool_alloc(sdma->pool, GFP_NOWAIT, &p_lli);
		if (!v_lli)
			goto err_lli_free;

		/* Ask the driver to initialise its hardware descriptor */
		if (sdma->ops->lli_init(v_lli, req->private,
					SDMA_TRANSFER_SLAVE,
					dir, sg_dma_address(sg),
					config->dst_addr, sg_dma_len(sg),
					config))
			goto err_lli_free;

		/*
		 * If it's our first item, initialise our descriptor
		 * pointers to the lli.
		 *
		 * Otherwise, queue it to the end of the LLI.
		 */
		if (!prev_v_lli) {
			sdesc->p_lli = p_lli;
			sdesc->v_lli = v_lli;
			prev_v_lli = v_lli;
		} else {
			/* And to queue it at the end of its hardware LLI */
			prev_v_lli = sdma->ops->lli_queue(prev_v_lli, v_lli, p_lli);
		}
	}

	return vchan_tx_prep(&req->vchan, &sdesc->vdesc, flags);

err_lli_free:
#warning "Free the LLI"

	kfree(sdesc);
	return NULL;
}

static void sdma_issue_pending(struct dma_chan *chan)
{
	struct sdma_request *sreq = to_sdma_request(chan);
	struct sdma *sdma = to_sdma(chan->device);
	struct virt_dma_desc *vdesc;
	struct sdma_channel *schan;
	struct sdma_desc *sdesc;
	unsigned long flags;

	spin_lock_irqsave(&sreq->vchan.lock, flags);

	/* See if we have anything to do */
	if (!vchan_issue_pending(&sreq->vchan))
		goto out_chan_unlock;

	/* Is some work in progress already? */
	if (sreq->chan)
		goto out_chan_unlock;

	spin_lock(&sdma->lock);

	/*
	 * If we can't get a channel to do our work, just queue the
	 * request in our pending list.
	 */
	schan = __sdma_elect_chan_by_req(sdma, sreq);
	if (!schan) {
		list_add_tail(&sreq->node, &sdma->pend_reqs);
		goto out_main_unlock;
	}

	sreq->chan = schan;

	/* Retrieve the next transfer descriptor */
	vdesc = vchan_next_desc(&sreq->vchan);
	schan->desc = sdesc = to_sdma_desc(&vdesc->tx);

	sdma->ops->channel_start(schan, sdesc);

out_main_unlock:
	spin_unlock(&sdma->lock);
out_chan_unlock:
	spin_unlock_irqrestore(&sreq->vchan.lock, flags);
}

static void sdma_free_chan_resources(struct dma_chan *chan)
{
	struct sdma_request *sreq = to_sdma_request(chan);
	unsigned long flags;

	spin_lock_irqsave(&sreq->vchan.lock, flags);
	list_del_init(&sreq->node);
	spin_unlock_irqrestore(&sreq->vchan.lock, flags);

	vchan_free_chan_resources(&sreq->vchan);
}

static void sdma_free_desc(struct virt_dma_desc *vdesc)
{
#warning "Free the descriptors"
}

struct sdma *sdma_alloc(struct device *dev,
			unsigned int channels,
			unsigned int requests,
			ssize_t lli_size,
			ssize_t priv_size)
{
	struct sdma *sdma;
	int ret, i;

	sdma = devm_kzalloc(dev, sizeof(*sdma) + priv_size, GFP_KERNEL);
	if (!sdma) {
		ret = -ENOMEM;
		goto out;
	}
	INIT_LIST_HEAD(&sdma->pend_reqs);
	INIT_LIST_HEAD(&sdma->avail_chans);
	spin_lock_init(&sdma->lock);

	sdma->pool = dmam_pool_create(dev_name(dev), dev, lli_size, 4, 0);
	if (!sdma->pool) {
		ret = -ENOMEM;
		goto out;
	}

	sdma->channels_nr = channels;
	sdma->channels = devm_kcalloc(dev, channels, sizeof(*sdma->channels), GFP_KERNEL);
	if (!sdma->channels) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = 0; i < channels; i++) {
		struct sdma_channel *schan = &sdma->channels[i];

		list_add_tail(&schan->node, &sdma->avail_chans);
		schan->index = i;
	}

	sdma->requests_nr = requests;
	sdma->requests = devm_kcalloc(dev, requests, sizeof(*sdma->requests), GFP_KERNEL);
	if (!sdma->channels) {
		ret = -ENOMEM;
		goto out;
	}

	INIT_LIST_HEAD(&sdma->ddev.channels);

	for (i = 0; i < requests; i++) {
		struct sdma_request *sreq = &sdma->requests[i];

		INIT_LIST_HEAD(&sreq->node);
		sreq->vchan.desc_free = sdma_free_desc;
		vchan_init(&sreq->vchan, &sdma->ddev);
	}

	return sdma;

out:
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(sdma_alloc);

void sdma_free(struct sdma *sdma)
{
	return;
}
EXPORT_SYMBOL(sdma_free);

int sdma_register(struct sdma *sdma,
		  struct sdma_ops *ops)
{
	struct dma_device *ddev = &sdma->ddev;

	sdma->ops = ops;

	ddev->device_config = sdma_config;
	ddev->device_tx_status = sdma_tx_status;
	ddev->device_issue_pending = sdma_issue_pending;
	ddev->device_free_chan_resources = sdma_free_chan_resources;

	if (ops->channel_pause)
		ddev->device_pause = sdma_pause;

	if (ops->channel_resume)
		ddev->device_resume = sdma_resume;

	if (ops->channel_terminate)
		ddev->device_terminate_all = sdma_terminate;

	if (dma_has_cap(DMA_SLAVE, ddev->cap_mask))
		ddev->device_prep_slave_sg = sdma_prep_slave_sg;

	if (dma_has_cap(DMA_MEMCPY, ddev->cap_mask))
		ddev->device_prep_dma_memcpy = sdma_prep_memcpy;

	dma_async_device_register(ddev);

	return 0;
}
EXPORT_SYMBOL(sdma_register);

int sdma_unregister(struct sdma *sdma)
{
	dma_async_device_unregister(&sdma->ddev);

	return 0;
}
EXPORT_SYMBOL(sdma_unregister);
