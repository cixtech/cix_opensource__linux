// SPDX-License-Identifier: GPL-2.0
/*
 * OV5645 sensor power-management platform driver.
 *
 * Splits GPIO/pinctrl device-tree parsing and the power-on / power-off
 * sequence out of the I2C sensor driver, so the sequencing logic can be
 * reused (or replaced per board) without touching the sensor driver core.
 *
 * Device tree binding (informative):
 *
 *   ov5645_pwr: ov5645-power {
 *           compatible    = "ovti,ov5645-power";
 *           pwdn-gpios    = <&gpio  ... GPIO_ACTIVE_HIGH>;
 *           reset-gpios   = <&gpio  ... GPIO_ACTIVE_HIGH>;
 *           power-gpios   = <&gpio  ... GPIO_ACTIVE_HIGH>;  // optional
 *           pinctrl-names = "default";
 *           pinctrl-0     = <&pinctrl_ov5645>;
 *   };
 *
 *   &i2c0 {
 *           ov5645@3c {
 *                   compatible        = "ovti,ov5645";
 *                   ov5645,power-mgr  = <&ov5645_pwr>;
 *                   ...
 *           };
 *   };
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "ov5645_power_manage.h"

#define OV5645_PM_DRIVER_NAME		"ov5645-power-manage"
#define OV5645_PM_PHANDLE_PROP		"ov5645,power-mgr"

struct ov5645_pm {
	struct device		*dev;

	struct gpio_desc	*pwdn_gpio;
	struct gpio_desc	*rst_gpio;
	struct gpio_desc	*power_gpio;

	struct pinctrl		*pinctrl;
	struct pinctrl_state	*pins_default;

	/*
	 * Multiple OV5645 sensors can share a single power-management
	 * instance (same rails / pwdn / reset signals). @lock serializes
	 * the @use_count refcount and the GPIO sequence so that:
	 *
	 *   - only the 0 -> 1 transition runs the physical power-on
	 *     sequence; subsequent ov5645_pm_power_on() calls just bump
	 *     the count and return immediately;
	 *   - only the 1 -> 0 transition runs the physical power-off
	 *     sequence; intermediate ov5645_pm_power_off() calls just
	 *     decrement the count and return.
	 */
	struct mutex		lock;
	unsigned int		use_count;
};

/* -------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------
 */

/*
 * Run the physical OV5645 power-up sequence.
 * Caller must hold pm->lock.
 */
static void ov5645_pm_hw_power_on(struct ov5645_pm *pm)
{
	/*
	 * Keep PWDN/RESET asserted, then bring rails on (DOVDD then AVDD;
	 * board may use power-gpios).
	 */
	if (pm->pwdn_gpio)
		gpiod_set_value_cansleep(pm->pwdn_gpio, 0);

	if (pm->rst_gpio)
		gpiod_set_value_cansleep(pm->rst_gpio, 1);

	if (pm->power_gpio)
		gpiod_set_value_cansleep(pm->power_gpio, 1);

	/*
	 * t2 >= 5 ms: after AVDD stable, release PWDN
	 * (polarity from DTS; 0 = run on this board).
	 */
	msleep(20);

	if (pm->pwdn_gpio)
		gpiod_set_value_cansleep(pm->pwdn_gpio, 1);

	/* t3 >= 1 ms: after PWDN released, release RESET. */
	msleep(10);
	if (pm->rst_gpio)
		gpiod_set_value_cansleep(pm->rst_gpio, 0);

	/* t4 >= 20 ms: after RESET released before SCCB (I2C) register access. */
	msleep(50);
}

/*
 * Run the physical OV5645 power-down sequence.
 * Caller must hold pm->lock.
 */
static void ov5645_pm_hw_power_off(struct ov5645_pm *pm)
{
	if (pm->rst_gpio)
		gpiod_set_value_cansleep(pm->rst_gpio, 1);

	if (pm->pwdn_gpio)
		gpiod_set_value_cansleep(pm->pwdn_gpio, 1);

	if (pm->power_gpio)
		gpiod_set_value_cansleep(pm->power_gpio, 0);
}

