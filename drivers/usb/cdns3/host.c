// SPDX-License-Identifier: GPL-2.0
/*
 * Cadence USBSS and USBSSP DRD Driver - host side
 *
 * Copyright (C) 2018-2019 Cadence Design Systems.
 * Copyright (C) 2017-2018 NXP
 *
 * Authors: Peter Chen <peter.chen@nxp.com>
 *          Pawel Laszczak <pawell@cadence.com>
 */

#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include "core.h"
#include "drd.h"
#include "host-export.h"
#include <linux/usb/hcd.h>
#include "../host/xhci.h"
#include "../host/xhci-plat.h"

/*
 * The XECP_PORT_CAP_REG and XECP_AUX_CTRL_REG1 exist only
 * in Cadence USB3 dual-role controller, so it can't be used
 * with Cadence CDNSP dual-role controller.
 */
#define XECP_PORT_CAP_REG	0x8000
#define XECP_AUX_CTRL_REG1	0x8120

#define CFG_RXDET_P3_EN		BIT(15)
#define LPM_2_STB_SWITCH_EN	BIT(25)

static void xhci_cdns3_plat_start(struct usb_hcd *hcd)
{
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	u32 value;

	/* set usbcmd.EU3S */
	value = readl(&xhci->op_regs->command);
	value |= CMD_PM_INDEX;
	writel(value, &xhci->op_regs->command);

	if (hcd->regs) {
		value = readl(hcd->regs + XECP_AUX_CTRL_REG1);
		value |= CFG_RXDET_P3_EN;
		writel(value, hcd->regs + XECP_AUX_CTRL_REG1);

		value = readl(hcd->regs + XECP_PORT_CAP_REG);
		value |= LPM_2_STB_SWITCH_EN;
		writel(value, hcd->regs + XECP_PORT_CAP_REG);
	}
}

static int xhci_cdns3_resume_quirk(struct usb_hcd *hcd)
{
	xhci_cdns3_plat_start(hcd);
	return 0;
}

static const struct xhci_plat_priv xhci_plat_cdns3_xhci = {
	.quirks = XHCI_SKIP_PHY_INIT | XHCI_AVOID_BEI,
	.plat_start = xhci_cdns3_plat_start,
	.resume_quirk = xhci_cdns3_resume_quirk,
};

static const struct xhci_plat_priv xhci_plat_cdnsp_xhci = {
	.quirks = XHCI_CDNS_SCTX_QUIRK | XHCI_SKIP_PHY_INIT | XHCI_HW_LPM_DISABLE,
};

/*
 * On a warm reboot the board keeps VBUS asserted in HW, so a USB3 device that
 * was already attached before the reboot never power-cycles: its SuperSpeed
 * link gets stuck in Polling and never reaches U0, so no connect event is
 * raised and the device is never enumerated. The hub thread only issues a warm
 * reset for links in SS.Inactive/Compliance (not Polling), so by itself it just
 * waits forever. By the time this runs the controller is up and the root hubs
 * are polling, so force a warm reset on any USB3 port stuck in
 * Polling/Compliance/Inactive to make the far-end LTSSM retrain from scratch -
 * which is what a manual unbind/bind of the xHCI ends up doing. PORTSC is
 * passed through xhci_port_state_to_neutral() so the RW1C change bits are
 * preserved, and writes are serialized against the xHCI irq/hub paths.
 *
 * USB2-only controllers do not need this: a USB2 device is detected via its
 * static D+/D- pull-up, which the initial root-hub scan already sees.
 */
static void cdns_host_reset_stuck_ss_ports(struct usb_hcd *hcd)
{
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	struct device *xdev = hcd->self.controller;
	unsigned long flags;
	u32 kicked_mask = 0;
	bool kicked = false;
	u32 portsc, pls;
	int wait_ms;
	int i;

	if (!xhci->usb3_rhub.num_ports)
		return;

	/*
	 * This controller autosuspends aggressively (short delay). Hold a PM
	 * reference across the whole sequence: if it dropped to D3 the clocks
	 * would be gated and the port status change would be lost.
	 */
	pm_runtime_get_sync(xdev);

	/* Nothing to do (and unsafe to touch PORTSC) if the HC is not running. */
	if (xhci->xhc_state & (XHCI_STATE_DYING | XHCI_STATE_HALTED))
		goto out;

	/* Let the ports settle into RxDetect/Polling after xhci_run(). */
	msleep(20);

	spin_lock_irqsave(&xhci->lock, flags);
	for (i = 0; i < xhci->usb3_rhub.num_ports; i++) {
		portsc = readl(xhci->usb3_rhub.ports[i]->addr);
		pls = portsc & PORT_PLS_MASK;

		/* Leave already-enabled ports and idle empty ports alone. */
		if (portsc & PORT_PE)
			continue;
		if (pls != XDEV_POLLING && pls != XDEV_COMP_MODE &&
		    pls != XDEV_INACTIVE)
			continue;

		portsc = xhci_port_state_to_neutral(portsc);
		writel(portsc | PORT_WR, xhci->usb3_rhub.ports[i]->addr);
		readl(xhci->usb3_rhub.ports[i]->addr); /* flush */
		kicked_mask |= BIT(i);
		kicked = true;
	}
	spin_unlock_irqrestore(&xhci->lock, flags);

	/*
	 * Only when we actually kicked a warm reset, wait for the link to finish
	 * training and raise a port status change so the hub thread enumerates
	 * the device (and so autosuspend does not gate clocks meanwhile). Poll so
	 * we return as soon as every kicked port has left the stuck state instead
	 * of always burning a fixed delay; cap the wait as a backstop.
	 */
	for (wait_ms = 0; kicked_mask && wait_ms < 200; wait_ms += 10) {
		msleep(10);
		for (i = 0; i < xhci->usb3_rhub.num_ports; i++) {
			if (!(kicked_mask & BIT(i)))
				continue;
			pls = readl(xhci->usb3_rhub.ports[i]->addr) & PORT_PLS_MASK;
			if (pls != XDEV_POLLING && pls != XDEV_COMP_MODE &&
			    pls != XDEV_INACTIVE)
				kicked_mask &= ~BIT(i);
		}
	}

	if (kicked)
		for (i = 0; i < xhci->usb3_rhub.num_ports; i++)
			dev_dbg(xdev, "ss port%d portsc=0x%08x\n", i,
				readl(xhci->usb3_rhub.ports[i]->addr));

out:
	pm_runtime_mark_last_busy(xdev);
	pm_runtime_put_autosuspend(xdev);
}

