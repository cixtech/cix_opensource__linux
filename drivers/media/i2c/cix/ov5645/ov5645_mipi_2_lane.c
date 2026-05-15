#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-dv-timings.h>
#include <media/v4l2-mediabus.h>
#include <csi_common.h>

#include "ov5645_power_manage.h"

#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/v4l2-dv-timings.h>
#include <linux/version.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <linux/compat.h>

#define OV5645_SYSTEM_CTRL0			0x3008
#define	OV5645_SYSTEM_CTRL0_START	0x02
#define	OV5645_SYSTEM_CTRL0_STOP	0x42
#define OV5645_CHIP_ID_HIGH			0x300a
#define	OV5645_CHIP_ID_HIGH_BYTE	0x56
#define OV5645_CHIP_ID_LOW			0x300b
#define	OV5645_CHIP_ID_LOW_BYTE		0x45
#define OV5645_IO_MIPI_CTRL00		0x300e
#define OV5645_PAD_OUTPUT00			0x3019
#define OV5645_AWB_MANUAL_CONTROL	0x3406
#define	OV5645_AWB_MANUAL_ENABLE	BIT(0)
#define OV5645_AEC_PK_MANUAL		0x3503
#define	OV5645_AEC_MANUAL_ENABLE	BIT(0)
#define	OV5645_AGC_MANUAL_ENABLE	BIT(1)
#define OV5645_TIMING_TC_REG20		0x3820
#define	OV5645_SENSOR_VFLIP			BIT(1)
#define	OV5645_ISP_VFLIP			BIT(2)
#define OV5645_TIMING_TC_REG21		0x3821
#define	OV5645_SENSOR_MIRROR		BIT(1)
#define OV5645_MIPI_CTRL00			0x4800
#define OV5645_PRE_ISP_TEST_SETTING_1	0x503d
#define	OV5645_TEST_PATTERN_MASK	0x3
#define	OV5645_SET_TEST_PATTERN(x)	((x) & OV5645_TEST_PATTERN_MASK)
#define	OV5645_TEST_PATTERN_ENABLE	BIT(7)
#define OV5645_SDE_SAT_U			0x5583
#define OV5645_SDE_SAT_V			0x5584

#define CIX_OV5645_SUBDEV_NAME "ov5645"

/* CSI-2 Virtual Channel identifiers. */
#ifndef V4L2_MBUS_CSI2_CHANNEL_0
#define V4L2_MBUS_CSI2_CHANNEL_0        BIT(4)
#define V4L2_MBUS_CSI2_CHANNEL_1        BIT(5)
#define V4L2_MBUS_CSI2_CHANNEL_2        BIT(6)
#define V4L2_MBUS_CSI2_CHANNEL_3        BIT(7)
#endif

#ifndef V4L2_MBUS_CSI2_CONTINUOUS_CLOCK
#define V4L2_MBUS_CSI2_CONTINUOUS_CLOCK BIT(8)
#endif


struct reg_value {
	u16 reg;
	u8 val;
};

struct ov5645_mode_info {
	u32 width;
	u32 height;
	const struct reg_value *data;
	u32 data_size;
	u32 pixel_clock;
	u32 link_freq;
};

/* LT7911UXC PAD ID */
enum ov5645_pad_id {
	OV5645_SD_PAD_SOURCE_CSI,
	OV5645_SD_PAD_MAX_NUM,
};

struct ov5645 {
	struct i2c_client *i2c_client;
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad[OV5645_SD_PAD_MAX_NUM];
	struct v4l2_fwnode_endpoint ep;
	struct v4l2_mbus_framefmt fmt;
	struct v4l2_rect crop;
	struct clk *xclk;
	const struct ov5645_mode_info *current_mode;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_clock;
	struct v4l2_ctrl *link_freq;

	/* Cached register values */
	u8 aec_pk_manual;
	u8 timing_tc_reg20;
	u8 timing_tc_reg21;

	struct mutex power_lock; /* lock to protect power state */

	/*
	 * GPIO / pinctrl / power-sequencing have moved to the companion
	 * platform driver (ov5645_power_manage.c). This is just an opaque
	 * handle obtained via ov5645_pm_get().
	 */
	struct ov5645_pm *pm;

	u8 platform_id;
};

static inline struct ov5645 *to_ov5645(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ov5645, sd);
}

