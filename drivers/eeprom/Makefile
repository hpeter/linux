#
# Makefile for eeprom drivers.
#

ccflags-$(CONFIG_EEPROM_DEBUG) += -DDEBUG

obj-$(CONFIG_EEPROM)		+= core.o

# Devices
obj-$(CONFIG_EEPROM_AT25)	+= eeprom-at25.o
obj-$(CONFIG_EEPROM_SUNXI_SID)	+= eeprom-sunxi-sid.o
