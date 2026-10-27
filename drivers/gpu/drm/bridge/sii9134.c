// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023 David Yang
 *
 * Silicon Image SiI9134 HDMI 1.4 transmitter core.
 *
 * Register interface: three banks of byte registers on a 4-byte MMIO stride
 * (see sii9134.h). The bank flattening is not covered by the SiI9134 datasheet,
 * so the register sequences here follow what an integrator's HDMI block
 * expects. The TMDS PHY window and where the DVI encoder sits are per
 * integration and come in through struct sii9134_phy_ops.
 *
 * Not implemented:
 *
 * - HDCP. The transmitter takes its authentication key from an on-chip
 *   key store, which is left blank on these boards, and the driver has
 *   no interface to install keys.
 * - CEC. The CEC controller is a fourth bank at offset 0x800, byte
 *   registers on the same 4-byte stride; wiring it to the CEC framework
 *   would be a separate driver.
 * - SPD and Gamut Metadata packets.
 * - Deep colour and pixel replication: the link runs 8 bits per channel.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <sound/hdmi-codec.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#include <linux/hdmi.h>

#include "sii9134.h"

/*
 * Bank 0 (I2C slave 0x72 in the original design; wrapper regmap .map, MMIO
 * offset 0x0000): TPI / system / timing / interrupts / master DDC.
 */
#define TX_VENDOR_ID_ADDR	0x0	/* reg 0x00, 16-bit, Silicon Image = 0x0001 */
#define TX_DEVICE_ID_ADDR	0x8	/* reg 0x02, 16-bit, SiI9134 = 0x9134 */
#define TX_DEVICE_REV_ADDR	0x10	/* reg 0x04 */
#define TX_SWRST_ADDR		0x14	/* reg 0x05 */
#define BIT_TX_SW_RST			0x01
#define BIT_TX_FIFO_RST			0x02
#define TX_SYS_CTRL1_ADDR	0x20	/* reg 0x08 */
#define BIT_TX_PD			0x01	/* 1 = powered up */
#define BIT_TX_CLOCK_RISING_EDGE	0x02
#define BIT_BSEL24BITS			0x04	/* 24-bit input bus */
#define TX_STAT_ADDR		0x24	/* reg 0x09 */
#define BIT_HDMI_MODE			0x01	/* TMDS clock stable */
#define BIT_HPD_PIN			0x02
#define BIT_RSEN			0x04
#define DATA_CTRL_ADDR		0x34	/* reg 0x0d */
#define BIT_AUD_MUTE			0x02
#define BIT_VID_BLANK			0x04

#define DE_HSTART_ADDR		0xc8	/* reg 0x32 */
#define DE_CNTRL_ADDR		0xcc	/* reg 0x33 */
#define BIT_DE_ENABLED			0x40
#define DE_CNTRL_HSTART_MSB		0x03
#define DE_CNTRL_SYNC_POL		0x30	/* 1 = negative sync */
#define DE_VSTART_ADDR		0xd0	/* reg 0x34 */
#define DE_HRES_ADDR		0xd8	/* reg 0x36, 16-bit */
#define DE_VRES_ADDR		0xe0	/* reg 0x38, 16-bit */
/*
 * read-only counters, only valid while IDCK plus H/VSYNC reach the TX; in the
 * sync-derived mode below 0x3a/0x3c double as the programmed totals, so treat
 * reads with care
 */
#define MEAS_HRES_ADDR		0xe8	/* reg 0x3a, H_RES[12:0], 13-bit */
#define MEAS_VRES_ADDR		0xf0	/* reg 0x3c, V_RES[10:0], 11-bit */

/*
 * Sync-derived video timing: the DE generator stays off and the TX derives the
 * active window from H/V sync and these counters.
 */
#define TX656_ADJ_MODE_ADDR	0xf8	/* reg 0x3e, low bits only */
#define TX656_HTOT_ADDR		0xe8	/* reg 0x3a, 16-bit, htotal */
#define TX656_VTOT_ADDR		0xf0	/* reg 0x3c, 16-bit, vtotal */
#define TX656_HBIT2HSYNC_ADDR	0x100	/* reg 0x40, 16-bit, h front porch */
#define TX656_FIELD2_ADDR	0x108	/* reg 0x42, 16-bit, field-2 offset */
#define TX656_HLENGTH_ADDR	0x110	/* reg 0x44, 16-bit, hsync width */
#define TX656_VBIT_ADDR		0x118	/* reg 0x46, v front porch */
#define TX656_VLENGTH_ADDR	0x11c	/* reg 0x47, vsync width */

#define TX_VID_IN_MODE_ADDR	0x1a4	/* reg 0x69, input bus width/mux */
#define TX_VID_MODE_ADDR		0x128	/* reg 0x4a */
#define TX_VID_MODE_ALL_MUX		0x3f	/* demux/csc/range/dither */
#define TX_VID_MODE_DITHER		0x20	/* reg 0x4a bit5 */
#define TX_VID_ACEN_ADDR		0x124	/* reg 0x49 */
#define TX_VID_ACEN_DWN_SAMPLE		0x01	/* 4:4:4 -> 4:2:2 chroma down sampler */
#define TX_VID_ACEN_RGB2YCBCR		0x04	/* RGB -> YCbCr colour-space converter */
#define TX_VID_ACEN_CLIP_CS		0x10	/* 1 = link carries YCbCr */
#define TX_VID_CTRL_ADDR		0x120	/* reg 0x48 */
#define TX_VID_CTRL_ICLK		0x03
#define TX_VID_CTRL_CSCSEL		0x10	/* BT.709 vs BT.601 */
#define TX_VID_DITHER_ADDR		0x13c	/* reg 0x4f, dither block */
#define TX_VID_DITHER_24BIT		0x0e	/* D_RC_EN | D_GC_EN | D_BC_EN */

#define HDMI_INT1_ADDR		0x1c4	/* reg 0x71, status, write 1 to clear */
#define HDMI_INT_STATE_ADDR	0x1c0	/* reg 0x70, summary state, write 1 to clear */
#define BIT_INT_HOT_PLUG		0x40
#define BIT_INT_RSEN			0x20
#define HDMI_INT_MASK_ADDR	0x1d8	/* reg 0x76 */
#define INT_CNTRL_ADDR		0x1ec	/* reg 0x7b */
#define INT_CONTROL			0x02	/* push-pull, active high */

/* master DDC (used for EDID when no ddc-i2c-bus is wired up) */
#define MDDC_SLAVE_ADDR_REG	0x3b4	/* reg 0xed */
#define MDDC_SEGMENT_ADDR	0x3b8	/* reg 0xee */
#define MDDC_STATUS_ADDR		0x3c8	/* reg 0xf2 */
#define MDDC_COMMAND_ADDR	0x3cc	/* reg 0xf3 */
#define MDDC_FIFO_ADDR		0x3d0	/* reg 0xf4 */
#define MDDC_FIFO_CNT_ADDR	0x3d4	/* reg 0xf5 */
#define MDDC_DELAY_CNT		0x3d8	/* reg 0xf6 */
#define BIT_MDDC_ST_I2C_LOW		0x40
#define BIT_MDDC_ST_NO_ACK		0x20
#define BIT_MDDC_ST_IN_PROGR		0x10
#define BIT_MDDC_ST_FIFO_FULL		0x08
/* SCL = oscclk / (delay * 35): 0x14 = 100 kHz, 0x1a = 75 kHz */
#define MDDC_DELAY_75KHZ		0x1a
#define MDDC_FIFO_DEPTH			16
#define MASTER_CMD_CLEAR_FIFO		0x09
#define MASTER_CMD_ABORT		0x0f
#define MASTER_CMD_CLOCK		0x0a
#define MASTER_CMD_SEQ_RD		0x02
#define MASTER_CMD_ENH_RD		0x04
#define DDC_EDID_ADDR			0xA0
#define DDC_SEGMENT_ADDR		0x30

/*
 * Bank 1 (I2C slave 0x7a in the original design; wrapper regmap .map1, MMIO
 * offset 0x0400): audio, ACR and infoframe packet memory.
 */
