/*
 * Copyright 2014 Emilio López <emilio@elopez.com.ar>
 * Copyright 2014 Jon Smirl <jonsmirl@gmail.com>
 *
 * Based on the Allwinner SDK driver, released under the GPL.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/clk.h>
#include <linux/regmap.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>
#include <sound/initval.h>
#include <sound/dmaengine_pcm.h>

/* Codec DAC register offsets and bit fields */
#define SUNXI_DAC_DPC			(0x00)
#define SUNXI_DAC_DPC_EN_DA			(31)
#define SUNXI_DAC_DPC_DVOL			(12)
#define SUNXI_DAC_FIFOC			(0x04)
#define SUNXI_DAC_FIFOC_DAC_FS			(29)
#define SUNXI_DAC_FIFOC_FIR_VERSION		(28)
#define SUNXI_DAC_FIFOC_SEND_LASAT		(26)
#define SUNXI_DAC_FIFOC_TX_FIFO_MODE		(24)
#define SUNXI_DAC_FIFOC_DRQ_CLR_CNT		(21)
#define SUNXI_DAC_FIFOC_TX_TRIG_LEVEL		(8)
#define SUNXI_DAC_FIFOC_MONO_EN			(6)
#define SUNXI_DAC_FIFOC_TX_SAMPLE_BITS		(5)
#define SUNXI_DAC_FIFOC_DAC_DRQ_EN		(4)
#define SUNXI_DAC_FIFOC_FIFO_FLUSH		(0)
#define SUNXI_DAC_FIFOS			(0x08)
#define SUNXI_DAC_TXDATA		(0x0c)
#define SUNXI_DAC_ACTL			(0x10)
#define SUNXI_DAC_ACTL_DACAENR			(31)
#define SUNXI_DAC_ACTL_DACAENL			(30)
#define SUNXI_DAC_ACTL_MIXEN			(29)
#define SUNXI_DAC_ACTL_LDACLMIXS		(15)
#define SUNXI_DAC_ACTL_RDACRMIXS		(14)
#define SUNXI_DAC_ACTL_LDACRMIXS		(13)
#define SUNXI_DAC_ACTL_DACPAS			(8)
#define SUNXI_DAC_ACTL_MIXPAS			(7)
#define SUNXI_DAC_ACTL_PA_MUTE			(6)
#define SUNXI_DAC_ACTL_PA_VOL			(0)
#define SUNXI_DAC_TUNE			(0x14)
#define SUNXI_DAC_DEBUG			(0x18)

/* Codec ADC register offsets and bit fields */
#define SUNXI_ADC_FIFOC			(0x1c)
#define SUNXI_ADC_FIFOC_EN_AD			(28)
#define SUNXI_ADC_FIFOC_RX_FIFO_MODE		(24)
#define SUNXI_ADC_FIFOC_RX_TRIG_LEVEL		(8)
#define SUNXI_ADC_FIFOC_MONO_EN			(7)
#define SUNXI_ADC_FIFOC_RX_SAMPLE_BITS		(6)
#define SUNXI_ADC_FIFOC_ADC_DRQ_EN		(4)
#define SUNXI_ADC_FIFOC_FIFO_FLUSH		(0)
#define SUNXI_ADC_FIFOS			(0x20)
#define SUNXI_ADC_RXDATA		(0x24)
#define SUNXI_ADC_ACTL			(0x28)
#define SUNXI_ADC_ACTL_ADC_R_EN			(31)
#define SUNXI_ADC_ACTL_ADC_L_EN			(30)
#define SUNXI_ADC_ACTL_PREG1EN			(29)
#define SUNXI_ADC_ACTL_PREG2EN			(28)
#define SUNXI_ADC_ACTL_VMICEN			(27)
#define SUNXI_ADC_ACTL_VADCG			(20)
#define SUNXI_ADC_ACTL_ADCIS			(17)
#define SUNXI_ADC_ACTL_PA_EN			(4)
#define SUNXI_ADC_ACTL_DDE			(3)
#define SUNXI_ADC_DEBUG			(0x2c)

/* Other various ADC registers */
#define SUNXI_DAC_TXCNT			(0x30)
#define SUNXI_ADC_RXCNT			(0x34)
#define SUNXI_AC_SYS_VERI		(0x38)
#define SUNXI_AC_MIC_PHONE_CAL		(0x3c)

