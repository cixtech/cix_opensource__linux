// SPDX-License-Identifier: GPL-2.0

#if ARMCB_CSI_DMA_DUMP_RAW

#include "armcb_csi_dma_dump.h"
#include "system_logger.h"

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/workqueue.h>

#ifdef LOG_MODULE
#undef LOG_MODULE
#endif
#define LOG_MODULE LOG_MODULE_ISP

extern struct device *mem_dev;

static u32 discard_buf_size;
static void *discard_buf_addr;
static dma_addr_t discard_dma_handle;

static struct delayed_work armcb_csi_dma_dump_dwork;
static bool armcb_csi_dma_dump_dwork_inited;

static int armcb_csi_dma_dump_to_file(void *vaddr, dma_addr_t dma_handle,
				      struct device *dma_dev, u32 size)
{
	struct file *filp;
	loff_t pos = 0;
	ssize_t written;
	int ret = 0;

	if (!vaddr || !size) {
		LOG(LOG_ERR, "CSI dump: invalid buffer vaddr=%p size=%u\n", vaddr,
		    size);
		return -EINVAL;
	}

	if (dma_dev && dma_handle)
		dma_sync_single_for_cpu(dma_dev, dma_handle, size,
					DMA_FROM_DEVICE);

	filp = filp_open(ARMCB_CSI_DUMP_FILENAME, O_WRONLY | O_CREAT | O_TRUNC,
			 0644);
	if (IS_ERR(filp)) {
		ret = PTR_ERR(filp);
		LOG(LOG_ERR, "CSI dump: open %s failed (%d)\n",
		    ARMCB_CSI_DUMP_FILENAME, ret);
		return ret;
	}

	written = kernel_write(filp, vaddr, size, &pos);
	if (written < 0) {
		ret = (int)written;
		LOG(LOG_ERR, "CSI dump: kernel_write failed (%d)\n", ret);
	} else if ((u32)written != size) {
		ret = -EIO;
		LOG(LOG_ERR, "CSI dump: short write %zd of %u\n", written, size);
	} else {
		LOG(LOG_INFO, "CSI dump: wrote %u bytes to %s\n", size,
		    ARMCB_CSI_DUMP_FILENAME);
	}

	filp_close(filp, NULL);
	return ret;
}

static int armcb_csi_dma_dump_phys_to_file(phys_addr_t phys, u32 size)
{
	void *vaddr;
	unsigned long map_size = PAGE_ALIGN(size);
	int ret;

	if (!size)
		return -EINVAL;

	vaddr = memremap(phys, map_size, MEMREMAP_WB);
	if (!vaddr) {
		LOG(LOG_ERR, "CSI dump: memremap phys=0x%llx size=%lu failed\n",
		    (unsigned long long)phys, map_size);
		return -ENOMEM;
	}

	ret = armcb_csi_dma_dump_to_file(vaddr, 0, NULL, size);
	memunmap(vaddr);
	return ret;
}

static void armcb_csi_dma_dump_dwork_fn(struct work_struct *work)
{
	int dump_ret;

	if (!discard_buf_size)
		return;

	dump_ret = armcb_csi_dma_dump_phys_to_file(
		(phys_addr_t)ARMCB_CSI_DUMP_PHYS_BASE, discard_buf_size);
	if (dump_ret)
		LOG(LOG_WARN, "CSI delayed dump failed (%d)\n", dump_ret);
}

void armcb_csi_dma_dump_cancel_delayed(void)
{
	if (!armcb_csi_dma_dump_dwork_inited)
		return;

	cancel_delayed_work_sync(&armcb_csi_dma_dump_dwork);
}

void armcb_csi_dma_dump_schedule_delayed(void)
{
	if (!discard_buf_size)
		return;

	if (!armcb_csi_dma_dump_dwork_inited) {
		INIT_DELAYED_WORK(&armcb_csi_dma_dump_dwork,
				  armcb_csi_dma_dump_dwork_fn);
		armcb_csi_dma_dump_dwork_inited = true;
	}

	cancel_delayed_work(&armcb_csi_dma_dump_dwork);
	schedule_delayed_work(&armcb_csi_dma_dump_dwork,
			      msecs_to_jiffies(ARMCB_CSI_DUMP_DELAY_MS));
	LOG(LOG_INFO, "CSI dump scheduled in %u ms -> %s\n",
	    ARMCB_CSI_DUMP_DELAY_MS, ARMCB_CSI_DUMP_FILENAME);
}

int armcb_csi_dma_dump_set_buffer_size(u32 size, struct device *dev)
{
	if (!dev) {
		LOG(LOG_ERR, "CSI dump: device not initialized\n");
		return -ENODEV;
	}

	discard_buf_addr = NULL;
	discard_dma_handle = 0;
	discard_buf_size = size;

	LOG(LOG_INFO,
	    "CSIDMA dump: fixed phys=0x%lx size=%u (no kernel CMA alloc)\n",
	    ARMCB_CSI_DUMP_PHYS_BASE, size);
	return 0;
}

int armcb_csi_dma_dump_teardown(u32 reserved_size, struct device *dev)
{
	struct device *dma_dev = mem_dev ? mem_dev : dev;

	armcb_csi_dma_dump_cancel_delayed();

	if (discard_buf_addr && dma_dev) {
		dma_free_coherent(dma_dev, PAGE_ALIGN(reserved_size),
				  discard_buf_addr, discard_dma_handle);
	}

	discard_buf_addr = NULL;
	discard_dma_handle = 0;
	discard_buf_size = 0;
	return 0;
}

#endif /* ARMCB_CSI_DMA_DUMP_RAW */