#define DIAG_PD_ADDR		0xf4	/* reg 0x3d, all three bits default 1 */
#define BIT_PDIDCK			0x04	/* 1 = IDCK input not blocked */
#define BIT_PDOSC			0x02	/* 1 = ring oscillator on */
#define BIT_PDTOT			0x01	/* 1 = nothing powered down */

#define ACR_CTRL_ADDR		0x4	/* reg 0x01 */
#define ACR_CTRL_N_CTS_ENABLE		0x02
#define ACR_CTRL_INTERNAL_MCLK		0x04
#define FREQ_SVAL_ADDR		0x8	/* reg 0x02, MCLK index */
#define N_SVAL_ADDR		0xc	/* reg 0x03, N value, 24-bit */

#define AUD_MODE_ADDR		0x50	/* reg 0x14 */
#define BIT_AUDIO_EN			0x01
#define BIT_SPDIF_SELECT		0x02
#define BIT_DSD_SELECT			0x08
#define SD_0				0x10	/* enable I2S SD0 */
#define SD_0_3_EN			0xF0	/* enable I2S SD0-3 */
#define I2S_IN_CTRL_ADDR		0x74	/* reg 0x1d */
#define I2S_SCK_RISING			0x40
#define I2S_COMPRESSED			0x10
#define I2S_HBR_ON			0x80
#define I2S_CHST0_ADDR		0x78	/* reg 0x1e */
#define I2S_CHST1_ADDR		0x7c	/* reg 0x1f */
#define I2S_CHST2_ADDR		0x80	/* reg 0x20 */
#define I2S_CHST4_ADDR		0x84	/* reg 0x21, CHST[31:24]: Fs */
#define I2S_CHST5_ADDR		0x88	/* reg 0x22, CHST[39:32]: Fs/len */
#define SAMPLE_RATE_CONVERSION	0x8c	/* reg 0x23 */
#define I2S_IN_LEN		0x90	/* reg 0x24 */
#define AUDP_TXCTRL_ADDR		0xbc	/* reg 0x2f */
#define BIT_TXHDMI_MODE			0x01
#define BIT_LAYOUT1			0x02	/* layout 1: >2 channels */
#define TEST_TX_ADDR		0xf0	/* reg 0x3c */
#define BIT_DVI_ENC_BYPASS		0x08
#define INF_CTRL1		0xf8	/* reg 0x3e, 16-bit */
#define BIT_AVI_REPEAT			0x01
#define BIT_AVI_ENABLE			0x02
#define BIT_SPD_REPEAT			0x04
#define BIT_SPD_ENABLE			0x08
#define BIT_AUD_REPEAT			0x10
#define BIT_AUD_ENABLE			0x20
#define BIT_VSDB_REPEAT			0x40	/* MPEG slot, see below */
#define BIT_VSDB_ENABLE			0x80
#define INF_CTRL2		0xfc	/* reg 0x3f */
#define BIT_CP_REPEAT			0x04
#define BIT_CP_ENABLE			0x08

/* control packet (GCP) interface: write 0x01 to mute, 0x10 to unmute */
#define CP_IF_ADDR		0x37c	/* reg 0xdf */
#define BIT_CP_AVI_MUTE_SET		0x01
#define BIT_CP_AVI_MUTE_CLEAR		0x10

/*
 * Infoframe packet memory: type, version, length, checksum, payload. The
 * HDMI-VSDB vendor packet goes into the MPEG packet slot.
 */
#define AVI_IF_ADDR		0x100	/* reg 0x40 */
#define AUD_IF_ADDR		0x200	/* reg 0x80 */
#define VSDB_IF_ADDR		0x280	/* reg 0xa0 */
#define IFRAME_POLL_MAX			60

/*
 * TMDS clock window. 165MHz is not this IP's ceiling -- that figure is the
 * single-link limit of HDMI 1.0/1.1. The PHY output swing is selected from the
 * TMDS clock and has a band of its own above 165MHz, and pixel clocks up to
 * 297MHz are supported. 4K@50/60 would need 594MHz and is out of range.
 */
#define SII9134_MIN_TMDSCLOCK		(25 * 1000)
#define SII9134_MAX_TMDSCLOCK		(297 * 1000)
/* DVI is the HDMI 1.0-era single link: no TMDS clock above 165MHz */
#define SII9134_MAX_DVI_CLOCK		(165 * 1000)

#define SII9134_MAX_AUDIO_CHANNELS	8

static inline struct sii9134 *
bridge_to_sii9134(struct drm_bridge *bridge)
{
	return container_of(bridge, struct sii9134, bridge);
}

/* ------------------------------------------------------------------ */
/* Low-level helpers                                                    */
/* ------------------------------------------------------------------ */

static int sii9134_modify(struct sii9134 *sii, struct regmap *map,
			  unsigned int reg, unsigned int mask,
			  unsigned int val)
{
	unsigned int tmp;
	int ret;

	ret = regmap_read(map, reg, &tmp);
	if (ret)
		return ret;

	tmp &= ~mask;
	tmp |= val;

	return regmap_write(map, reg, tmp);
}

static int sii9134_write_word(struct sii9134 *sii, struct regmap *map,
			      unsigned int reg, unsigned int val)
{
	int ret;

	ret = regmap_write(map, reg, val & 0xff);
	if (ret)
		return ret;

	return regmap_write(map, reg + 4, (val >> 8) & 0xff);
}

static int sii9134_read_word(struct sii9134 *sii, struct regmap *map,
			     unsigned int reg, unsigned int *val)
{
	unsigned int lo, hi;
	int ret;

	ret = regmap_read(map, reg, &lo);
	if (ret)
		return ret;

	ret = regmap_read(map, reg + 4, &hi);
	if (ret)
		return ret;

	*val = (lo & 0xff) | ((hi & 0xff) << 8);

	return 0;
}

static int sii9134_write_block(struct sii9134 *sii, struct regmap *map,
			       unsigned int reg, const u8 *buf, size_t len)
{
	int ret = 0;
	size_t i;

	for (i = 0; i < len && !ret; i++)
		ret = regmap_write(map, reg + 4 * i, buf[i]);

	return ret;
}


/* ------------------------------------------------------------------ */
/* Infoframes                                                           */
/* ------------------------------------------------------------------ */

static int sii9134_wait_buff_ready(struct sii9134 *sii, u16 ctrl_bits)
{
	u16 rpt_mask = (ctrl_bits >> 1) & ctrl_bits;
	u16 en_mask = (ctrl_bits << 1) & ctrl_bits;
	unsigned int regval;
	int i, ret;

	ret = regmap_read(sii->map1, INF_CTRL1, &regval);
	if (ret)
		return ret;

	if (regval & ctrl_bits) {
		if (regval & rpt_mask)
			sii9134_modify(sii, sii->map1, INF_CTRL1,
				       rpt_mask, 0);

		for (i = 0; i < IFRAME_POLL_MAX; i++) {
			if (regmap_read(sii->map1, INF_CTRL1, &regval))
				return -EIO;
			if (!(regval & en_mask))
				break;
			mdelay(1);
		}
		if (i == IFRAME_POLL_MAX) {
			/*
			 * The repeat bit can stay set even though the buffer is
			 * free; force it through instead of failing the
			 * modeset.
			 */
			dev_warn(sii->dev,
				 "infoframe repeat bit stuck, forcing buffer ready\n");
		}
	}

	return 0;
}

/*
 * Write an infoframe into the hardware packet memory and enable its
 * transmission. @data holds the packed infoframe as produced by
 * hdmi_*_infoframe_pack(): type, version, length, checksum, payload.
 */
static int sii9134_send_infoframe(struct sii9134 *sii, unsigned int dest,
				  u16 ctrl_bits, const u8 *data, u8 length)
{
	int ret;

	ret = sii9134_wait_buff_ready(sii, ctrl_bits);
	if (ret) {
		dev_err(sii->dev, "infoframe buffer not ready\n");
		return ret;
	}

	ret = sii9134_write_block(sii, sii->map1, dest, data, length + 4);
	if (ret)
		return ret;

	return sii9134_modify(sii, sii->map1, INF_CTRL1, ctrl_bits,
			      ctrl_bits);
}

