// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Silicon Image SiI9020 Tx Compatible HDMI Transmitters driver
 *
 * This driver may cover (but not limited to):
 *   SiI9020(A) / SiI9022 / SiI9024 / SiI9034 / SiI9134
 *   SiI9022A / SiI9024A / SiI9136-3 / SiI9136 / SiI9334
 *
 * The first line use SiI9020 register scheme natively, though functionalities
 * may vary. The second line are capable of Transmitter Programming Interface
 * (TPI), however they can still be operated under (and default to) SiI9020 Tx
 * Compatible Mode.
 *
 * Written for and tested on HiSilicon embedded SiI9334 IP core. TPI is not
 * desirable here since they don't expose internal I2C controller, which is a
 * must to perform EDID reading under TPI mode.
 *
 * To the extent of my knowledge, I have tried my best to cover every detail
 * about all devices listed above, but you should always test it yourself if you
 * want to adopt this driver to your needs.
 *
 * You may be interested in this file too:
 *   drivers/video/fbdev/omap2/omapfb/dss/hdmi4_core.h
 *
 * Copyright (c) 2023 David Yang
 */
// https://e2e.ti.com/support/processors-group/processors/f/processors-forum/225013/using-off-chip-hdmi-sii9134-9034

#include <linux/device.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/regmap.h>

#include "sii9020.h"

/******** hardware definitions ********/

/* Base */
#define VND_ID0			0x00
#define  VND_ID0_VAL			0x01
#define VND_ID1			0x01
#define  VND_ID1_VAL			0x00
#define DEV_ID0			0x02  /* BCD Device ID last 2 digits */
#define DEV_ID1			0x03  /* BCD Device ID first 2 digits */
#define DEV_REV			0x04
#define SW_RST			0x05
#define  SW_RST_BIT			BIT(0)
#define  AUDIO_FIFO_RST			BIT(1)
#define SYS_CTRL1		0x08
#define  POWER_UP			BIT(0)
#define  LATCH_RISING			BIT(1)
#define  BUS_WIDTH			BIT(2)  /* 0: 12b; 1: 24b */
#define  HSYNC_EN			BIT(4)
#define  VSYNC_EN			BIT(5)
#define  VSYNC_STATUS			BIT(6)  /* emit VSYNC_ACTIVE when rising */
#define SYS_STAT		0x09
#define  TMDS_CLK_STABLE		BIT(0)  /* emit TMDS_CLK_READY when rising, recommend SW_RST_BIT after rising */
#define  HOTPLUG_STATUS 		BIT(1)  /* emit HOTPLUG_CHANGED on change */
#define  RECEIVER_STATUS		BIT(2)  /* emit RECEIVER_CHANGED on change */
#define  VREF_MODE 			BIT(7)  /* always 1 */
#define SYS_CTRL3		0x0a
#define  DVI_10_CTRL			GENMASK(2, 1)
#define SYS_CTRL4		0x0c
#define  PLL_FILTER_EN			BIT(0)
#define  PLL_FILTER_uA			GENMASK(4, 1)  /* filter charge pump current */
#define   PLL_FILTER_uA_5			0
#define   PLL_FILTER_uA_10			1
#define   PLL_FILTER_uA_15			2
#define   PLL_FILTER_uA_25			4
#define   PLL_FILTER_uA_40			7
#define   PLL_FILTER_uA_45			8
#define   PLL_FILTER_uA_80			15
#define DATA_CTRL		0x0d
#define  AUDIO_MUTE			BIT(1)
#define  VIDEO_BLANK			BIT(2)  /* send VIDEO_BLANKn color as video */

