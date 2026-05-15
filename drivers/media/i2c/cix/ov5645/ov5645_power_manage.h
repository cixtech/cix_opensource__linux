/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OV5645 sensor power-management driver interface.
 *
 * The companion platform driver (compatible "ovti,ov5645-power") owns the
 * pwdn / reset / power GPIOs and pinctrl state for the OV5645 module, and
 * implements the board power-on / power-off sequence. The I2C sensor
 * driver references that platform device through the
 * "ov5645,power-mgr" DT phandle and drives power via the helpers below.
 */
#ifndef __OV5645_POWER_MANAGE_H__
#define __OV5645_POWER_MANAGE_H__

#include <linux/device.h>
#include <linux/err.h>

struct ov5645_pm;

/**
 * ov5645_pm_get - resolve the OV5645 power-manager from a consumer device
 * @dev: consumer device (the I2C sensor device); must have an of_node
 *       containing an "ov5645,power-mgr" phandle property.
 *
 * Resolves the phandle, looks up the bound platform device, and returns
 * an opaque handle that can be passed to ov5645_pm_power_{on,off}().
 *
 * Return:
 *   * valid pointer on success.
 *   * ERR_PTR(-ENODEV)        no phandle in DT.
 *   * ERR_PTR(-EPROBE_DEFER)  the platform driver has not probed yet.
 *   * ERR_PTR(-EINVAL)        invalid arguments.
 *
 * The returned reference must be released with ov5645_pm_put().
 */
struct ov5645_pm *ov5645_pm_get(struct device *dev);

/**
 * ov5645_pm_put - release a reference obtained from ov5645_pm_get()
 * @pm: handle previously returned by ov5645_pm_get(); NULL is allowed.
 */
void ov5645_pm_put(struct ov5645_pm *pm);

/**
 * ov5645_pm_power_on - run the OV5645 power-up sequence
 * @pm: handle returned by ov5645_pm_get().
 *
 * Idempotent: returns 0 immediately if the sensor is already on.
 */
int ov5645_pm_power_on(struct ov5645_pm *pm);

/**
 * ov5645_pm_power_off - run the OV5645 power-down sequence
 * @pm: handle returned by ov5645_pm_get().
 *
 * Idempotent: returns 0 immediately if the sensor is already off.
 */
int ov5645_pm_power_off(struct ov5645_pm *pm);

#endif /* __OV5645_POWER_MANAGE_H__ */
