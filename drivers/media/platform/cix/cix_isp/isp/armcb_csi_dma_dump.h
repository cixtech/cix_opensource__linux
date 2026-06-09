/* SPDX-License-Identifier: GPL-2.0 */
/*
 * CSIDMA discard buffer: delayed dump to file (UMD routes CSIDMA to fixed DDR).
 *
 * Build with ARMCB_CSI_DMA_DUMP_RAW=1 (see drivers/media/platform/cix/cix_isp/Makefile).
 */
#ifndef ARMCB_CSI_DMA_DUMP_H
#define ARMCB_CSI_DMA_DUMP_H

#include <linux/device.h>
#include <linux/types.h>

#ifndef ARMCB_CSI_DMA_DUMP_RAW
#define ARMCB_CSI_DMA_DUMP_RAW 0
#endif

#if ARMCB_CSI_DMA_DUMP_RAW

/* Fixed AXI destination (matches UMD CSIDMA AXI_ADDR / 0x118). */
#define ARMCB_CSI_DUMP_PHYS_BASE	0x90000000UL
#define ARMCB_CSI_DUMP_FILENAME		"/mnt/csi_dump.raw"
/* Allow frames to land before memremap + kernel_write. */
#define ARMCB_CSI_DUMP_DELAY_MS		20000

int armcb_csi_dma_dump_set_buffer_size(u32 size, struct device *dev);
int armcb_csi_dma_dump_teardown(u32 reserved_size, struct device *dev);

void armcb_csi_dma_dump_schedule_delayed(void);
void armcb_csi_dma_dump_cancel_delayed(void);

#endif /* ARMCB_CSI_DMA_DUMP_RAW */

#endif /* ARMCB_CSI_DMA_DUMP_H */