struct sunxi_priv {
	struct device	*dev;
	struct regmap	*regmap;
	struct clk	*clk_apb;
	struct clk	*clk_module;

	struct snd_dmaengine_dai_dma_data	playback_dma_data;
	struct snd_dmaengine_dai_dma_data	capture_dma_data;
};

static void sunxi_codec_play_start(struct sunxi_priv *priv)
{
	/* TODO: see if we need to drive PA GPIO high */

	/* Flush TX FIFO */
	regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
			   BIT(SUNXI_DAC_FIFOC_FIFO_FLUSH),
			   BIT(SUNXI_DAC_FIFOC_FIFO_FLUSH));

	/* Enable DAC DRQ */
	regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
			   BIT(SUNXI_DAC_FIFOC_DAC_DRQ_EN),
			   BIT(SUNXI_DAC_FIFOC_DAC_DRQ_EN));
}

static void sunxi_codec_play_stop(struct sunxi_priv *priv)
{
	/* TODO: see if we need to drive PA GPIO low */

	/* Disable DAC DRQ */
	regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
			   BIT(SUNXI_DAC_FIFOC_DAC_DRQ_EN), 0);
}

static void sunxi_codec_capture_start(struct sunxi_priv *priv)
{
	/* TODO: see if we need to drive PA GPIO high */

	/* Enable ADC DRQ */
	regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
			   BIT(SUNXI_ADC_FIFOC_ADC_DRQ_EN),
			   BIT(SUNXI_ADC_FIFOC_ADC_DRQ_EN));
}

static void sunxi_codec_capture_stop(struct sunxi_priv *priv)
{
	/* Disable ADC DRQ */
	regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
			   BIT(SUNXI_ADC_FIFOC_ADC_DRQ_EN), 0);

	/* Disable MIC1 Pre-Amplifier */
	regmap_update_bits(priv->regmap, SUNXI_ADC_ACTL,
			   BIT(SUNXI_ADC_ACTL_PREG1EN), 0);

	/* Disable VMIC */
	regmap_update_bits(priv->regmap, SUNXI_ADC_ACTL,
			   BIT(SUNXI_ADC_ACTL_VMICEN), 0);

	/* TODO: undocumented */
	if (of_device_is_compatible(priv->dev->of_node,
				    "allwinner,sun7i-a20-codec"))
		regmap_update_bits(priv->regmap, SUNXI_DAC_TUNE,
				   0x3 << 8, 0);

	/* Disable ADC digital part */
	regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
			   0x1 << SUNXI_ADC_FIFOC_EN_AD, 0);

	/* Fill in the least significant bits with 0 */
	regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
			   BIT(SUNXI_ADC_FIFOC_RX_FIFO_MODE), 0);

	/* Disable ADC1 analog part */
	regmap_update_bits(priv->regmap, SUNXI_ADC_ACTL,
			   BIT(SUNXI_ADC_ACTL_ADC_L_EN) |
			   BIT(SUNXI_ADC_ACTL_ADC_R_EN), 0);
}

