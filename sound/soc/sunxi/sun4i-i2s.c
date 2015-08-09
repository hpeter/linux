/*
 * Copyright (C) 2015 Maxime Ripard
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>

#define SUN4I_I2S_CTRL_REG		0x00
#define SUN4I_I2S_CTRL_SDO_EN_MASK		GENMASK(11, 8)
#define SUN4I_I2S_CTRL_SDO_EN(sdo)			BIT(8 + (sdo))
#define SUN4I_I2S_CTRL_MODE_MASK		BIT(5)
#define SUN4I_I2S_CTRL_MODE_SLAVE			(1 << 5)
#define SUN4I_I2S_CTRL_MODE_MASTER			(0 << 5)
#define SUN4I_I2S_CTRL_TX_EN			BIT(2)
#define SUN4I_I2S_CTRL_RX_EN			BIT(1)
#define SUN4I_I2S_CTRL_GL_EN			BIT(0)

#define SUN4I_I2S_FMT0_REG		0x04
#define SUN4I_I2S_FMT0_LRCLK_POLARITY_MASK	BIT(7)
#define SUN4I_I2S_FMT0_LRCLK_POLARITY_INVERTED		(1 << 7)
#define SUN4I_I2S_FMT0_LRCLK_POLARITY_NORMAL		(0 << 7)
#define SUN4I_I2S_FMT0_BCLK_POLARITY_MASK	BIT(6)
#define SUN4I_I2S_FMT0_BCLK_POLARITY_INVERTED		(1 << 6)
#define SUN4I_I2S_FMT0_BCLK_POLARITY_NORMAL		(0 << 6)
#define SUN4I_I2S_FMT0_SR_MASK			GENMASK(5, 4)
#define SUN4I_I2S_FMT0_SR(sr)				((sr) << 4)
#define SUN4I_I2S_FMT0_WSS_MASK			GENMASK(3, 2)
#define SUN4I_I2S_FMT0_WSS(wss)				((wss) << 2)
#define SUN4I_I2S_FMT0_FMT_MASK			GENMASK(1, 0)
#define SUN4I_I2S_FMT0_FMT_RIGHT_J			(2 << 0)
#define SUN4I_I2S_FMT0_FMT_LEFT_J			(1 << 0)
#define SUN4I_I2S_FMT0_FMT_I2S				(0 << 0)

#define SUN4I_I2S_FMT1_REG		0x08
#define SUN4I_I2S_FIFO_TX_REG		0x0c
#define SUN4I_I2S_FIFO_RX_REG		0x10

#define SUN4I_I2S_FIFO_CTRL_REG		0x14
#define SUN4I_I2S_FIFO_CTRL_FLUSH_TX		BIT(25)
#define SUN4I_I2S_FIFO_CTRL_FLUSH_RX		BIT(24)

#define SUN4I_I2S_FIFO_STA_REG		0x18

#define SUN4I_I2S_DMA_INT_CTRL_REG	0x1c
#define SUN4I_I2S_DMA_INT_CTRL_TX_DRQ_EN	BIT(7)
#define SUN4I_I2S_DMA_INT_CTRL_RX_DRQ_EN	BIT(3)

#define SUN4I_I2S_INT_STA_REG		0x20

#define SUN4I_I2S_CLK_DIV_REG		0x24
#define SUN4I_I2S_CLK_DIV_BCLK_MASK		GENMASK(6, 4)
#define SUN4I_I2S_CLK_DIV_BCLK(bclk)			((bclk) << 4)
#define SUN4I_I2S_CLK_DIV_MCLK_MASK		GENMASK(3, 0)
#define SUN4I_I2S_CLK_DIV_MCLK(mclk)			((mclk) << 0)

#define SUN4I_I2S_RX_CNT_REG		0x28
#define SUN4I_I2S_TX_CNT_REG		0x2c

#define SUN4I_I2S_TX_CHAN_SEL_REG	0x30
#define SUN4I_I2S_TX_CHAN_SEL(num_chan)		(((num_chan) - 1) << 0)

#define SUN4I_I2S_TX_CHAN_MAP_REG	0x34
#define SUN4I_I2S_TX_CHAN_MAP(chan, sample)	((sample) << (chan << 2))

#define SUN4I_I2S_RX_CHAN_SEL_REG	0x38
#define SUN4I_I2S_RX_CHAN_MAP_REG	0x3c

struct sun4i_i2s {
	struct clk	*mod_clk;
	struct regmap	*regmap;

	struct snd_dmaengine_dai_dma_data	playback_dma_data;
};

struct sun4i_i2s_clk_div {
	u8	div;
	u8	val;
};

static const struct sun4i_i2s_clk_div sun4i_i2s_bclk_div[] = {
	{ .div = 2, .val = 0 },
	{ .div = 4, .val = 1 },
	{ .div = 6, .val = 2 },
	{ .div = 8, .val = 3 },
	{ .div = 12, .val = 4 },
	{ .div = 16, .val = 5 },
	{ /* Sentinel */ },
};

