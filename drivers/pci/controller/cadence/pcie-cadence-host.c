// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Cadence
// Cadence PCIe host controller driver.
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/list_sort.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>

#include "pcie-cadence.h"

#define LINK_RETRAIN_TIMEOUT HZ

/* LTSSM FIFO registers — chip bug workaround */
#define CDNS_PCIE_LTSSM_FIFO_TRIG  (CDNS_PCIE_IP_REG_BANK_BASE + 0x760)
#define CDNS_PCIE_LTSSM_FIFO_DATA  (CDNS_PCIE_IP_REG_BANK_BASE + 0x764)
#define CDNS_PCIE_LTSSM_FIFO_DEPTH 64

static bool ltssm_fifo_debug;
static int __init ltssm_fifo_debug_setup(char *str)
{
	ltssm_fifo_debug = true;
	return 1;
}
early_param("ltssm_fifo_debug", ltssm_fifo_debug_setup);

static const char * const cdns_ltssm_names[] = {
	[0x00] = "DETECT_QUIET",
	[0x01] = "DETECT_QUIET_ENTRY",
	[0x02] = "DETECT_ACTIVE",
	[0x03] = "DETECT_ACTIVE_1",
	[0x04] = "DETECT_ACTIVE_2",
	[0x05] = "DETECT_ACTIVE_3",
	[0x06] = "RCVR_DETECTED_ST",
	[0x07] = "RCVR_DETECTED_1",
	[0x08] = "POLLING_ACTIVE",
	[0x09] = "POLLING_ACTIVE_1",
	[0x0a] = "POLLING_ACTIVE_2",
	[0x0b] = "POLLING_ACTIVE_3",
	[0x0c] = "POLLING_COMPLIANCE",
	[0x0d] = "POLLING_COMPLIANCE_1",
	[0x0e] = "POLLING_CONFIG",
	[0x0f] = "POLLING_CONFIG_1",
	[0x10] = "POLLING_CONFIG_2",
	[0x11] = "CONFIG_LW_START_RC",
	[0x12] = "CONFIG_LW_START_RC_1",
	[0x13] = "CONFIG_LW_START_RC_2",
	[0x14] = "CONFIG_ACC_RC",
	[0x15] = "CONFIG_LANENUM_WAIT_RC",
	[0x16] = "CONFIG_LANENUM_WAIT_RC_1",
	[0x17] = "CONFIG_LANENUM_ACC_RC",
	[0x18] = "CONFIG_LW_START_EP",
	[0x19] = "CONFIG_LW_START_EP_1",
	[0x1a] = "CONFIG_LW_START_EP_2",
	[0x1b] = "CONFIG_LW_ACC_EP",
	[0x1c] = "CONFIG_LANENUM_WAIT_EP",
	[0x1d] = "CONFIG_LANENUM_WAIT_EP_1",
	[0x1e] = "CONFIG_LANENUM_ACC_EP",
	[0x1f] = "CONFIG_LANENUM_ACC_EP_1",
	[0x20] = "CONFIG_DUMMY_STATE_1",
	[0x21] = "CONFIG_COMPLETE",
	[0x22] = "CONFIG_COMPLETE_1",
	[0x23] = "CONFIG_COMPLETE_2",
	[0x24] = "CONFIG_IDLE",
	[0x25] = "CONFIG_IDLE_1",
	[0x26] = "CONFIG_DUMMY_STATE_2",
	[0x27] = "CONFIG_DUMMY_STATE_3",
	[0x28] = "CONFIG_DUMMY_STATE_4",
	[0x29] = "L0_STATE",
	[0x2a] = "RECOVERY_RCVR_LOCK",
	[0x2b] = "RECOVERY_RCVR_LOCK_1",
	[0x2c] = "RECOVERY_RCVR_CFG",
	[0x2d] = "RECOVERY_RCVR_CFG_1",
	[0x2e] = "RECOVERY_IDLE",
	[0x2f] = "RECOVERY_IDLE_1",
	[0x30] = "DISABLE_LINK",
	[0x31] = "DISABLE_LINK_1",
	[0x32] = "DISABLE_LINK_2",
	[0x33] = "DISABLE_LINK_3",
	[0x34] = "DISABLE_LINK_4",
	[0x35] = "DISABLE_LINK_5",
	[0x36] = "DISABLE_LINK_6",
	[0x37] = "DISABLE_LINK_7",
	[0x38] = "HOT_RESET",
	[0x39] = "HOT_RESET_1",
	[0x3a] = "HOT_RESET_2",
	[0x3b] = "HOT_RESET_3",
	[0x3c] = "L0S_ENTRY",
	[0x3d] = "L0S_1",
	[0x3e] = "L0S_2",
	[0x3f] = "L0S_3",
	[0x40] = "L0S_4",
	[0x41] = "L0S_5",
	[0x42] = "WAIT_FOR_LINK_TX",
	[0x43] = "TX_FTS_ENTRY",
	[0x44] = "TX_FTS_1",
	[0x45] = "TX_FTS_2",
	[0x46] = "TX_ELEC_IDLE_ST",
	[0x47] = "TX_ELEC_IDLE_1",
	[0x48] = "TX_ELEC_IDLE_2",
	[0x49] = "TX_ELEC_IDLE_3",
	[0x4a] = "RECOVERY_SPEED",
	[0x4b] = "RECOVERY_SPEED_1",
	[0x4c] = "RECOVERY_SPEED_2",
	[0x4d] = "RECOVERY_SPEED_3",
	[0x4e] = "POLLING_COMPLIANCE_GEN23",
	[0x4f] = "POLLING_COMPLIANCE_GEN23_1",
	[0x50] = "POLLING_COMPLIANCE_GEN23_2",
	[0x51] = "POLLING_COMPLIANCE_GEN23_3",
	[0x52] = "POLLING_COMPLIANCE_GEN23_4",
	[0x53] = "POLLING_COMPLIANCE_GEN23_5",
	[0x54] = "POLLING_COMPLIANCE_GEN23_6",
	[0x55] = "POLLING_COMPLIANCE_GEN23_7",
	[0x56] = "POLLING_COMPLIANCE_GEN23_8",
	[0x57] = "LOOPBACK_SLAVE_ENTRY",
	[0x58] = "LOOPBACK_SLAVE_FROM_RECOVERY",
	[0x59] = "LOOPBACK_SLAVE_EXIT_1",
	[0x5a] = "LOOPBACK_SLAVE_EXIT",
	[0x5b] = "LOOPBACK_SLAVE_GEN2_1",
	[0x5c] = "LOOPBACK_SLAVE_GEN2_2",
	[0x5d] = "LOOPBACK_SLAVE_GEN2_3",
	[0x5e] = "LOOPBACK_SLAVE_GEN2_4",
	[0x5f] = "LOOPBACK_SLAVE_GEN2_5",
	[0x60] = "LOOPBACK_SLAVE_ACTIVE",
	[0x61] = "L1_ENTRY",
	[0x62] = "L1_1",
	[0x63] = "L1_2",
	[0x64] = "L1_3",
	[0x65] = "L1_4",
	[0x66] = "L1_IDLE",
	[0x67] = "L1_EXIT",
	[0x68] = "L2_ENTRY",
	[0x69] = "L2_1",
	[0x6a] = "L2_2",
	[0x6b] = "L2_3",
	[0x6c] = "L2_4",
	[0x6d] = "L2_5",
	[0x6e] = "L2_IDLE",
	[0x6f] = "LOOPBACK_MASTER_ENTRY",
	[0x70] = "LOOPBACK_MASTER_ENTRY_1",
	[0x71] = "LOOPBACK_MASTER_ENTRY_2",
	[0x72] = "LOOPBACK_MASTER_ENTRY_3",
	[0x73] = "LOOPBACK_MASTER_ENTRY_4",
	[0x74] = "LOOPBACK_MASTER_ENTRY_5",
	[0x75] = "LOOPBACK_MASTER_FROM_RECOVERY",
	[0x76] = "LOOPBACK_MASTER_ACTIVE",
	[0x77] = "LOOPBACK_MASTER_EXIT",
	[0x78] = "LOOPBACK_MASTER_EXIT_1",
	[0x79] = "LOOPBACK_MASTER_EXIT_2",
	[0x7a] = "RECOVERY_EQ_PHASE_0",
	[0x7b] = "RECOVERY_EQ_PHASE_1",
	[0x7c] = "RECOVERY_EQ_PHASE_2_1",
	[0x7d] = "RECOVERY_EQ_PHASE_2_2",
	[0x7e] = "RECOVERY_EQ_PHASE_3_1",
	[0x7f] = "RECOVERY_EQ_PHASE_3_2",
};