static int sunxi_codec_trigger(struct snd_pcm_substream *substream, int cmd,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct sunxi_priv *priv = snd_soc_card_get_drvdata(rtd->card);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
			sunxi_codec_capture_start(priv);
		else
			sunxi_codec_play_start(priv);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
			sunxi_codec_capture_stop(priv);
		else
			sunxi_codec_play_stop(priv);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int sunxi_codec_prepare(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct sunxi_priv *priv = snd_soc_card_get_drvdata(rtd->card);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		/* Flush the TX FIFO */
		regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
				   BIT(SUNXI_DAC_FIFOC_FIFO_FLUSH),
				   BIT(SUNXI_DAC_FIFOC_FIFO_FLUSH));

		/* Set TX FIFO Empty Trigger Level */
		regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
				   0x3f << SUNXI_DAC_FIFOC_TX_TRIG_LEVEL,
				   0xf << SUNXI_DAC_FIFOC_TX_TRIG_LEVEL);

		if (substream->runtime->rate > 32000)
			/* Use 64 bits FIR filter */
			regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
					   BIT(SUNXI_DAC_FIFOC_FIR_VERSION), 0);
		else
			/* Use 32 bits FIR filter */
			regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
					   BIT(SUNXI_DAC_FIFOC_FIR_VERSION),
					   BIT(SUNXI_DAC_FIFOC_FIR_VERSION));

		/* Send zeros when we have an underrun */
		regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
				   BIT(SUNXI_DAC_FIFOC_SEND_LASAT), 0);
	} else {
		/* Enable Mic1 Pre-Amplifier */
		regmap_update_bits(priv->regmap, SUNXI_ADC_ACTL,
				   BIT(SUNXI_ADC_ACTL_PREG1EN),
				   BIT(SUNXI_ADC_ACTL_PREG1EN));

		/*
		 * FIXME: Undocumented in the datasheet, but
		 *        Allwinner's code mentions that it is related
		 *        related to microphone gain
		 */
		regmap_update_bits(priv->regmap, SUNXI_ADC_ACTL,
				   0x3 << 25,
				   0x1 << 25);

		/* Enable VMIC */
		regmap_update_bits(priv->regmap, SUNXI_ADC_ACTL,
				   BIT(SUNXI_ADC_ACTL_VMICEN),
				   BIT(SUNXI_ADC_ACTL_VMICEN));

		if (of_device_is_compatible(priv->dev->of_node,
					    "allwinner,sun7i-a20-codec"))
			/* FIXME: Undocumented bits */
			regmap_update_bits(priv->regmap, SUNXI_DAC_TUNE,
					   0x3 << 8,
					   0x1 << 8);

		/* Enable ADC digital part */
		regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
				   BIT(SUNXI_ADC_FIFOC_EN_AD),
				   BIT(SUNXI_ADC_FIFOC_EN_AD));

		/* Fill most significant bits with valid data MSB */
		regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
				   BIT(SUNXI_ADC_FIFOC_RX_FIFO_MODE),
				   BIT(SUNXI_ADC_FIFOC_RX_FIFO_MODE));

		/* Flush RX FIFO */
		regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
				   BIT(SUNXI_ADC_FIFOC_FIFO_FLUSH),
				   BIT(SUNXI_ADC_FIFOC_FIFO_FLUSH));

		/* Set RX FIFO trigger level */
		regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
				   0xf << SUNXI_ADC_FIFOC_RX_TRIG_LEVEL,
				   0x7 << SUNXI_ADC_FIFOC_RX_TRIG_LEVEL);

		/* Enable ADC1 analog parts */
		regmap_update_bits(priv->regmap, SUNXI_ADC_ACTL,
				   BIT(SUNXI_ADC_ACTL_ADC_L_EN) |
				   BIT(SUNXI_ADC_ACTL_ADC_R_EN),
				   BIT(SUNXI_ADC_ACTL_ADC_L_EN) |
				   BIT(SUNXI_ADC_ACTL_ADC_R_EN));
	}

	return 0;
}

static int sunxi_codec_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct sunxi_priv *priv = snd_soc_card_get_drvdata(rtd->card);
	unsigned int rate = params_rate(params);
	unsigned int hwrate;

	switch (rate) {
	case 176400:
	case 88200:
	case 44100:
	case 33075:
	case 22050:
	case 14700:
	case 11025:
	case 7350:
	default:
		clk_set_rate(priv->clk_module, 22579200);
		break;

	case 192000:
	case 96000:
	case 48000:
	case 32000:
	case 24000:
	case 16000:
	case 12000:
	case 8000:
		clk_set_rate(priv->clk_module, 24576000);
		break;
	}

	switch (rate) {
	case 192000:
	case 176400:
		hwrate = 6;
		break;
	case 96000:
	case 88200:
		hwrate = 7;
		break;
	default:
	case 48000:
	case 44100:
		hwrate = 0;
		break;
	case 32000:
	case 33075:
		hwrate = 1;
		break;
	case 24000:
	case 22050:
		hwrate = 2;
		break;
	case 16000:
	case 14700:
		hwrate = 3;
		break;
	case 12000:
	case 11025:
		hwrate = 4;
		break;
	case 8000:
	case 7350:
		hwrate = 5;
		break;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		/* Set DAC sample rate */
		regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
				   7 << SUNXI_DAC_FIFOC_DAC_FS,
				   hwrate << SUNXI_DAC_FIFOC_DAC_FS);

		/* Set the number of channels we want to use */
		if (params_channels(params) == 1)
			regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
					   BIT(SUNXI_DAC_FIFOC_MONO_EN),
					   BIT(SUNXI_DAC_FIFOC_MONO_EN));
		else
			regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
					   BIT(SUNXI_DAC_FIFOC_MONO_EN), 0);

		/* Set the number of sample bits to either 16 or 24 bits */
		if (hw_param_interval(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS)->min == 32) {
			regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
					   BIT(SUNXI_DAC_FIFOC_TX_SAMPLE_BITS),
					   BIT(SUNXI_DAC_FIFOC_TX_SAMPLE_BITS));

			/* Set TX FIFO mode to padding the LSBs with 0 */
			regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
				   BIT(SUNXI_DAC_FIFOC_TX_FIFO_MODE), 0);

			priv->playback_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		} else {
			regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
					   BIT(SUNXI_DAC_FIFOC_TX_SAMPLE_BITS), 0);

			/* Set TX FIFO mode to repeat the MSB */
			regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
					   BIT(SUNXI_DAC_FIFOC_TX_FIFO_MODE),
					   BIT(SUNXI_DAC_FIFOC_TX_FIFO_MODE));

			priv->playback_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;
		}
	} else {
		/* Set DAC sample rate */
		regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
				   7 << SUNXI_DAC_FIFOC_DAC_FS,
				   hwrate << SUNXI_DAC_FIFOC_DAC_FS);

		/* Set the number of channels we want to use */
		if (params_channels(params) == 1)
			regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
					   BIT(SUNXI_ADC_FIFOC_MONO_EN),
					   BIT(SUNXI_ADC_FIFOC_MONO_EN));
		else
			regmap_update_bits(priv->regmap, SUNXI_ADC_FIFOC,
					   BIT(SUNXI_ADC_FIFOC_MONO_EN), 0);
	}

	return 0;
}