static const struct reg_value ov5645_global_init_setting[] = {
	{ 0x3103, 0x11 },
	{ 0x3008, 0x82 },
	{ 0x3008, 0x42 },
	{ 0x3103, 0x03 },
	{ 0x3503, 0x07 },
	{ 0x3002, 0x1c },
	{ 0x3006, 0xc3 },
	{ 0x3017, 0x00 },
	{ 0x3018, 0x00 },
	{ 0x302e, 0x0b },
	{ 0x3037, 0x13 },
	{ 0x3108, 0x01 },
	{ 0x3611, 0x06 },
	{ 0x3500, 0x00 },
	{ 0x3501, 0x01 },
	{ 0x3502, 0x00 },
	{ 0x350a, 0x00 },
	{ 0x350b, 0x3f },
	{ 0x3620, 0x33 },
	{ 0x3621, 0xe0 },
	{ 0x3622, 0x01 },
	{ 0x3630, 0x2e },
	{ 0x3631, 0x00 },
	{ 0x3632, 0x32 },
	{ 0x3633, 0x52 },
	{ 0x3634, 0x70 },
	{ 0x3635, 0x13 },
	{ 0x3636, 0x03 },
	{ 0x3703, 0x5a },
	{ 0x3704, 0xa0 },
	{ 0x3705, 0x1a },
	{ 0x3709, 0x12 },
	{ 0x370b, 0x61 },
	{ 0x370f, 0x10 },
	{ 0x3715, 0x78 },
	{ 0x3717, 0x01 },
	{ 0x371b, 0x20 },
	{ 0x3731, 0x12 },
	{ 0x3901, 0x0a },
	{ 0x3905, 0x02 },
	{ 0x3906, 0x10 },
	{ 0x3719, 0x86 },
	{ 0x3810, 0x00 },
	{ 0x3811, 0x10 },
	{ 0x3812, 0x00 },
	{ 0x3821, 0x01 },
	{ 0x3824, 0x01 },
	{ 0x3826, 0x03 },
	{ 0x3828, 0x08 },
	{ 0x3a19, 0xf8 },
	{ 0x3c01, 0x34 },
	{ 0x3c04, 0x28 },
	{ 0x3c05, 0x98 },
	{ 0x3c07, 0x07 },
	{ 0x3c09, 0xc2 },
	{ 0x3c0a, 0x9c },
	{ 0x3c0b, 0x40 },
	{ 0x3c01, 0x34 },
	{ 0x4001, 0x02 },
	{ 0x4514, 0x00 },
	{ 0x4520, 0xb0 },
	{ 0x460b, 0x37 },
	{ 0x460c, 0x20 },
	{ 0x4818, 0x01 },
	{ 0x481d, 0xf0 },
	{ 0x481f, 0x50 },
	{ 0x4823, 0x70 },
	{ 0x4831, 0x14 },
	{ 0x5000, 0xa7 },
	{ 0x5001, 0x83 },
	{ 0x501d, 0x00 },
	{ 0x501f, 0x00 },
	{ 0x503d, 0x00 },
	{ 0x505c, 0x30 },
	{ 0x5181, 0x59 },
	{ 0x5183, 0x00 },
	{ 0x5191, 0xf0 },
	{ 0x5192, 0x03 },
	{ 0x5684, 0x10 },
	{ 0x5685, 0xa0 },
	{ 0x5686, 0x0c },
	{ 0x5687, 0x78 },
	{ 0x5a00, 0x08 },
	{ 0x5a21, 0x00 },
	{ 0x5a24, 0x00 },
	{ 0x3008, 0x02 },
	{ 0x3503, 0x00 },
	{ 0x5180, 0xff },
	{ 0x5181, 0xf2 },
	{ 0x5182, 0x00 },
	{ 0x5183, 0x14 },
	{ 0x5184, 0x25 },
	{ 0x5185, 0x24 },
	{ 0x5186, 0x09 },
	{ 0x5187, 0x09 },
	{ 0x5188, 0x0a },
	{ 0x5189, 0x75 },
	{ 0x518a, 0x52 },
	{ 0x518b, 0xea },
	{ 0x518c, 0xa8 },
	{ 0x518d, 0x42 },
	{ 0x518e, 0x38 },
	{ 0x518f, 0x56 },
	{ 0x5190, 0x42 },
	{ 0x5191, 0xf8 },
	{ 0x5192, 0x04 },
	{ 0x5193, 0x70 },
	{ 0x5194, 0xf0 },
	{ 0x5195, 0xf0 },
	{ 0x5196, 0x03 },
	{ 0x5197, 0x01 },
	{ 0x5198, 0x04 },
	{ 0x5199, 0x12 },
	{ 0x519a, 0x04 },
	{ 0x519b, 0x00 },
	{ 0x519c, 0x06 },
	{ 0x519d, 0x82 },
	{ 0x519e, 0x38 },
	{ 0x5381, 0x1e },
	{ 0x5382, 0x5b },
	{ 0x5383, 0x08 },
	{ 0x5384, 0x0a },
	{ 0x5385, 0x7e },
	{ 0x5386, 0x88 },
	{ 0x5387, 0x7c },
	{ 0x5388, 0x6c },
	{ 0x5389, 0x10 },
	{ 0x538a, 0x01 },
	{ 0x538b, 0x98 },
	{ 0x5300, 0x08 },
	{ 0x5301, 0x30 },
	{ 0x5302, 0x10 },
	{ 0x5303, 0x00 },
	{ 0x5304, 0x08 },
	{ 0x5305, 0x30 },
	{ 0x5306, 0x08 },
	{ 0x5307, 0x16 },
	{ 0x5309, 0x08 },
	{ 0x530a, 0x30 },
	{ 0x530b, 0x04 },
	{ 0x530c, 0x06 },
	{ 0x5480, 0x01 },
	{ 0x5481, 0x08 },
	{ 0x5482, 0x14 },
	{ 0x5483, 0x28 },
	{ 0x5484, 0x51 },
	{ 0x5485, 0x65 },
	{ 0x5486, 0x71 },
	{ 0x5487, 0x7d },
	{ 0x5488, 0x87 },
	{ 0x5489, 0x91 },
	{ 0x548a, 0x9a },
	{ 0x548b, 0xaa },
	{ 0x548c, 0xb8 },
	{ 0x548d, 0xcd },
	{ 0x548e, 0xdd },
	{ 0x548f, 0xea },
	{ 0x5490, 0x1d },
	{ 0x5580, 0x02 },
	{ 0x5583, 0x40 },
	{ 0x5584, 0x10 },
	{ 0x5589, 0x10 },
	{ 0x558a, 0x00 },
	{ 0x558b, 0xf8 },
	{ 0x5800, 0x3f },
	{ 0x5801, 0x16 },
	{ 0x5802, 0x0e },
	{ 0x5803, 0x0d },
	{ 0x5804, 0x17 },
	{ 0x5805, 0x3f },
	{ 0x5806, 0x0b },
	{ 0x5807, 0x06 },
	{ 0x5808, 0x04 },
	{ 0x5809, 0x04 },
	{ 0x580a, 0x06 },
	{ 0x580b, 0x0b },
	{ 0x580c, 0x09 },
	{ 0x580d, 0x03 },
	{ 0x580e, 0x00 },
	{ 0x580f, 0x00 },
	{ 0x5810, 0x03 },
	{ 0x5811, 0x08 },
	{ 0x5812, 0x0a },
	{ 0x5813, 0x03 },
	{ 0x5814, 0x00 },
	{ 0x5815, 0x00 },
	{ 0x5816, 0x04 },
	{ 0x5817, 0x09 },
	{ 0x5818, 0x0f },
	{ 0x5819, 0x08 },
	{ 0x581a, 0x06 },
	{ 0x581b, 0x06 },
	{ 0x581c, 0x08 },
	{ 0x581d, 0x0c },
	{ 0x581e, 0x3f },
	{ 0x581f, 0x1e },
	{ 0x5820, 0x12 },
	{ 0x5821, 0x13 },
	{ 0x5822, 0x21 },
	{ 0x5823, 0x3f },
	{ 0x5824, 0x68 },
	{ 0x5825, 0x28 },
	{ 0x5826, 0x2c },
	{ 0x5827, 0x28 },
	{ 0x5828, 0x08 },
	{ 0x5829, 0x48 },
	{ 0x582a, 0x64 },
	{ 0x582b, 0x62 },
	{ 0x582c, 0x64 },
	{ 0x582d, 0x28 },
	{ 0x582e, 0x46 },
	{ 0x582f, 0x62 },
	{ 0x5830, 0x60 },
	{ 0x5831, 0x62 },
	{ 0x5832, 0x26 },
	{ 0x5833, 0x48 },
	{ 0x5834, 0x66 },
	{ 0x5835, 0x44 },
	{ 0x5836, 0x64 },
	{ 0x5837, 0x28 },
	{ 0x5838, 0x66 },
	{ 0x5839, 0x48 },
	{ 0x583a, 0x2c },
	{ 0x583b, 0x28 },
	{ 0x583c, 0x26 },
	{ 0x583d, 0xae },
	{ 0x5025, 0x00 },
	{ 0x3a0f, 0x30 },
	{ 0x3a10, 0x28 },
	{ 0x3a1b, 0x30 },
	{ 0x3a1e, 0x26 },
	{ 0x3a11, 0x60 },
	{ 0x3a1f, 0x14 },
	{ 0x0601, 0x02 },
	{ 0x3008, 0x42 },
	{ 0x3008, 0x02 },
	{ OV5645_IO_MIPI_CTRL00, 0x40 },
	{ OV5645_MIPI_CTRL00, 0x24 },	/* 0x04=continuous clock, 0x24=gate clock */
	{ OV5645_PAD_OUTPUT00, 0x70 }
};