static const char *cdns_ltssm_state_name(u8 idx)
{
	if (idx < ARRAY_SIZE(cdns_ltssm_names) && cdns_ltssm_names[idx])
		return cdns_ltssm_names[idx];
	return "UNKNOWN";
}

static u64 bar_max_size[] = {
	[RP_BAR0] = _ULL(128 * SZ_2G),
	[RP_BAR1] = SZ_2G,
	[RP_NO_BAR] = _BITULL(63),
};

static u8 bar_aperture_mask[] = {
	[RP_BAR0] = 0xF,
	[RP_BAR1] = 0xF,
};

void __iomem *cdns_pci_map_bus(struct pci_bus *bus, unsigned int devfn,
			       int where)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(bus);
	struct cdns_pcie_rc *rc = pci_host_bridge_priv(bridge);
	struct cdns_pcie *pcie = &rc->pcie;
	unsigned int busn = bus->number;
	u32 addr0, desc0, desc1, ctrl0;

	if (pci_is_root_bus(bus)) {
		/*
		 * Only the root port (devfn == 0) is connected to this bus.
		 * All other PCI devices are behind some bridge hence on another
		 * bus.
		 */
		if (devfn)
			return NULL;

		return pcie->reg_base + (where & 0xfff);
	}
	/* Check that the link is up */

	if (!(cdns_pcie_readl(pcie, CDNS_PCIE_IP_REG_BANK_BASE) & 0x1))
		return NULL;

	/* Clear AXI link-down status */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_LINKDOWN,
			cdns_pcie_readl(pcie, CDNS_PCIE_AT_LINKDOWN) & 0xe);

	/* Update Output registers for AXI region 0. */
	addr0 = CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_NBITS(12) |
		CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_DEVFN(devfn) |
		CDNS_PCIE_AT_OB_REGION_PCI_ADDR0_BUS(busn);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(0), addr0);

	/* Configuration Type 0 or Type 1 access. */
	desc1 = cdns_pcie_readl(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(0));
	desc1 &= ~CDNS_PCIE_AT_OB_REGION_DESC1_DEVFN_MASK;
	desc1 |= CDNS_PCIE_AT_OB_REGION_DESC1_DEVFN(0);
	ctrl0 = CDNS_PCIE_AT_OB_REGION_CTRL0_SUPPLY_BUS |
		CDNS_PCIE_AT_OB_REGION_CTRL0_SUPPLY_DEV_FN;

	/*
	 * The bus number was already set once for all in desc1 by
	 * cdns_pcie_host_init_address_translation().
	 */
	if (busn == bridge->busnr + 1)
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE0;
	else
		desc0 |= CDNS_PCIE_AT_OB_REGION_DESC0_TYPE_CONF_TYPE1;
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(0), desc0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(0), desc1);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CTRL0(0), ctrl0);

	return rc->cfg_base + (where & 0xfff);
}

