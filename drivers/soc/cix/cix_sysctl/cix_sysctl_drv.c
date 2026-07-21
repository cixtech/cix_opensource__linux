// SPDX-License-Identifier: GPL-2.0
/*
 * CIX PM fan mode sysfs driver — direct AP<->PM IPC (no SCMI common hooks).
 *
 * Wire format matches ACPI SFMD / UEFI MTL_POLL / FW poke:
 *   +0x00=0xc7f, +0x04=0(busy), +0x14=8, +0x18=0x802,
 *   +0x1c=mode(0/1/2), +0x20=fan_id(0), +0x80 doorbell.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>

#include "cix_sysctl_drv.h"

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable verbose cix_pm_fan IPC traces");

#define fan_dbg(dev, fmt, ...)						\
	do {								\
		if (debug)						\
			dev_info((dev), fmt, ##__VA_ARGS__);		\
	} while (0)

struct cix_pm_fan {
	struct device *dev;
	void __iomem *base;
	struct mutex lock; /* serializes IPC + mode cache */
	struct kobject *kobj;
	int mode;
};

static struct cix_pm_fan *cix_pm_fan;

static const char * const fan_available_mode[] = {
	[MUTE_MODE] = "mute",
	[NORMAL_MODE] = "normal",
	[PERF_MODE] = "performance",
};

static u32 fan_readl(struct cix_pm_fan *fan, u32 off)
{
	return readl(fan->base + off);
}

static void fan_writel(struct cix_pm_fan *fan, u32 val, u32 off)
{
	writel(val, fan->base + off);
}

static int cix_pm_fan_wait_free(struct cix_pm_fan *fan)
{
	ktime_t deadline = ktime_add_ms(ktime_get(), CIX_FAN_TIMEOUT_MS);
	u32 st;

	fan_dbg(fan->dev, "wait channel free\n");
	do {
		st = fan_readl(fan, CIX_FAN_OFF_CHAN_STAT);
		if (st & CIX_FAN_CHAN_STAT_FREE) {
			fan_dbg(fan->dev, "channel free (stat=0x%x)\n", st);
			return 0;
		}
		usleep_range(100, 200);
	} while (ktime_before(ktime_get(), deadline));

	dev_err(fan->dev, "timeout waiting for free channel (stat=0x%x)\n", st);
	return -ETIMEDOUT;
}

static void cix_pm_fan_dump_regs(struct cix_pm_fan *fan, const char *tag)
{
	if (!debug)
		return;

	dev_info(fan->dev,
		 "%s: info=0x%x stat=0x%x flags=0x%x len=0x%x hdr=0x%x mode=0x%x fan_id=0x%x db=0x%x\n",
		 tag,
		 fan_readl(fan, CIX_FAN_OFF_MSG_INFO),
		 fan_readl(fan, CIX_FAN_OFF_CHAN_STAT),
		 fan_readl(fan, CIX_FAN_OFF_FLAGS),
		 fan_readl(fan, CIX_FAN_OFF_LENGTH),
		 fan_readl(fan, CIX_FAN_OFF_MSG_HDR),
		 fan_readl(fan, CIX_FAN_OFF_PAYLOAD),
		 fan_readl(fan, CIX_FAN_OFF_FAN_ID),
		 fan_readl(fan, CIX_FAN_OFF_DOORBELL));
}

/*
 * Shared with ap_to_pm_scmi: if we cleared FREE then abort without PM
 * completing the transfer, restore FREE so SCMI is not wedged.
 */
static void cix_pm_fan_release_channel(struct cix_pm_fan *fan)
{
	fan_writel(fan, CIX_FAN_CHAN_STAT_FREE, CIX_FAN_OFF_CHAN_STAT);
	fan_dbg(fan->dev, "released channel (stat FREE)\n");
}

/*
 * Send fan mode using the same PM IPC framing as ACPI SFMD.
 * Caller must hold fan->lock.
 */