static const struct reg_value ov5645_setting_sxga[] = {
	{ 0x3612, 0xa9 },
	{ 0x3614, 0x50 },
	{ 0x3618, 0x00 },
	{ 0x3034, 0x18 },
	{ 0x3035, 0x21 },
	{ 0x3036, 0x70 },
	{ 0x3600, 0x09 },
	{ 0x3601, 0x43 },
	{ 0x3708, 0x66 },
	{ 0x370c, 0xc3 },
	{ 0x3800, 0x00 },
	{ 0x3801, 0x00 },
	{ 0x3802, 0x00 },
	{ 0x3803, 0x06 },
	{ 0x3804, 0x0a },
	{ 0x3805, 0x3f },
	{ 0x3806, 0x07 },
	{ 0x3807, 0x9d },
	{ 0x3808, 0x05 },
	{ 0x3809, 0x00 },
	{ 0x380a, 0x03 },
	{ 0x380b, 0xc0 },
	{ 0x380c, 0x07 },
	{ 0x380d, 0x68 },
	{ 0x380e, 0x03 },
	{ 0x380f, 0xd8 },
	{ 0x3813, 0x06 },
	{ 0x3814, 0x31 },
	{ 0x3815, 0x31 },
	{ 0x3820, 0x47 },
	{ 0x3a02, 0x03 },
	{ 0x3a03, 0xd8 },
	{ 0x3a08, 0x01 },
	{ 0x3a09, 0xf8 },
	{ 0x3a0a, 0x01 },
	{ 0x3a0b, 0xa4 },
	{ 0x3a0e, 0x02 },
	{ 0x3a0d, 0x02 },
	{ 0x3a14, 0x03 },
	{ 0x3a15, 0xd8 },
	{ 0x3a18, 0x00 },
	{ 0x4004, 0x02 },
	{ 0x4005, 0x18 },
	/*
	 * Bridge expects MEDIA_BUS_FMT_YUYV8_1X16; sensor manual: 0x30 = YUYV,
	 * 0x32 = UYVY.
	 */
	{ 0x4300, 0x32 },
	{ 0x4202, 0x00 }
};