static struct pci_ops cdns_pcie_host_ops = {
	.map_bus	= cdns_pci_map_bus,
	.read		= pci_generic_config_read,
	.write		= pci_generic_config_write,
};

static int cdns_pcie_host_training_complete(struct cdns_pcie *pcie)
{
	u32 pcie_cap_off = CDNS_PCIE_RP_CAP_OFFSET;
	unsigned long end_jiffies;
	u16 lnk_stat;

	/* Wait for link training to complete. Exit after timeout. */
	end_jiffies = jiffies + LINK_RETRAIN_TIMEOUT;
	do {
		lnk_stat = cdns_pcie_rp_readw(pcie, pcie_cap_off + PCI_EXP_LNKSTA);
		if (!(lnk_stat & PCI_EXP_LNKSTA_LT))
			break;
		usleep_range(0, 1000);
	} while (time_before(jiffies, end_jiffies));

	if (!(lnk_stat & PCI_EXP_LNKSTA_LT))
		return 0;

	return -ETIMEDOUT;
}

static void cdns_pcie_dump_ltssm_fifo(struct cdns_pcie *pcie)
{
	struct device *dev = pcie->dev;
	int i;
	u32 data;

	dev_info(dev, "--- LTSSM FIFO dump (%d entries) ---\n",
		 CDNS_PCIE_LTSSM_FIFO_DEPTH);

	for (i = 0; i < CDNS_PCIE_LTSSM_FIFO_DEPTH; i++) {
		/*
		 * Chip bug workaround: must read 0x760 (trigger) before
		 * reading 0x764 (FIFO data), otherwise FIFO returns stale data.
		 */
		cdns_pcie_readl(pcie, CDNS_PCIE_LTSSM_FIFO_TRIG);
		data = cdns_pcie_readl(pcie, CDNS_PCIE_LTSSM_FIFO_DATA);

		dev_info(
			dev,
			"#%02d: 0x%08X, cause: 0x%02X, current: 0x%02X (%s), previous: 0x%02X (%s)\n",
			i, data, (data >> 16) & 0xFF, (data >> 8) & 0x7F,
			cdns_ltssm_state_name((data >> 8) & 0x7F), data & 0x7F,
			cdns_ltssm_state_name(data & 0x7F));
	}
}