static int sunxi_codec_dai_probe(struct snd_soc_dai *dai)
{
	struct snd_soc_card *card = snd_soc_dai_get_drvdata(dai);
	struct sunxi_priv *priv = snd_soc_card_get_drvdata(card);

	snd_soc_dai_init_dma_data(dai, &priv->playback_dma_data,
				  &priv->capture_dma_data);

	return 0;
}

static void sunxi_codec_init(struct sunxi_priv *priv)
{
	/* Use a 32 bits FIR */
	regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
			   BIT(SUNXI_DAC_FIFOC_FIR_VERSION),
			   BIT(SUNXI_DAC_FIFOC_FIR_VERSION));

	/* Set digital volume to maximum */
	if (of_device_is_compatible(priv->dev->of_node,
				    "allwinner,sun4i-a10a-codec"))
		regmap_update_bits(priv->regmap, SUNXI_DAC_DPC,
				   0x3F << SUNXI_DAC_DPC_DVOL,
				   0 << SUNXI_DAC_DPC_DVOL);

	/*
	 * Stop issuing DRQ when we have room for less than 16 samples
	 * in our TX FIFO
	 */
	regmap_update_bits(priv->regmap, SUNXI_DAC_FIFOC,
			   3 << SUNXI_DAC_FIFOC_DRQ_CLR_CNT,
			   3 << SUNXI_DAC_FIFOC_DRQ_CLR_CNT);

	/* FIXME: is A10A inverted? */
	/* Set default volume */
	if (of_device_is_compatible(priv->dev->of_node,
				    "allwinner,sun4i-a10a-codec"))
		regmap_update_bits(priv->regmap, SUNXI_DAC_ACTL,
				   0x3f << SUNXI_DAC_ACTL_PA_VOL,
				   1 << SUNXI_DAC_ACTL_PA_VOL);
	else
		regmap_update_bits(priv->regmap, SUNXI_DAC_ACTL,
				   0x3f << SUNXI_DAC_ACTL_PA_VOL,
				   0x3b << SUNXI_DAC_ACTL_PA_VOL);
}

static int sunxi_codec_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct sunxi_priv *priv = snd_soc_card_get_drvdata(rtd->card);

	sunxi_codec_init(priv);

	return clk_prepare_enable(priv->clk_module);
}

static void sunxi_codec_shutdown(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct sunxi_priv *priv = snd_soc_card_get_drvdata(rtd->card);

	clk_disable_unprepare(priv->clk_module);
}

/*** Codec DAI ***/

static const struct snd_soc_dai_ops sunxi_codec_dai_ops = {
	.startup	= sunxi_codec_startup,
	.shutdown	= sunxi_codec_shutdown,
	.trigger	= sunxi_codec_trigger,
	.hw_params	= sunxi_codec_hw_params,
	.prepare	= sunxi_codec_prepare,
};