static const struct reg_value ov5645_setting_1080p[] = {
	{ 0x3612, 0xab },
	{ 0x3614, 0x50 },
	{ 0x3618, 0x04 },
	{ 0x3034, 0x18 },
	{ 0x3035, 0x11 },
	{ 0x3036, 0x54 },
	{ 0x3600, 0x08 },
	{ 0x3601, 0x33 },
	{ 0x3708, 0x63 },
	{ 0x370c, 0xc0 },
	{ 0x3800, 0x01 },
	{ 0x3801, 0x50 },
	{ 0x3802, 0x01 },
	{ 0x3803, 0xb2 },
	{ 0x3804, 0x08 },
	{ 0x3805, 0xef },
	{ 0x3806, 0x05 },
	{ 0x3807, 0xf1 },
	{ 0x3808, 0x07 },
	{ 0x3809, 0x80 },
	{ 0x380a, 0x04 },
	{ 0x380b, 0x38 },
	{ 0x380c, 0x09 },
	{ 0x380d, 0xc4 },
	{ 0x380e, 0x04 },
	{ 0x380f, 0x60 },
	{ 0x3813, 0x04 },
	{ 0x3814, 0x11 },
	{ 0x3815, 0x11 },
	{ 0x3820, 0x47 },
	{ 0x4514, 0x88 },
	{ 0x3a02, 0x04 },
	{ 0x3a03, 0x60 },
	{ 0x3a08, 0x01 },
	{ 0x3a09, 0x50 },
	{ 0x3a0a, 0x01 },
	{ 0x3a0b, 0x18 },
	{ 0x3a0e, 0x03 },
	{ 0x3a0d, 0x04 },
	{ 0x3a14, 0x04 },
	{ 0x3a15, 0x60 },
	{ 0x3a18, 0x00 },
	{ 0x4004, 0x06 },
	{ 0x4005, 0x18 },
	{ 0x4300, 0x32 },
	{ 0x4202, 0x00 },
	{ 0x4837, 0x0b },
	{ OV5645_IO_MIPI_CTRL00, 0x40 },
	{ OV5645_SYSTEM_CTRL0, OV5645_SYSTEM_CTRL0_STOP }
};

static const struct reg_value ov5645_setting_full[] = {
	{ 0x3612, 0xab },
	{ 0x3614, 0x50 },
	{ 0x3618, 0x04 },
	{ 0x3034, 0x18 },
	{ 0x3035, 0x11 },
	{ 0x3036, 0x54 },
	{ 0x3600, 0x08 },
	{ 0x3601, 0x33 },
	{ 0x3708, 0x63 },
	{ 0x370c, 0xc0 },
	{ 0x3800, 0x00 },
	{ 0x3801, 0x00 },
	{ 0x3802, 0x00 },
	{ 0x3803, 0x00 },
	{ 0x3804, 0x0a },
	{ 0x3805, 0x3f },
	{ 0x3806, 0x07 },
	{ 0x3807, 0x9f },
	{ 0x3808, 0x0a },
	{ 0x3809, 0x20 },
	{ 0x380a, 0x07 },
	{ 0x380b, 0x98 },
	{ 0x380c, 0x0b },
	{ 0x380d, 0x1c },
	{ 0x380e, 0x07 },
	{ 0x380f, 0xb0 },
	{ 0x3813, 0x06 },
	{ 0x3814, 0x11 },
	{ 0x3815, 0x11 },
	{ 0x3820, 0x47 },
	{ 0x4514, 0x88 },
	{ 0x3a02, 0x07 },
	{ 0x3a03, 0xb0 },
	{ 0x3a08, 0x01 },
	{ 0x3a09, 0x27 },
	{ 0x3a0a, 0x00 },
	{ 0x3a0b, 0xf6 },
	{ 0x3a0e, 0x06 },
	{ 0x3a0d, 0x08 },
	{ 0x3a14, 0x07 },
	{ 0x3a15, 0xb0 },
	{ 0x3a18, 0x01 },
	{ 0x4004, 0x06 },
	{ 0x4005, 0x18 },
	{ 0x4300, 0x32 },
	{ 0x4837, 0x0b },
	{ 0x4202, 0x00 }
};

static const s64 link_freq[] = {
	224000000,
	200000000
};

static const struct ov5645_mode_info ov5645_mode_info_data[] = {
	{
		.width = 1280,
		.height = 960,
		.data = ov5645_setting_sxga,
		.data_size = ARRAY_SIZE(ov5645_setting_sxga),
		.pixel_clock = 112000000,
		.link_freq = 0 /* an index in link_freq[] */
	},
	{
		.width = 1920,
		.height = 1080,
		.data = ov5645_setting_1080p,
		.data_size = ARRAY_SIZE(ov5645_setting_1080p),
		.pixel_clock = 168000000,
		.link_freq = 1 /* an index in link_freq[] */
	},
	{
		.width = 2592,
		.height = 1944,
		.data = ov5645_setting_full,
		.data_size = ARRAY_SIZE(ov5645_setting_full),
		.pixel_clock = 168000000,
		.link_freq = 1 /* an index in link_freq[] */
	},
};

static int ov5645_read_reg(struct ov5645 *ov5645, u16 reg, u8 *val)
{
	struct i2c_msg msgs[2];
    u8 addr[2] = { reg >> 8, reg & 0xff };
    int ret;

	msgs[0].addr  = ov5645->i2c_client->addr;
    msgs[0].flags = 0;
    msgs[0].len   = 2;
    msgs[0].buf   = addr;

    msgs[1].addr  = ov5645->i2c_client->addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = 1;
	msgs[1].buf   = val;

	ret = i2c_transfer(ov5645->i2c_client->adapter, msgs, 2);
	if (ret != 2) {
		dev_err(ov5645->dev, "%s: i2c_transfer failed ret=%d reg=0x%04x\n",
			__func__, ret, reg);
		return (ret < 0) ? ret : -EIO;
	}
	return 0;
}

static int ov5645_write_reg(struct ov5645 *ov5645, u16 reg, u8 val)
{
	struct i2c_msg msg;
	u8 buf[3];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	buf[2] = val;

	msg.addr  = ov5645->i2c_client->addr;
	msg.flags = 0;
	msg.len   = 3;
	msg.buf   = buf;

	ret = i2c_transfer(ov5645->i2c_client->adapter, &msg, 1);
	if (ret != 1) {
		dev_err(ov5645->dev, "%s: i2c_transfer failed ret=%d reg=0x%04x val=0x%02x\n",
			__func__, ret, reg, val);
		return (ret < 0) ? ret : -EIO;
	}

	return 0;
}

