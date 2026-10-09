// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026 Cix Technology Group Co., Ltd.
 * CIX ACPI EC fan control over ASL methods.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "cix-acpi-ec.h"

enum cix_aec_fan_mode_id {
	CIX_AEC_FAN_MODE_MUTE = 0,
	CIX_AEC_FAN_MODE_NORMAL,
	CIX_AEC_FAN_MODE_MANUAL,
	CIX_AEC_FAN_MODE_PERFORMANCE,
	CIX_AEC_FAN_MODE_MAX,
};

struct cix_aec_fan_mode_desc {
	const char *name;
	const char *set_method;
};

static const struct cix_aec_fan_mode_desc cix_aec_fan_modes[CIX_AEC_FAN_MODE_MAX] = {
	[CIX_AEC_FAN_MODE_MUTE] = {
		.name = "mute",
		.set_method = "SFMT",
	},
	[CIX_AEC_FAN_MODE_NORMAL] = {
		.name = "normal",
		.set_method = "SFAT",
	},
	[CIX_AEC_FAN_MODE_MANUAL] = {
		.name = "manual",
		.set_method = "SFMN",
	},
	[CIX_AEC_FAN_MODE_PERFORMANCE] = {
		.name = "performance",
		.set_method = "SFPF",
	},
};

static int cix_aec_fan_set_mode(struct cix_aec *aec, int mode)
{
	acpi_status status;

	if (mode < 0 || mode >= CIX_AEC_FAN_MODE_MAX)
		return -EINVAL;
	if (!cix_aec_fan_modes[mode].set_method)
		return -EINVAL;

	mutex_lock(&aec->lock);
	status = acpi_evaluate_object(aec->handle,
				      (acpi_string)cix_aec_fan_modes[mode].set_method,
				      NULL, NULL);
	if (!ACPI_FAILURE(status))
		aec->fan_mode = mode;
	mutex_unlock(&aec->lock);

	return ACPI_FAILURE(status) ? -EIO : 0;
}

static int cix_aec_fan_set_pwm(struct cix_aec *aec, int duty, int type, int index)
{
	union acpi_object args[3];
	struct acpi_object_list arg_list = {
		.count = ARRAY_SIZE(args),
		.pointer = args,
	};
	unsigned long long ret;
	acpi_status status;

	args[0].type = ACPI_TYPE_INTEGER;
	args[0].integer.value = duty;
	args[1].type = ACPI_TYPE_INTEGER;
	args[1].integer.value = type;
	args[2].type = ACPI_TYPE_INTEGER;
	args[2].integer.value = index;

	mutex_lock(&aec->lock);
	status = acpi_evaluate_integer(aec->handle, "SFPW", &arg_list, &ret);
	if (!ACPI_FAILURE(status) && ret == 0) {
		aec->fan_duty = duty;
		aec->fan_type = type;
		aec->fan_index = index;
		aec->fan_mode = CIX_AEC_FAN_MODE_MANUAL;
	}
	mutex_unlock(&aec->lock);
	if (ACPI_FAILURE(status))
		return -EIO;
	if (ret != 0)
		return -EIO;

	return 0;
}

static int cix_aec_fan_get_pwm(struct cix_aec *aec)
{
	union acpi_object args[2];
	struct acpi_object_list arg_list = {
		.count = ARRAY_SIZE(args),
		.pointer = args,
	};
	unsigned long long duty;
	acpi_status status;

	args[0].type = ACPI_TYPE_INTEGER;
	args[0].integer.value = aec->fan_type;
	args[1].type = ACPI_TYPE_INTEGER;
	args[1].integer.value = aec->fan_index;

	mutex_lock(&aec->lock);
	status = acpi_evaluate_integer(aec->handle, "GFPW", &arg_list, &duty);
	mutex_unlock(&aec->lock);
	if (ACPI_FAILURE(status))
		return -EIO;
	if (duty == 0xFFFFFFFFULL)
		return -EIO;

	return (int)duty;
}

static ssize_t fan_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct cix_aec *aec = dev_get_drvdata(dev);

	if (aec->fan_mode < 0 || aec->fan_mode >= CIX_AEC_FAN_MODE_MAX)
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", cix_aec_fan_modes[aec->fan_mode].name);
}

static ssize_t fan_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	int mode;
	int ret;

	for (mode = 0; mode < CIX_AEC_FAN_MODE_MAX; mode++) {
		if (sysfs_streq(buf, cix_aec_fan_modes[mode].name))
			break;
	}
	if (mode == CIX_AEC_FAN_MODE_MAX)
		return -EINVAL;

	ret = cix_aec_fan_set_mode(aec, mode);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(fan_mode);

static ssize_t fan_available_mode_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	ssize_t len = 0;
	int i;

	for (i = 0; i < CIX_AEC_FAN_MODE_MAX; i++) {
		len += scnprintf(buf + len, PAGE_SIZE - len, "%s%s",
				 cix_aec_fan_modes[i].name,
				 (i == CIX_AEC_FAN_MODE_MAX - 1) ? "\n" : " ");
	}

	return len;
}
static DEVICE_ATTR_RO(fan_available_mode);

static ssize_t fan_pwm_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	int duty;

	duty = cix_aec_fan_get_pwm(aec);
	if (duty < 0)
		return duty;

	return sysfs_emit(buf, "%d\n", duty);
}

static ssize_t fan_pwm_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	int duty;
	int type;
	int index;
	int ret;

	ret = sscanf(buf, "%d %d %d", &duty, &type, &index);
	if (ret != 3)
		return -EINVAL;

	ret = cix_aec_fan_set_pwm(aec, duty, type, index);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(fan_pwm);

static struct attribute *cix_aec_fan_attrs[] = {
	&dev_attr_fan_mode.attr,
	&dev_attr_fan_available_mode.attr,
	&dev_attr_fan_pwm.attr,
	NULL,
};

static const struct attribute_group cix_aec_fan_attr_group = {
	.name = "fan",
	.attrs = cix_aec_fan_attrs,
};

int cix_aec_fan_init(struct cix_aec *aec)
{
	aec->fan_mode = CIX_AEC_FAN_MODE_NORMAL;
	aec->fan_type = 0;
	aec->fan_index = 0;
	aec->fan_duty = -1;

	return sysfs_create_group(&aec->class_dev->kobj, &cix_aec_fan_attr_group);
}

void cix_aec_fan_exit(struct cix_aec *aec)
{
	sysfs_remove_group(&aec->class_dev->kobj, &cix_aec_fan_attr_group);
}
