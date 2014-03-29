/*
 * Allwinner sunXi SoCs Security ID support.
 *
 * Copyright (c) 2013 Oliver Schinagl <oliver@schinagl.nl>
 * Copyright (C) 2014 Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/device.h>
#include <linux/eeprom.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

struct eeprom_sid {
	void __iomem		*membase;
	size_t			size;
};

/* We read the entire key, due to a 32 bit read alignment requirement. Since we
 * want to return the requested byte, this results in somewhat slower code and
 * uses 4 times more reads as needed but keeps code simpler. Since the SID is
 * only very rarely probed, this is not really an issue.
 */
static u8 sunxi_sid_read_byte(const struct eeprom_sid *sid,
			      const unsigned int offset)
{
	u32 sid_key;

	if (offset >= sid->size)
		return 0;

	sid_key = ioread32be(sid->membase + round_down(offset, 4));
	sid_key >>= (offset % 4) * 8;

	return sid_key; /* Only return the last byte */
}

static ssize_t sunxi_sid_read(struct eeprom_device *eeprom, char *buf,
			     loff_t offset, size_t count)
{
	struct eeprom_sid *sid = eeprom_priv(eeprom);
	int i;

	if (offset < 0 || offset >= sid->size)
		return 0;

	if (count > (sid->size - offset))
		count = sid->size - offset;

	for (i = 0; i < count; i++)
		buf[i] = sunxi_sid_read_byte(sid, offset + i);

	return i;
}

static ssize_t sunxi_sid_write(struct eeprom_device *eeprom, const char *buf,
			       loff_t offset, size_t count)
{
	/* Unimplemented */
	return 0;
}

static const struct of_device_id sunxi_sid_dt_ids[];

static int sunxi_sid_probe(struct platform_device *pdev)
{
	const struct of_device_id *device;
	struct eeprom_device *eeprom;
	struct eeprom_sid *sid;
	struct resource *res;

	eeprom = eeprom_alloc(&pdev->dev, sizeof(*sid));
	if (IS_ERR(eeprom))
		return PTR_ERR(eeprom);
	platform_set_drvdata(pdev, eeprom);

	sid = eeprom_priv(eeprom);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	sid->membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(sid->membase))
		return PTR_ERR(sid->membase);

	device = of_match_device(sunxi_sid_dt_ids, &pdev->dev);
	if (!device)
		return -ENODEV;

	sid->size = (unsigned int)device->data;

	eeprom->read = sunxi_sid_read;
	eeprom->write = sunxi_sid_write;

	return eeprom_register(eeprom);
}

static int sunxi_sid_remove(struct platform_device *pdev)
{
	struct eeprom_device *eeprom = platform_get_drvdata(pdev);

	return eeprom_unregister(eeprom);
}

static const struct of_device_id sunxi_sid_dt_ids[] = {
	{ .compatible = "allwinner,sun4i-a10-sid", .data = (void*)16, },
	{ .compatible = "allwinner,sun7i-a20-sid", .data = (void*)512, },
	{},
};
MODULE_DEVICE_TABLE(of, sunxi_sid_dt_ids);

static struct platform_driver sunxi_sid_driver = {
	.probe	= sunxi_sid_probe,
	.remove	= sunxi_sid_remove,
	.driver	= {
		   .name = "sunxi-sid",
		   .of_match_table = sunxi_sid_dt_ids,
	},
};
module_platform_driver(sunxi_sid_driver);