static int ov5645_set_aec_mode(struct ov5645 *ov5645, u32 mode)
{
	u8 val = ov5645->aec_pk_manual;
	int ret;

	if (mode == V4L2_EXPOSURE_AUTO)
		val &= ~OV5645_AEC_MANUAL_ENABLE;
	else /* V4L2_EXPOSURE_MANUAL */
		val |= OV5645_AEC_MANUAL_ENABLE;

	ret = ov5645_write_reg(ov5645, OV5645_AEC_PK_MANUAL, val);
	if (!ret)
		ov5645->aec_pk_manual = val;

	return ret;
}

static int ov5645_set_agc_mode(struct ov5645 *ov5645, u32 enable)
{
	u8 val = ov5645->aec_pk_manual;
	int ret;

	if (enable)
		val &= ~OV5645_AGC_MANUAL_ENABLE;
	else
		val |= OV5645_AGC_MANUAL_ENABLE;

	ret = ov5645_write_reg(ov5645, OV5645_AEC_PK_MANUAL, val);
	if (!ret)
		ov5645->aec_pk_manual = val;

	return ret;
}

static int ov5645_set_register_array(struct ov5645 *ov5645,
		const struct reg_value *settings,
		unsigned int num_settings)
{
	unsigned int i;
	int ret;

	for (i = 0; i < num_settings; ++i, ++settings) {
		ret = ov5645_write_reg(ov5645, settings->reg, settings->val);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int ov5645_set_saturation(struct ov5645 *ov5645, s32 value)
{
	u32 reg_value = (value * 0x10) + 0x40;
	int ret;

	ret = ov5645_write_reg(ov5645, OV5645_SDE_SAT_U, reg_value);
	if (ret < 0)
		return ret;

	return ov5645_write_reg(ov5645, OV5645_SDE_SAT_V, reg_value);
}

static int ov5645_set_hflip(struct ov5645 *ov5645, s32 value)
{
	u8 val = ov5645->timing_tc_reg21;
	int ret;

	if (value == 0)
		val &= ~(OV5645_SENSOR_MIRROR);
	else
		val |= (OV5645_SENSOR_MIRROR);

	ret = ov5645_write_reg(ov5645, OV5645_TIMING_TC_REG21, val);
	if (!ret)
		ov5645->timing_tc_reg21 = val;

	return ret;
}

static int ov5645_set_vflip(struct ov5645 *ov5645, s32 value)
{
	u8 val = ov5645->timing_tc_reg20;
	int ret;

	if (value == 0)
		val |= (OV5645_SENSOR_VFLIP | OV5645_ISP_VFLIP);
	else
		val &= ~(OV5645_SENSOR_VFLIP | OV5645_ISP_VFLIP);

	ret = ov5645_write_reg(ov5645, OV5645_TIMING_TC_REG20, val);
	if (!ret)
		ov5645->timing_tc_reg20 = val;

	return ret;
}

static int ov5645_set_test_pattern(struct ov5645 *ov5645, s32 value)
{
	u8 val = 0;

	if (value) {
		val = OV5645_SET_TEST_PATTERN(value - 1);
		val |= OV5645_TEST_PATTERN_ENABLE;
	}

	return ov5645_write_reg(ov5645, OV5645_PRE_ISP_TEST_SETTING_1, val);
}

static const char * const ov5645_test_pattern_menu[] = {
	"Disabled",
	"Vertical Color Bars",
	"Pseudo-Random Data",
	"Color Square",
	"Black Image",
};

static int ov5645_set_awb(struct ov5645 *ov5645, s32 enable_auto)
{
	u8 val = 0;

	if (!enable_auto)
		val = OV5645_AWB_MANUAL_ENABLE;

	return ov5645_write_reg(ov5645, OV5645_AWB_MANUAL_CONTROL, val);
}

static int ov5645_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ov5645 *ov5645 = container_of(ctrl->handler,
					     struct ov5645, ctrls);
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_SATURATION:
		ret = ov5645_set_saturation(ov5645, ctrl->val);
		break;
	case V4L2_CID_AUTO_WHITE_BALANCE:
		ret = ov5645_set_awb(ov5645, ctrl->val);
		break;
	case V4L2_CID_AUTOGAIN:
		ret = ov5645_set_agc_mode(ov5645, ctrl->val);
		break;
	case V4L2_CID_EXPOSURE_AUTO:
		ret = ov5645_set_aec_mode(ov5645, ctrl->val);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = ov5645_set_test_pattern(ov5645, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
		ret = ov5645_set_hflip(ov5645, ctrl->val);
		break;
	case V4L2_CID_VFLIP:
		ret = ov5645_set_vflip(ov5645, ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static const struct v4l2_ctrl_ops ov5645_ctrl_ops = {
	.s_ctrl = ov5645_s_ctrl,
};

static int ov5645_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	//code->code = MEDIA_BUS_FMT_UYVY8_1X16;
	/* Bridge only supports YUYV8. */
	code->code = MEDIA_BUS_FMT_YUYV8_1X16;


	return 0;
}

static int ov5645_enum_frame_size(struct v4l2_subdev *subdev,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->code != MEDIA_BUS_FMT_YUYV8_1X16)
	return -EINVAL;

	if (fse->index >= ARRAY_SIZE(ov5645_mode_info_data))
		return -EINVAL;

	fse->min_width = ov5645_mode_info_data[fse->index].width;
	fse->max_width = ov5645_mode_info_data[fse->index].width;
	fse->min_height = ov5645_mode_info_data[fse->index].height;
	fse->max_height = ov5645_mode_info_data[fse->index].height;

	return 0;
}

static struct v4l2_mbus_framefmt *
__ov5645_get_pad_format(struct ov5645 *ov5645,
			struct v4l2_subdev_state *sd_state,
			unsigned int pad,
			enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&ov5645->sd, sd_state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &ov5645->fmt;
	default:
		return NULL;
	}
}

static int ov5645_get_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_format *format)
{
	struct ov5645 *ov5645 = to_ov5645(sd);

	format->format = *__ov5645_get_pad_format(ov5645, sd_state,
						  format->pad,
						  format->which);
	return 0;
}

