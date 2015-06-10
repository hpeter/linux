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
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define SUN4I_DAI_CTRL_REG		0x00
#define SUN4I_DAI_FMT0_REG		0x04
#define SUN4I_DAI_FMT1_REG		0x08
#define SUN4I_DAI_FIFO_TX_REG		0x0c
#define SUN4I_DAI_FIFO_RX_REG		0x10
#define SUN4I_DAI_FIFO_CTRL_REG		0x14
#define SUN4I_DAI_FIFO_STA_REG		0x18
#define SUN4I_DAI_INT_CTRL_REG		0x1c
#define SUN4I_DAI_INT_STA_REG		0x20
#define SUN4I_DAI_CLK_DIV_REG		0x24
#define SUN4I_DAI_RX_CNT_REG		0x28
#define SUN4I_DAI_TX_CNT_REG		0x2c
#define SUN4I_DAI_TX_CHAN_SEL_REG	0x2c
#define SUN4I_DAI_TX_CHAN_MAP_REG	0x2c

struct sun4i_dai {
	struct clk	*clk;
	struct regmap	*regmap;
};

static bool sun4i_dai_precious_reg(struct device *dev,
				   unsigned int reg)
{
	switch (reg) {
	case SUN4I_DAI_INT_STA_REG:
		return true;
	default:
		return false;
	}
}

static bool sun4i_dai_readable_reg(struct device *dev,
				   unsigned int reg)
{
	switch (reg) {
	case SUN4I_DAI_FIFO_TX_REG:
		return false;
	default:
		return true;
	}
}

static bool sun4i_dai_volatile_reg(struct device *dev,
				   unsigned int reg)
{
	switch (reg) {
	case SUN4I_DAI_FIFO_RX_REG:
	case SUN4I_DAI_FIFO_STA_REG:
	case SUN4I_DAI_INT_STA_REG:
		return true;
	default:
		return false;
	}
}

static bool sun4i_dai_writable_reg(struct device *dev,
				   unsigned int reg)
{
	switch (reg) {
	case SUN4I_DAI_FIFO_RX_REG:
	case SUN4I_DAI_FIFO_STA_REG:
		return false;
	default:
		return true;
	}
}

static const struct regmap_config sun4i_dai_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,

	.max_register	= SUN4I_DAI_TX_CHAN_MAP_REG,

	.precious_reg	= sun4i_dai_precious_reg,
	.readable_reg	= sun4i_dai_readable_reg,
	.volatile_reg	= sun4i_dai_volatile_reg,
	.writeable_reg	= sun4i_dai_writable_reg,
};

static int sun4i_dai_probe(struct platform_device *pdev)
{
	struct sun4i_dai *dai;
	struct resource *res;
	void __iomem *regs;

	dai = devm_kzalloc(&pdev->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(regs)) {
		dev_err(&pdev->dev, "Can't request IO region\n");
		return PTR_ERR(regs);
	}

	dai->regmap = devm_regmap_init_mmio(&pdev->dev, regs,
					    &sun4i_dai_regmap_config);
	if (IS_ERR(dai->regmap)) {
		dev_err(&pdev->dev, "Regmap initialisation failed\n");
		return PTR_ERR(dai->regmap);
	};
	
	dai->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(dai->clk)) {
		dev_err(&pdev->dev, "Can't get our clock\n");
		return PTR_ERR(dai->clk);
	}
	
	return 0;
}

static int sun4i_dai_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id sun4i_dai_match[] = {
	{ .compatible = "allwinner,sun4i-a10-dai", },
	{}
};
MODULE_DEVICE_TABLE(of, sun4i_dai_match);

static struct platform_driver sun4i_dai_driver = {
	.probe	= sun4i_dai_probe,
	.remove	= sun4i_dai_remove,
	.driver	= {
		.name		= "sun4i-dai",
		.of_match_table	= sun4i_dai_match,
	},
};
module_platform_driver(sun4i_dai_driver);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_DESCRIPTION("Allwinner A10 DAI driver");
MODULE_LICENSE("GPL");
