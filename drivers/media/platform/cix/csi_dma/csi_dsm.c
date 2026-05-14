// SPDX-License-Identifier: GPL-2.0
/*
 * Add dsm interface for vi system
 *
 * Copyright 2025 Cix Technology Group Co., Ltd.
 *
 */
#include "csi_dsm.h"

#define EVENT_ERROR  0

#undef DSM_ERROR_ID_DEF
#define DSM_ERROR_ID_DEF(name, subname, value) \
		{ .error_id = DSM_##name##_##subname, .error_name = #subname }

static dsm_error_word csi_error_words[] = { DSM_CSI_DMA_ERR_LIST };

static dsm_error_map csi_error_map = {
	.num = ARRAY_SIZE(csi_error_words),
	.words = csi_error_words,
};

struct dsm_dev csi_dsm_dev = {
	.name = "csi",
	.device_name = NULL,
	.ic_name = NULL,
	.module_name = NULL,
	.fops = NULL,
	.error_map = &csi_error_map,
	.buff_size = 1000,
};

struct dsm_client *csi_dsm_init(void)
{
	struct dsm_client *dc = dsm_register_client(&csi_dsm_dev);

	if (!dc)
		pr_err("Failed to register csi dsm device");

	return dc;
}

int csi_dsm_remove(struct dsm_client *dsm_client)
{
	if (dsm_client)
		dsm_unregister_client(dsm_client, &csi_dsm_dev);

	return 0;
}

void dsm_send_event(struct dsm_client *dsm_client, char *msg)
{
	if (!dsm_client)
		return;

	if (!dsm_client_ocuppy(dsm_client)) {
		dsm_client_record(dsm_client, msg);
		dsm_client_notify(dsm_client,
				  csi_error_words[EVENT_ERROR].error_id);
	}
}