static struct v4l2_rect *
__ov5645_get_pad_crop(struct ov5645 *ov5645,
		      struct v4l2_subdev_state *sd_state,
		      unsigned int pad, enum v4l2_subdev_format_whence which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_crop(&ov5645->sd, sd_state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &ov5645->crop;
	default:
		return NULL;
	}
}

static int ov5645_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_format *format)
{
	struct ov5645 *ov5645 = to_ov5645(sd);
	struct v4l2_mbus_framefmt *__format;
	struct v4l2_rect *__crop;
	const struct ov5645_mode_info *new_mode;
	int ret;

	__crop = __ov5645_get_pad_crop(ov5645, sd_state, format->pad,
				       format->which);

	new_mode = v4l2_find_nearest_size(ov5645_mode_info_data,
			       ARRAY_SIZE(ov5645_mode_info_data),
			       width, height,
			       format->format.width, format->format.height);

	__crop->width = new_mode->width;
	__crop->height = new_mode->height;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		ret = __v4l2_ctrl_s_ctrl_int64(ov5645->pixel_clock,
					     new_mode->pixel_clock);
		if (ret < 0)
			return ret;

		ret = __v4l2_ctrl_s_ctrl(ov5645->link_freq,
				       new_mode->link_freq);
		if (ret < 0)
			return ret;

		ov5645->current_mode = new_mode;

		dev_info(ov5645->dev,
			"set_fmt ACTIVE: mode=%ux%u link_freq_idx=%d pixel_rate=%u\n",
			new_mode->width, new_mode->height,
			v4l2_ctrl_g_ctrl(ov5645->link_freq),
			new_mode->pixel_clock);
	}

	__format = __ov5645_get_pad_format(ov5645, sd_state, format->pad,
					   format->which);
	__format->width = __crop->width;
	__format->height = __crop->height;
	// __format->code = MEDIA_BUS_FMT_UYVY8_1X16;
	/* Bridge only supports YUYV8. */
	__format->code = MEDIA_BUS_FMT_YUYV8_1X16;
	__format->field = V4L2_FIELD_NONE;
	__format->colorspace = V4L2_COLORSPACE_SRGB;

	format->format = *__format;

	return 0;
}

static int ov5645_entity_init_cfg(struct v4l2_subdev *subdev,
				  struct v4l2_subdev_state *sd_state)
{
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.format.width = 1920;
	fmt.format.height = 1080;

	ov5645_set_format(subdev, sd_state, &fmt);

	return 0;
}

static int ov5645_get_selection(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *sd_state,
			   struct v4l2_subdev_selection *sel)
{
	struct ov5645 *ov5645 = to_ov5645(sd);

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	sel->r = *__ov5645_get_pad_crop(ov5645, sd_state, sel->pad,
					sel->which);
	return 0;
}

static int ov5645_get_mbus_config(struct v4l2_subdev *sd,
				  unsigned int pad,
				  struct v4l2_mbus_config *config)
{
	struct ov5645 *ov5645 = to_ov5645(sd);
	u32 lane_num;
	u32 flags = 0;

	if (pad != OV5645_SD_PAD_SOURCE_CSI)
		return -EINVAL;

	memset(config, 0, sizeof(*config));

	lane_num = ov5645->ep.bus.mipi_csi2.num_data_lanes;
	if (!lane_num)
		lane_num = 2;

	config->type = ov5645->ep.bus_type;

	flags = (1 << (lane_num - 1));
	flags |= V4L2_MBUS_CSI2_CHANNEL_0;
	flags |= V4L2_MBUS_CSI2_CONTINUOUS_CLOCK;

	config->bus.mipi_csi2.flags = flags;

	dev_info(ov5645->dev,
		"### OV5645 G_MBUS_CONFIG ### type=%u lanes=%u flags=0x%x\n",
		config->type, lane_num, flags);

	return 0;
}

static int ov5645_s_stream(struct v4l2_subdev *subdev, int enable)
{
	struct ov5645 *ov5645 = to_ov5645(subdev);
	int ret;

	if (!ov5645->current_mode) {
		dev_err(ov5645->dev, "current_mode is NULL\n");
		return -EINVAL;
	}

	if (enable) {
		/* Start streaming */
		ret = ov5645_write_reg(ov5645, OV5645_IO_MIPI_CTRL00, 0x45);
		if (ret < 0) {
			dev_err(ov5645->dev, "failed to enable mipi output\n");

		}

		dev_info(ov5645->dev, "about to start sensor stream\n");
		ret = ov5645_write_reg(ov5645, OV5645_SYSTEM_CTRL0,
				       OV5645_SYSTEM_CTRL0_START);
		if (ret < 0) {
			dev_err(ov5645->dev, "failed to start sensor stream\n");

		}

		dev_info(ov5645->dev, "stream on ok\n");

		return 0;

	} else {

		/* Stop streaming */
		ret = ov5645_write_reg(ov5645, OV5645_IO_MIPI_CTRL00, 0x40);
		if (ret < 0)
			dev_warn(ov5645->dev, "failed to stop mipi output: %d\n", ret);

		ret = ov5645_write_reg(ov5645, OV5645_SYSTEM_CTRL0,
				OV5645_SYSTEM_CTRL0_STOP);
		if (ret < 0)
			dev_warn(ov5645->dev, "failed to stop sensor stream: %d\n", ret);

		dev_info(ov5645->dev, "stream off ok\n");

		return 0;
	}
}

static const struct v4l2_subdev_video_ops ov5645_video_ops = {
	.s_stream = ov5645_s_stream,
};

