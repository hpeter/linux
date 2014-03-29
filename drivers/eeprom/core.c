/*
 * EEPROM framework core.
 *
 * Copyright (C) 2013 Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/device.h>
#include <linux/eeprom.h>
#include <linux/export.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

static DEFINE_MUTEX(eeprom_list_mutex);
static LIST_HEAD(eeprom_list);
static DEFINE_IDA(eeprom_ida);

static ssize_t bin_attr_eeprom_read(struct file *filp, struct kobject *kobj,
				    struct bin_attribute *attr,
				    char *buf, loff_t offset, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct eeprom_device *eeprom = container_of(dev, struct eeprom_device,
						    dev);

	return eeprom->read(eeprom, buf, offset, count);
}

static ssize_t bin_attr_eeprom_write(struct file *filp, struct kobject *kobj,
				     struct bin_attribute *attr,
				     char *buf, loff_t offset, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct eeprom_device *eeprom = container_of(dev, struct eeprom_device,
						    dev);

	return eeprom->write(eeprom, buf, offset, count);
}

static struct bin_attribute bin_attr_eeprom = {
	.attr	= {
		.name	= "eeprom",
		.mode	= 0660,
	},
	.read	= bin_attr_eeprom_read,
	.write	= bin_attr_eeprom_write,
};

static struct bin_attribute *eeprom_bin_attributes[] = {
	&bin_attr_eeprom,
	NULL,
};

static const struct attribute_group eeprom_bin_group = {
	.bin_attrs	= eeprom_bin_attributes,
};

static const struct attribute_group *eeprom_dev_groups[] = {
	&eeprom_bin_group,
	NULL,
};

static void eeprom_release(struct device *dev)
{
	struct eeprom_device *eeprom = container_of(dev, struct eeprom_device,
						    dev);

	kfree(eeprom);
}

static struct class eeprom_class = {
	.name		= "eeprom",
	.dev_groups	= eeprom_dev_groups,
	.dev_release	= eeprom_release,
};

struct eeprom_device *eeprom_alloc(struct device *dev, size_t priv_size)
{
	struct eeprom_device *eeprom;

	eeprom = kzalloc(sizeof(struct eeprom_device) + priv_size, GFP_KERNEL);
	if (!eeprom)
		return ERR_PTR(-ENOMEM);

	eeprom->id = ida_simple_get(&eeprom_ida, 0, 0, GFP_KERNEL);
	if (eeprom->id < 0)
		return ERR_PTR(eeprom->id);

	eeprom->dev.class = &eeprom_class;
	eeprom->dev.parent = dev;
	eeprom->dev.of_node = dev ? dev->of_node : NULL;
	dev_set_name(&eeprom->dev, "eeprom%d", eeprom->id);
	device_initialize(&eeprom->dev);

	return eeprom;
}
EXPORT_SYMBOL(eeprom_alloc);

int eeprom_register(struct eeprom_device *eeprom)
{
	if (!eeprom)
		return -EINVAL;

	mutex_lock(&eeprom_list_mutex);
	list_add(&eeprom->list, &eeprom_list);
	mutex_unlock(&eeprom_list_mutex);

	dev_dbg(&eeprom->dev, "Registering eeprom device %s\n",
		dev_name(&eeprom->dev));

	return device_add(&eeprom->dev);
}
EXPORT_SYMBOL(eeprom_register);

int eeprom_unregister(struct eeprom_device *eeprom)
{
	device_del(&eeprom->dev);

	mutex_lock(&eeprom_list_mutex);
	list_del(&eeprom->list);
	mutex_unlock(&eeprom_list_mutex);

	return 0;
}
EXPORT_SYMBOL(eeprom_unregister);

struct eeprom_cell *of_eeprom_cell_get(struct device_node *node, const char *id)
{
	struct eeprom_device *eeprom, *e;
	struct of_phandle_args args;
	struct eeprom_cell *cell;
	int ret, index = 0;

	if (id)
		index = of_property_match_string(node,
						 "eeprom-names",
						 id);

	ret = of_parse_phandle_with_args(node, "eeproms",
					 "#eeprom-cells", index, &args);
	if (ret)
		return ERR_PTR(ret);

	mutex_lock(&eeprom_list_mutex);
	eeprom = NULL;
	list_for_each_entry(e, &eeprom_list, list) {
		if (args.np == e->dev.of_node) {
			eeprom = e;
			break;
		}
	}
	mutex_unlock(&eeprom_list_mutex);
	of_node_put(args.np);

	if (!eeprom)
		return ERR_PTR(-EPROBE_DEFER);

	if (args.args_count != 2)
		return ERR_PTR(-EINVAL);

	cell = kzalloc(sizeof(*cell), GFP_KERNEL);
	if (!cell)
		return ERR_PTR(-ENOMEM);

	cell->eeprom = eeprom;
	cell->offset = args.args[0];
	cell->count = args.args[1];

	return cell;
}
EXPORT_SYMBOL(of_eeprom_cell_get);

struct eeprom_cell *eeprom_cell_get(struct device *dev, const char *id)
{
	if (!dev)
		return ERR_PTR(-EINVAL);

	/* First, attempt to retrieve the cell through the DT */
	if (dev->of_node)
		return of_eeprom_cell_get(dev->of_node, id);

	/* We don't support anything else yet */
	return ERR_PTR(-ENODEV);
}
EXPORT_SYMBOL(eeprom_cell_get);

void eeprom_cell_put(struct eeprom_cell *cell)
{
	kfree(cell);
}
EXPORT_SYMBOL(eeprom_cell_put);

char *eeprom_cell_read(struct eeprom_cell *cell, ssize_t *len)
{
	char *buf;

	if (!cell)
		return ERR_PTR(-EINVAL);

	if (!cell->eeprom)
		return ERR_PTR(-EINVAL);

	if (!cell->eeprom->read)
		return ERR_PTR(-EINVAL);

	buf = kzalloc(cell->count, GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);

	*len = cell->eeprom->read(cell->eeprom, buf, cell->offset,
				  cell->count);
	if (!(*len)) {
		kfree(buf);
		return ERR_PTR(-EIO);
	}

	return buf;
}
EXPORT_SYMBOL(eeprom_cell_read);

int eeprom_cell_write(struct eeprom_cell *cell, const char *buf, ssize_t len)
{
	int count;

	if (!cell)
		return -EINVAL;

	if (!cell->eeprom)
		return -EINVAL;

	if (!cell->eeprom->write)
		return -EINVAL;

	count = (len < cell->count) ? len : cell->count;

	return cell->eeprom->write(cell->eeprom, buf, cell->offset,
				   count);
}
EXPORT_SYMBOL(eeprom_cell_write);

static int __init eeprom_init(void)
{
	return class_register(&eeprom_class);
}

static void __exit eeprom_exit(void)
{
	class_unregister(&eeprom_class);
}

subsys_initcall(eeprom_init);
module_exit(eeprom_exit);

MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com");
MODULE_DESCRIPTION("EEPROM Driver Core");
MODULE_LICENSE("GPL");
