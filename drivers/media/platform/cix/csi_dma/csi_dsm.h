/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Add dsm interface for vi syste
 *
 * Copyright 2025 Cix Technology Group Co., Ltd.
 *
 */
#ifndef __CSI_DSM_H_
#define __CSI_DSM_H_

#include <linux/soc/cix/dsm_pub.h>

#ifdef CONFIG_PLAT_DSM_SYSEVENT
struct dsm_client *csi_dsm_init(void);
int csi_dsm_remove(struct dsm_client *dsm_client);
void dsm_send_event(struct dsm_client *dsm_client, char *msg);
#else
static inline struct dsm_client *csi_dsm_init(void)
{
	return NULL;
}

static inline int csi_dsm_remove(struct dsm_client *dsm_client)
{
	return 0;
}

static void dsm_send_event(struct dsm_client *dsm_client, char *msg)
{
}
#endif // !CONFIG_PLAT_DSM_SYSEVENT

#endif
