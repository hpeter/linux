/*
 * sound\soc\sunxi\audiocodec\sunxi_sndcodec.c
 * (C) Copyright 2010-2016
 * Reuuimlla Technology Co., Ltd. <www.reuuimllatech.com>
 * huangxin <huangxin@Reuuimllatech.com>
 *
 * some simple description for this code
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/clk.h>
#include <linux/mutex.h>

#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include <linux/io.h>

static const struct snd_soc_dapm_widget sun6i_card_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
};

static const struct snd_soc_dapm_route sun6i_card_route[] = {
	{ "Headphone Jack",	NULL,	"HPL" },
	{ "Headphone Jack",	NULL,	"HPR" },
};

static int sun6i_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;

	snd_soc_dapm_enable_pin(dapm, "Headphone Jack");

	return 0;
}

static struct snd_soc_dai_link sun6i_card_dai[] = {
	{
		.name		= "sun6i-audio",
		.stream_name	= "sun6i-test",

		.cpu_dai_name	= "sun6i-audio-pcm",
		.codec_dai_name = "sun6i-audio-codec-pcm",

		.platform_name	= "sun6i-audio-pcm",
		.codec_name	= "sun6i-audio-codec",

		.init		= sun6i_dai_init,
	},
};

static struct snd_soc_card sun6i_codec_card = {
	.name	= "sun6i-audio-card",
	.owner	= THIS_MODULE,

	.dai_link = sun6i_card_dai,
	.num_links = ARRAY_SIZE(sun6i_card_dai),

	.dapm_widgets = sun6i_card_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sun6i_card_dapm_widgets),
	.dapm_routes = sun6i_card_route,
	.num_dapm_routes = ARRAY_SIZE(sun6i_card_route),
};

static struct platform_device *sun6i_dai_device;
static struct platform_device *sun6i_codec_device;

static int sun6i_audio_card_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &sun6i_codec_card;
	int ret;

	printk("Blip\n");

	sun6i_dai_device = platform_device_alloc("sun6i-audio-pcm", -1);
	if (!sun6i_dai_device)
		return -ENOMEM;

	/* sun6i_dai_device->dev.parent = &pdev->dev; */
	/* sun6i_dai_device->dev.of_node = pdev->dev.of_node; */

	printk("Bloup\n");

	ret = platform_device_add(sun6i_dai_device);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't register DAI device\n");
		goto err_free_dai;
	}

	/* sun6i_codec_device->dev.parent = &pdev->dev; */
	/* sun6i_codec_device->dev.of_node = pdev->dev.of_node; */

	printk("Blop\n");

	sun6i_codec_device = platform_device_alloc("sun6i-audio-codec", -1);
	if (!sun6i_codec_device)
		goto err_unregister_dai;

	printk("Blap\n");

	ret = platform_device_add(sun6i_codec_device);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't register CODEC device\n");
		goto err_free_codec;
	}

	printk("Blep\n");

	card->dev = &pdev->dev;
	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "Couldn't register ASoC card\n");
		goto err_unregister_codec;
	}

	printk("Pfiouuu\n");

	return 0;

err_unregister_codec:
	platform_device_del(sun6i_codec_device);
err_free_codec:
	platform_device_put(sun6i_codec_device);
err_unregister_dai:
	platform_device_del(sun6i_dai_device);
err_free_dai:
	platform_device_put(sun6i_dai_device);

	return ret;
}

static int sun6i_audio_card_remove(struct platform_device *pdev)
{
	snd_soc_unregister_card(&sun6i_codec_card);

	platform_device_unregister(sun6i_codec_device);
	platform_device_unregister(sun6i_dai_device);

	return 0;
};

static const struct of_device_id sun6i_audio_card_dt_ids[] = {
	{ .compatible = "allwinner,sun6i-a31-audio-codec", },
	{}
};
MODULE_DEVICE_TABLE(of, sun6i_codec_match);

static struct platform_driver sun6i_audio_card_driver = {
	.probe		= sun6i_audio_card_probe,
	.remove		= sun6i_audio_card_remove,
	.driver		= {
		.name	= "sun6i-audio-card",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(sun6i_audio_card_dt_ids),
	},
};
module_platform_driver(sun6i_audio_card_driver);
MODULE_AUTHOR("huangxin");
MODULE_DESCRIPTION("SUNXI_sndpcm ALSA SoC audio driver");
MODULE_LICENSE("GPL");