static const struct v4l2_subdev_pad_ops ov5645_subdev_pad_ops = {
	.init_cfg = ov5645_entity_init_cfg,
	.enum_mbus_code = ov5645_enum_mbus_code,
	.enum_frame_size = ov5645_enum_frame_size,
	.get_fmt = ov5645_get_format,
	.set_fmt = ov5645_set_format,
	.get_selection = ov5645_get_selection,
	.get_mbus_config = ov5645_get_mbus_config,
};

static const struct v4l2_subdev_ops ov5645_subdev_ops = {
	.video = &ov5645_video_ops,
	.pad = &ov5645_subdev_pad_ops,
};

/* parse ov5645 */
static int ov5645_parse(struct ov5645 *ov5645)
{
	struct device *dev = &ov5645->i2c_client->dev;
	struct fwnode_handle *endpoint;
	struct ov5645_pm *pm;
	u32 xclk_freq;
	int ret;

	/* Graph: next endpoint under the port node */
	endpoint = fwnode_graph_get_next_endpoint(
			dev_fwnode(&ov5645->i2c_client->dev), NULL);
	if (!endpoint) {
		dev_err(&ov5645->i2c_client->dev,
				"endpoint node not found\n");
		return -EINVAL;
	}

	/* Parse into ov5645->ep */
	ret = v4l2_fwnode_endpoint_parse(endpoint, &ov5645->ep);
	/* Release endpoint reference */
	fwnode_handle_put(endpoint);

	if (ret) {
		dev_err(&ov5645->i2c_client->dev,
				"Could not parse endpoint\n");
		return ret;
	}

	if (ov5645->ep.bus_type != V4L2_MBUS_CSI2_DPHY) {
		dev_err(dev, "invalid bus type, must be CSI2\n");
		return -EINVAL;
	}

	if (ov5645->platform_id == CIX_PLATFORM_SOC) {

		ov5645->xclk = devm_clk_get_optional(dev, "mclk");
		if (IS_ERR(ov5645->xclk)) {
			dev_err(dev, "could not get xclk");
			return PTR_ERR(ov5645->xclk);
		}

		/* get clock-frequency */
		ret = of_property_read_u32(dev->of_node, "clock-frequency", &xclk_freq);
		if (ret) {
			dev_err(dev, "could not get xclk frequency\n");
			return ret;
		}

		/* external clock must be 24MHz, allow 1% tolerance */
		if (xclk_freq < 23760000 || xclk_freq > 24240000) {
			dev_err(dev, "external clock frequency %u is not supported\n",
					xclk_freq);
			return -EINVAL;
		}

		ret = clk_set_rate(ov5645->xclk, xclk_freq);
		if (ret) {
			dev_err(dev, "could not set xclk frequency\n");
			return ret;
		}

		ret = clk_prepare_enable(ov5645->xclk);
		if (ret) {
			dev_err(ov5645->dev, "Failed to enable mclk1\n");
			return ret;
		}

		/*
		 * GPIO / pinctrl parsing and the power-on / power-off
		 * sequence live in the companion ov5645_power_manage
		 * platform driver. Resolve the phandle to that driver here.
		 */
		pm = ov5645_pm_get(dev);
		if (IS_ERR(pm)) {
			ret = PTR_ERR(pm);
			if (ret != -EPROBE_DEFER)
				dev_err(dev,
					"failed to get ov5645 power-mgr: %d\n",
					ret);
			return ret;
		}
		ov5645->pm = pm;
	}

	dev_info(ov5645->dev, "### OV5645 NEW BUILD ### ep lanes=%u, bus_type=%u\n",
			ov5645->ep.bus.mipi_csi2.num_data_lanes,
			ov5645->ep.bus_type);

	return ret;
}

static int ov5645_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct ov5645 *ov5645;
	struct v4l2_subdev *sd;
	int ret;
	u8 idh, idl;

	ov5645 = devm_kzalloc(dev, sizeof(struct ov5645), GFP_KERNEL);
	if (!ov5645)
		return -ENOMEM;

	ov5645->i2c_client = client;
	ov5645->dev = dev;
	ov5645->platform_id = CIX_PLATFORM_SOC;

	ret = ov5645_parse(ov5645);
	if (ret) {
		v4l2_err(&ov5645->sd, "ov5645_parse failed! err:%d\n", ret);
		return ret;
	}

	ov5645->current_mode = &ov5645_mode_info_data[1];

	mutex_init(&ov5645->power_lock);

	v4l2_ctrl_handler_init(&ov5645->ctrls, 9);
	v4l2_ctrl_new_std(&ov5645->ctrls, &ov5645_ctrl_ops,
			  V4L2_CID_SATURATION, -4, 4, 1, 0);
	v4l2_ctrl_new_std(&ov5645->ctrls, &ov5645_ctrl_ops,
			  V4L2_CID_HFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&ov5645->ctrls, &ov5645_ctrl_ops,
			  V4L2_CID_VFLIP, 0, 1, 1, 0);
	v4l2_ctrl_new_std(&ov5645->ctrls, &ov5645_ctrl_ops,
			  V4L2_CID_AUTOGAIN, 0, 1, 1, 1);
	v4l2_ctrl_new_std(&ov5645->ctrls, &ov5645_ctrl_ops,
			  V4L2_CID_AUTO_WHITE_BALANCE, 0, 1, 1, 1);
	v4l2_ctrl_new_std_menu(&ov5645->ctrls, &ov5645_ctrl_ops,
			       V4L2_CID_EXPOSURE_AUTO, V4L2_EXPOSURE_MANUAL,
			       0, V4L2_EXPOSURE_AUTO);
	v4l2_ctrl_new_std_menu_items(&ov5645->ctrls, &ov5645_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(ov5645_test_pattern_menu) - 1,
				     0, 0, ov5645_test_pattern_menu);
	ov5645->pixel_clock = v4l2_ctrl_new_std(&ov5645->ctrls,
						NULL,
						V4L2_CID_PIXEL_RATE,
						1, INT_MAX, 1, 1);
	ov5645->link_freq = v4l2_ctrl_new_int_menu(&ov5645->ctrls,
						   NULL,
						   V4L2_CID_LINK_FREQ,
						   ARRAY_SIZE(link_freq) - 1,
						   0, link_freq);
	ov5645->sd.ctrl_handler = &ov5645->ctrls;

	if (ov5645->ctrls.error) {
		dev_err(dev, "%s: control initialization error %d\n",
		       __func__, ov5645->ctrls.error);
		ret = ov5645->ctrls.error;
		goto free_ctrl;
	}

	ret = __v4l2_ctrl_s_ctrl(ov5645->link_freq, ov5645->current_mode->link_freq);
	if (ret < 0) {
		dev_err(dev, "failed to init link_freq ctrl: %d\n", ret);
		goto free_ctrl;
	}

	ret = __v4l2_ctrl_s_ctrl_int64(ov5645->pixel_clock,
					ov5645->current_mode->pixel_clock);
	if (ret < 0) {
		dev_err(dev, "failed to init pixel_rate ctrl: %d\n", ret);
		goto free_ctrl;
	}

	dev_info(dev, "init ctrls: link_freq_idx=%d pixel_rate=%lld\n",
		v4l2_ctrl_g_ctrl(ov5645->link_freq),
		(long long)v4l2_ctrl_g_ctrl_int64(ov5645->pixel_clock));

	sd = &ov5645->sd;
	snprintf(sd->name, sizeof(sd->name), "%s", CIX_OV5645_SUBDEV_NAME);

	v4l2_i2c_subdev_init(sd, client, &ov5645_subdev_ops);

