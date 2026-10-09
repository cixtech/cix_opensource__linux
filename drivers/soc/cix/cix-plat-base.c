// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2026 Cix Technology Group Co., Ltd.
 * CIX platform base context implementation.
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>

#include "cix-plat-base.h"

static struct class *cix_class;

struct class *cix_plat_base_get_class(void)
{
	return cix_class;
}

static int __init cix_plat_base_init(void)
{
	cix_class = class_create("cix");
	if (IS_ERR(cix_class)) {
		int ret = PTR_ERR(cix_class);

		cix_class = NULL;
		return ret;
	}

	return 0;
}
subsys_initcall(cix_plat_base_init);

MODULE_AUTHOR("Zichar Zhang <zichar.zhang@cixtech.com>");
MODULE_DESCRIPTION("CIX platform base context");
MODULE_ALIAS("cix-plat-base");
MODULE_LICENSE("GPL");