/*
 * Tell the sink that valid AV data is being sent: a GCP with AVMUTE cleared,
 * repeated. A receiver stays black -- clock, sync and DE window all correct --
 * as long as it was last told AVMUTE, which is a state a restarted pipeline can
 * be left in.
 */
static void sii9134_clear_avmute(struct sii9134 *sii)
{
	unsigned int val;
	int i;

	/* stop the repeating GCP before changing the payload */
	sii9134_modify(sii, sii->map1, INF_CTRL2,
		       BIT_CP_ENABLE | BIT_CP_REPEAT, 0);
	for (i = 0; i < 64; i++) {
		if (regmap_read(sii->map1, INF_CTRL2, &val))
			return;
		if (!(val & BIT_CP_ENABLE))
			break;
		usleep_range(1000, 2000);
	}

	regmap_write(sii->map1, CP_IF_ADDR, BIT_CP_AVI_MUTE_CLEAR);
	sii9134_modify(sii, sii->map1, INF_CTRL2,
		       BIT_CP_ENABLE | BIT_CP_REPEAT,
		       BIT_CP_ENABLE | BIT_CP_REPEAT);
}

static int sii9134_disable_infoframe(struct sii9134 *sii, u16 ctrl_bits)
{
	return sii9134_modify(sii, sii->map1, INF_CTRL1, ctrl_bits, 0);
}

/*
 * The 4K formats have no CEA-861 VIC that a sink can act on, so the link has to
 * say "this is one of the extended 4K formats" in the HDMI-VSDB vendor
 * infoframe. The 4K_VIC ids below are 1 = 3840x2160@30, 2 = @25, 3 = @24, 4 =
 * 4096x2160@24. CEA-861 folds each format's 1000/1001 variant into the same id,
 * and hdmi_vendor_infoframe_pack() emits the packet -- OUI 00:0C:03, video
 * format code 1 (extended resolutions) and the id -- into the MPEG packet slot.
 */
static u8 sii9134_hdmi_4k_vic(u8 cea_vic)
{
	switch (cea_vic) {
	case 95:
		return 1;
	case 94:
		return 2;
	case 93:
		return 3;
	case 111:
		return 4;
	default:
		return 0;
	}
}

static void sii9134_send_vsdb(struct sii9134 *sii, u8 hdmi_vic)
{
	struct hdmi_vendor_infoframe frame;
	u8 buf[HDMI_MAX_INFOFRAME_SIZE];
	int ret;

	if (!hdmi_vic) {
		sii9134_disable_infoframe(sii,
					  BIT_VSDB_ENABLE | BIT_VSDB_REPEAT);
		return;
	}

	hdmi_vendor_infoframe_init(&frame);
	frame.vic = hdmi_vic;

	ret = hdmi_vendor_infoframe_pack(&frame, buf, sizeof(buf));
	if (ret < 0) {
		dev_err(sii->dev, "failed to pack vendor infoframe: %d\n", ret);
		return;
	}

	ret = sii9134_send_infoframe(sii, VSDB_IF_ADDR,
				     BIT_VSDB_ENABLE | BIT_VSDB_REPEAT,
				     buf, ret - 4);
	if (ret)
		dev_err(sii->dev, "failed to send vendor infoframe\n");
}

/*
 * "Encoded Audio Sampling Frequency" table from the SiI9134 datasheet. 4K
 * formats use their own table: the N value is divided into the CTS of the TMDS
 * clock, and 297MHz is a different column of the CTA table than 74.25MHz or
 * 148.5MHz. sii9134_hdmi_4k_vic() picks between the two.
 */
static u32 sii9134_audio_n_value(u8 fs_code, bool fourk)
{
	static const u32 n_val[] = {
		6272,	/* 0: 44.1kHz */
		0,
		6144,	/* 2: 48kHz */
		4096,	/* 3: 32kHz */
		0, 0, 0, 0,
		12544,	/* 8: 88.2kHz */
		0,
		12288,	/* a: 96kHz */
		0,
		25088,	/* c: 176.4kHz */
		0,
		24576,	/* e: 192kHz */
	};
	static const u32 n_val_4k[] = {
		4704,	/* 0: 44.1kHz */
		0,
		5120,	/* 2: 48kHz */
		3072,	/* 3: 32kHz */
		0, 0, 0, 0,
		9408,	/* 8: 88.2kHz */
		0,
		10240,	/* a: 96kHz */
		0,
		18816,	/* c: 176.4kHz */
		0,
		20480,	/* e: 192kHz */
	};

	return fourk ? n_val_4k[fs_code] : n_val[fs_code];
}

static void sii9134_write_n_value(struct sii9134 *sii, u8 fs_code, bool fourk)
{
	unsigned int n_val = sii9134_audio_n_value(fs_code, fourk);

	regmap_write(sii->map1, N_SVAL_ADDR + 0, n_val & 0xff);
	regmap_write(sii->map1, N_SVAL_ADDR + 4, (n_val >> 8) & 0xff);
	regmap_write(sii->map1, N_SVAL_ADDR + 8, (n_val >> 16) & 0xff);
}

/* ------------------------------------------------------------------ */
/* EDID over the hardware master DDC                                    */
/* ------------------------------------------------------------------ */

static int sii9134_ddc_fifo_read(struct sii9134 *sii, u8 len, u8 *buf)
{
	unsigned int count, val;
	int retry, i = 0;

	for (retry = 20 + len; i < len && retry; retry--) {
		regmap_read(sii->map, MDDC_FIFO_CNT_ADDR, &count);
		if (!count) {
			usleep_range(1000, 2000);
			continue;
		}
		if (regmap_read(sii->map, MDDC_FIFO_ADDR, &val))
			return -EIO;
		buf[i++] = val;
	}

	return i == len ? 0 : -EIO;
}

static int sii9134_ddc_wait_done(struct sii9134 *sii, unsigned int want)
{
	unsigned int status, count;
	int i;

	for (i = 0; i < want + 5; i++) {
		usleep_range(1000, 2000);
		regmap_read(sii->map, MDDC_STATUS_ADDR, &status);
		if (status & BIT_MDDC_ST_NO_ACK)
			return -EIO;
		if (status & BIT_MDDC_ST_IN_PROGR)
			continue;
		regmap_read(sii->map, MDDC_FIFO_CNT_ADDR, &count);
		if (count == want)
			return 0;
	}

	return -ETIMEDOUT;
}

/*
 * One master-DDC read transaction. len must fit the hardware FIFO: the
 * engine only drops IN_PROGR once every requested byte is *in* the FIFO,
 * so a request larger than MDDC_FIFO_DEPTH parks with STATUS =
 * IN_PROGR|FIFO_FULL (0x18) and FIFO_CNT stuck at 16. EDID is read in
 * 16-byte chunks for exactly this reason.
 */
