// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026 Cix Technology Group Co., Ltd.
 * CIX ACPI EC light/backlight control over ASL methods.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "cix-acpi-ec.h"

static int cix_aec_get_kbl(struct cix_aec *aec, int *percent, int *enable)
{
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int ret = -EIO;

	mutex_lock(&aec->lock);
	status = acpi_evaluate_object(aec->handle, "GKBL", NULL, &buffer);
	mutex_unlock(&aec->lock);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = buffer.pointer;
	if (!obj || obj->type != ACPI_TYPE_PACKAGE || obj->package.count < 2)
		goto out;
	if (obj->package.elements[0].type != ACPI_TYPE_INTEGER)
		goto out;
	if (obj->package.elements[1].type != ACPI_TYPE_INTEGER)
		goto out;

	*percent = (int)obj->package.elements[0].integer.value;
	*enable = (int)obj->package.elements[1].integer.value;
	ret = 0;
out:
	kfree(buffer.pointer);
	return ret;
}

static int cix_aec_set_kbl(struct cix_aec *aec, int percent)
{
	union acpi_object arg;
	struct acpi_object_list arg_list = {
		.count = 1,
		.pointer = &arg,
	};
	acpi_status status;

	arg.type = ACPI_TYPE_INTEGER;
	arg.integer.value = percent;

	mutex_lock(&aec->lock);
	status = acpi_evaluate_object(aec->handle, "SKBL", &arg_list, NULL);
	mutex_unlock(&aec->lock);
	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static int cix_aec_get_lss(struct cix_aec *aec, int *type, int *value)
{
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int ret = -EIO;

	mutex_lock(&aec->lock);
	status = acpi_evaluate_object(aec->handle, "GLSS", NULL, &buffer);
	mutex_unlock(&aec->lock);
	if (ACPI_FAILURE(status))
		return -EIO;

	obj = buffer.pointer;
	if (!obj || obj->type != ACPI_TYPE_PACKAGE || obj->package.count < 2)
		goto out;
	if (obj->package.elements[0].type != ACPI_TYPE_INTEGER)
		goto out;
	if (obj->package.elements[1].type != ACPI_TYPE_INTEGER)
		goto out;

	*type = (int)obj->package.elements[0].integer.value;
	*value = (int)obj->package.elements[1].integer.value;
	ret = 0;
out:
	kfree(buffer.pointer);
	return ret;
}

static ssize_t kbl_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	int percent;
	int enable;
	int ret;

	ret = cix_aec_get_kbl(aec, &percent, &enable);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d %d\n", percent, enable);
}

static ssize_t kbl_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	int percent;
	int ret;

	ret = kstrtoint(buf, 0, &percent);
	if (ret)
		return ret;

	ret = cix_aec_set_kbl(aec, percent);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(kbl);

static ssize_t lss_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	int type;
	int value;
	int ret;

	ret = cix_aec_get_lss(aec, &type, &value);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d %d\n", type, value);
}
static DEVICE_ATTR_RO(lss);

static struct attribute *cix_aec_light_attrs[] = {
	&dev_attr_kbl.attr,
	&dev_attr_lss.attr,
	NULL,
};

static const struct attribute_group cix_aec_light_attr_group = {
	.name = "light",
	.attrs = cix_aec_light_attrs,
};

int cix_aec_light_init(struct cix_aec *aec)
{
	return sysfs_create_group(&aec->class_dev->kobj, &cix_aec_light_attr_group);
}

void cix_aec_light_exit(struct cix_aec *aec)
{
	sysfs_remove_group(&aec->class_dev->kobj, &cix_aec_light_attr_group);
}