static int cdns_host_restore(struct cdns *cdns)
{
	u32 ready_bit, value;

	if (cdns->version == CDNSP_CONTROLLER_V2)
		ready_bit = OTGSTS_CDNSP_XHCI_READY;
	else
		ready_bit = OTGSTS_CDNS3_XHCI_READY;

	value = readl(&cdns->otg_regs->sts);
	if (value & ready_bit) {
		dev_dbg(cdns->dev, "already at host mode, quit\n");
		return 0;
	}

	if (cdns->xhci_device) {
		struct usb_hcd *hcd = platform_get_drvdata(cdns->xhci_device);
		if (hcd) {
			struct xhci_hcd *xhci = hcd_to_xhci(hcd);

			/* Mark hcd state is halt and not accessible */
			hcd->state = 0;
			xhci = hcd_to_xhci(hcd);
			clear_bit(HCD_FLAG_HW_ACCESSIBLE, &hcd->flags);
			if (xhci->shared_hcd)
				clear_bit(HCD_FLAG_HW_ACCESSIBLE, &xhci->shared_hcd->flags);
		}
	}

	return 0;
}
static int __cdns_host_init(struct cdns *cdns)
{
	struct platform_device *xhci;
	int ret;
	struct usb_hcd *hcd;

	ret = cdns_drd_host_on(cdns);
	if (ret) {
		dev_err(cdns->dev, "failed to enable host on\n");
		return ret;
	}

	xhci = platform_device_alloc("xhci-hcd", PLATFORM_DEVID_AUTO);
	if (!xhci) {
		dev_err(cdns->dev, "couldn't allocate xHCI device\n");
		return -ENOMEM;
	}

	xhci->dev.parent = cdns->dev;
	cdns->host_dev = xhci;

	ret = platform_device_add_resources(xhci, cdns->xhci_res,
					    CDNS_XHCI_RESOURCES_NUM);
	if (ret) {
		dev_err(cdns->dev, "couldn't add resources to xHCI device\n");
		goto err1;
	}

	if (cdns->version < CDNSP_CONTROLLER_V2)
		cdns->xhci_plat_data = kmemdup(&xhci_plat_cdns3_xhci,
				sizeof(struct xhci_plat_priv), GFP_KERNEL);
	else
		cdns->xhci_plat_data = kmemdup(&xhci_plat_cdnsp_xhci,
				sizeof(struct xhci_plat_priv), GFP_KERNEL);

	if (!cdns->xhci_plat_data) {
		ret = -ENOMEM;
		goto err1;
	}

	if (cdns->pdata && (cdns->pdata->quirks & CDNS3_DEFAULT_PM_RUNTIME_ALLOW))
		cdns->xhci_plat_data->quirks |= XHCI_DEFAULT_PM_RUNTIME_ALLOW;

	ret = platform_device_add_data(xhci, cdns->xhci_plat_data,
			sizeof(struct xhci_plat_priv));
	if (ret)
		goto free_memory;

	ret = platform_device_add(xhci);
	if (ret) {
		dev_err(cdns->dev, "failed to register xHCI device\n");
		goto free_memory;
	}

	cdns->xhci_device = xhci;
	/* Glue needs to access xHCI region register for Power management */
	hcd = platform_get_drvdata(xhci);
	if (hcd)
		cdns->xhci_regs = hcd->regs;

	/*
	 * The CDNSP host keeps VBUS asserted across a warm reboot, so a device
	 * attached before the reboot never generates a connect event for the
	 * re-initialized controller. Warm reset any USB3 port left stuck in
	 * Polling now that the root hubs are up so such devices get enumerated.
	 */
	dev_dbg(cdns->dev, "cdns host init done: version=%d hcd=%p\n",
		cdns->version, hcd);
	if (hcd && cdns->version >= CDNSP_CONTROLLER_V2)
		cdns_host_reset_stuck_ss_ports(hcd);

	return 0;

free_memory:
	kfree(cdns->xhci_plat_data);
err1:
	platform_device_put(xhci);
	return ret;
}

static void cdns_host_exit(struct cdns *cdns)
{
	kfree(cdns->xhci_plat_data);
	platform_device_unregister(cdns->host_dev);
	cdns->host_dev = NULL;
	cdns_drd_host_off(cdns);
}

int cdns_host_init(struct cdns *cdns)
{
	struct cdns_role_driver *rdrv;

	rdrv = devm_kzalloc(cdns->dev, sizeof(*rdrv), GFP_KERNEL);
	if (!rdrv)
		return -ENOMEM;

	rdrv->start	= __cdns_host_init;
	rdrv->stop	= cdns_host_exit;
	rdrv->restore	= cdns_host_restore;
	rdrv->state	= CDNS_ROLE_STATE_INACTIVE;
	rdrv->name	= "host";

	cdns->roles[USB_ROLE_HOST] = rdrv;

	return 0;
}