static const struct sun4i_i2s_clk_div sun4i_i2s_mclk_div[] = {
	{ .div = 1, .val = 0 },
	{ .div = 2, .val = 1 },
	{ .div = 4, .val = 2 },
	{ .div = 6, .val = 3 },
	{ .div = 8, .val = 4 },
	{ .div = 12, .val = 5 },
	{ .div = 16, .val = 6 },
	{ .div = 24, .val = 7 },
	{ /* Sentinel */ },
};

static u8 sun4i_i2s_params_to_sr(struct snd_pcm_hw_params *params)
{
	switch (params_width(params)) {
	case 24:
		return 2;
	}

	return 0;
}

static u8 sun4i_i2s_params_to_wss(struct snd_pcm_hw_params *params)
{
	switch (params_width(params)) {
	case 24:
		return 2;
	}

	return 0;
}

static int sun4i_i2s_get_bclk_div(struct sun4i_i2s *i2s,
				  unsigned int oversample_rate,
				  unsigned int word_size)
{
	int div = oversample_rate / word_size / 2;
	int i;

	for (i = 0; sun4i_i2s_bclk_div[i].div; i++) {
		const struct sun4i_i2s_clk_div *bdiv = sun4i_i2s_bclk_div + i;

		if (bdiv->div == div)
			return bdiv->val;
	}

	return -EINVAL;
}

static int sun4i_i2s_get_mclk_div(struct sun4i_i2s *i2s,
				  unsigned int oversample_rate,
				  unsigned int module_rate,
				  unsigned int sampling_rate)
{
	int div = module_rate / sampling_rate / oversample_rate;
	int i;

	for (i = 0; sun4i_i2s_mclk_div[i].div; i++) {
		const struct sun4i_i2s_clk_div *mdiv = sun4i_i2s_mclk_div + i;

		if (mdiv->div == div)
			return mdiv->val;
	}

	return -EINVAL;
}

static int sun4i_i2s_oversample_rates[] = { 128, 192, 256, 384, 512, 768 };

static int sun4i_i2s_set_clk_rate(struct sun4i_i2s *i2s,
				  unsigned int rate,
				  unsigned int word_size)
{
	unsigned int clk_rate;
	int bclk_div, mclk_div;
	int i;

	switch (rate) {
        case 176400:
        case 88200:
        case 44100:
        case 22050:
        case 11025:
                clk_rate = 22579200;
                break;

        case 192000:
        case 128000:
        case 96000:
        case 64000:
        case 48000:
        case 32000:
        case 24000:
        case 16000:
        case 12000:
        case 8000:
		clk_rate = 24576000;
                break;

        default:
		return -EINVAL;
        }

	clk_set_rate(i2s->mod_clk, clk_rate);

	/* Always favor the highest oversampling rate */
	for (i = (ARRAY_SIZE(sun4i_i2s_oversample_rates) - 1); i >= 0; i--) {
		unsigned int oversample_rate = sun4i_i2s_oversample_rates[i];

		bclk_div = sun4i_i2s_get_bclk_div(i2s, oversample_rate,
						  word_size);
		mclk_div = sun4i_i2s_get_mclk_div(i2s, oversample_rate,
						  clk_rate,
						  rate);

		if (bclk_div > 0 || mclk_div > 0)
			break;
	}

	if (bclk_div <= 0 || mclk_div <= 0)
		return -EINVAL;

	regmap_update_bits(i2s->regmap, SUN4I_I2S_CLK_DIV_REG,
			   SUN4I_I2S_CLK_DIV_BCLK_MASK |
			   SUN4I_I2S_CLK_DIV_MCLK_MASK,
			   SUN4I_I2S_CLK_DIV_BCLK(bclk_div) |
			   SUN4I_I2S_CLK_DIV_MCLK(mclk_div));

	return 0;
}