static int cix_pm_fan_set_mode(struct cix_pm_fan *fan, int mode)
{
	int ret;
	u32 st, hdr, pay, fan_id;

	fan_dbg(fan->dev, "set_mode enter mode=%d (%s)\n",
		mode, fan_available_mode[mode]);

	ret = cix_pm_fan_wait_free(fan);
	if (ret)
		return ret;

	cix_pm_fan_dump_regs(fan, "pre-tx");

	/* Mark busy (clear FREE/ERROR), poll completion (flags=0). */
	fan_writel(fan, 0, CIX_FAN_OFF_CHAN_STAT);
	fan_writel(fan, 0, CIX_FAN_OFF_FLAGS);
	fan_writel(fan, CIX_FAN_MSG_LEN, CIX_FAN_OFF_LENGTH);
	fan_writel(fan, CIX_FAN_MSG_ATTR, CIX_FAN_OFF_MSG_INFO);
	fan_writel(fan, CIX_FAN_MSG_ID, CIX_FAN_OFF_MSG_HDR);
	fan_writel(fan, mode, CIX_FAN_OFF_PAYLOAD);
	fan_writel(fan, CIX_FAN_ID, CIX_FAN_OFF_FAN_ID);

	/*
	 * Read back before doorbell: prove FREE was cleared and the frame is
	 * still ours. If FREE is already set again, or hdr!=0x802, another
	 * client (typically ap_to_pm_scmi) stole the shared channel.
	 */
	st = fan_readl(fan, CIX_FAN_OFF_CHAN_STAT);
	hdr = fan_readl(fan, CIX_FAN_OFF_MSG_HDR);
	pay = fan_readl(fan, CIX_FAN_OFF_PAYLOAD);
	fan_id = fan_readl(fan, CIX_FAN_OFF_FAN_ID);
	fan_dbg(fan->dev,
		"TX written attr=0x%x len=%u hdr=0x%x mode=%u fan_id=%u; readback stat=0x%x hdr=0x%x mode=0x%x fan_id=0x%x\n",
		CIX_FAN_MSG_ATTR, CIX_FAN_MSG_LEN, CIX_FAN_MSG_ID, mode,
		CIX_FAN_ID, st, hdr, pay, fan_id);

	if (st & CIX_FAN_CHAN_STAT_FREE) {
		/* Already free again; do not touch channel ownership. */
		dev_err(fan->dev,
			"channel stolen before doorbell (stat=0x%x hdr=0x%x mode=0x%x)\n",
			st, hdr, pay);
		return -EBUSY;
	}
	if (hdr != CIX_FAN_MSG_ID || pay != mode || fan_id != CIX_FAN_ID) {
		dev_err(fan->dev,
			"frame corrupted before doorbell (hdr=0x%x/%x mode=0x%x/%u fan_id=0x%x/%u)\n",
			hdr, CIX_FAN_MSG_ID, pay, mode, fan_id, CIX_FAN_ID);
		ret = -EIO;
		goto release;
	}

	/* Ensure payload is visible before doorbell. */
	wmb();
	fan_writel(fan, CIX_FAN_DB_INT_BIT, CIX_FAN_OFF_DOORBELL);
	fan_dbg(fan->dev, "doorbell rung\n");

	ret = cix_pm_fan_wait_free(fan);
	if (ret)
		goto release;

	st = fan_readl(fan, CIX_FAN_OFF_CHAN_STAT);
	cix_pm_fan_dump_regs(fan, "post-tx");
	if (st & CIX_FAN_CHAN_STAT_ERROR) {
		dev_err(fan->dev, "PM channel error after set mode=%d stat=0x%x\n",
			mode, st);
		ret = -EIO;
		goto release;
	}

	fan_dbg(fan->dev, "set_mode done mode=%d\n", mode);
	return 0;

release:
	cix_pm_fan_release_channel(fan);
	return ret;
}

/*
 * mode sysfs reports the last successfully written value only. There is no
 * PM get-fan-mode command.
 */
static ssize_t mode_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	struct cix_pm_fan *fan = cix_pm_fan;
	int mode;

	if (!fan)
		return -ENODEV;

	mutex_lock(&fan->lock);
	mode = fan->mode;
	mutex_unlock(&fan->lock);

	if (mode < 0 || mode >= FAN_MODE_MAX)
		return -EINVAL;

	return sysfs_emit(buf, "%s\n", fan_available_mode[mode]);
}