static int sii9134_ddc_read(struct sii9134 *sii, u8 segment, u8 offset,
			    u8 *buf, unsigned int len)
{
	u8 header[7] = {
		DDC_EDID_ADDR,
		segment,
		offset,
		len,
		0,			/* byte count MSB */
		0,			/* dummy */
		/* enhanced (segmented) read only when a segment is set */
		segment ? MASTER_CMD_ENH_RD : MASTER_CMD_SEQ_RD,
	};
	unsigned int status, count, cmd;
	int ret;

	/* abort anything pending and clear the DDC FIFO */
	regmap_write(sii->map, MDDC_COMMAND_ADDR, MASTER_CMD_CLEAR_FIFO);

	/* one burst starting at 0xED programs addr/segment/offset/count/cmd */
	ret = sii9134_write_block(sii, sii->map, MDDC_SLAVE_ADDR_REG, header,
				  sizeof(header));
	if (ret)
		return ret;

	ret = sii9134_ddc_wait_done(sii, len);
	if (ret) {
		regmap_read(sii->map, MDDC_STATUS_ADDR, &status);
		regmap_read(sii->map, MDDC_FIFO_CNT_ADDR, &count);
		regmap_read(sii->map, MDDC_COMMAND_ADDR, &cmd);
		dev_warn(sii->dev,
			 "mddc seg%u off0x%02x len%u timeout: status=%02x%s%s cnt=%u cmd=%02x\n",
			 segment, offset, len, status,
			 status & BIT_MDDC_ST_I2C_LOW ? " BUS_LOW" : "",
			 status & BIT_MDDC_ST_FIFO_FULL ? " FIFO_FULL" : "",
			 count, cmd);
		regmap_write(sii->map, MDDC_COMMAND_ADDR, MASTER_CMD_ABORT);
		regmap_write(sii->map, MDDC_COMMAND_ADDR, MASTER_CMD_CLOCK);
		return ret;
	}

	return sii9134_ddc_fifo_read(sii, len, buf);
}

/*
 * The master DDC engine is a reduced I2C master: it only performs the
 * transaction shape the DRM DDC reader uses -- the optional segment pointer
 * write (0x30), the in-segment offset write (0x50), then one read from 0x50.
 * Exposing it as an i2c_adapter, like inno-hdmi.c and rk3066_hdmi.c do for
 * their own DDC hardware, is what lets the EDID come from drm_edid_read_ddc();
 * the bus then lives and dies with this device instead of depending on whether
 * a DT-referenced master happened to have probed before we did.
 */
static int sii9134_ddc_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			    int num)
{
	struct sii9134 *sii = i2c_get_adapdata(adap);
	struct i2c_msg *read = NULL;
	unsigned int segment = 0, offset = 0, done;
	int i, ret = 0;

	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_RD) {
			if (read || i != num - 1)
				return -EOPNOTSUPP;
			read = &msgs[i];
			continue;
		}

		if (read || msgs[i].len != 1)
			return -EOPNOTSUPP;

		if (msgs[i].addr == DDC_SEGMENT_ADDR)
			segment = msgs[i].buf[0];
		else if (msgs[i].addr == DDC_EDID_ADDR >> 1)
			offset = msgs[i].buf[0];
		else
			return -EOPNOTSUPP;
	}

	if (!read)
		return -EOPNOTSUPP;

	mutex_lock(&sii->lock);

	/* SCL = oscclk / (delay * 35); 75 kHz */
	regmap_write(sii->map, MDDC_DELAY_CNT, MDDC_DELAY_75KHZ);

	for (done = 0; done < read->len; ) {
		/* one transaction is capped by the engine's FIFO depth */
		unsigned int len = min_t(unsigned int, read->len - done,
					 MDDC_FIFO_DEPTH);

		ret = sii9134_ddc_read(sii, segment, offset + done,
				       read->buf + done, len);
		if (ret)
			break;

		done += len;
	}

	/* leave the segment register as found */
	regmap_write(sii->map, MDDC_SEGMENT_ADDR, 0);

	mutex_unlock(&sii->lock);

	if (ret)
		return ret;

	return num;
}

static u32 sii9134_ddc_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm sii9134_ddc_algorithm = {
	.master_xfer	= sii9134_ddc_xfer,
	.functionality	= sii9134_ddc_func,
};

static int sii9134_ddc_adapter_add(struct sii9134 *sii)
{
	struct i2c_adapter *adap;
	int ret;

	adap = devm_kzalloc(sii->dev, sizeof(*adap), GFP_KERNEL);
	if (!adap)
		return -ENOMEM;

	adap->owner = THIS_MODULE;
	adap->dev.parent = sii->dev;
	/* lets a connector node point ddc-i2c-bus back at this device */
	adap->dev.of_node = sii->dev->of_node;
	adap->algo = &sii9134_ddc_algorithm;
	strscpy(adap->name, "sii9134 DDC", sizeof(adap->name));
	i2c_set_adapdata(adap, sii);

	ret = devm_i2c_add_adapter(sii->dev, adap);
	if (ret)
		return ret;

	sii->ddc = adap;

	return 0;
}

/* ------------------------------------------------------------------ */
/* HPD / detect                                                         */
/* ------------------------------------------------------------------ */

static enum drm_connector_status sii9134_detect_status(struct sii9134 *sii)
{
	unsigned int val;
	int ret;

	ret = regmap_read(sii->map, TX_STAT_ADDR, &val);
	if (ret)
		return connector_status_unknown;

	if (val & BIT_HPD_PIN)
		return connector_status_connected;

	return connector_status_disconnected;
}

/* ------------------------------------------------------------------ */
/* drm_bridge_funcs                                                     */
/* ------------------------------------------------------------------ */

static int sii9134_bridge_attach(struct drm_bridge *bridge,
				 struct drm_encoder *encoder,
				 enum drm_bridge_attach_flags flags)
{
	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR)) {
		pr_err_once("sii9134: DRM_BRIDGE_ATTACH_NO_CONNECTOR is required\n");
		return -EINVAL;
	}

	return 0;
}

static enum drm_mode_status
sii9134_bridge_mode_valid(struct drm_bridge *bridge,
			  const struct drm_display_info *info,
			  const struct drm_display_mode *mode)
{
	unsigned int max_clock = info->is_hdmi ? SII9134_MAX_TMDSCLOCK :
						  SII9134_MAX_DVI_CLOCK;

	if (mode->clock < SII9134_MIN_TMDSCLOCK ||
	    mode->clock > max_clock)
		return MODE_CLOCK_RANGE;

	return MODE_OK;
}

static void sii9134_enable_video_output(struct sii9134 *sii, bool enable)
{
	unsigned int val;

	regmap_read(sii->map, DATA_CTRL_ADDR, &val);
	if (enable)
		regmap_write(sii->map, DATA_CTRL_ADDR,
			     val & ~BIT_VID_BLANK);
	else
		regmap_write(sii->map, DATA_CTRL_ADDR,
			     val | BIT_VID_BLANK);
}

/*
 * The ID bytes are ROM constants, so they are the only proof that the flattened
 * MMIO window really ends at this SiI9134; the rest are the power-down/wake
 * bits and the sync measurement counters.
 */
static void sii9134_dump_regs(struct sii9134 *sii, const char *when)
{
	unsigned int vendor, dev_id, rev, ctrl1, stat, dpd, hres, vres;

	sii9134_read_word(sii, sii->map, TX_VENDOR_ID_ADDR, &vendor);
	sii9134_read_word(sii, sii->map, TX_DEVICE_ID_ADDR, &dev_id);
	regmap_read(sii->map, TX_DEVICE_REV_ADDR, &rev);
	regmap_read(sii->map, TX_SYS_CTRL1_ADDR, &ctrl1);
	regmap_read(sii->map, TX_STAT_ADDR, &stat);
	regmap_read(sii->map1, DIAG_PD_ADDR, &dpd);
	sii9134_read_word(sii, sii->map, MEAS_HRES_ADDR, &hres);
	sii9134_read_word(sii, sii->map, MEAS_VRES_ADDR, &vres);

	dev_dbg(sii->dev,
		"%s: id=%04x:%04x rev=%02x ctrl1=%02x stat=%02x dpd=%02x hres=%u vres=%u\n",
		when, vendor, dev_id, rev, ctrl1, stat, dpd,
		hres & 0x1fff, vres & 0x7ff);
}