#if defined(CONFIG_MEDIA_CONTROLLER)
	/* Camera sensor entity */
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	/* CSI source pad */
	ov5645->pad[OV5645_SD_PAD_SOURCE_CSI].flags = MEDIA_PAD_FL_SOURCE;
	/* Register pads on the media entity */
	ret = media_entity_pads_init(&sd->entity, OV5645_SD_PAD_MAX_NUM,
						ov5645->pad);
	if (ret < 0) {
		dev_err(dev, "pads init failed %d", ret);
		goto free_ctrl;
	}
	dev_info(dev, "media entity pads initialized\n");
#endif

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_IS_I2C;
	sd->dev = &client->dev;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;

	if (ov5645->platform_id == CIX_PLATFORM_SOC) {

		ret = ov5645_pm_power_on(ov5645->pm);
		if (ret)
			goto free_entity;

		ret = ov5645_read_reg(ov5645, OV5645_CHIP_ID_HIGH, &idh);
		ret |= ov5645_read_reg(ov5645, OV5645_CHIP_ID_LOW, &idl);

		if (idh != OV5645_CHIP_ID_HIGH_BYTE || idl != OV5645_CHIP_ID_LOW_BYTE) {
			dev_err(dev, "OV5645 chip id mismatch: %02x %02x\n", idh, idl);
			return -ENODEV;
		}

		ret = ov5645_set_register_array(ov5645, ov5645_global_init_setting,
				ARRAY_SIZE(ov5645_global_init_setting));
		if (ret) {
			dev_err(ov5645->dev, "could not set global init registers\n");
			ret = -EIO;
			return -ENODEV;
		}

		ret = ov5645_set_register_array(ov5645,
					ov5645->current_mode->data,
					ov5645->current_mode->data_size);
		if (ret < 0) {
			dev_err(ov5645->dev, "could not set mode %dx%d\n",
				ov5645->current_mode->width,
				ov5645->current_mode->height);
			return -ENODEV;
		}
	}

	ov5645_entity_init_cfg(sd, NULL);

	ret = v4l2_async_register_subdev(sd);
	if (ret < 0) {
		dev_err(ov5645->dev, "ov5645 subdev registration failed\n");
		goto free_entity;
	}

	return 0;

free_entity:
	media_entity_cleanup(&ov5645->sd.entity);
free_ctrl:
	v4l2_ctrl_handler_free(&ov5645->ctrls);
	mutex_destroy(&ov5645->power_lock);
	if (ov5645->pm) {
		ov5645_pm_put(ov5645->pm);
		ov5645->pm = NULL;
	}

	return ret;
}

static void ov5645_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5645 *ov5645 = to_ov5645(sd);

	/* ov5645_pm_power_off() is idempotent; safe to call unconditionally. */
	if (ov5645->pm) {
		ov5645_pm_power_off(ov5645->pm);
		ov5645_pm_put(ov5645->pm);
		ov5645->pm = NULL;
	}

	v4l2_async_unregister_subdev(&ov5645->sd);
	media_entity_cleanup(&ov5645->sd.entity);
	v4l2_ctrl_handler_free(&ov5645->ctrls);
	mutex_destroy(&ov5645->power_lock);
}

static const struct i2c_device_id ov5645_id[] = {
	{ "ov5645", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, ov5645_id);

static const struct of_device_id ov5645_of_match[] = {
	{ .compatible = "ovti,ov5645" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov5645_of_match);

static struct i2c_driver ov5645_i2c_driver = {
	.driver = {
		.of_match_table = ov5645_of_match,
		.name  = "ov5645",
	},
	.probe = ov5645_probe,
	.remove = ov5645_remove,
	.id_table = ov5645_id,
};

module_i2c_driver(ov5645_i2c_driver);

MODULE_DESCRIPTION("Omnivision OV5645 Camera Driver");
MODULE_AUTHOR("camera team of CIX");
MODULE_LICENSE("GPL");