static int cdns_pcie_host_wait_for_link(struct cdns_pcie *pcie)
{
	struct device *dev = pcie->dev;
	int retries;

	/* Check if the link is up or not */
	for (retries = 0; retries < LINK_WAIT_MAX_RETRIES; retries++) {
		if (cdns_pcie_link_up(pcie)) {
			dev_info(dev, "Link up, retries %d times\n", retries);
			if (ltssm_fifo_debug)
				cdns_pcie_dump_ltssm_fifo(pcie);
			return 0;
		}
		if (pcie->plat_emu)
			udelay(30);
		else
			usleep_range(LINK_WAIT_USLEEP_MIN,
				     LINK_WAIT_USLEEP_MAX);
	}
	dev_info(dev, "Link fail, retries %d times\n", retries);
	if (ltssm_fifo_debug)
		cdns_pcie_dump_ltssm_fifo(pcie);
	return -ETIMEDOUT;
}

static int cdns_pcie_retrain(struct cdns_pcie *pcie)
{
	u32 lnk_cap_sls, pcie_cap_off = CDNS_PCIE_RP_CAP_OFFSET;
	u16 lnk_stat, lnk_ctl;
	int ret = 0;

	/*
	 * Set retrain bit if current speed is 2.5 GB/s,
	 * but the PCIe root port support is > 2.5 GB/s.
	 */

	lnk_cap_sls = cdns_pcie_readl(pcie, (CDNS_PCIE_RP_BASE + pcie_cap_off +
					     PCI_EXP_LNKCAP));
	if ((lnk_cap_sls & PCI_EXP_LNKCAP_SLS) <= PCI_EXP_LNKCAP_SLS_2_5GB)
		return ret;

	lnk_stat = cdns_pcie_rp_readw(pcie, pcie_cap_off + PCI_EXP_LNKSTA);
	if ((lnk_stat & PCI_EXP_LNKSTA_CLS) == PCI_EXP_LNKSTA_CLS_2_5GB) {
		lnk_ctl = cdns_pcie_rp_readw(pcie,
					     pcie_cap_off + PCI_EXP_LNKCTL);
		lnk_ctl |= PCI_EXP_LNKCTL_RL;
		cdns_pcie_rp_writew(pcie, pcie_cap_off + PCI_EXP_LNKCTL,
				    lnk_ctl);

		ret = cdns_pcie_host_training_complete(pcie);
		if (ret)
			return ret;

		ret = cdns_pcie_host_wait_for_link(pcie);
	}
	return ret;
}

static void cdns_pcie_host_enable_ptm_response(struct cdns_pcie *pcie)
{
	u32 val;

	val = cdns_pcie_readl(pcie, CDNS_PCIE_LM_PTM_CTRL);
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_PTM_CTRL, val | CDNS_PCIE_LM_TPM_CTRL_PTMRSEN);
}

static int cdns_pcie_host_start_link(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	int ret;

	ret = cdns_pcie_host_wait_for_link(pcie);
	dev_info(rc->pcie.dev, "ret=%d, rc->quirk_retrain_flag = %d \n", ret, rc->quirk_retrain_flag);
	/*
	 * Retrain link for Gen2 training defect
	 * if quirk flag is set.
	 */
	if (!ret && rc->quirk_retrain_flag)
		ret = cdns_pcie_retrain(pcie);
	dev_info(rc->pcie.dev, "ret=%d, rc->quirk_retrain_flag = %d \n", ret, rc->quirk_retrain_flag);
	return ret;
}

static int cdns_pcie_host_init_root_port(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	u32 value, ctrl;
	u32 id;

	/*
	 * Set the root complex BAR configuration register:
	 * - disable both BAR0 and BAR1.
	 * - enable Prefetchable Memory Base and Limit registers in type 1
	 *   config space (64 bits).
	 * - enable IO Base and Limit registers in type 1 config
	 *   space (32 bits).
	 */
	ctrl = CDNS_PCIE_LM_BAR_CFG_CTRL_DISABLED;
	value = CDNS_PCIE_LM_RC_BAR_CFG_BAR0_CTRL(ctrl) |
		CDNS_PCIE_LM_RC_BAR_CFG_BAR1_CTRL(ctrl) |
		CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_ENABLE |
		CDNS_PCIE_LM_RC_BAR_CFG_PREFETCH_MEM_64BITS |
		CDNS_PCIE_LM_RC_BAR_CFG_IO_ENABLE |
		CDNS_PCIE_LM_RC_BAR_CFG_IO_32BITS;
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_RC_BAR_CFG, value);

	/* Set root port configuration space */
	if (rc->vendor_id != 0xffff) {
		id = CDNS_PCIE_LM_ID_VENDOR(rc->vendor_id) |
			CDNS_PCIE_LM_ID_SUBSYS(rc->vendor_id);
		cdns_pcie_writel(pcie, CDNS_PCIE_LM_ID, id);
	}

	if (rc->vendor_id != 0xffff)
		cdns_pcie_rp_writew(pcie, PCI_VENDOR_ID, rc->vendor_id);

	if (rc->device_id != 0xffff)
		cdns_pcie_rp_writew(pcie, PCI_DEVICE_ID, rc->device_id);

	cdns_pcie_rp_writeb(pcie, PCI_CLASS_REVISION, 0);
	cdns_pcie_rp_writeb(pcie, PCI_CLASS_PROG, 0);
	cdns_pcie_rp_writew(pcie, PCI_CLASS_DEVICE, PCI_CLASS_BRIDGE_PCI);

	return 0;
}