static void sii9134_hw_init(struct sii9134 *sii)
{
	/* Basic bring-up; clocks and resets are handled by the wrapper. */
	/*
	 * SiI-PR-0039-F p.123 Table 17: PDTOT#=0 powers down the whole chip,
	 * PDIDCK#=0 blocks all IDCK-based logic. With any of the four
	 * power-down bits asserted only part of the register file accepts
	 * writes (Table 18, and the infoframe EN bits on p.51 additionally
	 * require a live IDCK). Only bits 3:0 are ours to touch; bit 3 reads 1
	 * at reset and is left as found.
	 */
	sii9134_modify(sii, sii->map1, DIAG_PD_ADDR,
		       BIT_PDIDCK | BIT_PDOSC | BIT_PDTOT,
		       BIT_PDIDCK | BIT_PDOSC | BIT_PDTOT);

	/*
	 * Disable the INT output: it is level and stays asserted while the sink
	 * holds HPD, so an interrupt-driven connector would storm. The DRM core
	 * polls the detect callback instead.
	 */
	regmap_write(sii->map, INT_CNTRL_ADDR, 0x00);

	/* master DDC bus timing */
	regmap_write(sii->map, MDDC_DELAY_CNT, MDDC_DELAY_75KHZ);

	/*
	 * The encoder sits either in the SiI controller or in the PHY; the glue
	 * says which, because clearing TEST_TX.DVI_ENC_BYPASS in the wrong
	 * place takes the link down.
	 */
	if (sii->phy->dvi_enc_in_ctrl)
		sii9134_modify(sii, sii->map1, TEST_TX_ADDR,
			       BIT_DVI_ENC_BYPASS, 0);

	/* 24-bit input bus, power up */
	sii9134_modify(sii, sii->map, TX_SYS_CTRL1_ADDR,
		       BIT_TX_PD | BIT_BSEL24BITS,
		       BIT_TX_PD | BIT_BSEL24BITS);

	/* internal MCLK generator + N/CTS packet generation */
	regmap_write(sii->map1, ACR_CTRL_ADDR,
		     ACR_CTRL_N_CTS_ENABLE | ACR_CTRL_INTERNAL_MCLK);
	regmap_write(sii->map1, FREQ_SVAL_ADDR, 0x01);
	/* default N for 48kHz; recomputed on audio hw_params */
	regmap_write(sii->map1, N_SVAL_ADDR + 0, 6144 & 0xff);
	regmap_write(sii->map1, N_SVAL_ADDR + 4, (6144 >> 8) & 0xff);
	regmap_write(sii->map1, N_SVAL_ADDR + 8, (6144 >> 16) & 0xff);

	/* audio/video output: keep blanked and muted until enabled */
	regmap_update_bits(sii->map, DATA_CTRL_ADDR,
			   BIT_VID_BLANK | BIT_AUD_MUTE,
			   BIT_VID_BLANK | BIT_AUD_MUTE);

	/*
	 * enable the HPD interrupt only: RSEN and the audio error sources
	 * re-latch continuously while no TMDS input clock is running
	 */
	regmap_update_bits(sii->map, HDMI_INT_MASK_ADDR,
			   BIT_INT_HOT_PLUG, BIT_INT_HOT_PLUG);
	/* write 1 to clear everything latched while we were coming up */
	regmap_write(sii->map, HDMI_INT1_ADDR, 0xff);
}

static void
sii9134_bridge_atomic_enable(struct drm_bridge *bridge,
			     struct drm_atomic_commit *state)
{
	struct sii9134 *sii = bridge_to_sii9134(bridge);
	struct drm_connector *connector;
	struct drm_connector_state *conn_state;
	struct drm_crtc_state *crtc_state;
	const struct drm_display_mode *mode;
	struct hdmi_avi_infoframe frame;
	const unsigned int acen_conv = TX_VID_ACEN_RGB2YCBCR |
				       TX_VID_ACEN_DWN_SAMPLE |
				       TX_VID_ACEN_CLIP_CS;
	u8 vic, hdmi_vic;
	u8 buf[17];
	unsigned int val;
	bool hdmi_sink, interlaced;
	int i;
	int ret;

	connector = drm_atomic_get_new_connector_for_encoder(state,
							     bridge->encoder);
	if (WARN_ON(!connector))
		return;

	conn_state = drm_atomic_get_new_connector_state(state, connector);
	if (WARN_ON(!conn_state))
		return;

	crtc_state = drm_atomic_get_new_crtc_state(state, conn_state->crtc);
	if (WARN_ON(!crtc_state))
		return;

	mode = &crtc_state->adjusted_mode;
	vic = drm_match_cea_mode(mode);
	hdmi_vic = sii9134_hdmi_4k_vic(vic);

	dev_dbg(sii->dev, "enable: enter %dx%d\n",
		mode->hdisplay, mode->vdisplay);

	mutex_lock(&sii->lock);

	/* cache for the hdmi-codec ELD / plug-status callbacks */
	sii->connector = connector;

	/*
	 * What the sink's EDID says it is: a DVI panel gets no HDMI-only
	 * packets and no YCbCr on the link. The DVI recipe is
	 * AUDP_TXCTRL.hdmi_mode cleared, the AVI and audio infoframes off, and
	 * the link output forced to RGB444.
	 */
	hdmi_sink = connector->display_info.is_hdmi;
	sii->hdmi_sink = hdmi_sink;

	/*
	 * The TMDS clock class decides the audio N value (4K formats use their
	 * own table), and the mode may change while a stream is up, so record
	 * it here for whichever side comes asking last.
	 */
	sii->video_4k = hdmi_vic != 0;

	/*
	 * Reset first, program everything afterwards: the soft reset puts the
	 * TPI register file back to its reset defaults (TX_SYS_CTRL1 reads 0x00
	 * after the pulse), so a DE generator enable written before it is
	 * wiped -- with DE off the TX waits for an input DE the DHD HDMI bus
	 * never sends, and no picture reaches the sink.
	 */

	/*
	 * wait for the input pixel clock before resetting (50 ms timeout, reset
	 * anyway)
	 */
	for (i = 0; i < 50; i++) {
		regmap_read(sii->map, TX_STAT_ADDR, &val);
		if (val & BIT_HDMI_MODE)
			break;
		usleep_range(1000, 2000);
	}

	/* software reset of the TMDS/AV state machines */
	regmap_write(sii->map, TX_SWRST_ADDR,
		     BIT_TX_SW_RST | BIT_TX_FIFO_RST);
	usleep_range(1000, 2000);
	regmap_write(sii->map, TX_SWRST_ADDR, 0);
	usleep_range(1000, 2000);

	/*
	 * The soft reset clears TX_SYS_CTRL1 to its powered-down default; wake
	 * the TX again. The TMDS PLL and the HPD/TPI status bits live behind
	 * this power switch.
	 */
	sii9134_modify(sii, sii->map, TX_SYS_CTRL1_ADDR,
		       BIT_TX_PD | BIT_BSEL24BITS,
		       BIT_TX_PD | BIT_BSEL24BITS);

	/* HDMI mode on the link; a DVI sink stays in the encoder's DVI mode */
	sii9134_modify(sii, sii->map1, AUDP_TXCTRL_ADDR, BIT_TXHDMI_MODE,
		       hdmi_sink ? BIT_TXHDMI_MODE : 0);

	/*
	 * Video input: the VDP puts RGB on the HDMI pins (the VDP driver sets
	 * DHD0_CTRL.hdmi_mode and DATA_SEL.hdmi_data_sel to that for every
	 * format), so this chip makes the link stream: RGB2YCbCr plus the 4:4:4
	 * -> 4:2:2 down-sampler, with CLIP_CS_ID declaring the link YCbCr.
	 * Feeding YCbCr in and passing it straight through does not work with
	 * the interface as the VDP drives it, so every mode goes as RGB.
	 *
	 * The demux/422to444/CSC bits of TX_VID_MODE belong to the opposite
	 * (YCbCr in, RGB out) direction and must stay clear: with DeMux set the
	 * chroma collapses to one value and red and blue both read back as
	 * green.
	 */
	/*
	 * Input bus width: the VDP's HDMI bus carries 12 bits per channel, so
	 * VID_IN_MODE[2:1] is programmed for a 12-bit input. The soft reset
	 * above clears the field back to its default, and with the wrong width
	 * the sink keeps sync but no pixel data arrives.
	 *
	 * Bits [7:6] of the same register are the wide-bus code (00 = 24 bits,
	 * 01 = 30, 10 = 36) and stay 00: only the deep-colour path writes them
	 * non-zero, so the link runs 8 bits per channel end to end.
	 */
	regmap_update_bits(sii->map, TX_VID_IN_MODE_ADDR, 0x06, 0x06);