static struct snd_soc_dai_driver sunxi_codec_dai = {
	.name	= "Codec",
	.ops	= &sunxi_codec_dai_ops,
	.playback = {
		.stream_name	= "Codec Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rate_min	= 8000,
		.rate_max	= 192000,
		.rates		= SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_96000 |
				  SNDRV_PCM_RATE_192000 |
				  SNDRV_PCM_RATE_KNOT,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
		.sig_bits	= 24,
	},
	.capture = {
		.stream_name	= "Codec Capture",
		.channels_min	= 1,
		.channels_max	= 2,
		.rate_min	= 8000,
		.rate_max	= 192000,
		.rates		= SNDRV_PCM_RATE_8000_48000 |
				  SNDRV_PCM_RATE_96000 |
				  SNDRV_PCM_RATE_192000 |
				  SNDRV_PCM_RATE_KNOT,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE |
				  SNDRV_PCM_FMTBIT_S32_LE,
		.sig_bits	= 24,
	},
};

/*** Codec ***/
static const struct snd_kcontrol_new sun4i_codec_pa_mute =
	SOC_DAPM_SINGLE("Switch", SUNXI_DAC_ACTL,
			SUNXI_DAC_ACTL_PA_MUTE, 1, 0);

static DECLARE_TLV_DB_SCALE(sunxi_pa_volume_scale, -6300, 100, 1);

static const struct snd_kcontrol_new sunxi_codec_widgets[] = {
	SOC_SINGLE_TLV("PA Volume", SUNXI_DAC_ACTL, SUNXI_DAC_ACTL_PA_VOL,
		       0x3F, 0, sunxi_pa_volume_scale),
};

static const struct snd_kcontrol_new sun4i_codec_left_mixer_controls[] = {
	SOC_DAPM_SINGLE("Left DAC Playback Switch", SUNXI_DAC_ACTL,
			SUNXI_DAC_ACTL_LDACLMIXS, 1, 0),
};

static const struct snd_kcontrol_new sun4i_codec_right_mixer_controls[] = {
	SOC_DAPM_SINGLE("Right DAC Playback Switch", SUNXI_DAC_ACTL,
			SUNXI_DAC_ACTL_RDACRMIXS, 1, 0),
	SOC_DAPM_SINGLE("Left DAC Playback Switch", SUNXI_DAC_ACTL,
			SUNXI_DAC_ACTL_LDACRMIXS, 1, 0),
};

static const char *sunxi_dac_output_text[] = { "Muted", "Mixed", "Direct" };
static const unsigned int sunxi_dac_output_values[] = { 0x0, 0x1, 0x2 };
static SOC_VALUE_ENUM_SINGLE_DECL(dac_output_mux, SUNXI_DAC_ACTL,
				  SUNXI_DAC_ACTL_MIXPAS, 0x3,
				  sunxi_dac_output_text,
				  sunxi_dac_output_values);

static const struct snd_kcontrol_new sunxi_dac_output =
	SOC_DAPM_ENUM("DAC Output", dac_output_mux);

static const struct snd_soc_dapm_widget codec_dapm_widgets[] = {
	/* Digital parts of the DACs */
	SND_SOC_DAPM_SUPPLY("DAC", SUNXI_DAC_DPC,
			    SUNXI_DAC_DPC_EN_DA, 0,
			    NULL, 0),

	/* Analog parts of the DACs */
	SND_SOC_DAPM_DAC("Left DAC", "Codec Playback", SUNXI_DAC_ACTL,
			 SUNXI_DAC_ACTL_DACAENL, 0),
	SND_SOC_DAPM_DAC("Right DAC", "Codec Playback", SUNXI_DAC_ACTL,
			 SUNXI_DAC_ACTL_DACAENR, 0),

	/* Mixers */
	SND_SOC_DAPM_MIXER("Left Mixer", SND_SOC_NOPM, 0, 0,
			   sun4i_codec_left_mixer_controls,
			   ARRAY_SIZE(sun4i_codec_left_mixer_controls)),
	SND_SOC_DAPM_MIXER("Right Mixer", SND_SOC_NOPM, 0, 0,
			   sun4i_codec_right_mixer_controls,
			   ARRAY_SIZE(sun4i_codec_right_mixer_controls)),

	/* Global Mixer Enable */
	SND_SOC_DAPM_SUPPLY("Mixer Enable", SUNXI_DAC_ACTL,
			    SUNXI_DAC_ACTL_MIXEN, 0, NULL, 0),

	/* Pre-Amplifier */
	SND_SOC_DAPM_PGA("Pre-Amplifier", SUNXI_ADC_ACTL,
			 SUNXI_ADC_ACTL_PA_EN, 0, NULL, 0),
	SND_SOC_DAPM_SWITCH("Pre-Amplifier Mute", SND_SOC_NOPM, 0, 0,
			    &sun4i_codec_pa_mute),

	SND_SOC_DAPM_MUX("DAC Output", SUNXI_DAC_ACTL, SUNXI_DAC_ACTL_MIXPAS, 0, &sunxi_dac_output),

	SND_SOC_DAPM_OUTPUT("Mic Bias"),
	SND_SOC_DAPM_OUTPUT("HP Right"),
	SND_SOC_DAPM_OUTPUT("HP Left"),
	SND_SOC_DAPM_INPUT("MIC_IN"),
	SND_SOC_DAPM_INPUT("LINE_IN"),
};