/* HDCP */
#define HDCP_CTRL		0x0f
#define  HDCP_EN			BIT(0)  /* unsetting emit ENC_DIS */
#define  Ri_READY			BIT(1)
#define  CP_RST_NEG			BIT(2)  /* content protection ~reset */
#define  An_STOP			BIT(3)
#define  IS_REPEATER			BIT(4)
#define  Bksv_ERR			BIT(5)
#define  ENC_EN				BIT(6)  /* emit ENC_DISABLED on unsetting */
#define HDCP_Bksv1		0x10  /* till 5 (0x14), write HDCP_Bksv5 last */
#define HDCP_An1		0x15  /* till 8 (0x1c) */
#define HDCP_Aksv1		0x1d  /* till 5 (0x21), write HDCP_Aksv5 last */
#define HDCP_Ri1		0x22
#define HDCP_Ri2		0x23
#define HDCP_Ri_128_CNT		0x24  /* mod 128, emit RI_128 every HDCP_I_CNT == val */
#define HDCP_I_CNT		0x25  /* mod 128 */
#define Ri_STATUS		0x26
#define  Ri_CHECK_ENABLED		BIT(0)  /* take DDC exclusively */
#define Ri_CMD			0x27
#define  Ri_CHECK_EN			BIT(0)
#define  BCAP_POLL_EN			BIT(1)  /* poll BCAP_DONE */
#define Ri_START		0x28  /* starting I counter */
#define Ri_RX1			0x29  /* Ri' if Ri check failed */
#define Ri_RX2			0x2a
#define Ri_DEBUG		0x2b
#define  Ri_HOLD			BIT(6)
#define  Ri_FAULT_INJECT		BIT(7)

/* Video */
#define DE_DELAY1		0x32  /* horizontal front porch + sync + back porch */
#define  DE_DELAY_MIN			1u
#define  DE_DELAY_MAX			4095u
#define DE_CTRL			0x33
#define  DE_DELAY2			GENMASK(3, 0)
#define  DE_EN				BIT(6)
#define DE_TOP			0x34  /* vertical front porch + sync + back porch*/
#define  DE_TOP_MIN			1u
#define  DE_TOP_MAX			127u
#define HBIT_TO_HSYNC1		0x40
#define HBIT_TO_HSYNC2		0x41
#define  HBIT_TO_HSYNC_MIN		1u
#define  HBIT_TO_HSYNC_MAX		1023u
#define FIELD2_HSYNC_OFFSET1	0x42
#define FIELD2_HSYNC_OFFSET2	0x43
#define  FIELD2_HSYNC_OFFSET_MIN	1u
#define  FIELD2_HSYNC_OFFSET_MAX	4095u
#define HSYNC_WIDTH1		0x44
#define HSYNC_WIDTH2		0x45
#define  HSYNC_WIDTH_MIN		1u
#define  HSYNC_WIDTH_MAX		1023u
#define VBIT_TO_VSYNC		0x46
#define  VBIT_TO_VSYNC_MIN		1u
#define  VBIT_TO_VSYNC_MAX		63u
#define VSYNC_WIDTH		0x47
#define  VSYNC_WIDTH_MIN		1u
#define  VSYNC_WIDTH_MAX		63u
#define VIDEO_CTRL		0x48
#define  PIXEL_REPLICATE		GENMASK(1, 0)
#define   PIXEL_REPLICATE_1			0
#define   PIXEL_REPLICATE_2			1
#define   PIXEL_REPLICATE_4			3
#define  COLORSPACE_SEL			BIT(4)  /* 0: BT.601; 1: BT.709 */
#define  EXT_BIT_MODE			BIT(5)  /* 0: 8b; 1: 12b 4:2:2 */
#define  INVERT_FIELD_BIT		BIT(7)
#define VIDEO_ACTION_EN		0x49
#define  DOWNSAMPLE_422			BIT(0)  /* 4:4:4 to 4:2:2 */
#define  RANGE_COMPRESS			BIT(1)  /* 0-255 to 16-235/240 */
#define  RGB_TO_YCbCr			BIT(2)
#define  RANGE_CLIP			BIT(3)  /* direct clip to 16-235/240 */
#define  CLIP_COLORSPACE		BIT(4)  /* 0: RGB; 1: YCbCr */
#define  CHANNEL_WIDTH			GENMASK(7, 6)
#define   CHANNEL_WIDTH_8B			0
#define   CHANNEL_WIDTH_10B			1
#define   CHANNEL_WIDTH_12B			2
#define VIDEO_MODE		0x4a
#define  SYNC_EXTRACT			BIT(0)
#define  CHANNEL_DEMUX			BIT(1)
#define  UPSAMPLE_444			BIT(2)  /* 4:2:2 to 4:4:4 */
#define  YCbCr_TO_RGB			BIT(3)
#define  RANGE_EXPAND			BIT(4)  /* 16-235 to 0-255 */
#define  DITHER_EN			BIT(5)
#define  DITHER_MODE			GENMASK(7, 6)
#define   DITHER_MODE_8B			0
#define   DITHER_MODE_10B			1  /* SiI9134 only */
#define   DITHER_MODE_12B			2  /* SiI9134 only */
#define VIDEO_BLANK1		0x4b  /* channel 1 (blue) */
#define VIDEO_BLANK2		0x4c  /* channel 2 (green) */
#define VIDEO_BLANK3		0x4d  /* channel 3 (red) */
#define DC_HEADER		0x4e

