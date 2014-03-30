/*
 * at24.h - platform_data for the at24 (generic eeprom) driver
 * (C) Copyright 2008 by Pengutronix
 * (C) Copyright 2012 by Wolfram Sang
 * same license as the driver
 */

#ifndef _LINUX_AT24_H
#define _LINUX_AT24_H

#include <linux/types.h>
#include <linux/memory.h>

/**
 * struct at24_platform_data - data to set up at24 (generic eeprom) driver
 * @byte_len: size of eeprom in byte
 * @page_size: number of byte which can be written in one go
 * @flags: tunable options, check AT24_FLAG_* defines
 * @setup: an optional callback invoked after eeprom is probed; enables kernel
	code to access eeprom via memory_accessor, see example
 * @context: optional parameter passed to setup()
 */

struct at24_platform_data {
	u32		byte_len;		/* size (sum of all addr) */
	u16		page_size;		/* for writes */
	u8		flags;
#define AT24_FLAG_ADDR16	0x80	/* address pointer is 16 bit */
#define AT24_FLAG_READONLY	0x40	/* sysfs-entry will be read-only */
#define AT24_FLAG_TAKE8ADDR	0x10	/* take always 8 addresses (24c00) */
};

#endif /* _LINUX_AT24_H */