static int sun4i_i2s_hw_params(struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct snd_soc_dai *dai)
{
	struct sun4i_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	u32 width;
	u8 sr, wss;

	if (params_channels(params) != 2)
		return -EINVAL;

	/* Enable the first output line */
	regmap_update_bits(i2s->regmap, SUN4I_I2S_CTRL_REG,
			   SUN4I_I2S_CTRL_SDO_EN_MASK,
			   SUN4I_I2S_CTRL_SDO_EN(0));

	/* Enable the first two channels */
	regmap_write(i2s->regmap, SUN4I_I2S_TX_CHAN_SEL_REG,
		     SUN4I_I2S_TX_CHAN_SEL(2));

	/* Map them to the two first samples coming in */
	regmap_write(i2s->regmap, SUN4I_I2S_TX_CHAN_MAP_REG,
		     SUN4I_I2S_TX_CHAN_MAP(0, 0) | SUN4I_I2S_TX_CHAN_MAP(1, 1));

	switch (params_physical_width(params)) {
	case 32:
		width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		break;
	default:
		return -EINVAL;
	}
	i2s->playback_dma_data.addr_width = width;

	sr = sun4i_i2s_params_to_sr(params);
	wss = sun4i_i2s_params_to_wss(params);
	regmap_update_bits(i2s->regmap, SUN4I_I2S_FMT0_REG,
			   SUN4I_I2S_FMT0_WSS_MASK | SUN4I_I2S_FMT0_SR_MASK,
			   SUN4I_I2S_FMT0_WSS(wss) | SUN4I_I2S_FMT0_SR(sr));

	return sun4i_i2s_set_clk_rate(i2s, params_rate(params),
				      params_width(params));
}

static int sun4i_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
        struct sun4i_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	u32 val;

	/* DAI Mode */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		val = SUN4I_I2S_FMT0_FMT_I2S;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		val = SUN4I_I2S_FMT0_FMT_LEFT_J;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		val = SUN4I_I2S_FMT0_FMT_RIGHT_J;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(i2s->regmap, SUN4I_I2S_FMT0_REG,
			   SUN4I_I2S_FMT0_FMT_MASK,
			   val);

	/* DAI clock polarity */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_IB_IF:
		/* Invert both clocks */
		val = SUN4I_I2S_FMT0_BCLK_POLARITY_INVERTED |
			SUN4I_I2S_FMT0_LRCLK_POLARITY_INVERTED;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		/* Invert bit clock */
		val = SUN4I_I2S_FMT0_BCLK_POLARITY_INVERTED | 
			SUN4I_I2S_FMT0_LRCLK_POLARITY_NORMAL;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		/* Invert frame clock */
		val = SUN4I_I2S_FMT0_LRCLK_POLARITY_INVERTED |
			SUN4I_I2S_FMT0_BCLK_POLARITY_NORMAL;
		break;
	case SND_SOC_DAIFMT_NB_NF:
		/* Nothing to do for both normal cases */
		val = SUN4I_I2S_FMT0_BCLK_POLARITY_NORMAL |
			SUN4I_I2S_FMT0_LRCLK_POLARITY_NORMAL;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(i2s->regmap, SUN4I_I2S_FMT0_REG,
			   SUN4I_I2S_FMT0_BCLK_POLARITY_MASK |
			   SUN4I_I2S_FMT0_LRCLK_POLARITY_MASK,
			   val);

	/* DAI clock master masks */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		/* BCLK and LRCLK master */
		val = SUN4I_I2S_CTRL_MODE_MASTER;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		/* BCLK and LRCLK slave */
		val = SUN4I_I2S_CTRL_MODE_SLAVE;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(i2s->regmap, SUN4I_I2S_CTRL_REG,
			   SUN4I_I2S_CTRL_MODE_MASK,
			   val);

	return 0;
}

static void sun4i_i2s_start_playback(struct sun4i_i2s *i2s)
{
	/* Flush TX FIFO */
        regmap_update_bits(i2s->regmap, SUN4I_I2S_FIFO_CTRL_REG,
			   SUN4I_I2S_FIFO_CTRL_FLUSH_TX,
			   SUN4I_I2S_FIFO_CTRL_FLUSH_TX);

        /* Clear TX counter */
	regmap_write(i2s->regmap, SUN4I_I2S_TX_CNT_REG, 0);

        /* Enable TX Block */
        regmap_update_bits(i2s->regmap, SUN4I_I2S_CTRL_REG,
			   SUN4I_I2S_CTRL_TX_EN,
			   SUN4I_I2S_CTRL_TX_EN);

        /* Enable TX DRQ */
        regmap_update_bits(i2s->regmap, SUN4I_I2S_DMA_INT_CTRL_REG,
			   SUN4I_I2S_DMA_INT_CTRL_TX_DRQ_EN,
			   SUN4I_I2S_DMA_INT_CTRL_TX_DRQ_EN);
}