/* Interrupt */
#define INT_STATUS		0x70
#define INT1_STATUS		0x71  /* setting to clear */
#define  AUDIO_FIFO_EMPTY		BIT(0)
#define  AUDIO_FIFO_FULL		BIT(1)
#define  Ri_128				BIT(2)  /* see HDCP_Ri_128_CNT */
#define  SPDIF_BI_PHASE_ERR		BIT(3)
#define  SPDIF_DROP_SAMPLE		BIT(4)
#define  RECEIVER_CHANGED		BIT(5)  /* see RECEIVER_STATUS */
#define  HOTPLUG_CHANGED		BIT(6)  /* see HOTPLUG_STATUS */
#define  INT_SOFT			BIT(7)  /* see INT_SOFT_SET */
#define INT2_STATUS		0x72  /* setting to clear */
#define  VSYNC_ACTIVE			BIT(0)  /* see VSYNC_STATUS */
#define  TMDS_CLK_READY			BIT(1)  /* see TMDS_CLK_STABLE */
#define  ACR_FULL			BIT(2)
#define  ACR_CTS_CHANGED		BIT(3)
#define  SPDIF_PREAMBLE_NOT_FOUND	BIT(4)
#define  ENC_DISABLED			BIT(5)  /* see ENC_EN */
#define  SPDIF_PARITY_ERR		BIT(6)
#define  BCAP_DONE			BIT(7)
#define INT3_STATUS		0x73  /* setting to clear */
#define  DDC_FIFO_EMPTY			BIT(0)
#define  DDC_FIFO_FULL			BIT(1)
#define  DDC_FIFO_HALF			BIT(2)
#define  DDC_CMD_DONE			BIT(3)
#define  Ri_MISSMATCH_LAST_FRAME	BIT(4)  /* Ri mismatch at default #127 */
#define  Ri_MISSMATCH_FIRST_FRAME	BIT(5)  /* Ri mismatch at default #0 */
#define  Ri_NOT_CHANGED			BIT(6)
#define  Ri_READING_MORE_ONE_FRAME	BIT(7)  /* Ri not read within one frame */
#define INT1_EN			0x76
#define INT2_EN			0x77
#define INT3_EN			0x78
#define INT_CTRL		0x7b
#define  INT_LEVEL_LOW			BIT(1)
#define  INT_OPEN_DRAIN			BIT(2)
#define  INT_SOFT_SET			BIT(3)  /* emit INT_SOFT, manually */