static int cdns_pcie_host_bar_ib_config(struct cdns_pcie_rc *rc,
					enum cdns_pcie_rp_bar bar,
					u64 cpu_addr, u64 size,
					unsigned long flags)
{
	struct cdns_pcie *pcie = &rc->pcie;
	u32 addr0, addr1, aperture, value;

	if (!rc->avail_ib_bar[bar])
		return -EBUSY;

	rc->avail_ib_bar[bar] = false;

	aperture = ilog2(size);
	addr0 = CDNS_PCIE_AT_IB_RP_BAR_ADDR0_NBITS(aperture) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_RP_BAR_ADDR0(bar), addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_IB_RP_BAR_ADDR1(bar), addr1);

	if (bar == RP_NO_BAR)
		return 0;

	value = cdns_pcie_readl(pcie, CDNS_PCIE_LM_RC_BAR_CFG);
	value &= ~(LM_RC_BAR_CFG_CTRL_MEM_64BITS(bar) |
		   LM_RC_BAR_CFG_CTRL_PREF_MEM_64BITS(bar) |
		   LM_RC_BAR_CFG_CTRL_MEM_32BITS(bar) |
		   LM_RC_BAR_CFG_CTRL_PREF_MEM_32BITS(bar) |
		   LM_RC_BAR_CFG_APERTURE(bar, bar_aperture_mask[bar] + 7));
	if (size + cpu_addr >= SZ_4G) {
		if (!(flags & IORESOURCE_PREFETCH))
			value |= LM_RC_BAR_CFG_CTRL_MEM_64BITS(bar);
		value |= LM_RC_BAR_CFG_CTRL_PREF_MEM_64BITS(bar);
	} else {
		if (!(flags & IORESOURCE_PREFETCH))
			value |= LM_RC_BAR_CFG_CTRL_MEM_32BITS(bar);
		value |= LM_RC_BAR_CFG_CTRL_PREF_MEM_32BITS(bar);
	}

	value |= LM_RC_BAR_CFG_APERTURE(bar, aperture);
	cdns_pcie_writel(pcie, CDNS_PCIE_LM_RC_BAR_CFG, value);

	return 0;
}

static enum cdns_pcie_rp_bar
cdns_pcie_host_find_min_bar(struct cdns_pcie_rc *rc, u64 size)
{
	enum cdns_pcie_rp_bar bar, sel_bar;

	sel_bar = RP_BAR_UNDEFINED;
	for (bar = RP_BAR0; bar <= RP_NO_BAR; bar++) {
		if (!rc->avail_ib_bar[bar])
			continue;

		if (size <= bar_max_size[bar]) {
			if (sel_bar == RP_BAR_UNDEFINED) {
				sel_bar = bar;
				continue;
			}

			if (bar_max_size[bar] < bar_max_size[sel_bar])
				sel_bar = bar;
		}
	}

	return sel_bar;
}

static enum cdns_pcie_rp_bar
cdns_pcie_host_find_max_bar(struct cdns_pcie_rc *rc, u64 size)
{
	enum cdns_pcie_rp_bar bar, sel_bar;

	sel_bar = RP_BAR_UNDEFINED;
	for (bar = RP_BAR0; bar <= RP_NO_BAR; bar++) {
		if (!rc->avail_ib_bar[bar])
			continue;

		if (size >= bar_max_size[bar]) {
			if (sel_bar == RP_BAR_UNDEFINED) {
				sel_bar = bar;
				continue;
			}

			if (bar_max_size[bar] > bar_max_size[sel_bar])
				sel_bar = bar;
		}
	}

	return sel_bar;
}

static int cdns_pcie_host_bar_config(struct cdns_pcie_rc *rc,
				     struct resource_entry *entry)
{
	u64 cpu_addr, pci_addr, size, winsize;
	struct cdns_pcie *pcie = &rc->pcie;
	struct device *dev = pcie->dev;
	enum cdns_pcie_rp_bar bar;
	unsigned long flags;
	int ret;

