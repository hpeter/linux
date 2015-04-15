/*
 * Copyright (C) 2015 Maxime Ripard
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "virt-dma.h"

#ifndef _SCHEDULED_DMA_H_
#define _SCHEDULED_DMA_H_

/**
 * enum sdma_transfer_type - Transfer type identifier
 *
 * @SDMA_TRANSFER_MEMCPY:	memory to memory copy
 * @SDMA_TRANSFER_SLAVE:	memory to device or device to memory transfer
 */
enum sdma_transfer_type {
	SDMA_TRANSFER_MEMCPY,
	SDMA_TRANSFER_SLAVE,
};

/**
 * struct sdma_desc - Representation of a hardware descriptor.
 *
 * @vdesc:	Virtual DMA descriptor handle
 * @p_lli:	Physical address of the first descriptor in our LLI
 * @v_lli:	Virtual address of the first descriptor in our LLI
 *
 * This structure is our software representation of a hardware
 * descriptor, ie an in-memory structure fed to the DMA controller to
 * configure it and describe the transfer and its parameters.
 *
 * This structure is also our link link between our LLI and the
 * virt_dma descriptor.
 *
 * It is meant to be allocated and initialize in the prep_* calls, and
 * will be used to identify a unique transfer within the Scheduled DMA
 * framework.
 */
struct sdma_desc {
	struct virt_dma_desc	vdesc;
	dma_addr_t		p_lli;
	void			*v_lli;
};

/**
 * struct sdma_channel - Representation of a hardware channel.
 *
 * @desc:	Pointer to the transfer currently happening on this channel.
 * @index:	Physical index of this particular channel.
 * @node:	List node for the available channels list
 * @private:	Pointer to some opaque private data to be filled by client
 *		drivers.
 *
 * This structure is our software representation of a DMA channel, ie
 * a hardware entity using the descriptors to transfer data between a
 * device and memory, or operate directly on memory.
 *
 * This structure will be allocated at initialization, and a pointer
 * to one of its instance will be passed to all the channel-related
 * operations.
 */
struct sdma_channel {
	struct sdma_desc	*desc;
	unsigned int		index;
	struct list_head	node;
	void			*private;
};

/**
 * struct sdma_request - Representation of a hardware request
 *
 * @cfg:	Channel configuration to use for the transfers using this
 *		request.
 * @node:	List node for the pending requests list
 * @vchan:	virt_dma channel handle
 * @chan:	Current channel associated with this request
 * @private:	Pointer to some opaque private data to be filled by client
 *		drivers.
 *
 * This structure is our software representation of a DMA request, ie
 * one of the possible endpoints of a DMA transfer.
 *
 * This structure is the one that will be associated to a struct
 * dma_chan pointer requested by our client drivers, meaning that
 * there might be more requests than actual DMA channel on the device,
 * and the association between a request and a channel will be very
 * volatile, changing from one transfer to another.
 */
struct sdma_request {
	struct dma_slave_config	cfg;
	struct list_head	node;
	struct virt_dma_chan	vchan;

	struct sdma_channel	*chan;
	void			*private;
};

struct sdma_ops {
	/* LLI management operations */
	bool			(*lli_has_next)(void *v_lli);
	void			*(*lli_next)(void *v_lli);
	int			(*lli_init)(void *v_lli, void *sreq_priv,
					    enum sdma_transfer_type type,
					    enum dma_transfer_direction dir,
					    dma_addr_t src,
					    dma_addr_t dst, u32 len,
					    struct dma_slave_config *config);
	void			*(*lli_queue)(void *prev_v_lli, void *v_lli, dma_addr_t p_lli);
	size_t			(*lli_size)(void *v_lli);

	/* Scheduler helper */
	struct sdma_request	*(*validate_request)(struct sdma_channel *chan,
						     struct sdma_request *req);

	/* Transfer Management Functions */
	int			(*channel_pause)(struct sdma_channel *chan);
	int			(*channel_resume)(struct sdma_channel *chan);
	int			(*channel_start)(struct sdma_channel *chan, struct sdma_desc *sdesc);
	int			(*channel_terminate)(struct sdma_channel *chan);
	size_t			(*channel_residue)(struct sdma_channel *chan);
};

struct sdma {
	struct dma_device	ddev;
	struct sdma_ops		*ops;

	struct dma_pool		*pool;

	struct sdma_channel	*channels;
	int			channels_nr;
	struct sdma_request	*requests;
	int			requests_nr;

	struct list_head	avail_chans;
	struct list_head	pend_reqs;

	spinlock_t		lock;

	unsigned long		private[];
};

static inline struct sdma *to_sdma(struct dma_device *d)
{
	return container_of(d, struct sdma, ddev);
}

static inline struct sdma_request *to_sdma_request(struct dma_chan *chan)
{
	return container_of(chan, struct sdma_request, vchan.chan);
}

static inline struct sdma_desc *to_sdma_desc(struct dma_async_tx_descriptor *tx)
{
	return container_of(tx, struct sdma_desc, vdesc.tx);
}

static inline void *sdma_priv(struct sdma *sdma)
{
	return (void*)sdma->private;
}

static inline void sdma_set_chan_private(struct sdma *sdma, void *ptr)
{
	int i;

	for (i = 0; i < sdma->channels_nr; i++) {
		struct sdma_channel *schan = &sdma->channels[i];

		schan->private = ptr;
	}
}

struct sdma_desc *sdma_report_transfer(struct sdma *sdma,
				       struct sdma_channel *chan);

struct sdma *sdma_alloc(struct device *dev,
			unsigned int channels,
			unsigned int requests,
			ssize_t lli_size,
			ssize_t priv_size);
void sdma_free(struct sdma *sdma);

int sdma_register(struct sdma *sdma,
		  struct sdma_ops *ops);
int sdma_unregister(struct sdma *sdma);

#endif /* _SCHEDULED_DMA_H_ */