static const struct snd_soc_dapm_route codec_dapm_routes[] = {
	/* Left DAC Routes */
	{ "Left DAC", NULL, "DAC" },

	/* Right DAC Routes */
	{ "Right DAC", NULL, "DAC" },

	/* DAC -> PA path */
	{ "DAC Output", "Direct", "Left DAC" },
	{ "DAC Output", "Direct", "Right DAC" },
	{ "Pre-Amplifier", NULL, "DAC Output"},

	/* Right Mixer Routes */
	{ "Right Mixer", NULL, "Mixer Enable" },
	{ "Right Mixer", "Left DAC Playback Switch", "Left DAC" },
	{ "Right Mixer", "Right DAC Playback Switch", "Right DAC" },

	/* Left Mixer Routes */
	{ "Left Mixer", NULL, "Mixer Enable" },
	{ "Left Mixer", "Left DAC Playback Switch", "Left DAC" },

	/* DAC -> MIX -> PA path */
	{ "DAC Output", "Mixed", "Mixer" },
	{ "Pre-Amplifier", NULL, "DAC Output" },

	/* PA -> HP path */
	{ "Pre-Amplifier Mute", "Switch", "Pre-Amplifier" },
	{ "HP Right", NULL, "Pre-Amplifier Mute" },
	{ "HP Left", NULL, "Pre-Amplifier Mute" },
};

static struct snd_soc_codec_driver sunxi_codec = {
	.controls = sunxi_codec_widgets,
	.num_controls = ARRAY_SIZE(sunxi_codec_widgets),
	.dapm_widgets = codec_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(codec_dapm_widgets),
	.dapm_routes = codec_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(codec_dapm_routes),
};

/*** Board routing ***/
/* TODO: do this with DT */

static const struct snd_soc_dapm_widget sunxi_board_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
};

static const struct snd_soc_dapm_route sunxi_board_routing[] = {
	{ "Headphone Jack",	NULL,	"HP Right" },
	{ "Headphone Jack",	NULL,	"HP Left" },
};

/*** Card and DAI Link ***/

static struct snd_soc_dai_link cdc_dai = {
	.name = "cdc",

	.stream_name = "CDC PCM",
	.codec_dai_name = "Codec",
	.cpu_dai_name = "1c22c00.codec",
	.codec_name = "1c22c00.codec",
	.platform_name = "1c22c00.codec",
	.dai_fmt = SND_SOC_DAIFMT_I2S,
};

static struct snd_soc_card snd_soc_sunxi_codec = {
	.name = "sunxi-codec",
	.owner = THIS_MODULE,
	.dai_link = &cdc_dai,
	.num_links = 1,
	.dapm_widgets = sunxi_board_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sunxi_board_dapm_widgets),
	.dapm_routes = sunxi_board_routing,
	.num_dapm_routes = ARRAY_SIZE(sunxi_board_routing),
};

/*** CPU DAI ***/

static const struct snd_soc_component_driver sunxi_codec_component = {
	.name = "sunxi-codec",
};

#define SUNXI_RATES	SNDRV_PCM_RATE_8000_192000
#define SUNXI_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | \
			SNDRV_PCM_FMTBIT_S32_LE)

