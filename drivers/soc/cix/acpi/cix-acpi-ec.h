/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2026 Cix Technology Group Co., Ltd.
 * CIX ACPI EC shared definitions for platform submodules.
 */

#ifndef __CIX_ACPI_EC_H__
#define __CIX_ACPI_EC_H__

#include <linux/acpi.h>
#include <linux/bitmap.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/mutex.h>

#define CIX_AEC_GPIO_COUNT		(88 + 16)
#define CIX_AEC_GPIO_GAP_START		88
#define CIX_AEC_GPIO_GAP_SIZE		112

struct cix_aec {
	struct device *dev;
	struct device *class_dev;
	acpi_handle handle;
	struct mutex lock;
	unsigned int gpio_read_index;
	struct gpio_chip gc;
	unsigned long gpio_dir_out[BITS_TO_LONGS(CIX_AEC_GPIO_COUNT)];

	/* EC fan state */
	int fan_mode;
	int fan_duty;
	int fan_type;
	int fan_index;
};

int cix_aec_gpio_init(struct cix_aec *aec);
void cix_aec_gpio_exit(struct cix_aec *aec);

int cix_aec_light_init(struct cix_aec *aec);
void cix_aec_light_exit(struct cix_aec *aec);

int cix_aec_fan_init(struct cix_aec *aec);
void cix_aec_fan_exit(struct cix_aec *aec);

#endif /* __CIX_ACPI_EC_H__ */