static ssize_t mode_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	struct cix_pm_fan *fan = cix_pm_fan;
	int mode;
	int ret;

	if (!fan)
		return -ENODEV;

	mode = sysfs_match_string(fan_available_mode, buf);
	if (mode < 0)
		return -EINVAL;

	fan_dbg(fan->dev, "sysfs store '%s' -> mode=%d\n",
		fan_available_mode[mode], mode);

	mutex_lock(&fan->lock);
	ret = cix_pm_fan_set_mode(fan, mode);
	if (ret) {
		mutex_unlock(&fan->lock);
		dev_err(fan->dev, "Failed to set fan mode %d: %d\n", mode, ret);
		return ret;
	}
	fan->mode = mode;
	mutex_unlock(&fan->lock);

	return count;
}

static struct kobj_attribute mode_attr = __ATTR_RW(mode);

static ssize_t available_mode_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	ssize_t count = 0;
	int i;

	for (i = 0; i < FAN_MODE_MAX; i++) {
		count += scnprintf(&buf[count], (PAGE_SIZE - count - 2),
				   "%s ", fan_available_mode[i]);
	}
	if (count)
		count--;
	count += scnprintf(&buf[count], PAGE_SIZE - count, "\n");
	return count;
}

static struct kobj_attribute available_mode_attr = __ATTR_RO(available_mode);

static int cix_pm_fan_probe(struct platform_device *pdev)
{
	struct cix_pm_fan *fan;
	struct resource *res;
	int ret;

	fan = devm_kzalloc(&pdev->dev, sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;

	fan->dev = &pdev->dev;
	mutex_init(&fan->lock);
	fan->mode = NORMAL_MODE;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	if (resource_size(res) < CIX_FAN_REG_SIZE) {
		dev_err(&pdev->dev, "reg size %#llx < %#x\n",
			(unsigned long long)resource_size(res), CIX_FAN_REG_SIZE);
		return -EINVAL;
	}

	/*
	 * Alias the AP->PM mailbox/shmem PA (also used by cix-mailbox and
	 * arm,scmi-shmem). Use ioremap, not ioremap_resource, to avoid
	 * exclusive region conflicts with those drivers.
	 */
	fan->base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!fan->base)
		return -ENOMEM;

	fan_dbg(&pdev->dev, "mapped ipc %pR -> %p\n", res, fan->base);

	fan->kobj = kobject_create_and_add("cix_scmi_fan", NULL);
	if (!fan->kobj)
		return -ENOMEM;

	ret = sysfs_create_file(fan->kobj, &mode_attr.attr);
	if (ret)
		goto err_kobj;

	ret = sysfs_create_file(fan->kobj, &available_mode_attr.attr);
	if (ret)
		goto err_mode;

	cix_pm_fan = fan;
	platform_set_drvdata(pdev, fan);
	dev_info(&pdev->dev, "PM fan mode sysfs ready (/sys/cix_scmi_fan)\n");
	return 0;

err_mode:
	sysfs_remove_file(fan->kobj, &mode_attr.attr);
err_kobj:
	kobject_put(fan->kobj);
	return ret;
}

static int cix_pm_fan_remove(struct platform_device *pdev)
{
	struct cix_pm_fan *fan = platform_get_drvdata(pdev);

	if (!fan || !fan->kobj)
		return 0;

	/*
	 * sysfs_remove_file waits for in-flight show/store before returning.
	 */
	sysfs_remove_file(fan->kobj, &mode_attr.attr);
	sysfs_remove_file(fan->kobj, &available_mode_attr.attr);
	kobject_put(fan->kobj);
	fan->kobj = NULL;
	cix_pm_fan = NULL;
	fan_dbg(&pdev->dev, "removed\n");
	return 0;
}

static const struct of_device_id cix_pm_fan_of_match[] = {
	{ .compatible = "cix,pm-fan-mode" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, cix_pm_fan_of_match);

static struct platform_driver cix_pm_fan_driver = {
	.probe = cix_pm_fan_probe,
	.remove = cix_pm_fan_remove,
	.driver = {
		.name = "cix-pm-fan",
		.of_match_table = cix_pm_fan_of_match,
	},
};
module_platform_driver(cix_pm_fan_driver);

MODULE_DESCRIPTION("CIX PM fan mode control (direct IPC)");
MODULE_LICENSE("GPL v2");