	/*
	 * With dithering on, the dither block is programmed to 0x0e
	 * (D_RC_EN | D_GC_EN | D_BC_EN) for the 24-bit case. The formats that
	 * leave the block and its enable alone are the analog SD pixel-repeat
	 * ones, which need a 27MHz row clock this link does not generate, so
	 * every mode accepted here is in the 0x0e class.
	 */
	regmap_write(sii->map, TX_VID_MODE_ADDR, TX_VID_MODE_DITHER);
	regmap_write(sii->map, TX_VID_DITHER_ADDR, TX_VID_DITHER_24BIT);

	/*
	 * On a DVI sink the RGB the channel puts on the pins goes straight onto
	 * the link: the converter, the down-sampler and the CLIP_CS_ID
	 * declaration all stay clear.
	 */
	sii9134_modify(sii, sii->map, TX_VID_ACEN_ADDR, acen_conv,
		       hdmi_sink ? acen_conv : 0);

	/* BT.709 matrix for HD modes, BT.601 otherwise */
	if (mode->hdisplay > 720 || mode->vdisplay > 576)
		sii9134_modify(sii, sii->map, TX_VID_CTRL_ADDR,
			       TX_VID_CTRL_CSCSEL, TX_VID_CTRL_CSCSEL);
	else
		sii9134_modify(sii, sii->map, TX_VID_CTRL_ADDR,
			       TX_VID_CTRL_CSCSEL, 0);

	/*
	 * Video timing: the DHD HDMI bus carries no DE signal, so the DE
	 * generator stays off (0x72:0x33 keeps its reset default) and the
	 * sync-derived window is programmed instead.
	 */
	/*
	 * IADJUST bit2:0 keep their reset values (DE_ADJ#=1, datasheet p.14):
	 * use a read-modify-write here.
	 */
	sii9134_modify(sii, sii->map, TX656_ADJ_MODE_ADDR, 0, 0);
	/*
	 * In interlaced modes the transmitter counts fields, so the vertical
	 * totals are halved (for 1080i that is vtotal 562 and v front porch 2),
	 * and FIELD2_HSYNC_OFFSET shifts the second field by half a line (1100
	 * of a 2200 pixel line).
	 */
	interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE;
	sii9134_write_word(sii, sii->map, TX656_HTOT_ADDR, mode->htotal);
	sii9134_write_word(sii, sii->map, TX656_VTOT_ADDR,
			   interlaced ? mode->vtotal / 2 : mode->vtotal);
	sii9134_write_word(sii, sii->map, TX656_HBIT2HSYNC_ADDR,
			   mode->hsync_start - mode->hdisplay);
	sii9134_write_word(sii, sii->map, TX656_FIELD2_ADDR,
			   interlaced ? mode->htotal / 2 : 0);
	sii9134_write_word(sii, sii->map, TX656_HLENGTH_ADDR,
			   mode->hsync_end - mode->hsync_start);
	if (interlaced)
		regmap_write(sii->map, TX656_VBIT_ADDR,
			     (mode->vsync_start - mode->vdisplay) / 2);
	else
		regmap_write(sii->map, TX656_VBIT_ADDR,
			     mode->vsync_start - mode->vdisplay);
	regmap_write(sii->map, TX656_VLENGTH_ADDR,
		     mode->vsync_end - mode->vsync_start);

	{
		unsigned int c1, st, de, vim;

		regmap_read(sii->map, TX_SYS_CTRL1_ADDR, &c1);
		regmap_read(sii->map, TX_STAT_ADDR, &st);
		regmap_read(sii->map, DE_CNTRL_ADDR, &de);
		regmap_read(sii->map, TX_VID_IN_MODE_ADDR, &vim);
		dev_dbg(sii->dev,
			"enable: after-config ctrl1=%02x stat=%02x de=%02x vim=%02x\n",
			c1, st, de, vim);
	}

	/*
	 * set the PHY output swing for this TMDS clock rate, then enable the
	 * PHY output (TMDS on)
	 */
	sii->phy->swing(sii, mode->clock);
	sii->phy->power(sii, true);

	/*
	 * Packets are only accepted once TMDS is stable: wait for the TMDS
	 * clock PLL to lock before writing the AVI infoframe.
	 */
	for (i = 0; i < 100; i++) {
		regmap_read(sii->map, TX_STAT_ADDR, &val);
		if (val & BIT_HDMI_MODE)
			break;
		mdelay(1);
	}
	if (i == 100) {
		unsigned int hres, vres;

		sii9134_read_word(sii, sii->map, MEAS_HRES_ADDR, &hres);
		sii9134_read_word(sii, sii->map, MEAS_VRES_ADDR, &vres);
		/* nonzero counters prove IDCK and H/VSYNC reach the encoder */
		dev_warn(sii->dev,
			 "TMDS clock not stable (stat=%02x hres=%u vres=%u), sending AVI infoframe anyway\n",
			 val, hres & 0x1fff, vres & 0x7ff);
	}

	/*
	 * AVI infoframe, plus the HDMI-VSDB vendor packet the 4K formats need.
	 *
	 * The declaration has to match what the link carries: the converter and
	 * down-sampler above make it YCbCr 4:2:2. Declaring RGB instead makes
	 * the sink read the chroma bytes as pixel data.
	 *
	 * A DVI panel gets neither packet.
	 */
	if (hdmi_sink) {
		hdmi_avi_infoframe_init(&frame);
		frame.colorspace = HDMI_COLORSPACE_YUV422;
		frame.ycc_quantization_range = HDMI_YCC_QUANTIZATION_RANGE_LIMITED;
		frame.active_aspect = HDMI_ACTIVE_ASPECT_PICTURE;
		/*
		 * Name the matrix the converter above used: BT.709 for HD,
		 * BT.601 below, the same split as the TX_VID_CTRL CSCSEL write.
		 */
		frame.colorimetry = (mode->hdisplay > 720 || mode->vdisplay > 576) ?
			HDMI_COLORIMETRY_ITU_709 : HDMI_COLORIMETRY_ITU_601;
		frame.scan_mode = HDMI_SCAN_MODE_NONE;
		frame.video_code = vic;

		/*
		 * hdmi_avi_infoframe_pack() emits the CTA-861 version-2,
		 * 13-byte AVI, which is the header this hardware expects for
		 * the same packet.
		 */
		ret = hdmi_avi_infoframe_pack(&frame, buf, sizeof(buf));
		if (ret < 0) {
			dev_err(sii->dev, "failed to pack AVI infoframe: %d\n", ret);
		} else {
			ret = sii9134_send_infoframe(sii, AVI_IF_ADDR,
						     BIT_AVI_ENABLE | BIT_AVI_REPEAT,
						     buf, ret - 4);
			if (ret)
				dev_err(sii->dev, "failed to send AVI infoframe\n");
		}

		sii9134_send_vsdb(sii, hdmi_vic);
	} else {
		sii9134_disable_infoframe(sii, BIT_AVI_ENABLE | BIT_AVI_REPEAT);
		sii9134_disable_infoframe(sii, BIT_VSDB_ENABLE | BIT_VSDB_REPEAT);
	}

	/* re-derive the audio N value for this mode's TMDS clock class */
	if (sii->audio_fs_code >= 0)
		sii9134_write_n_value(sii, sii->audio_fs_code, sii->video_4k);

	/* unblank video */
	sii9134_enable_video_output(sii, true);

	/* release the sink from any AVMUTE left over from before */
	sii9134_clear_avmute(sii);

	sii9134_dump_regs(sii, "enable-done");

	mutex_unlock(&sii->lock);
}

static void
sii9134_bridge_atomic_disable(struct drm_bridge *bridge,
			      struct drm_atomic_commit *state)
{
	struct sii9134 *sii = bridge_to_sii9134(bridge);

	mutex_lock(&sii->lock);

