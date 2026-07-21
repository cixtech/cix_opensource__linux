/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CIX PM fan mode control via direct AP<->PM IPC (ACPI SFMD wire format).
 *
 * Does not use SCMI protocol 0x82 or modify arm_scmi common transport.
 */
#ifndef __CIX_SYSCTL_DRV_H__
#define __CIX_SYSCTL_DRV_H__

enum {
	MUTE_MODE,
	NORMAL_MODE,
	PERF_MODE,
	FAN_MODE_MAX
};

/* CIX PM IPC / ACPI SFMD framing (must stay aligned with PM firmware). */
#define CIX_FAN_MSG_ATTR		0xc7f
#define CIX_FAN_MSG_ID			0x802
#define CIX_FAN_MSG_LEN			8	/* header(4) + mode(4); fan_id at +0x20 */
#define CIX_FAN_ID			0	/* default fan_id */

#define CIX_FAN_CHAN_STAT_FREE		BIT(0)
#define CIX_FAN_CHAN_STAT_ERROR		BIT(1)
#define CIX_FAN_DB_INT_BIT		BIT(0)

#define CIX_FAN_OFF_MSG_INFO		0x00
#define CIX_FAN_OFF_CHAN_STAT		0x04
#define CIX_FAN_OFF_FLAGS		0x10
#define CIX_FAN_OFF_LENGTH		0x14
#define CIX_FAN_OFF_MSG_HDR		0x18
#define CIX_FAN_OFF_PAYLOAD		0x1c	/* mode: 0/1/2 */
#define CIX_FAN_OFF_FAN_ID		0x20	/* fan_id */
#define CIX_FAN_OFF_DOORBELL		0x80

#define CIX_FAN_REG_SIZE		0x84
#define CIX_FAN_TIMEOUT_MS		2000

#endif
