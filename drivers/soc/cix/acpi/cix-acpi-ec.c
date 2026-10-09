// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026 Cix Technology Group Co., Ltd.
 * CIX ACPI EC platform driver entry.
 */

#include <linux/acpi.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "cix-acpi-ec.h"
#include "../cix-plat-base.h"

#define CIX_EC_ACPI_HID "CIXHA015"
#define CIX_EC_MAX_ASL_ARGS 16

struct cix_aec_asl_item {
	struct list_head node;
	char name[5];
	int argc;
};

struct cix_aec_ctx {
	struct list_head items;
	int item_count;
};

static struct cix_aec_ctx cix_aec_ctx = {
	.items = LIST_HEAD_INIT(cix_aec_ctx.items),
	.item_count = 0,
};

struct cix_aec_scan_ctx {
	int err;
};

static const char * const cix_aec_method_blacklist[] = {
	"_STA",
	"_INI",
	"REST",
	"STAT",
	"STOP",
	"READ",
	"WRIT",
	"CKSB",
	"CLRB",
	"SETB",
	"CLRF",
	"TRAS",
	"EVNT",
	"NTII",
};

static bool cix_aec_method_is_blacklisted(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cix_aec_method_blacklist); i++) {
		if (!strcmp(name, cix_aec_method_blacklist[i]))
			return true;
	}

	return false;
}

static const struct cix_aec_asl_item *cix_aec_find_method_spec(const char *name)
{
	struct cix_aec_asl_item *item;

	list_for_each_entry(item, &cix_aec_ctx.items, node)
		if (!strcmp(name, item->name))
			return item;

	return NULL;
}

static void cix_aec_clear_methods(void)
{
	struct cix_aec_asl_item *item;
	struct cix_aec_asl_item *tmp;

	list_for_each_entry_safe(item, tmp, &cix_aec_ctx.items, node) {
		list_del(&item->node);
		kfree(item);
	}
	cix_aec_ctx.item_count = 0;
}

static acpi_status cix_aec_scan_method_cb(acpi_handle handle, u32 level,
					  void *context, void **return_value)
{
	struct cix_aec_scan_ctx *ctx = context;
	struct cix_aec_asl_item *item;
	struct acpi_device_info *info = NULL;
	struct acpi_buffer name_buf = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;

	status = acpi_get_name(handle, ACPI_SINGLE_NAME, &name_buf);
	if (ACPI_FAILURE(status))
		return AE_OK;

	if (cix_aec_method_is_blacklisted(name_buf.pointer)) {
		kfree(name_buf.pointer);
		return AE_OK;
	}

	item = kzalloc(sizeof(*item), GFP_KERNEL);
	if (!item) {
		ctx->err = -ENOMEM;
		kfree(name_buf.pointer);
		return AE_CTRL_TERMINATE;
	}

	strscpy(item->name, name_buf.pointer, sizeof(item->name));
	item->argc = -1;

	status = acpi_get_object_info(handle, &info);
	if (ACPI_SUCCESS(status) && info)
		item->argc = info->param_count;

	kfree(info);
	kfree(name_buf.pointer);
	list_add_tail(&item->node, &cix_aec_ctx.items);
	cix_aec_ctx.item_count++;

	return AE_OK;
}

static int cix_aec_scan_methods(struct cix_aec *aec)
{
	struct cix_aec_scan_ctx ctx = {
		.err = 0,
	};
	acpi_status status;

	cix_aec_clear_methods();

	status = acpi_walk_namespace(ACPI_TYPE_METHOD, aec->handle, ACPI_UINT32_MAX,
				     cix_aec_scan_method_cb, NULL, &ctx, NULL);
	if (ACPI_FAILURE(status) && status != AE_CTRL_TERMINATE)
		return -EIO;
	if (ctx.err)
		return ctx.err;
	if (!cix_aec_ctx.item_count)
		return -ENOENT;

	return 0;
}

static char *cix_aec_next_token(char **cursor)
{
	char *tok;

	while ((tok = strsep(cursor, " \t\n")) != NULL) {
		if (*tok != '\0')
			return tok;
	}

	return NULL;
}

static ssize_t cix_aec_dump_methods(char *buf)
{
	ssize_t len = 0;
	struct cix_aec_asl_item *item;

	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "METHOD\tARGC\n");
	len += scnprintf(buf + len, PAGE_SIZE - len,
			 "------\t----\n");

	list_for_each_entry(item, &cix_aec_ctx.items, node) {
		if (item->argc >= 0) {
			len += scnprintf(buf + len, PAGE_SIZE - len, "%s\t%d\n",
					 item->name, item->argc);
		} else {
			len += scnprintf(buf + len, PAGE_SIZE - len, "%s\t?\n",
					 item->name);
		}
		if (len >= PAGE_SIZE - 1)
			break;
	}

	return len;
}

static ssize_t asl_call_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&aec->lock);
	len = cix_aec_dump_methods(buf);
	mutex_unlock(&aec->lock);

	return len;
}

static ssize_t asl_methods_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&aec->lock);
	len = cix_aec_dump_methods(buf);
	mutex_unlock(&aec->lock);

	return len;
}
static DEVICE_ATTR_RO(asl_methods);

