/*
 * EEPROM framework core.
 *
 * Copyright (C) 2013 Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef _LINUX_EEPROM_H
#define _LINUX_EEPROM_H

#include <linux/device.h>
#include <linux/list.h>

struct eeprom_device {
	struct device		dev;
	struct list_head	list;
	int			id;

	ssize_t	(*read)(struct eeprom_device *, char *, loff_t, size_t);
	ssize_t	(*write)(struct eeprom_device *, const char *, loff_t, size_t);
	size_t			size;

	unsigned long		private[];
};

struct eeprom_cell {
	struct eeprom_device	*eeprom;
	loff_t			offset;
	size_t			count;
};

struct eeprom_device *eeprom_alloc(struct device *dev, size_t priv_size);
int eeprom_register(struct eeprom_device *eeprom);
int eeprom_unregister(struct eeprom_device *eeprom);

static inline void *eeprom_priv(struct eeprom_device *eeprom)
{
	return (void*)eeprom->private;
}

struct eeprom_cell *eeprom_cell_get(struct device *dev, const char *id);
void eeprom_cell_put(struct eeprom_cell *eeprom);
char *eeprom_cell_read(struct eeprom_cell *eeprom, ssize_t *len);
int eeprom_cell_write(struct eeprom_cell *eeprom, const char *buf, ssize_t len);

#endif  /* ifndef _LINUX_EEPROM_H */