	cpu_addr = entry->res->start;
	pci_addr = entry->res->start - entry->offset;
	flags = entry->res->flags;
	size = resource_size(entry->res);

	if (entry->offset) {
		dev_err(dev, "PCI addr: %llx must be equal to CPU addr: %llx\n",
			pci_addr, cpu_addr);
		return -EINVAL;
	}

	while (size > 0) {
		/*
		 * Try to find a minimum BAR whose size is greater than
		 * or equal to the remaining resource_entry size. This will
		 * fail if the size of each of the available BARs is less than
		 * the remaining resource_entry size.
		 * If a minimum BAR is found, IB ATU will be configured and
		 * exited.
		 */
		bar = cdns_pcie_host_find_min_bar(rc, size);
		if (bar != RP_BAR_UNDEFINED) {
			ret = cdns_pcie_host_bar_ib_config(rc, bar, cpu_addr,
							   size, flags);
			if (ret)
				dev_err(dev, "IB BAR: %d config failed\n", bar);
			return ret;
		}

		/*
		 * If the control reaches here, it would mean the remaining
		 * resource_entry size cannot be fitted in a single BAR. So we
		 * find a maximum BAR whose size is less than or equal to the
		 * remaining resource_entry size and split the resource entry
		 * so that part of resource entry is fitted inside the maximum
		 * BAR. The remaining size would be fitted during the next
		 * iteration of the loop.
		 * If a maximum BAR is not found, there is no way we can fit
		 * this resource_entry, so we error out.
		 */
		bar = cdns_pcie_host_find_max_bar(rc, size);
		if (bar == RP_BAR_UNDEFINED) {
			dev_err(dev, "No free BAR to map cpu_addr %llx\n",
				cpu_addr);
			return -EINVAL;
		}

		winsize = bar_max_size[bar];
		ret = cdns_pcie_host_bar_ib_config(rc, bar, cpu_addr, winsize,
						   flags);
		if (ret) {
			dev_err(dev, "IB BAR: %d config failed\n", bar);
			return ret;
		}

		size -= winsize;
		cpu_addr += winsize;
	}

	return 0;
}

static int cdns_pcie_host_dma_ranges_cmp(void *priv, const struct list_head *a,
					 const struct list_head *b)
{
	struct resource_entry *entry1, *entry2;

	entry1 = container_of(a, struct resource_entry, node);
	entry2 = container_of(b, struct resource_entry, node);

	return resource_size(entry2->res) - resource_size(entry1->res);
}

static int cdns_pcie_host_map_dma_ranges(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	struct device *dev = pcie->dev;
	struct pci_host_bridge *bridge;
	struct resource_entry *entry;
	u32 no_bar_nbits = 32;
	int err;

	bridge = pci_host_bridge_from_priv(rc);
	if (!bridge)
		return -ENOMEM;

	if (list_empty(&bridge->dma_ranges)) {
		device_property_read_u32(dev, "cdns,no-bar-match-nbits",
				     &no_bar_nbits);
		err = cdns_pcie_host_bar_ib_config(rc, RP_NO_BAR, 0x0,
						   (u64)1 << no_bar_nbits, 0);
		if (err)
			dev_err(dev, "IB BAR: %d config failed\n", RP_NO_BAR);
		return err;
	}
	list_sort(NULL, &bridge->dma_ranges, cdns_pcie_host_dma_ranges_cmp);

	resource_list_for_each_entry(entry, &bridge->dma_ranges) {
		err = cdns_pcie_host_bar_config(rc, entry);
		if (err) {
			dev_err(dev, "Fail to configure IB using dma-ranges\n");
			return err;
		}
	}

	return 0;
}

static int cdns_pcie_create_region_for_ecam(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rc);
	struct resource *cfg_res = rc->cfg_res;
	struct resource_entry *entry;
	resource_size_t size;
	u64 sz;
	int nbits;
	int busnr = 0, secbus = 0, subbus = 0;
	u32 value, root_port_req_id_reg, pcie_bus_number_reg;
	u32 ecam_addr_0, region_size_0, request_id_0;
	u32 axi_address_low;

	entry = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	if (entry) {
		busnr = entry->res->start;
		secbus = (busnr < 0xff) ? (busnr + 1) : 0xff;
		subbus = entry->res->end;
	}
	size = resource_size(cfg_res);
	sz = 1ULL << fls64(size - 1);
	nbits = ilog2(sz);
	if (nbits < 8)
		nbits = 8;

	root_port_req_id_reg = ((busnr & 0xff) << 8);
	pcie_bus_number_reg = ((subbus & 0xff) << 16) | ((secbus & 0xff) << 8) | (busnr & 0xff);
	ecam_addr_0 = cfg_res->start;
	region_size_0 = nbits - 1;
	request_id_0 = ((busnr & 0xff) << 8);