static ssize_t asl_call_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct cix_aec *aec = dev_get_drvdata(dev);
	const struct cix_aec_asl_item *method;
	union acpi_object objs[CIX_EC_MAX_ASL_ARGS];
	struct acpi_object_list arg_list = {
		.pointer = objs,
	};
	unsigned long long number;
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	char *kbuf;
	char *cursor;
	char *tok;
	int argc = 0;
	acpi_status status;

	kbuf = kstrdup(buf, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	cursor = kbuf;
	tok = cix_aec_next_token(&cursor);
	if (!tok)
		goto err_inval;

	method = cix_aec_find_method_spec(tok);
	if (!method)
		goto err_noent;

	while ((tok = cix_aec_next_token(&cursor)) != NULL) {
		if (argc >= CIX_EC_MAX_ASL_ARGS)
			goto err_too_many;
		if (!kstrtoull(tok, 0, &number)) {
			objs[argc].type = ACPI_TYPE_INTEGER;
			objs[argc].integer.value = number;
		} else {
			objs[argc].type = ACPI_TYPE_STRING;
			objs[argc].string.pointer = tok;
			objs[argc].string.length = strlen(tok);
		}
		argc++;
	}

	if (method->argc >= 0 && argc != method->argc)
		goto err_inval;

	arg_list.count = argc;
	mutex_lock(&aec->lock);
	status = acpi_evaluate_object(aec->handle, (acpi_string)method->name,
				      argc ? &arg_list : NULL, &out);
	if (ACPI_FAILURE(status))
		goto err_eval;
	mutex_unlock(&aec->lock);

	kfree(out.pointer);
	kfree(kbuf);
	return count;
err_inval:
	kfree(kbuf);
	return -EINVAL;
err_noent:
	kfree(kbuf);
	return -ENOENT;
err_too_many:
	kfree(kbuf);
	return -E2BIG;
err_eval:
	mutex_unlock(&aec->lock);
	kfree(out.pointer);
	kfree(kbuf);
	return -EIO;
}
static DEVICE_ATTR_RW(asl_call);

static int cix_aec_probe(struct platform_device *pdev)
{
	struct cix_aec *aec;
	struct class *cix_class;
	int ret;

	aec = devm_kzalloc(&pdev->dev, sizeof(*aec), GFP_KERNEL);
	if (!aec)
		return -ENOMEM;

	aec->dev = &pdev->dev;
	aec->handle = ACPI_HANDLE(&pdev->dev);
	if (!aec->handle)
		return -ENODEV;

	mutex_init(&aec->lock);
	platform_set_drvdata(pdev, aec);

	cix_class = cix_plat_base_get_class();
	if (!cix_class)
		return -EPROBE_DEFER;

	aec->class_dev = device_create(cix_class, &pdev->dev,
				       MKDEV(0, 0), aec, "ec");
	if (IS_ERR(aec->class_dev))
		return PTR_ERR(aec->class_dev);

	ret = device_create_file(aec->class_dev, &dev_attr_asl_call);
	if (ret)
		goto err_destroy_class_dev;

	ret = device_create_file(aec->class_dev, &dev_attr_asl_methods);
	if (ret)
		goto err_remove_asl_call;

	ret = cix_aec_scan_methods(aec);
	if (ret)
		goto err_remove_asl_methods;

	ret = cix_aec_gpio_init(aec);
	if (ret)
		goto err_free_asl_list;

	ret = cix_aec_light_init(aec);
	if (ret)
		goto err_gpio;

	ret = cix_aec_fan_init(aec);
	if (ret) {
		cix_aec_light_exit(aec);
		goto err_gpio;
	}

	return 0;

err_gpio:
	cix_aec_gpio_exit(aec);
err_free_asl_list:
	cix_aec_clear_methods();
err_remove_asl_methods:
	device_remove_file(aec->class_dev, &dev_attr_asl_methods);
err_remove_asl_call:
	device_remove_file(aec->class_dev, &dev_attr_asl_call);
err_destroy_class_dev:
	device_unregister(aec->class_dev);
	aec->class_dev = NULL;
	return ret;
}

static int cix_aec_remove(struct platform_device *pdev)
{
	struct cix_aec *aec = platform_get_drvdata(pdev);

	cix_aec_fan_exit(aec);
	cix_aec_light_exit(aec);
	cix_aec_gpio_exit(aec);
	if (aec->class_dev) {
		cix_aec_clear_methods();
		device_remove_file(aec->class_dev, &dev_attr_asl_methods);
		device_remove_file(aec->class_dev, &dev_attr_asl_call);
		device_unregister(aec->class_dev);
		aec->class_dev = NULL;
	}

	return 0;
}

static const struct acpi_device_id cix_aec_acpi_match[] = {
	{ CIX_EC_ACPI_HID, 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, cix_aec_acpi_match);

static struct platform_driver cix_aec_driver = {
	.probe = cix_aec_probe,
	.remove = cix_aec_remove,
	.driver = {
		.name = "cix-acpi-ec",
		.acpi_match_table = cix_aec_acpi_match,
	},
};
module_platform_driver(cix_aec_driver);

MODULE_AUTHOR("Zichar Zhang <zichar.zhang@cixtech.com>");
MODULE_DESCRIPTION("CIX ACPI EC platform driver");
MODULE_ALIAS("platform:cix-acpi-ec");
MODULE_LICENSE("GPL");