	/*
	 * The cached connector is only valid while the pipeline is up: the DRM
	 * core frees it when the device goes away, which happens on an
	 * aggregate unbind without this device ever disappearing.
	 */
	sii->connector = NULL;

	sii9134_enable_video_output(sii, false);

	/* mute audio and blank video */
	regmap_update_bits(sii->map, DATA_CTRL_ADDR,
			   BIT_VID_BLANK | BIT_AUD_MUTE,
			   BIT_VID_BLANK | BIT_AUD_MUTE);

	/* disable infoframes */
	sii9134_disable_infoframe(sii, BIT_AVI_ENABLE | BIT_AVI_REPEAT);
	sii9134_disable_infoframe(sii, BIT_AUD_ENABLE | BIT_AUD_REPEAT);
	sii9134_disable_infoframe(sii, BIT_VSDB_ENABLE | BIT_VSDB_REPEAT);

	/* TMDS off */
	sii->phy->power(sii, false);

	mutex_unlock(&sii->lock);
}

static enum drm_connector_status
sii9134_bridge_detect(struct drm_bridge *bridge,
		      struct drm_connector *connector)
{
	struct sii9134 *sii = bridge_to_sii9134(bridge);
	enum drm_connector_status status;

	status = sii9134_detect_status(sii);
	/* also the visible trace of the connector poll work */
	dev_dbg(sii->dev, "detect: %s\n",
		status == connector_status_connected ? "connected" :
						       "disconnected");

	return status;
}

static const struct drm_edid *
sii9134_bridge_edid_read(struct drm_bridge *bridge,
			 struct drm_connector *connector)
{
	struct sii9134 *sii = bridge_to_sii9134(bridge);

	/*
	 * Reading through the core, rather than pulling bytes into our own
	 * buffer, is what lets drm.edid_firmware and the connector's debugfs
	 * edid_override stand in for the DDC read, and gives us the core's
	 * per-block validation and retry.
	 */
	return drm_edid_read_ddc(connector, sii->ddc);
}

const struct drm_bridge_funcs sii9134_bridge_funcs = {
	.attach			= sii9134_bridge_attach,
	.mode_valid		= sii9134_bridge_mode_valid,
	.atomic_enable		= sii9134_bridge_atomic_enable,
	.atomic_disable		= sii9134_bridge_atomic_disable,
	.detect			= sii9134_bridge_detect,
	.edid_read		= sii9134_bridge_edid_read,
	.atomic_duplicate_state	= drm_atomic_helper_bridge_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_bridge_destroy_state,
	.atomic_create_state	= drm_atomic_helper_bridge_create_state,
};

/* ------------------------------------------------------------------ */
/* Audio: hdmi-audio-codec platform device                              */
/* ------------------------------------------------------------------ */

/*
 * I2S wiring between the AIAO block and the HDMI TX IP is fixed in the
 * SoC (AIAO I2S TX SD0..3 -> HDMI I2S SD0..3, plus WS/SCK/MCLK).
 * The registers of that path are:
 *   0x7A:0x14 AUD_MODE   - enable SD0 (stereo) or SD0..3 (multichannel)
 *   0x7A:0x1D I2S_IN_CTRL- 0x40: sample on rising SCK edge
 *   0x7A:0x24 I2S_IN_LEN - input sample length code
 *   0x7A:0x21..22 CHST   - channel status Fs / length
 *   0x7A:0x01..05 ACR    - N value + N/CTS packet generation
 */
static int sii9134_audio_fs_code(unsigned int rate)
{
	switch (rate) {
	case 32000:
		return 0x03;
	case 44100:
		return 0x00;
	case 48000:
		return 0x02;
	case 88200:
		return 0x08;
	case 96000:
		return 0x0a;
	case 176400:
		return 0x0c;
	case 192000:
		return 0x0e;
	default:
		return -EINVAL;
	}
}

/* input sample length code */
static u8 sii9134_audio_len_code(unsigned int sample_width)
{
	switch (sample_width) {
	case 16:
		return 0x02;
	case 18:
		return 0x04;
	case 20:
		return 0x0a;
	case 22:
		return 0x05;
	case 24:
		return 0x0b;
	default:
		return 0x02;
	}
}

static int sii9134_audio_startup(struct device *dev, void *data)
{
	struct sii9134 *sii = data;

	mutex_lock(&sii->lock);

	/* flush the audio FIFO */
	regmap_write(sii->map, TX_SWRST_ADDR, BIT_TX_FIFO_RST);
	regmap_write(sii->map, TX_SWRST_ADDR, 0);

	/* N/CTS packet generation on */
	sii9134_modify(sii, sii->map1, ACR_CTRL_ADDR,
		       ACR_CTRL_N_CTS_ENABLE | ACR_CTRL_INTERNAL_MCLK,
		       ACR_CTRL_N_CTS_ENABLE | ACR_CTRL_INTERNAL_MCLK);

	mutex_unlock(&sii->lock);

	return 0;
}

static void sii9134_audio_shutdown(struct device *dev, void *data)
{
	struct sii9134 *sii = data;

	mutex_lock(&sii->lock);

	/* mute audio output */
	sii9134_modify(sii, sii->map, DATA_CTRL_ADDR, BIT_AUD_MUTE,
		       BIT_AUD_MUTE);

	/* disable I2S input */
	sii9134_modify(sii, sii->map1, AUD_MODE_ADDR, BIT_AUDIO_EN, 0);
	sii9134_disable_infoframe(sii, BIT_AUD_ENABLE | BIT_AUD_REPEAT);

	/* flush the audio FIFO */
	regmap_write(sii->map, TX_SWRST_ADDR, BIT_TX_FIFO_RST);
	regmap_write(sii->map, TX_SWRST_ADDR, 0);

	mutex_unlock(&sii->lock);
}

static int sii9134_audio_hw_params(struct device *dev, void *data,
				   struct hdmi_codec_daifmt *fmt,
				   struct hdmi_codec_params *hparms)
{
	struct sii9134 *sii = data;
	struct hdmi_audio_infoframe frame;
	u8 buf[14];
	u8 len_code;
	u8 ctrl_bits;
	unsigned int val;
	int fs_code;
	int ret;

	fs_code = sii9134_audio_fs_code(hparms->sample_rate);
	if (fs_code < 0) {
		dev_err(dev, "unsupported sample rate %d\n",
			hparms->sample_rate);
		return fs_code;
	}

	if (hparms->channels < 1 ||
	    hparms->channels > SII9134_MAX_AUDIO_CHANNELS) {
		dev_err(dev, "unsupported channel count %d\n",
			hparms->channels);
		return -EINVAL;
	}

	if (fmt->fmt != HDMI_I2S) {
		dev_err(dev, "unsupported DAI format %d\n", fmt->fmt);
		return -EINVAL;
	}

	len_code = sii9134_audio_len_code(hparms->sample_width);

	mutex_lock(&sii->lock);

	/*
	 * A DVI sink has no audio on the link: stay muted and announce no audio
	 * packet.
	 */
	if (!sii->hdmi_sink) {
		ret = 0;
		goto out;
	}

	/* N value for the current Fs and the video format on the link */
	sii->audio_fs_code = fs_code;
	sii9134_write_n_value(sii, fs_code, sii->video_4k);

	/* channel status: PCM, Fs + length */
	regmap_write(sii->map1, I2S_CHST0_ADDR, 0x00);
	regmap_write(sii->map1, I2S_CHST1_ADDR, 0x00);
	regmap_write(sii->map1, I2S_CHST2_ADDR, 0x00);
	regmap_write(sii->map1, I2S_CHST4_ADDR, fs_code);
	regmap_write(sii->map1, I2S_CHST5_ADDR,
		     len_code | ((fs_code & 0x0f) << 4));

	/* I2S input: sample on rising SCK, uncompressed */
	regmap_write(sii->map1, I2S_IN_CTRL_ADDR, I2S_SCK_RISING);
	regmap_write(sii->map1, I2S_IN_LEN, len_code);

