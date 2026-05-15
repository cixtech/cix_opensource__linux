# SPDX-License-Identifier: GPL-2.0-only
ifneq ($(KERNELRELEASE),)
	ccflags-y += -I$(srctree)/drivers/media/i2c/cix/ov5645
	ccflags-y += -I$(srctree)/drivers/media/platform/cix/csi_dma
	# Power-management platform driver must load first; the I2C sensor
	# driver below resolves it via the "ov5645,power-mgr" DT phandle.
	obj-m += ov5645_power_manage.o
	obj-m += ov5645_mipi_2_lane.o
endif