static struct snd_soc_dai_driver dummy_cpu_dai = {
	.name = "sunxi-cpu-dai",
	.probe = sunxi_codec_dai_probe,
	.playback = {
		.stream_name	= "Playback",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates		= SUNXI_RATES,
		.formats	= SUNXI_FORMATS,
		.sig_bits	= 24,
	},
	.capture = {
		.stream_name	= "Capture",
		.channels_min	= 1,
		.channels_max	= 2,
		.rates 		= SUNXI_RATES,
		.formats 	= SUNXI_FORMATS,
		.sig_bits	= 24,
	 },
};

static const struct regmap_config sunxi_codec_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = SUNXI_AC_MIC_PHONE_CAL,
};

static const struct of_device_id sunxi_codec_of_match[] = {
	{ .compatible = "allwinner,sun4i-a10a-codec" },
	{ .compatible = "allwinner,sun4i-a10-codec" },
	{ .compatible = "allwinner,sun5i-a13-codec" },
	{ .compatible = "allwinner,sun7i-a20-codec" },
	{}
};
MODULE_DEVICE_TABLE(of, sunxi_codec_of_match);

static int sunxi_codec_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &snd_soc_sunxi_codec;
	struct device *dev = &pdev->dev;
	struct sunxi_priv *priv;
	struct resource *res;
	void __iomem *base;
	int ret;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = &pdev->dev;
	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, priv);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	priv->regmap = devm_regmap_init_mmio(&pdev->dev, base,
					     &sunxi_codec_regmap_config);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	/* Get the clocks from the DT */
	priv->clk_apb = devm_clk_get(dev, "apb");
	if (IS_ERR(priv->clk_apb)) {
		dev_err(dev, "failed to get apb clock\n");
		return PTR_ERR(priv->clk_apb);
	}

	priv->clk_module = devm_clk_get(dev, "codec");
	if (IS_ERR(priv->clk_module)) {
		dev_err(dev, "failed to get codec clock\n");
		return PTR_ERR(priv->clk_module);
	}

	/* Enable the clock on a basic rate */
	ret = clk_set_rate(priv->clk_module, 24576000);
	if (ret) {
		dev_err(dev, "failed to set codec base clock rate\n");
		return ret;
	}

	/* Enable the bus clock */
	if (clk_prepare_enable(priv->clk_apb)) {
		dev_err(dev, "failed to enable apb clock\n");
		clk_disable_unprepare(priv->clk_module);
		return -EINVAL;
	}

	/* DMA configuration for TX FIFO */
	priv->playback_dma_data.addr = res->start + SUNXI_DAC_TXDATA;
	priv->playback_dma_data.maxburst = 4;
	priv->playback_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;

	/* DMA configuration for RX FIFO */
	priv->capture_dma_data.addr = res->start + SUNXI_ADC_RXDATA;
	priv->capture_dma_data.maxburst = 4;
	priv->capture_dma_data.addr_width = DMA_SLAVE_BUSWIDTH_2_BYTES;

	ret = snd_soc_register_codec(&pdev->dev, &sunxi_codec, &sunxi_codec_dai,
				     1);

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &sunxi_codec_component,
					      &dummy_cpu_dai, 1);
	if (ret)
		goto err_clk_disable;

	ret = devm_snd_dmaengine_pcm_register(&pdev->dev, NULL, 0);
	if (ret)
		goto err_clk_disable;

	sunxi_codec_init(priv);

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto err_fini_utils;
	}

	ret = snd_soc_of_parse_audio_routing(card, "routing");
	if (ret)
		goto err;

	return 0;

err_fini_utils:
err:
err_clk_disable:
	clk_disable_unprepare(priv->clk_apb);
	return ret;
}

static int sunxi_codec_remove(struct platform_device *pdev)
{
	struct sunxi_priv *priv = platform_get_drvdata(pdev);

	clk_disable_unprepare(priv->clk_apb);
	clk_disable_unprepare(priv->clk_module);

	return 0;
}

static struct platform_driver sunxi_codec_driver = {
	.driver = {
		.name = "sunxi-codec",
		.owner = THIS_MODULE,
		.of_match_table = sunxi_codec_of_match,
	},
	.probe = sunxi_codec_probe,
	.remove = sunxi_codec_remove,
};
module_platform_driver(sunxi_codec_driver);

MODULE_DESCRIPTION("sunxi codec ASoC driver");
MODULE_AUTHOR("Emilio López <emilio@elopez.com.ar>");
MODULE_AUTHOR("Jon Smirl <jonsmirl@gmail.com>");
MODULE_LICENSE("GPL");