/* TMDS */
#define TMDS_C_CTRL		0x80
#define  TMDS_C_POST_CNT_DIV		BIT(5)  /* PLL post counter divider ratio */
#define   TMDS_C_POST_CNT_DIV_1			0
#define   TMDS_C_POST_CNT_DIV_2			1
#define TMDS_CTRL		0x82
#define  TMDS_SRC_TERM			BIT(0)  /* internal source termination */
#define  TMDS_LEVEL_BIAS		BIT(2)  /* driver level shifter bias, should be set 1 */
#define  TMDS_PLL_FACTOR		GENMASK(6, 5)
#define   TMDS_PLL_FACTOR_DIV_2			0
#define   TMDS_PLL_FACTOR_1			1
#define   TMDS_PLL_FACTOR_2			2
#define   TMDS_PLL_FACTOR_4			3
#define TMDS_CTRL2		0x83
#define  TMDS_FFR_CNT_DIV		GENMASK(2, 0)  /* PLL filter front counter divider ratio */
#define   TMDS_FFR_CNT_DIV_1			0
#define   TMDS_FFR_CNT_DIV_2			1
#define   TMDS_FFR_CNT_DIV_4			3
#define   TMDS_FFR_CNT_DIV_8			7
#define  TMDS_FFB_CNT_DIV		GENMASK(5, 3)  /* PLL filter feedback counter divider ratio */
#define   TMDS_FFB_CNT_DIV_1			0
#define   TMDS_FFB_CNT_DIV_2			1
#define   TMDS_FFB_CNT_DIV_3			2
#define   TMDS_FFB_CNT_DIV_4			3
#define   TMDS_FFB_CNT_DIV_5			4
#define   TMDS_FFB_CNT_DIV_6			5
#define   TMDS_FFB_CNT_DIV_7			6
#define  TMDS_POST_CNT_DIV		GENMASK(7, 6)  /* PLL post counter divider ratio */
#define   TMDS_POST_CNT_DIV_1			0
#define   TMDS_POST_CNT_DIV_2			1
#define   TMDS_POST_CNT_DIV_4			2
#define TMDS_CTRL3		0x84
#define  TMDS_FPOST_CNT_DIV		GENMASK(2, 0)  /* PLL filter post counter divider ratio */
#define   TMDS_FPOST_CNT_DIV_1			0
#define   TMDS_FPOST_CNT_DIV_2			1
#define   TMDS_FPOST_CNT_DIV_4			3
#define   TMDS_FPOST_CNT_DIV_8			7
#define  TMDS_ITPLL_uA			GENMASK(6, 3)  /* low pass filter frequency response */
#define   TMDS_ITPLL_uA_5			0x0
#define   TMDS_ITPLL_uA_10			0x1
#define   TMDS_ITPLL_uA_20			0x2
#define   TMDS_ITPLL_uA_25			0x3
#define   TMDS_ITPLL_uA_40			0x4
#define   TMDS_ITPLL_uA_50			0x6
#define   TMDS_ITPLL_uA_80			0x8
#define   TMDS_ITPLL_uA_100			0xb
#define   TMDS_ITPLL_uA_135			0xf
#define TMDS_CTRL4		0x85
#define  TMDS_TFR_CNT_DIV		GENMASK(1, 0)  /* PLL front counter divider ratio */
#define   TMDS_TFR_CNT_DIV_1			0
#define   TMDS_TFR_CNT_DIV_2			1
#define   TMDS_TFR_CNT_DIV_4			2

#define TMDS_SHA_CTRL		0xcc
#define  M0_READABLE_EN                    (0x08)  /* M0 readable (1=external, 0-default=internal) */
#define  SHA_DONE                      (0x02)  /* The SHA generator picked up the SHA GO START, write a "1" to clear before any new SHA GO START */
#define  SHA_GO_START                  (0x01)  /* Start the SHA generator */