	/* I2S input mapping: SD0 only for stereo, SD0..3 for >2 ch */
	ctrl_bits = BIT_AUDIO_EN;
	regmap_read(sii->map1, AUD_MODE_ADDR, &val);
	val &= ~(BIT_SPDIF_SELECT | BIT_DSD_SELECT | SD_0_3_EN);
	if (hparms->channels > 2) {
		val |= SD_0_3_EN;
		ctrl_bits |= BIT_LAYOUT1;
	} else {
		val |= SD_0;
	}
	regmap_write(sii->map1, AUD_MODE_ADDR, val);
	regmap_update_bits(sii->map1, AUDP_TXCTRL_ADDR, BIT_LAYOUT1,
			   ctrl_bits & BIT_LAYOUT1);

	usleep_range(1000, 2000);

	/*
	 * Audio infoframe: start from the frame hdmi-codec already filled in
	 * (hparms->cea carries the channel allocation etc., cf. sii902x.c) and
	 * only adjust what the hardware needs.
	 */
	frame = hparms->cea;

	ret = hdmi_audio_infoframe_pack(&frame, buf, sizeof(buf));
	if (ret < 0) {
		dev_err(dev, "failed to pack audio infoframe: %d\n", ret);
		goto out;
	}

	ret = sii9134_send_infoframe(sii, AUD_IF_ADDR,
				     BIT_AUD_ENABLE | BIT_AUD_REPEAT,
				     buf, ret - 4);
	if (ret) {
		dev_err(dev, "failed to send audio infoframe\n");
		goto out;
	}

	/* unmute */
	sii9134_modify(sii, sii->map1, AUD_MODE_ADDR, BIT_AUDIO_EN,
		       BIT_AUDIO_EN);
	sii9134_modify(sii, sii->map, DATA_CTRL_ADDR, BIT_AUD_MUTE, 0);

out:
	mutex_unlock(&sii->lock);
	return ret;
}

static int sii9134_audio_mute(struct device *dev, void *data, bool mute,
			      int direction)
{
	struct sii9134 *sii = data;

	mutex_lock(&sii->lock);
	sii9134_modify(sii, sii->map, DATA_CTRL_ADDR, BIT_AUD_MUTE,
		       mute ? BIT_AUD_MUTE : 0);
	mutex_unlock(&sii->lock);

	return 0;
}

static int sii9134_audio_get_eld(struct device *dev, void *data,
				 u8 *buf, size_t len)
{
	struct sii9134 *sii = data;
	struct drm_connector *connector;

	/*
	 * The display/DRM-master side creates the connector via
	 * drm_bridge_connector_init(); we cache it while the pipeline is
	 * enabled and copy the ELD the DRM core computed from the EDID. A
	 * zeroed buffer means "no ELD available", which the ASoC side treats as
	 * "sink unknown" and keeps the audio path functional.
	 */
	mutex_lock(&sii->lock);
	connector = sii->connector;
	mutex_unlock(&sii->lock);

	memset(buf, 0, len);
	if (connector && connector->eld[0]) {
		len = min(len, sizeof(connector->eld));
		memcpy(buf, connector->eld, len);
	}

	return 0;
}

static int sii9134_audio_hook_plugged_cb(struct device *dev, void *data,
					 hdmi_codec_plugged_cb fn,
					 struct device *codec_dev)
{
	struct sii9134 *sii = data;
	bool plugged;

	mutex_lock(&sii->lock);
	sii->plugged_cb = fn;
	sii->codec_dev = codec_dev;
	plugged = sii->connector &&
		  sii->connector->status == connector_status_connected;
	mutex_unlock(&sii->lock);

	/* report the current status, like dw-hdmi does */
	if (fn)
		fn(codec_dev, plugged);

	return 0;
}

static const struct hdmi_codec_ops sii9134_audio_codec_ops = {
	.audio_startup		= sii9134_audio_startup,
	.hw_params		= sii9134_audio_hw_params,
	.audio_shutdown		= sii9134_audio_shutdown,
	.mute_stream		= sii9134_audio_mute,
	.get_eld		= sii9134_audio_get_eld,
	.hook_plugged_cb	= sii9134_audio_hook_plugged_cb,
};

static int sii9134_audio_codec_init(struct sii9134 *sii, struct device *dev)
{
	struct hdmi_codec_pdata codec_data = {
		.ops = &sii9134_audio_codec_ops,
		.i2s = 1,
		.spdif = 0,
		.max_i2s_channels = SII9134_MAX_AUDIO_CHANNELS,
		.no_capture_mute = 1,
		.data = sii,
	};

	if (!of_property_present(dev->of_node, "#sound-dai-cells")) {
		dev_dbg(dev, "no #sound-dai-cells, audio disabled\n");
		return 0;
	}

	sii->audio_pdev = platform_device_register_data(dev,
							HDMI_CODEC_DRV_NAME,
							PLATFORM_DEVID_AUTO,
							&codec_data,
							sizeof(codec_data));
	if (IS_ERR(sii->audio_pdev))
		return PTR_ERR(sii->audio_pdev);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Probe / remove                                                       */
/* ------------------------------------------------------------------ */

int sii9134_probe(struct sii9134 *sii9134, int irq)
{
	struct device *dev = sii9134->dev;
	int ret;

	if (IS_ERR_OR_NULL(sii9134->map) || IS_ERR_OR_NULL(sii9134->map1) ||
	    IS_ERR_OR_NULL(sii9134->map_phy) || !sii9134->phy)
		return -EINVAL;

	mutex_init(&sii9134->lock);
	sii9134->irq = irq;
	sii9134->audio_fs_code = -1;
	sii9134->hdmi_sink = true;

	/* the transmitter's own DDC master, exposed as an I2C bus */
	ret = sii9134_ddc_adapter_add(sii9134);
	if (ret) {
		dev_err_probe(dev, ret, "failed to add DDC adapter\n");
		return ret;
	}

	sii9134_dump_regs(sii9134, "before-init");
	/*
	 * The PHY is configured while the controller is still held in reset,
	 * and the controller's own bring-up only starts once it is released.
	 */
	if (sii9134->reset_hold)
		sii9134->reset_hold(sii9134);
	sii9134->phy->init(sii9134);
	if (sii9134->reset_release)
		sii9134->reset_release(sii9134);
	sii9134_hw_init(sii9134);
	sii9134_dump_regs(sii9134, "after-init");

	sii9134->bridge.of_node = dev->of_node;
	sii9134->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID;
	/*
	 * drm_bridge_connector_init() rejects chains whose last bridge has no
	 * connector type
	 */
	sii9134->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	/*
	 * drm_bridge_connector_init() drops every interlaced mode unless each
	 * bridge in the chain declares the capability, and the TPI does count
	 * fields itself (see the FIELD2_HSYNC_OFFSET write).
	 */
	sii9134->bridge.interlace_allowed = true;

	ret = sii9134_audio_codec_init(sii9134, dev);
	if (ret) {
		dev_err_probe(dev, ret,
			      "failed to register audio codec\n");
		return ret;
	}

	return 0;
}

/*
 * The bridge becomes visible to DRM when the aggregate that owns the pipeline
 * binds, and disappears with it: while the master is bound the bridge may be
 * attached to, and while it is not, nothing may reach it. Registering from
 * probe instead would keep a bridge that no DRM device owns on the global list
 * for the whole lifetime of the platform device.
 */
void sii9134_bridge_add(struct sii9134 *sii9134)
{
	drm_bridge_add(&sii9134->bridge);
}

void sii9134_bridge_remove(struct sii9134 *sii9134)
{
	drm_bridge_remove(&sii9134->bridge);
}

void sii9134_remove(struct sii9134 *sii9134)
{
	/*
	 * sii9134_bridge_remove() has already run from the component unbind,
	 * and the DDC adapter goes with devm; the audio codec pdev must go away
	 * first so no audio callback can reach us while the bridge is being
	 * torn down.
	 */
	if (sii9134->audio_pdev)
		platform_device_unregister(sii9134->audio_pdev);

	sii9134->phy->power(sii9134, false);
}