static void sun4i_i2s_stop_playback(struct sun4i_i2s *i2s)
{
        /* Disable TX Block */
        regmap_update_bits(i2s->regmap, SUN4I_I2S_CTRL_REG,
			   SUN4I_I2S_CTRL_TX_EN,
			   0);

        /* Disable TX DRQ */
        regmap_update_bits(i2s->regmap, SUN4I_I2S_DMA_INT_CTRL_REG,
			   SUN4I_I2S_DMA_INT_CTRL_TX_DRQ_EN,
			   0);
}

static int sun4i_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
			     struct snd_soc_dai *dai)
{
	struct sun4i_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			sun4i_i2s_start_playback(i2s);
		else
			return -EINVAL;
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			sun4i_i2s_stop_playback(i2s);
		else
			return -EINVAL;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static const struct snd_soc_dai_ops sun4i_i2s_dai_ops = {
	.hw_params	= sun4i_i2s_hw_params,
	.set_fmt	= sun4i_i2s_set_fmt,
	.trigger	= sun4i_i2s_trigger,
};

static int sun4i_i2s_dai_probe(struct snd_soc_dai *dai)
{
	struct sun4i_i2s *i2s = dev_get_drvdata(dai->dev);

	/* Enable the whole hardware block */
	regmap_write(i2s->regmap, SUN4I_I2S_CTRL_REG,
		     SUN4I_I2S_CTRL_GL_EN);

	snd_soc_dai_init_dma_data(dai, &i2s->playback_dma_data, NULL);

	snd_soc_dai_set_drvdata(dai, i2s);

	return 0;
}

static struct snd_soc_dai_driver sun4i_i2s_dai = {
	.probe = sun4i_i2s_dai_probe,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_192000,
		.formats = SNDRV_PCM_FMTBIT_S24_LE,
	},
	.ops = &sun4i_i2s_dai_ops,
	.symmetric_rates = 1,
};

static const struct snd_soc_component_driver sun4i_i2s_component = {
	.name	= "sun4i-i2s",
};

static const struct regmap_config sun4i_i2s_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN4I_I2S_TX_CHAN_MAP_REG,
};

static int sun4i_i2s_probe(struct platform_device *pdev)
{
	struct sun4i_i2s *i2s;
	struct resource *res;
	void __iomem *regs;
	int irq, ret;

	i2s = devm_kzalloc(&pdev->dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(regs)) {
		dev_err(&pdev->dev, "Can't request IO region\n");
		return PTR_ERR(regs);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "Can't retrieve our interrupt\n");
		return irq;
	}

	i2s->regmap = devm_regmap_init_mmio_clk(&pdev->dev, "bus", regs,
						&sun4i_i2s_regmap_config);
	if (IS_ERR(i2s->regmap)) {
		dev_err(&pdev->dev, "Regmap initialisation failed\n");
		return PTR_ERR(i2s->regmap);
	};

	i2s->mod_clk = devm_clk_get(&pdev->dev, "mod");
	if (IS_ERR(i2s->mod_clk)) {
		dev_err(&pdev->dev, "Can't get our mod clock\n");
		return PTR_ERR(i2s->mod_clk);
	}
	
	i2s->playback_dma_data.addr = res->start + SUN4I_I2S_FIFO_TX_REG;
	i2s->playback_dma_data.maxburst = 4;

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &sun4i_i2s_component,
					      &sun4i_i2s_dai, 1);
	if (ret) {
		dev_err(&pdev->dev, "Could not register DAI\n");
		return ret;
	}

	ret = snd_dmaengine_pcm_register(&pdev->dev, NULL, 0);
	if (ret) {
		dev_err(&pdev->dev, "Could not register PCM\n");
		return ret;
	}

	return 0;
}

static int sun4i_i2s_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id sun4i_i2s_match[] = {
	{ .compatible = "allwinner,sun4i-a10-i2s", },
	{}
};
MODULE_DEVICE_TABLE(of, sun4i_i2s_match);

static struct platform_driver sun4i_i2s_driver = {
	.probe	= sun4i_i2s_probe,
	.remove	= sun4i_i2s_remove,
	.driver	= {
		.name		= "sun4i-i2s",
		.of_match_table	= sun4i_i2s_match,
	},
};
module_platform_driver(sun4i_i2s_driver);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_DESCRIPTION("Allwinner A10 DAI driver");
MODULE_LICENSE("GPL");
