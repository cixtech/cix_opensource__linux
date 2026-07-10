# SPDX-License-Identifier: GPL-2.0
# In-tree (CONFIG_CAMERA_ARMCB) and out-of-tree / DKMS (armcb_isp.ko).

ccflags-y += -I$(src)/platform
ccflags-y += -I$(src)/common
ccflags-y += -I$(src)/common/isp_hw_if
ccflags-y += -I$(src)/sensor
ccflags-y += -I$(src)/isp
ccflags-y += -I$(src)/platform/logger
ccflags-y += -I$(src)/platform/hdmi
ccflags-y += -I$(src)/cixvihw
ifneq ($(wildcard $(srctree)/drivers/misc),)
ccflags-y += -I$(srctree)/drivers/misc
endif

ccflags-y += -DARMCB_CAM_KO
ccflags-y += -DARMCB_CAM_AHB

ifeq ($(ARMCB_CSI_DMA_DUMP_RAW),1)
ccflags-y += -DARMCB_CSI_DMA_DUMP_RAW=1
else
ccflags-y += -DARMCB_CSI_DMA_DUMP_RAW=0
endif

armcb_isp-objs := sensor/armcb_sensor.o \
	common/armcb_camera_io_drv.o \
	common/system_dma.o \
	common/armcb_v4l_sd.o \
	platform/armcb_platform.o \
	isp/armcb_vb2.o \
	isp/armcb_v4l2_stream.o \
	isp/armcb_isp_driver.o \
	isp/armcb_v4l2_config.o \
	isp/armcb_v4l2_core.o \
	sensor/actuator/armcb_actuator.o \
	common/isp_hw_if/isp_hw_ops.o \
	common/isp_hw_if/isp_hw_utils.o \
	platform/logger/system_logger.o \
	cixvihw/cix_vi_hw.o \
	armcb_isp_entry.o

ifeq ($(ARMCB_CSI_DMA_DUMP_RAW),1)
armcb_isp-objs += isp/armcb_csi_dma_dump.o
endif

ifneq ($(KERNELRELEASE),)
ifneq ($(KBUILD_EXTMOD),)
obj-m += armcb_isp.o
else
obj-$(CONFIG_CAMERA_ARMCB) += armcb_isp.o
endif
else

obj-m += armcb_isp.o

ifeq ($(MAKELEVEL),0)

ARCH ?= arm64

PATH_ROOT ?= $(abspath $(CURDIR)/../../../../../..)

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
ifneq ($(CROSS_COMPILE),)
KERNEL_SRC := $(PATH_ROOT)/linux
else ifeq ($(wildcard $(KERNEL_SRC)/Makefile),)
KERNEL_SRC := $(PATH_ROOT)/linux
endif

ifeq ($(origin CROSS_COMPILE),undefined)
ifneq ($(shell uname -m),aarch64)
CROSS_COMPILE := aarch64-none-linux-gnu-
else
CROSS_COMPILE :=
endif
endif

.PHONY: modules build clean

modules:
	$(MAKE) -C "$(KERNEL_SRC)" ARCH=$(ARCH) M="$(CURDIR)" modules

build:
	$(MAKE) -C "$(KERNEL_SRC)" ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M="$(CURDIR)" modules

clean:
	-$(MAKE) -C "$(KERNEL_SRC)" M="$(CURDIR)" clean
	find . \( -name '*.o' -o -name '*.ko' -o -name '*.mod.c' -o -name '*.mod' \
		-o -name '*.symvers' -o -name '*.order' -o -name '*.a' \
		-o -name '*.cmd' -o -name '*.o.d' \) 2>/dev/null | xargs -r rm -f

endif
endif
