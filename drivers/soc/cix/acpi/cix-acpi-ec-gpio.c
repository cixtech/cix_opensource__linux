// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026 Cix Technology Group Co., Ltd.
 * CIX ACPI EC GPIO implementation over ASL methods.
 */

#include <linux/acpi.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include "cix-acpi-ec.h"

static int cix_aec_gpio_to_asl_num(unsigned int gpio)
{
	if (gpio >= CIX_AEC_GPIO_COUNT)
		return -EINVAL;

	if (gpio >= CIX_AEC_GPIO_GAP_START)
		gpio += CIX_AEC_GPIO_GAP_SIZE;

	return gpio;
}

static int cix_aec_gpio_write(acpi_handle handle, struct mutex *lock,
			      unsigned int gpio, int value)
{
	union acpi_object args[2];
	struct acpi_object_list arg_list = {
		.count = ARRAY_SIZE(args),
		.pointer = args,
	};
	acpi_status status;
	int ec_gpio;

	if (!handle || !lock)
		return -EINVAL;

	ec_gpio = cix_aec_gpio_to_asl_num(gpio);
	if (ec_gpio < 0)
		return ec_gpio;

	args[0].type = ACPI_TYPE_INTEGER;
	args[0].integer.value = ec_gpio;
	args[1].type = ACPI_TYPE_INTEGER;
	args[1].integer.value = !!value;

	mutex_lock(lock);
	status = acpi_evaluate_object(handle, "WRGP", &arg_list, NULL);
	mutex_unlock(lock);
	if (ACPI_FAILURE(status))
		return -EIO;

	return 0;
}

static int cix_aec_gpio_read(acpi_handle handle, struct mutex *lock,
			     unsigned int gpio)
{
	union acpi_object arg;
	struct acpi_object_list arg_list = {
		.count = 1,
		.pointer = &arg,
	};
	unsigned long long val;
	acpi_status status;
	int ec_gpio;

	if (!handle || !lock)
		return -EINVAL;

	ec_gpio = cix_aec_gpio_to_asl_num(gpio);
	if (ec_gpio < 0)
		return ec_gpio;

	arg.type = ACPI_TYPE_INTEGER;
	arg.integer.value = ec_gpio;

	mutex_lock(lock);
	status = acpi_evaluate_integer(handle, "RDGP", &arg_list, &val);
	mutex_unlock(lock);
	if (ACPI_FAILURE(status))
		return -EIO;

	return !!val;
}

static int cix_aec_gpio_get_direction(struct gpio_chip *gc, unsigned int gpio)
{
	struct cix_aec *aec = gpiochip_get_data(gc);

	if (test_bit(gpio, aec->gpio_dir_out))
		return GPIO_LINE_DIRECTION_OUT;

	return GPIO_LINE_DIRECTION_IN;
}

static int cix_aec_gpio_direction_input(struct gpio_chip *gc, unsigned int gpio)
{
	struct cix_aec *aec = gpiochip_get_data(gc);

	clear_bit(gpio, aec->gpio_dir_out);
	return 0;
}

static int cix_aec_gpio_direction_output(struct gpio_chip *gc, unsigned int gpio,
					 int value)
{
	struct cix_aec *aec = gpiochip_get_data(gc);
	int ret;

	ret = cix_aec_gpio_write(aec->handle, &aec->lock, gpio, value);
	if (ret)
		return ret;

	set_bit(gpio, aec->gpio_dir_out);
	return 0;
}

static int cix_aec_gpio_get(struct gpio_chip *gc, unsigned int gpio)
{
	struct cix_aec *aec = gpiochip_get_data(gc);

	return cix_aec_gpio_read(aec->handle, &aec->lock, gpio);
}

static void cix_aec_gpio_set(struct gpio_chip *gc, unsigned int gpio, int value)
{
	struct cix_aec *aec = gpiochip_get_data(gc);

	cix_aec_gpio_write(aec->handle, &aec->lock, gpio, value);
}

static ssize_t gpio_count_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", CIX_AEC_GPIO_COUNT);
}
static DEVICE_ATTR_RO(gpio_count);

static ssize_t gpio_read_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	int ret;

	ret = cix_aec_gpio_read(aec->handle, &aec->lock, aec->gpio_read_index);
	if (ret < 0)
		return ret;

	return sysfs_emit(buf, "%d\n", ret);
}

static ssize_t gpio_read_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	unsigned int gpio;

	if (kstrtouint(buf, 0, &gpio))
		return -EINVAL;

	aec->gpio_read_index = gpio;

	return count;
}
static DEVICE_ATTR_RW(gpio_read);

static ssize_t gpio_write_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	unsigned int gpio;
	unsigned int value;
	int ret;

	if (sscanf(buf, "%u %u", &gpio, &value) != 2)
		return -EINVAL;

	ret = cix_aec_gpio_write(aec->handle, &aec->lock, gpio, value);
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_WO(gpio_write);

static struct attribute *cix_aec_gpio_attrs[] = {
	&dev_attr_gpio_count.attr,
	&dev_attr_gpio_read.attr,
	&dev_attr_gpio_write.attr,
	NULL,
};

static const struct attribute_group cix_aec_gpio_attr_group = {
	.name = "gpio",
	.attrs = cix_aec_gpio_attrs,
};

int cix_aec_gpio_init(struct cix_aec *aec)
{
	int ret;

	ret = sysfs_create_group(&aec->class_dev->kobj, &cix_aec_gpio_attr_group);
	if (ret)
		return ret;

	bitmap_zero(aec->gpio_dir_out, CIX_AEC_GPIO_COUNT);

	aec->gc.label = "cix-acpi-ec-gpio";
	aec->gc.parent = aec->dev;
	aec->gc.owner = THIS_MODULE;
	aec->gc.base = -1;
	aec->gc.ngpio = CIX_AEC_GPIO_COUNT;
	aec->gc.can_sleep = true;
	aec->gc.get_direction = cix_aec_gpio_get_direction;
	aec->gc.direction_input = cix_aec_gpio_direction_input;
	aec->gc.direction_output = cix_aec_gpio_direction_output;
	aec->gc.get = cix_aec_gpio_get;
	aec->gc.set = cix_aec_gpio_set;

	ret = devm_gpiochip_add_data(aec->dev, &aec->gc, aec);
	if (ret) {
		sysfs_remove_group(&aec->class_dev->kobj, &cix_aec_gpio_attr_group);
		return ret;
	}

	return 0;
}

void cix_aec_gpio_exit(struct cix_aec *aec)
{
	sysfs_remove_group(&aec->class_dev->kobj, &cix_aec_gpio_attr_group);
}