int ov5645_pm_power_on(struct ov5645_pm *pm)
{
	if (!pm)
		return -EINVAL;

	mutex_lock(&pm->lock);

	/*
	 * Multiple sensors may share this power instance. Only the first
	 * caller (0 -> 1 transition) runs the GPIO sequence; subsequent
	 * callers just bump the refcount and return.
	 */
	if (pm->use_count > 0) {
		pm->use_count++;
		dev_dbg(pm->dev,
			"already powered on, use_count=%u, skip sequence\n",
			pm->use_count);
		mutex_unlock(&pm->lock);
		return 0;
	}

	ov5645_pm_hw_power_on(pm);
	pm->use_count = 1;

	dev_info(pm->dev, "powered on, use_count=%u\n", pm->use_count);
	mutex_unlock(&pm->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(ov5645_pm_power_on);

int ov5645_pm_power_off(struct ov5645_pm *pm)
{
	if (!pm)
		return -EINVAL;

	mutex_lock(&pm->lock);

	if (pm->use_count == 0) {
		dev_warn(pm->dev,
			 "power_off called while already off, ignoring\n");
		mutex_unlock(&pm->lock);
		return 0;
	}

	/*
	 * Only the last consumer (1 -> 0 transition) drops the rails;
	 * earlier consumers just decrement and leave the sensor running
	 * for the remaining ones.
	 */
	if (--pm->use_count > 0) {
		dev_dbg(pm->dev,
			"power_off: %u consumer(s) still active, skip sequence\n",
			pm->use_count);
		mutex_unlock(&pm->lock);
		return 0;
	}

	ov5645_pm_hw_power_off(pm);

	dev_info(pm->dev, "powered off, use_count=%u\n", pm->use_count);
	mutex_unlock(&pm->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(ov5645_pm_power_off);

struct ov5645_pm *ov5645_pm_get(struct device *dev)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct ov5645_pm *pm;

	if (!dev || !dev->of_node)
		return ERR_PTR(-EINVAL);

	np = of_parse_phandle(dev->of_node, OV5645_PM_PHANDLE_PROP, 0);
	if (!np) {
		dev_err(dev, "missing '%s' phandle in DT\n",
			OV5645_PM_PHANDLE_PROP);
		return ERR_PTR(-ENODEV);
	}

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		dev_dbg(dev, "ov5645 power-manage not yet bound, defer\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	pm = platform_get_drvdata(pdev);
	if (!pm) {
		put_device(&pdev->dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	/*
	 * Hold the device reference taken by of_find_device_by_node();
	 * it is released in ov5645_pm_put().
	 */
	return pm;
}
EXPORT_SYMBOL_GPL(ov5645_pm_get);

void ov5645_pm_put(struct ov5645_pm *pm)
{
	if (!pm)
		return;
	put_device(pm->dev);
}
EXPORT_SYMBOL_GPL(ov5645_pm_put);

/* -------------------------------------------------------------------------
 * Platform driver
 * -------------------------------------------------------------------------
 */

static int ov5645_pm_parse_dt(struct ov5645_pm *pm)
{
	struct device *dev = pm->dev;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
	int ret;

	pm->pwdn_gpio = devm_gpiod_get(dev, "pwdn", GPIOD_OUT_HIGH);
	if (IS_ERR(pm->pwdn_gpio)) {
		ret = PTR_ERR(pm->pwdn_gpio);
		dev_err(dev, "cannot get pwdn gpio: %d\n", ret);
		return ret;
	}

	pm->rst_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(pm->rst_gpio)) {
		ret = PTR_ERR(pm->rst_gpio);
		dev_err(dev, "cannot get reset gpio: %d\n", ret);
		return ret;
	}

	pm->power_gpio = devm_gpiod_get_optional(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(pm->power_gpio)) {
		ret = PTR_ERR(pm->power_gpio);
		dev_err(dev, "cannot get power gpio: %d\n", ret);
		return ret;
	}

	pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(pinctrl)) {
		ret = PTR_ERR(pinctrl);
		dev_err(dev, "failed to get pinctrl: %d\n", ret);
		return ret;
	}

	pins_default = pinctrl_lookup_state(pinctrl, PINCTRL_STATE_DEFAULT);
	if (IS_ERR(pins_default)) {
		ret = PTR_ERR(pins_default);
		dev_err(dev, "failed to get default pinctrl state: %d\n", ret);
		return ret;
	}

	ret = pinctrl_select_state(pinctrl, pins_default);
	if (ret) {
		dev_err(dev, "failed to select default pinctrl state: %d\n",
			ret);
		return ret;
	}

	pm->pinctrl = pinctrl;
	pm->pins_default = pins_default;

	return 0;
}

static int ov5645_pm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ov5645_pm *pm;
	int ret;

	pm = devm_kzalloc(dev, sizeof(*pm), GFP_KERNEL);
	if (!pm)
		return -ENOMEM;

	pm->dev = dev;
	mutex_init(&pm->lock);

	ret = ov5645_pm_parse_dt(pm);
	if (ret) {
		mutex_destroy(&pm->lock);
		return ret;
	}

	platform_set_drvdata(pdev, pm);

	dev_info(dev, "ov5645 power-manage probed\n");
	return 0;
}

static int ov5645_pm_remove(struct platform_device *pdev)
{
	struct ov5645_pm *pm = platform_get_drvdata(pdev);

	if (!pm)
		return 0;

	mutex_lock(&pm->lock);
	/*
	 * Consumers should have called ov5645_pm_power_off() in their own
	 * remove(); if any reference is still outstanding, force the rails
	 * down here so we leave the hardware in a known state.
	 */
	if (pm->use_count > 0) {
		dev_warn(pm->dev,
			 "removing with %u outstanding power reference(s); forcing power-off\n",
			 pm->use_count);
		ov5645_pm_hw_power_off(pm);
		pm->use_count = 0;
	}
	mutex_unlock(&pm->lock);

	mutex_destroy(&pm->lock);
	return 0;
}

static const struct of_device_id ov5645_pm_of_match[] = {
	{ .compatible = "ovti,ov5645-power" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov5645_pm_of_match);

static struct platform_driver ov5645_pm_driver = {
	.driver = {
		.name		= OV5645_PM_DRIVER_NAME,
		.of_match_table	= ov5645_pm_of_match,
	},
	.probe	= ov5645_pm_probe,
	.remove	= ov5645_pm_remove,
};
module_platform_driver(ov5645_pm_driver);

MODULE_DESCRIPTION("Omnivision OV5645 power-management driver");
MODULE_AUTHOR("camera team of CIX");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" OV5645_PM_DRIVER_NAME);