/* DDC */
#define DDC_MANUAL		0xec
#define  DDC_MANUAL_SCL_IN		BIT(0)
#define  DDC_MANUAL_SDA_IN		BIT(1)
#define  DDC_MANUAL_SCL_OUT		BIT(4)
#define  DDC_MANUAL_SDA_OUT		BIT(5)
#define  DDC_MANUAL_EN			BIT(7)
#define DDC_ADDR		0xed
#define DDC_SEGMENT		0xee
#define DDC_OFFSET		0xef
#define DDC_COUNT1		0xf0  /* 7:0 */
#define DDC_COUNT2		0xf1  /* 9:8 */
#define DDC_STATUS 		0xf2
#define  DDC_FIFO_WRITE_BUST 		BIT(0)
#define  DDC_FIFO_READ_BUST 		BIT(1)
#define  DDC_FIFO_EMPTY			BIT(2)
#define  DDC_FIFO_FULL 			BIT(3)
#define  DDC_BUSY			BIT(4)
#define  DDC_NO_ACK			BIT(5)  /* R/W HW rising only */
#define  DDC_I2C_LOW 			BIT(6)  /* R/W HW rising only */
#define  DDC_STATUS_RESV 		BIT(7)
#define DDC_CMD			0xf3
#define  DDC_CMD_MASK			GENMASK(3, 0)
#define   DDC_CMD_REQUIRE_ACK			BIT(0)
#define   DDC_CMD_READ_CUR			(0 << 1)
#define   DDC_CMD_READ_SEQ			(1 << 1)
#define   DDC_CMD_READ_ENH			(2 << 1)
#define   DDC_CMD_WRITE_SEQ			(3 << 1)
#define   DDC_CMD_CLEAR_FIFO			0x9
#define   DDC_CMD_CLOCK				0xa
#define   DDC_CMD_ABORT				0xf
#define  DDC_CMD_DEL_EN			BIT(4)  /* 3 ns glitch filtering */
#define  DDC_CMD_FLT_EN			BIT(5)  /* disable 300 ns transition time */
#define DDC_DATA 		0xf4  /* fifo */
#define DDC_DATA_CNT 		0xf5
#define  DDC_DATA_MAX 			0x10u

/* ROM */
#define KEY_STATUS		0xf9
#define  KEY_STATUS_CMD_DONE		BIT(0)  /* R/W HW rising only */
#define  KEY_STATUS_CRC_ERR		BIT(1)  /* R/W HW rising only */
#define  KEY_STATUS_BIST1_ERR		BIT(5)  /* R/W HW rising only */
#define  KEY_STATUS_BIST2_ERR		BIT(6)  /* R/W HW rising only */
#define KEY_CMD			0xfa
#define  KEY_CMD_MASK			GENMASK(4, 0)  /* check KEY_STATUS_CMD_DONE */
#define   KEY_CMD_NO_BIST_TESTS			0x00
#define   KEY_CMD_ALL_BIST_TESTS		0x03
#define   KEY_CMD_CRC_TEST			0x04
#define   KEY_CMD_BIST_TEST1			0x08
#define   KEY_CMD_BIST_TEST2			0x10
#define  KEY_CMD_LOAD_KSV		BIT(5)

/* Audio */
#define ACR_CTRL		0x00
#define  ACR_CTRL_NCTS_PKT_EN		BIT(0)
#define  ACR_CTRL_CTS_USE_SOFTWARE	BIT(1)  /* enable ACR_CTSn */
#define ACR_FREQ		0x02
#define  ACR_FREQ_MCLK			GENMASK(2, 0)
#define   ACR_FREQ_MCLK_128			0
#define   ACR_FREQ_MCLK_256			1
#define   ACR_FREQ_MCLK_384			2
#define   ACR_FREQ_MCLK_512			3
#define   ACR_FREQ_MCLK_768			4
#define   ACR_FREQ_MCLK_1024			5
#define   ACR_FREQ_MCLK_1152			6
#define   ACR_FREQ_MCLK_192			7
#define ACR_N1			0x03  /* 7:0 */
#define ACR_N2			0x04  /* 15:8 */
#define ACR_N3			0x05  /* 19:16 */
#define ACR_CTS1		0x06  /* 7:0 */
#define ACR_CTS2		0x07  /* 15:8 */
#define ACR_CTS3		0x08  /* 19:16 */
#define ACR_CTS_STATUS1		0x09  /* 7:0 */
#define ACR_CTS_STATUS2		0x0a  /* 15:8 */
#define ACR_CTS_STATUS3		0x0b  /* 19:16 */
#define AUDIO_MODE		0x14
#define  AUDIO_EN 			BIT(0)
#define  AUDIO_SPDIF_EN 		BIT(1)
#define  AUDIO_DSD_EN			BIT(3)
#define  AUDIO_SD0_EN 			BIT(4)
#define  AUDIO_SD1_EN			BIT(5)
#define  AUDIO_SD2_EN			BIT(6)
#define  AUDIO_SD3_EN 			BIT(7)
#define SPDIF_CTRL		0x15
#define  SPDIF_FREQ_OVERRIDE		BIT(1)  /* force I2S_CHST4 */
#define  SPDIF_NO_AUDIO			BIT(3)  /* input nothing */