#define CDNS_PCIE_TAG_MANAGEMENT (CDNS_PCIE_AXI_SLAVE_OFFSET + 0x0)
	cdns_pcie_writel(pcie, CDNS_PCIE_TAG_MANAGEMENT, 0x200000);

	/* Taking slave err as OKAY */
#define CDNS_PCIE_SLAVE_RESP (CDNS_PCIE_AXI_SLAVE_OFFSET + 0x100)
	cdns_pcie_writel(pcie, CDNS_PCIE_SLAVE_RESP, 0x0);
	cdns_pcie_writel(pcie, CDNS_PCIE_SLAVE_RESP + 0x4, 0x0);

	/* Program the register “i_root_port_req_id_reg” with RP’s BDF */
#define I_ROOT_PORT_REQ_ID_REG (CDNS_PCIE_IP_REG_BANK_BASE + 0x141c)
	cdns_pcie_writel(pcie, I_ROOT_PORT_REQ_ID_REG, root_port_req_id_reg);

	/* Program the register “i_pcie_bus_numbers” with Primary(RP’s bus number), secondary and subordinate bus numbers */
#define I_PCIE_BUS_NUMBERS (CDNS_PCIE_RP_BASE + 0x18)
	cdns_pcie_writel(pcie, I_PCIE_BUS_NUMBERS, pcie_bus_number_reg);

	/* Program the register “lm_hal_sbsa_ctrl[0]” to enable the sbsa */
#define LM_HAL_SBSA_CTRL (CDNS_PCIE_IP_REG_BANK_BASE + 0x1170)
	value = cdns_pcie_readl(pcie, LM_HAL_SBSA_CTRL);
	value |= BIT(0);
	cdns_pcie_writel(pcie, LM_HAL_SBSA_CTRL, value);

	/* Program region[0] for ECAM */
	axi_address_low = (ecam_addr_0 & 0xfff00000) | region_size_0;
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(0), axi_address_low);

	/* rc0-high-axi-address */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(0), 0x0);
	/* Type-1 CFG */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC0(0), 0x05000000);

	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(0), (request_id_0 << 16));

	/* All AXI bits pass through PCIe */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR0(0), 0x1b);
	/* PCIe address-high */
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(0), 0);

	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CTRL0(0), 0x06000000);

	return 0;
}

static int cdns_pcie_create_region_for_cfg(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rc);
	struct resource *cfg_res = rc->cfg_res;
	struct resource_entry *entry;
	u64 cpu_addr = cfg_res->start;
	u32 addr0, addr1, desc1;
	int busnr = 0;

	entry = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	if (entry)
		busnr = entry->res->start;

	/*
	 * Reserve region 0 for PCI configure space accesses:
	 * OB_REGION_PCI_ADDR0 and OB_REGION_DESC0 are updated dynamically by
	 * cdns_pci_map_bus(), other region registers are set here once for all.
	 */
	addr1 = 0; /* Should be programmed to zero. */
	desc1 = CDNS_PCIE_AT_OB_REGION_DESC1_BUS(busnr);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_PCI_ADDR1(0), addr1);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_DESC1(0), desc1);

	if (pcie->ops->cpu_addr_fixup)
		cpu_addr = pcie->ops->cpu_addr_fixup(pcie, cpu_addr);

	addr0 = CDNS_PCIE_AT_OB_REGION_CPU_ADDR0_NBITS(12) |
		(lower_32_bits(cpu_addr) & GENMASK(31, 8));
	addr1 = upper_32_bits(cpu_addr);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR0(0), addr0);
	cdns_pcie_writel(pcie, CDNS_PCIE_AT_OB_REGION_CPU_ADDR1(0), addr1);

	return 0;
}

static int cdns_pcie_host_init_address_translation(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	struct device *dev = pcie->dev;
	struct pci_host_bridge *bridge = pci_host_bridge_from_priv(rc);
	struct resource_entry *entry;
	int r, busnr = 0;

	if (rc->ecam_support_flag)
		cdns_pcie_create_region_for_ecam(rc);
	else
		cdns_pcie_create_region_for_cfg(rc);

	entry = resource_list_first_type(&bridge->windows, IORESOURCE_BUS);
	if (entry)
		busnr = entry->res->start;

	r = 1;
	cdns_pcie_set_outbound_region_for_normal_msg(pcie, busnr, 0, r, pcie->msg_res->start);

	r = 2;
	resource_list_for_each_entry(entry, &bridge->windows) {
		struct resource *res = entry->res;
		u64 pci_addr = res->start - entry->offset;

		if (resource_type(res) == IORESOURCE_IO)
			cdns_pcie_set_outbound_region(pcie, busnr, 0, r,
						      true,
						      pci_pio_to_address(res->start),
						      pci_addr,
						      resource_size(res));
		else if (resource_type(res) == IORESOURCE_MEM)
			cdns_pcie_set_outbound_region(pcie, busnr, 0, r,
						      false,
						      res->start,
						      pci_addr,
						      resource_size(res));

		r++;
	}

	if (device_property_read_bool(dev, "cdns,no-inbound-bar"))
		return 0;
	else
		return cdns_pcie_host_map_dma_ranges(rc);
}