/******** driver definitions ********/

static const struct sii9020_db {
	/* BCD Device ID */
	unsigned int id;
	unsigned int rev;
	/* fill every field; unspecified means unknown */
	struct sii9020_cap cap;
} sii9020_dbs[] = {
	{.id = 0x9134, .cap = {}},
	{}
};

static const struct sii9020_cap *
sii9020_dbs_get_cap(unsigned int id, unsigned int rev)
{
	for (const struct sii9020_db *db = sii9020_dbs; db->id; db++)
		if (db->id == id && (db->rev && db->rev == rev))
			return &db->cap;

	return NULL;
}

/******** device_attribute ********/

int sii9020_get_blank(const struct sii9020 *ctx)
{
	return regmap_test_bits(ctx->map, DATA_CTRL, VIDEO_BLANK);
}
EXPORT_SYMBOL_GPL(sii9020_get_blank);

int sii9020_set_blank(const struct sii9020 *ctx, int color)
{
	bool blank = color >= 0;

	if (blank) {
		u8 colors[3] = {color >> 16, color >> 8, color >> 0};
		int ret = regmap_bulk_write(ctx->map, VIDEO_BLANK1, colors, 3);

		if (ret)
			return ret;
	}

	return regmap_set_bits(ctx->map, DATA_CTRL, blank ? VIDEO_BLANK : 0);
}
EXPORT_SYMBOL_GPL(sii9020_set_blank);

/*
static ssize_t
blank_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sii9020 *ctx = dev_get_drvdata(dev);
	int ret;

	ret = sii9020_get_blank(ctx);
	if (ret < 0)
		return ret;

	return sprintf(buf, "%d\n", ret);
}

static ssize_t
blank_store(struct device *dev, struct device_attribute *attr,
	    const char *buf, size_t count)
{
	struct sii9020 *ctx = dev_get_drvdata(dev);
	int color = -1;

	if (!isspace(buf[0]) && kstrtoint(buf, 0, &color))
		return -ERANGE;

	return sii9020_set_blank(ctx, color) ?: count;
}

static DEVICE_ATTR_RW(blank);

struct attribute *sii9020_attrs[] = {
	&dev_attr_blank.attr,
	NULL,
};
EXPORT_SYMBOL_GPL(sii9020_attrs);
*/

/******** DDC ********/

#if 0

int sii9020_set_ddc_gpio(struct sii9020 *ctx, bool ddc_gpio)
{
}

int sii9020_set_ddc_gpio(struct sii9020 *ctx, bool ddc_gpio)
{
	int ret;

	ret = regmap_update_bits(ctx->map, DDC_MANUAL, DDC_MANUAL_EN,
				 ddc_gpio ? DDC_MANUAL_EN : 0);
	if (ret)
		return ret;

	ctx->ddc_gpio = ddc_gpio;
	return 0;
}
EXPORT_SYMBOL_GPL(sii9020_set_ddc_gpio);

static int
sii9020_i2c_access(struct i2c_adapter *adap, u16 addr, unsigned short flags,
		   char read_write, u8 command, int size,
		   union i2c_smbus_data *data)
{
	struct sii9020 *ctx = i2c_get_adapdata(adap);
	struct device *dev = ctx->dev;
	void __iomem *base = ctx->base;
	bool write = read_write == I2C_SMBUS_WRITE;
	int ret = 0;

	switch (addr) {
	case 0x72:
		break;
	case 0x7a:
		base += 0x400;
		break;
	default:
		return -ENXIO;
	}

	if (size == I2C_SMBUS_QUICK)
		return 0;

	pm_runtime_get_sync(dev);

	switch (size) {
	case I2C_SMBUS_BYTE:
		sii9020_rw(base, 0, &command, 1, write);
		break;
	case I2C_SMBUS_BYTE_DATA:
		sii9020_rw(base, command, &data->byte, 1, write);
		break;
	case I2C_SMBUS_WORD_DATA:
		sii9020_rw(base, command, &data->word, 2, write);
		break;
	case I2C_SMBUS_PROC_CALL:
		sii9020_rw(base, command, &data->word, 2, true);
		sii9020_rw(base, command, &data->word, 2, false);
		break;
	case I2C_SMBUS_BLOCK_DATA:
	case I2C_SMBUS_I2C_BLOCK_DATA:
		sii9020_rw(base, command, &data->block[1], data->block[0],
				 write);
		break;
	default:
		dev_err(dev, "Unsupported transaction %d\n", size);
		ret = -EOPNOTSUPP;
	}

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static u32 sii9020_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_NOSTART | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm sii9020_i2c_algorithm = {
	.smbus_xfer = sii9020_i2c_access,
	.functionality = sii9020_i2c_func,
};

/******** device ********/

irqreturn_t sii9020_handle(int irq, void *dev_id)
{
	struct sii9020 *ctx = dev_id;
	u8 vals[6];
	int ret;

	ret = regmap_bulk_read(ctx->map, INT_CNT, vals, ARRAY_SIZE(vals));
	if (WARN_ON(ret))
		return IRQ_HANDLED;
	//if (!vals[0])
	//	return IRQ_NONE;

	printk("INT_CNT: %x\n", vals[0]);
	printk("INT1_STATE: %x\n", vals[1]);
	printk("INT2_STATE: %x\n", vals[2]);
	printk("INT3_STATE: %x\n", vals[3]);
	printk("INT4_STATE: %x\n", vals[4]);
	printk("INT5_STATE: %x\n", vals[5]);

	return IRQ_HANDLED;
}
EXPORT_SYMBOL_GPL(sii9020_handle);

int sii9020_resume(struct sii9020 *ctx)
{
	return regmap_write_async(ctx->map, INT3_MASK,
				  INT3_DDC_FIFO_EMPTY | INT3_DDC_FIFO_FULL |
				  INT3_DDC_CMD_DONE);
}
EXPORT_SYMBOL_GPL(sii9020_resume);

int sii9020_probe(struct sii9020 *ctx, int irq)
{
	struct device *dev = ctx->dev;
	int ret;

	ret = sii9020_set_ddc_gpio(ctx, ctx->ddc_gpio);
	if (ret)
		return ret;

	ctx->adap = (typeof(ctx->adap)) {
		.algo = &sii9020_i2c_algorithm,
		.algo_data = priv,
		.dev = {
			.of_node = dev->of_node,
			.parent = dev,
		}
	};
	i2c_set_adapdata(&ctx->adap, priv);
	strscpy(ctx->adap.name, "sii9134 ddc", sizeof(ctx->adap.name));
	ret = devm_i2c_add_adapter(dev, &ctx->adap);
	if (ret)
		return ret;

	if (irq > 0) {
		ret = devm_request_irq(dev, irq, sii9020_handle,
				       IRQF_SHARED, dev_name(dev), priv);
		if (ret)
			return ret;
	}

	ret = sii9020_resume(priv);
	if (ret)
		return ret;

	pr_debug("%s: probed %s\n", __func__, dev_name(dev));
	return 0;
}
#endif
int sii9020_probe(struct sii9020 *ctx, int irq)
{
	return 0;
}
EXPORT_SYMBOL_GPL(sii9020_probe);

MODULE_DESCRIPTION("SiI9134 HDMI Deep Color Transmitter");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