int cdns_pcie_host_init(struct cdns_pcie_rc *rc)
{
	int err;

	err = cdns_pcie_host_init_root_port(rc);
	if (err)
		return err;

	return cdns_pcie_host_init_address_translation(rc);
}

int cdns_pcie_host_link_setup(struct cdns_pcie_rc *rc)
{
	struct cdns_pcie *pcie = &rc->pcie;
	struct device *dev = rc->pcie.dev;
	int ret;

	if (rc->quirk_detect_quiet_flag)
		cdns_pcie_detect_quiet_min_delay_set(&rc->pcie);

	cdns_pcie_host_enable_ptm_response(pcie);

	ret = cdns_pcie_start_link(pcie);
	if (ret) {
		dev_err(dev, "Failed to start link\n");
		return ret;
	}

	ret = cdns_pcie_host_start_link(rc);
	if (ret)
		dev_dbg(dev, "PCIe link never came up\n");

	return ret;
}

int cdns_pcie_host_setup(struct cdns_pcie_rc *rc)
{
	struct device *dev = rc->pcie.dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct pci_host_bridge *bridge;
	enum cdns_pcie_rp_bar bar;
	struct cdns_pcie *pcie;
	struct resource *res;
	int ret;

	bridge = pci_host_bridge_from_priv(rc);
	if (!bridge)
		return -ENOMEM;

	pcie = &rc->pcie;
	pcie->is_rc = true;

	if ((rc->vendor_id==0)||(rc->vendor_id==0xffff)) {
		device_property_read_u32(dev, "vendor-id", &rc->vendor_id);
	}

	if ((rc->device_id==0)||(rc->device_id==0xffff)) {
		device_property_read_u32(dev, "device-id", &rc->device_id);
	}

	if (!pcie->reg_base) {
		pcie->reg_base = devm_platform_ioremap_resource_byname(pdev, "reg");
		if (IS_ERR(pcie->reg_base)) {
			dev_err(dev, "missing \"reg\"\n");
			return PTR_ERR(pcie->reg_base);
		}
	}

	if (!rc->cfg_base) { /* ECAM config space is remapped at glue layer */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cfg");
		rc->cfg_base = devm_pci_remap_cfg_resource(dev, res);
		if (IS_ERR(rc->cfg_base))
			return PTR_ERR(rc->cfg_base);
		rc->cfg_res = res;
	}

	ret = cdns_pcie_host_link_setup(rc);
	if (ret)
		return ret;

	for (bar = RP_BAR0; bar <= RP_NO_BAR; bar++)
		rc->avail_ib_bar[bar] = true;

	ret = cdns_pcie_host_init(rc);
	if (ret)
		return ret;

	if (!bridge->ops)
		bridge->ops = &cdns_pcie_host_ops;
	ret = pci_host_probe(bridge);
	if (ret < 0) {
		dev_err(dev, "pci_host_probe error\n");
		goto err_link;
	}

	return 0;
err_link:
	dev_err(dev, "PCIe start link failed\n");

	return ret;
}

int cdns_pcie_host_restore(struct cdns_pcie_rc *rc)
{
	struct device *dev = rc->pcie.dev;
	struct pci_host_bridge *bridge;
	struct cdns_pcie *pcie;
	int ret = 0;

	bridge = pci_host_bridge_from_priv(rc);
	pcie = &rc->pcie;
	if (rc->quirk_detect_quiet_flag)
		cdns_pcie_detect_quiet_min_delay_set(&rc->pcie);

	cdns_pcie_host_enable_ptm_response(pcie);

	ret = cdns_pcie_start_link(pcie);
	if (ret) {
		dev_err(dev, "Failed to start link\n");
		return ret;
	}

	ret = cdns_pcie_host_start_link(rc);
	if (ret)
		dev_dbg(dev, "PCIe link never came up\n");

	ret = cdns_pcie_host_init(rc);
	if (ret)
		return ret;

	dev_info(dev, "%s end\n", __func__);

	return ret;
}
