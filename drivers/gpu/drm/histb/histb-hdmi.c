// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Hi3798MV200 HDMI transmitter
 *
 * The on-chip HDMI 2.0 transmitter of the Hi3798MV200 family: it takes the
 * pixel stream of a VDP HD channel and turns it into a TMDS link. It is a
 * drm_bridge, the node the VDP's of_graph port@0 points at; the connector
 * belongs to the bridge connector the VDP master builds over the chain, so
 * this driver creates none of its own.
 *
 * The transmitter is two register windows -- the digital blocks and the PHY --
 * plus two divider fields and the resets inside the SoC clock generator, which
 * this driver reaches through that node's syscon regmap.
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/mutex.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_edid.h>
#include <drm/drm_print.h>
#include <sound/hdmi-codec.h>

/*
 * Digital window. Offsets and bit layouts are the ones in the SDK's register
 * file for this part, transcribed into docs/mv200h-hdmitx-regmap-2026-10-22;
 * the reset value of TX_PWD_RST_CTRL, where only the HDCP 2.2 MCU request bit
 * is set, was read back from a board running the vendor's own system.
 */
#define TX_PWD_RST_CTRL		0x0010
#define  TX_PWD_SRST		BIT(0)
#define  TX_SYS_SRST		BIT(1)
#define  TX_VID_SRST		BIT(2)
#define  TX_HDMI_SRST		BIT(3)
#define  TX_HDCP1X_SRST		BIT(4)
#define  TX_PHY_SRST		BIT(5)
#define  TX_AUD_SRST		BIT(6)
#define  TX_ACR_SRST		BIT(7)
#define  TX_AFIFO_SRST		BIT(8)
#define  TX_HDCP2X_SRST		BIT(9)
#define  TX_MCU_SRST		BIT(10)
/* HDCP is not implemented here, so those two request bits stay set. */
#define  TX_PWD_HELD		(TX_HDCP2X_SRST | TX_MCU_SRST)

#define VIDEO_DMUX_CTRL		0x09a8
/*
 * Video path (TX + 0x800) -- hal/ctrl/hisiv100/regs/hdmi_reg_video_path.h.
 * The two halves of this block decide the question the panel cannot answer for
 * us on this board: VIDEO_PATH_CTRL bit 0 blanks the outgoing video (so a set
 * bit means black even with a live source), and the FORMAT DETECTOR measures the
 * timing that actually arrives from the VDP -- hsync_active_cnt [12:0] and
 * hsync_total_cnt [25:13] at +0x8c0, the vertical pair at +0x8c4, flags
 * interlaced[0]/hsync[1]/vsync[2] at +0x8bc.  FDET_STATUS is latched: the
 * vendor re-arms it by clearing bit 3 of +0x8b8 and raising it again
 * (hdmi_hal_ctrl.c:1563-1564), which is what histb_hdmi_fdet_dump() does.
 */
#define TIMING_GEN_CTRL		0x0800
#define   TG_GEN_EN			BIT(0)
#define   TG_EXTMODE			BIT(1)
#define   TG_TIMING_SEL			GENMASK(7, 2)
#define VIDEO_PATH_CTRL		0x0830
#define   VIDEO_BLANK_EN		BIT(0)
#define FORMAT_DET_CONFIG	0x08b8
#define   FDET_SYNC_FORCE		BIT(0)
#define   FDET_HSYNC_POL_VAL		BIT(1)
#define   FDET_VSYNC_POL_VAL		BIT(2)
#define   FDET_STATUS_CLEAR		BIT(3)
#define   FDET_PIXEL_CNT_THREHOLD	GENMASK(7, 4)
#define FDET_STATUS			0x08bc
#define   FDET_INTERLACED		BIT(0)
#define FDET_HORI_RES		0x08c0
#define FDET_VERT_RES		0x08c4
#define   FDET_ACTIVE			GENMASK(12, 0)
#define   FDET_TOTAL			GENMASK(25, 13)

#define  DMUX_SYNCMASK_EN	GENMASK(24, 21)
#define  DMUX_INVER_SYNC	GENMASK(28, 25)
#define  DMUX_INVER_V		BIT(0)
#define  DMUX_INVER_H		BIT(1)
#define  DMUX_INVER_DE		BIT(3)

#define AVMIXER_CONFIG		0x1a08
#define  AVMIX_HDMI_MODE	BIT(0)

#define HDMI_ENC_CTRL		0x1a60
#define  ENC_HDMI2_ON		BIT(0)
#define  ENC_BYPASS		BIT(5)
#define  ENC_CK_DIV_SEL		GENMASK(7, 6)

/*
 * PHY window. The power-down fields are active-high in name and active-low in
 * value: the vendor's PHY_POWER_ON is 0, and the values written below are the
 * ones a board running the vendor system at 3840x2160@30 was found holding
 * (PHY_TOP_PD 0x0830 out of a reset value of 0x17ffff), with the de-emphasis,
 * BIST and termination bits left as that system leaves them.
 */
#define PHY_AFIFO_DATA_SEL	0x0050
#define  AFIFO_DATA_CLK_H	GENMASK(18, 9)

#define PHY_AFIFO_CLK		0x0054
#define  AFIFO_DATA_CLK_L	GENMASK(29, 0)

#define PHY_TOP_PD		0x0064
#define  PHY_GC_PD		GENMASK(3, 0)
#define  PHY_GC_PD_BIST		BIT(4)
#define  PHY_GC_PD_DE		GENMASK(8, 5)
#define  PHY_GC_PD_LDO		GENMASK(10, 9)
#define  PHY_GC_PD_RTERM	GENMASK(18, 11)
#define  PHY_GC_PD_RXSENSE	BIT(19)
#define  PHY_GC_TXPLL_PD	BIT(20)
#define  PHY_DIVSEL		GENMASK(22, 21)
#define  PHY_PD_ALL		FIELD_PREP(PHY_GC_PD, 0xf)
#define  PHY_PD_BIST		FIELD_PREP(PHY_GC_PD_BIST, 1)
#define  PHY_PD_DE		FIELD_PREP(PHY_GC_PD_DE, 1)
#define  PHY_PD_LDO		FIELD_PREP(PHY_GC_PD_LDO, 3)
#define  PHY_PD_RTERM		FIELD_PREP(PHY_GC_PD_RTERM, 1)
#define  PHY_PD_RXSENSE		PHY_GC_PD_RXSENSE
#define  PHY_PD_TXPLL		PHY_GC_TXPLL_PD

#define PHY_PLL_LOCK_STATUS	0x0098
#define  PHY_CLK_FB_EXIST	BIT(0)
#define  PHY_CLK_LOCK		BIT(20)

#define PHY_FBSSC_SET		0x00b0
#define  FBSSC_SET_FB		GENMASK(23, 0)
#define  FBSSC_SET_LOAD		BIT(24)
#define  FBSSC_SSC_MODE		BIT(27)
/*
 * The loop divider is written as (multiplier * 4) << 16, which is why the
 * vendor names it after 25. Every 24-bit row of its table uses this one.
 */
#define  FBSSC_LOOP_DIV_25	0x640000

/*
 * Clock generator fields this device programs. Both hold a divider minus one.
 * They are picked from the mode rather than from a rate relation, which is why
 * they are not modelled as clocks (see histb_hdmi_parse_dt()).
 */
#define CRG_HDMI_CTRL		0x10c
#define  CRG_SSC_CLK_DIV	GENMASK(13, 10)
#define CRG_HDMI_PHY		0x110
#define  CRG_PHY_SRST_REQ		BIT(4)
#define  CRG_TMDS_CLK_DIV	GENMASK(8, 6)

/**
 * struct histb_hdmi_phy_cfg - one row of the PHY mode table
 * @min_khz: lowest pixel clock, in kHz, this row serves
 * @ssc_div: value for the clock generator's ssc_clk_div field, divider minus one
 * @tmds_div: value for the clock generator's tmds_clk_div field, divider minus one
 * @divsel: PHY interpolation code: 0 for 1x, 1 for 2x, 2 for 4x, 3 for 8x
 * @clk_l: per-lane pattern for the AFIFO data clock, low 30 bits
 * @clk_h: the same pattern's tenth bit of each lane, in the select register
 * @hdmi2: run the encoder in HDMI 2.0 timing
 *
 * The rows are the vendor's, unchanged: the same fields for the same pixel
 * clock bands. The 200001 kHz row is the one a board was found using at
 * 3840x2160@30 -- ssc_clk_div 9, tmds_clk_div 0, interpolation 0, and both
 * AFIFO pattern fields holding the 1:10 value below -- and 30- and 36-bit
 * modes have their own vendor rows that nothing here can ask for yet, since no
 * part of this driver programs bpc.
 */
struct histb_hdmi_phy_cfg {
	unsigned int min_khz;
	u32 ssc_div;
	u32 tmds_div;
	u32 divsel;
	u32 clk_l;
	u32 clk_h;
	bool hdmi2;
};

/*
 * Each lane field of the AFIFO pattern word is ten bits wide, so a 1:10 ratio
 * is three times 0x1f at strides of ten, not of eight: 0x01f07c1f is what the
 * transmitter holds while it is driving a panel (0xf8d30054, read live).
 */
#define PHY_RATIO_1_10_L	0x01f07c1f
#define PHY_RATIO_1_40_L	0x000fffff

/*
 * One row per pixel-clock band, at 24-bit colour, which is the only depth this
 * pipeline is driven at.  ssc_div is the bypass divider minus one (CRG67's
 * field), tmds_div the TMDS Nx divider minus one (CRG68's field), divsel the
 * interpolation code (0 = 1X), and clk_l/clk_h the lane patterns of the clock
 * ratio.
 *
 * The band below 200 MHz runs 1X with tmds_div 0 because that is what this part
 * was measured to need: the system driving the attached capture card holds
 * CRG68 = 0x00000001 and PHY_TOP_PD divsel = 1X at the board's 74.25 MHz pixel
 * clock (CRG54 = 0x400d0019 in both), while the CV200's table -- which this one
 * was copied from -- interpolates 2X/4X/8X in that range.  Handing this PHY the
 * CV200 values left the sink decoding only some of the frames, so the
 * interpolated rows are this part's absence, not a smaller band.  The HDMI 2.0
 * row is the CV200's and has not been read back here.
 */
static const struct histb_hdmi_phy_cfg histb_hdmi_phy_cfgs[] = {
	{     25000, 9, 0, 0, PHY_RATIO_1_10_L, 0x1f, false, },
	{    200001, 9, 0, 0, PHY_RATIO_1_10_L, 0x1f, false, },
	{    340001, 4, 0, 0, PHY_RATIO_1_40_L, 0x00, true,  },
};

/*
 * DDC: the transmitter's own I2C master, which the SDK names PWD.
 *
 * The master is not a general-purpose one: a transfer is described by a slave
 * address plus a segment pointer plus an offset, and the hardware emits the
 * pointer write and the repeated start itself. That is exactly the shape of an
 * EDID read, so the adapter below answers the two message forms drm_do_get_edid
 * uses -- a one or two byte pointer write followed by a block read -- plus plain
 * writes, and nothing else.
 *
 * Timeouts and the order of the status clears are the vendor's
 * (hal/ctrl/hisiv100/hdmi_hal_ddc.c). Its DDC speed counter in the always-on
 * block is left alone: the vendor never writes it either, and nothing here
 * documents what unit it counts in.
 */
#define PWD_FIFO_RDATA		0x0038
#define  PWD_FIFO_DATA_OUT	GENMASK(7, 0)
#define PWD_FIFO_WDATA		0x003c
#define  PWD_FIFO_DATA_IN	GENMASK(7, 0)
#define PWD_DATA_CNT		0x0040
#define  PWD_FIFO_DATA_CNT	GENMASK(4, 0)
#define  PWD_DATA_OUT_CNT	GENMASK(17, 8)
#define PWD_SLAVE_CFG		0x0044
#define  PWD_SLAVE_ADDR		GENMASK(7, 0)
#define  PWD_SLAVE_OFFSET	GENMASK(15, 8)
#define  PWD_SLAVE_SEG		GENMASK(23, 16)
#define PWD_MST_STATE		0x0048
#define  PWD_NO_ACK		BIT(0)
#define  PWD_BUS_LOW		BIT(1)
#define  PWD_IN_PROG		BIT(2)
#define  PWD_FIFO_EMPTY		BIT(5)
#define PWD_MST_CMD		0x004c
#define  PWD_CMD_READ_MULTI	0x2
#define  PWD_CMD_READ_SEGMENT	0x4
#define  PWD_CMD_WRITE_MULTI	0x6
#define  PWD_CMD_FIFO_CLR	0x9
#define  PWD_CMD_ABORT		0xf
#define  PWD_CMD		GENMASK(3, 0)
#define PWD_MST_CLR		0x0050
#define  PWD_CLR_STATUS		(PWD_BUS_LOW | PWD_NO_ACK)
#define DDC_MST_ARB_CTRL	0x0064
#define  DDC_AUTO_ABORT		BIT(1)
#define DDC_MST_ARB_REQ		0x0068
#define  DDC_REQ		BIT(0)
#define DDC_MST_ARB_ACK		0x006c
#define  DDC_REQ_ACK		BIT(0)
/* the always-on block's DDC status, used to check the bus is idle */
#define AON_DDC_MAN_CTRL	0x405c
#define  DDC_SCL_HIGH		BIT(0)
#define  DDC_SDA_HIGH		BIT(1)

/*
 * Hot plug lives in the always-on block too.  The reported state is inverted
 * when the polarity bit says so; the override bit is left at its reset value
 * of 0, so the pin and not a software value decides what bit 0 reports.
 */
#define AON_HOTPLUG_ST_CFG	0x4020
#define  HPD_OVERRIDE_EN	BIT(1)
#define  HPD_POLARITY_CTL	BIT(2)
#define  HPD_SOFT_VALUE		BIT(3)
#define AON_TX_AON_STATE	0x4028
#define  HPD_STATE		BIT(0)
#define  PHY_RX_SENSE		BIT(1)

#define DDC_EDID_ADDR		0xa0
#define DDC_FIFO_DEPTH		16
#define DDC_TIMEOUT_ARB_MS	100
#define DDC_TIMEOUT_SDA_MS	30
#define DDC_TIMEOUT_INPROG_MS	20
#define DDC_TIMEOUT_DATA_MS	20

struct histb_hdmi_ddc {
	struct i2c_adapter adapter;
	struct mutex lock;
};


#define HISTB_HDMI_MAX_KHZ	600000

struct histb_hdmi {
	struct device *dev;
	void __iomem *regs;
	void __iomem *phy;
	struct regmap *crg;
	struct clk *clk_pix;
	struct clk *clk_tmds;
	struct delayed_work fdet_work;
	int fdet_left;
	struct reset_control *resets;
	struct histb_hdmi_ddc ddc;
	unsigned int audio_tmds_khz;
	struct drm_bridge bridge;
};

static struct histb_hdmi *bridge_to_histb_hdmi(struct drm_bridge *bridge)
{
	return container_of(bridge, struct histb_hdmi, bridge);
}

static u32 histb_hdmi_read(struct histb_hdmi *hdmi, u32 offset)
{
	return readl_relaxed(hdmi->regs + offset);
}

static void histb_hdmi_write(struct histb_hdmi *hdmi, u32 offset, u32 val)
{
	writel_relaxed(val, hdmi->regs + offset);
}

static u32 histb_hdmi_phy_read(struct histb_hdmi *hdmi, u32 offset)
{
	return readl_relaxed(hdmi->phy + offset);
}

static void histb_hdmi_phy_write(struct histb_hdmi *hdmi, u32 offset, u32 val)
{
	writel_relaxed(val, hdmi->phy + offset);
}

static const struct histb_hdmi_phy_cfg *histb_hdmi_phy_cfg(u32 khz)
{
	const struct histb_hdmi_phy_cfg *best = NULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(histb_hdmi_phy_cfgs); i++)
		if (khz >= histb_hdmi_phy_cfgs[i].min_khz)
			best = &histb_hdmi_phy_cfgs[i];

	return best;
}

/*
 * Bring the PHY up for one mode. The order is the vendor's and the board shows
 * what it leaves behind: divider fields first, then a wait, then a pulse of the
 * PHY's soft reset, then the interpolation and pattern fields, then the loop
 * divider with its load strobe.
 *
 * Spreading spectrum is left off: the writes below take the shape of the
 * vendor's disabled branch, whose step and span registers stay at zero, and the
 * enabled branch needs a span and step computed from the mode that nothing here
 * derives yet.
 */
static int histb_hdmi_set_phy(struct histb_hdmi *hdmi,
			       const struct histb_hdmi_phy_cfg *cfg)
{
	u32 val;
	int ret;

	ret = regmap_update_bits(hdmi->crg, CRG_HDMI_CTRL, CRG_SSC_CLK_DIV,
				 FIELD_PREP(CRG_SSC_CLK_DIV, cfg->ssc_div));
	if (ret)
		return ret;

	ret = regmap_update_bits(hdmi->crg, CRG_HDMI_PHY, CRG_TMDS_CLK_DIV,
				 FIELD_PREP(CRG_TMDS_CLK_DIV, cfg->tmds_div));
	if (ret)
		return ret;

	/* The vendor waits after the clock generator before touching the PHY. */
	usleep_range(1000, 1200);

	/*
	 * Then it pulses the PHY's own soft reset, on this part only, so that
	 * what the loop locks to is the new divider. That request bit is a
	 * field of the same register the dividers live in and is written here
	 * rather than handed through the reset framework, because the pulse
	 * belongs to the divider change and to nothing else.
	 */
	ret = regmap_update_bits(hdmi->crg, CRG_HDMI_PHY, CRG_PHY_SRST_REQ,
				 CRG_PHY_SRST_REQ);
	if (ret)
		return ret;
	udelay(10);
	ret = regmap_update_bits(hdmi->crg, CRG_HDMI_PHY, CRG_PHY_SRST_REQ, 0);
	if (ret)
		return ret;
	usleep_range(1000, 1200);

	histb_hdmi_phy_write(hdmi, PHY_TOP_PD,
			     PHY_PD_BIST | PHY_PD_DE | PHY_PD_RTERM |
			     FIELD_PREP(PHY_DIVSEL, cfg->divsel));

	histb_hdmi_phy_write(hdmi, PHY_AFIFO_CLK,
			     FIELD_PREP(AFIFO_DATA_CLK_L, cfg->clk_l));
	val = histb_hdmi_phy_read(hdmi, PHY_AFIFO_DATA_SEL);
	histb_hdmi_phy_write(hdmi, PHY_AFIFO_DATA_SEL,
			     (val & ~AFIFO_DATA_CLK_H) |
			     FIELD_PREP(AFIFO_DATA_CLK_H, cfg->clk_h));

	/*
	 * The loop divider takes a load strobe: the value without it, then with
	 * it, then without again, which is the state the board was found in
	 * (its reset value leaves the strobe set).
	 */
	val = FIELD_PREP(FBSSC_SET_FB, FBSSC_LOOP_DIV_25);
	histb_hdmi_phy_write(hdmi, PHY_FBSSC_SET, val);
	histb_hdmi_phy_write(hdmi, PHY_FBSSC_SET, val | FBSSC_SET_LOAD);
	histb_hdmi_phy_write(hdmi, PHY_FBSSC_SET, val);

	return readl_relaxed_poll_timeout(hdmi->phy + PHY_PLL_LOCK_STATUS, val,
					  (val & PHY_CLK_LOCK) &&
					  (val & PHY_CLK_FB_EXIST), 100, 20000);
}

/*
 * A mode survives only if both halves of the clock chain accept it: the PHY has
 * a divider set for its band, and the display FPLL can synthesise the pixel
 * clock itself.  The FPLL solver only tolerates 0.03 %, which is what rejects a
 * mode like 1024x768@60 reduced blanking (65.12256 MHz has no configuration)
 * here instead of at commit time.
 */
static enum drm_mode_status histb_hdmi_mode_valid(struct drm_bridge *bridge,
						  const struct drm_display_info *info,
						  const struct drm_display_mode *mode)
{
	struct histb_hdmi *hdmi = bridge_to_histb_hdmi(bridge);

	if (!histb_hdmi_phy_cfg(mode->clock))
		return MODE_CLOCK_RANGE;

	if (mode->clock > HISTB_HDMI_MAX_KHZ)
		return MODE_CLOCK_HIGH;

	if (clk_round_rate(hdmi->clk_pix, mode->clock * 1000) < 0)
		return MODE_NOCLOCK;

	return MODE_OK;
}

static int histb_hdmi_atomic_check(struct drm_bridge *bridge,
				   struct drm_bridge_state *bridge_state,
				   struct drm_crtc_state *crtc_state,
				   struct drm_connector_state *conn_state)
{
	const struct drm_display_mode *mode = &crtc_state->mode;

	if (!histb_hdmi_phy_cfg(mode->clock) || mode->clock > HISTB_HDMI_MAX_KHZ)
		return -EINVAL;

	return 0;
}

/*
 * Board test instrument, not for submission: ask the transmitter what timing it
 * actually sees.  Nothing here writes anything but the detector's own re-arm
 * bit, so the reading is the device's measurement of the VDP's output rather
 * than our opinion about it -- which is the only kind of verdict this board can
 * give with no capture card attached.
 */
/*
 * The detector latches, so the reading that matters is taken a few frames after
 * the channel starts -- and never by sleeping in the enable path.  This runs
 * from the bridge's atomic_enable, which must not schedule.  The first version
 * did sleep there, and that boot was the first one where /sbin/init and /bin/sh
 * both came back ETXTBSY ("No working init found"); the link is a hypothesis,
 * not a proven cause, and the next round's whole point is to see whether the
 * panic survives this version.  A delayed work is the legal way to wait either
 * way, and its extra samples tell "measured zero once at start-up" apart from
 * "no timing ever arrives at the transmitter".
 */
static void histb_hdmi_fdet_dump(struct histb_hdmi *hdmi);

static void histb_hdmi_fdet_work(struct work_struct *work)
{
	struct histb_hdmi *hdmi = container_of(to_delayed_work(work),
					       struct histb_hdmi, fdet_work);

	histb_hdmi_fdet_dump(hdmi);
	if (hdmi->fdet_left-- > 0)
		schedule_delayed_work(&hdmi->fdet_work, msecs_to_jiffies(4000));
}

static void histb_hdmi_fdet_dump(struct histb_hdmi *hdmi)
{
	u32 cfg, h, v, status;

	/*
	 * The result words hold the last measured frame and the clear below
	 * empties them, so read before clearing; clearing first made every dump
	 * report 0 no matter what the channel was sending.
	 */
	status = histb_hdmi_read(hdmi, FDET_STATUS);
	h = histb_hdmi_read(hdmi, FDET_HORI_RES);
	v = histb_hdmi_read(hdmi, FDET_VERT_RES);

	dev_info(hdmi->dev,
		 "hdmi-fdet: hactive %u htotal %u vactive %u vtotal %u status %08x\n",
		 (u32)FIELD_GET(FDET_ACTIVE, h), (u32)FIELD_GET(FDET_TOTAL, h),
		 (u32)FIELD_GET(FDET_ACTIVE, v), (u32)FIELD_GET(FDET_TOTAL, v),
		 status);

	/*
	 * Clear and re-measure, the vendor's own sequence
	 * (hdmi_hal_ctrl.c:1563-1564): a detector that refills within a frame is
	 * what says the numbers above were live rather than a stale latch.
	 */
	cfg = histb_hdmi_read(hdmi, FORMAT_DET_CONFIG);
	histb_hdmi_write(hdmi, FORMAT_DET_CONFIG, cfg & ~FDET_STATUS_CLEAR);
	histb_hdmi_write(hdmi, FORMAT_DET_CONFIG, cfg | FDET_STATUS_CLEAR);
	msleep(30);
	h = histb_hdmi_read(hdmi, FDET_HORI_RES);

	dev_info(hdmi->dev, "hdmi-fdet: refilled hactive %u htotal %u\n",
		 (u32)FIELD_GET(FDET_ACTIVE, h), (u32)FIELD_GET(FDET_TOTAL, h));
	dev_info(hdmi->dev, "hdmi-fdet: vgapath %08x timing_gen %08x fdet_cfg %08x\n",
		 histb_hdmi_read(hdmi, VIDEO_PATH_CTRL),
		 histb_hdmi_read(hdmi, TIMING_GEN_CTRL),
		 histb_hdmi_read(hdmi, FORMAT_DET_CONFIG));
}

static void histb_hdmi_atomic_enable(struct drm_bridge *bridge,
				     struct drm_atomic_commit *commit)
{
	struct histb_hdmi *hdmi = bridge_to_histb_hdmi(bridge);
	struct drm_device *drm = bridge->dev;
	const struct drm_display_mode *mode;
	const struct histb_hdmi_phy_cfg *cfg;
	u32 invers;
	int ret;

	mode = &bridge->encoder->crtc->state->adjusted_mode;
	cfg = histb_hdmi_phy_cfg(mode->clock);
	if (!cfg) {
		drm_err(drm, "no HDMI PHY setting for %u kHz\n", mode->clock);
		return;
	}

	/*
	 * The ratio bit that could halve the TMDS clock against the pixel clock
	 * is pinned to 1:1 by the pixel clock's .prepare, since the VDP register
	 * file cannot be reached otherwise, so the mode clock is also the TMDS
	 * clock here -- which is what the audio ACR needs in order to pick N.
	 */
	hdmi->audio_tmds_khz = mode->clock;

	ret = clk_prepare_enable(hdmi->clk_pix);
	if (ret) {
		drm_err(drm, "failed to enable the pixel clock: %d\n", ret);
		return;
	}

	ret = clk_prepare_enable(hdmi->clk_tmds);
	if (ret) {
		drm_err(drm, "failed to enable the TMDS clock: %d\n", ret);
		goto err_pix;
	}

	ret = reset_control_deassert(hdmi->resets);
	if (ret) {
		drm_err(drm, "failed to release the transmitter resets: %d\n", ret);
		goto err_tmds;
	}

	/* The controller's own block resets are released here, not in the
	 * clock generator: everything but HDCP comes out of reset at once.
	 */
	histb_hdmi_write(hdmi, TX_PWD_RST_CTRL, TX_PWD_HELD);

	ret = histb_hdmi_set_phy(hdmi, cfg);
	if (ret) {
		drm_err(drm, "HDMI PHY did not lock for %u kHz: %d\n",
			mode->clock, ret);
		goto err_resets;
	}

	/* Pass the channel's syncs through, inverted as the mode asks. */
	invers = FIELD_PREP(DMUX_INVER_SYNC,
			    (mode->flags & DRM_MODE_FLAG_NVSYNC ? DMUX_INVER_V : 0) |
			    (mode->flags & DRM_MODE_FLAG_NHSYNC ? DMUX_INVER_H : 0));
	histb_hdmi_write(hdmi, VIDEO_DMUX_CTRL,
			 (histb_hdmi_read(hdmi, VIDEO_DMUX_CTRL) &
			  ~(DMUX_INVER_SYNC | DMUX_SYNCMASK_EN)) | invers);

	histb_hdmi_write(hdmi, AVMIXER_CONFIG,
			 histb_hdmi_read(hdmi, AVMIXER_CONFIG) | AVMIX_HDMI_MODE);
	ret = histb_hdmi_read(hdmi, HDMI_ENC_CTRL);
	ret &= ~(ENC_HDMI2_ON | ENC_BYPASS);
	if (cfg->hdmi2)
		ret |= ENC_HDMI2_ON;
	histb_hdmi_write(hdmi, HDMI_ENC_CTRL, ret);

	/* Drive the pins last: from here a sink can find a clock. */
	histb_hdmi_phy_write(hdmi, PHY_TOP_PD,
			     histb_hdmi_phy_read(hdmi, PHY_TOP_PD) & ~PHY_GC_PD);

	hdmi->fdet_left = 2;
	mod_delayed_work(system_wq, &hdmi->fdet_work, 0);

	return;

err_resets:
	reset_control_assert(hdmi->resets);
err_tmds:
	clk_disable_unprepare(hdmi->clk_tmds);
err_pix:
	clk_disable_unprepare(hdmi->clk_pix);
}

static void histb_hdmi_atomic_disable(struct drm_bridge *bridge,
				      struct drm_atomic_commit *commit)
{
	struct histb_hdmi *hdmi = bridge_to_histb_hdmi(bridge);
	u32 val;

	cancel_delayed_work_sync(&hdmi->fdet_work);

	/* Stop driving before the PLL goes down, the vendor's order. */
	val = histb_hdmi_phy_read(hdmi, PHY_TOP_PD);
	histb_hdmi_phy_write(hdmi, PHY_TOP_PD, val | PHY_PD_ALL);

	val |= PHY_PD_TXPLL | PHY_PD_RXSENSE | PHY_PD_LDO;
	histb_hdmi_phy_write(hdmi, PHY_TOP_PD, val);

	histb_hdmi_write(hdmi, TX_PWD_RST_CTRL,
			 histb_hdmi_read(hdmi, TX_PWD_RST_CTRL) |
			 (TX_PWD_SRST | TX_SYS_SRST | TX_VID_SRST | TX_HDMI_SRST |
			  TX_HDCP1X_SRST | TX_PHY_SRST | TX_AUD_SRST | TX_ACR_SRST |
			  TX_AFIFO_SRST));

	reset_control_assert(hdmi->resets);
	clk_disable_unprepare(hdmi->clk_tmds);
	clk_disable_unprepare(hdmi->clk_pix);
}

static int histb_hdmi_attach(struct drm_bridge *bridge,
			     struct drm_encoder *encoder,
			     enum drm_bridge_attach_flags flags)
{
	/*
	 * The connector is the bridge connector the VDP master builds over the
	 * chain, and it reads EDID through this bridge's ddc adapter.  HPD is
	 * polled, not interrupt-driven: the datasheet names a line for this block
	 * (GIC SPI 147, "HDMITX_AON"), but the vendor hdmi_2_0 driver never
	 * requests it -- HAL_HDMI_IrqEnableSet() is an empty stub there and HPD
	 * runs as a 10 ms polling machine that only touches TX_AON_INTR_STATE/MASK.
	 * With no measurement showing the line carry anything, it stays out of the
	 * binding, because an interrupt number published in DT cannot be taken back.
	 */
	if (!(flags & DRM_BRIDGE_ATTACH_NO_CONNECTOR))
		return -EINVAL;

	return 0;
}

static int histb_hdmi_ddc_arb(struct histb_hdmi *hdmi, bool acquire)
{
	u32 want = acquire ? DDC_REQ_ACK : 0;
	u32 val;

	val = histb_hdmi_read(hdmi, DDC_MST_ARB_REQ);
	histb_hdmi_write(hdmi, DDC_MST_ARB_REQ,
			 acquire ? (val | DDC_REQ) : (val & ~DDC_REQ));

	return readl_relaxed_poll_timeout(hdmi->regs + DDC_MST_ARB_ACK, val,
					  (val & DDC_REQ_ACK) == want, 1000,
					  DDC_TIMEOUT_ARB_MS * USEC_PER_MSEC);
}

static int histb_hdmi_ddc_idle(struct histb_hdmi *hdmi)
{
	u32 val;
	int ret;

	ret = readl_relaxed_poll_timeout(hdmi->regs + AON_DDC_MAN_CTRL, val,
					 val & DDC_SCL_HIGH, 1000,
					 USEC_PER_MSEC);
	if (ret)
		return ret;

	return readl_relaxed_poll_timeout(hdmi->regs + AON_DDC_MAN_CTRL, val,
					  val & DDC_SDA_HIGH, 1000,
					  DDC_TIMEOUT_SDA_MS * USEC_PER_MSEC);
}

/*
 * The master stretches the clock while the FIFO needs feeding, so writing the
 * whole payload in before it drains is safe, and the empty flag rather than the
 * count is what says a byte has arrived.
 */
static int histb_hdmi_ddc_write(struct histb_hdmi *hdmi, const u8 *buf, int len)
{
	int i;
	u32 val;

	for (i = 0; i < len; i++) {
		if (readl_relaxed_poll_timeout(hdmi->regs + PWD_DATA_CNT, val,
					       FIELD_GET(PWD_FIFO_DATA_CNT, val) <
					       DDC_FIFO_DEPTH, 1000,
					       DDC_TIMEOUT_DATA_MS * USEC_PER_MSEC))
			return -ETIMEDOUT;

		writel(FIELD_PREP(PWD_FIFO_DATA_IN, buf[i]),
		       hdmi->regs + PWD_FIFO_WDATA);
	}

	return i;
}

static int histb_hdmi_ddc_read(struct histb_hdmi *hdmi, u8 *buf, int len)
{
	int i;
	u32 val;

	for (i = 0; i < len; i++) {
		if (readl_relaxed_poll_timeout(hdmi->regs + PWD_MST_STATE, val,
					       !(val & PWD_FIFO_EMPTY), 1000,
					       DDC_TIMEOUT_DATA_MS * USEC_PER_MSEC))
			return -ETIMEDOUT;

		buf[i] = FIELD_GET(PWD_FIFO_DATA_OUT,
				   readl(hdmi->regs + PWD_FIFO_RDATA));
	}

	return i;
}

/* One addressed transfer: the pointer the master has to emit, then the payload. */
static int histb_hdmi_ddc_msg(struct histb_hdmi *hdmi, u8 addr, u8 seg, u8 offset,
			      u8 cmd, u8 *buf, int len, bool read)
{
	u32 val;
	int ret, done;

	writel(PWD_CMD_FIFO_CLR, hdmi->regs + PWD_MST_CMD);
	writel(FIELD_PREP(PWD_SLAVE_ADDR, addr) |
	       FIELD_PREP(PWD_SLAVE_OFFSET, offset) |
	       FIELD_PREP(PWD_SLAVE_SEG, seg), hdmi->regs + PWD_SLAVE_CFG);
	/* Read-modify-written because the low bits are the fill count itself. */
	val = readl(hdmi->regs + PWD_DATA_CNT);
	writel((val & ~PWD_DATA_OUT_CNT) | FIELD_PREP(PWD_DATA_OUT_CNT, len),
	       hdmi->regs + PWD_DATA_CNT);
	writel(cmd, hdmi->regs + PWD_MST_CMD);

	if (read)
		done = histb_hdmi_ddc_read(hdmi, buf, len);
	else
		done = histb_hdmi_ddc_write(hdmi, buf, len);
	if (done < 0) {
		ret = done;
		goto out;
	}

	writel(PWD_CLR_STATUS, hdmi->regs + PWD_MST_CLR);
	ret = readl_relaxed_poll_timeout(hdmi->regs + PWD_MST_STATE, val,
					 !(val & PWD_IN_PROG), 1000,
					 DDC_TIMEOUT_INPROG_MS * USEC_PER_MSEC);
	if (ret)
		goto out;

	if (readl(hdmi->regs + PWD_MST_STATE) & PWD_NO_ACK) {
		ret = -ENXIO;
		goto out;
	}

	return done;

out:
	writel(PWD_CMD_ABORT, hdmi->regs + PWD_MST_CMD);
	writel(PWD_CLR_STATUS, hdmi->regs + PWD_MST_CLR);
	return ret;
}

#define DDC_SEGMENT_ADDR	0x30

static int histb_hdmi_ddc_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs,
			       int num)
{
	struct histb_hdmi *hdmi = i2c_get_adapdata(adapter);
	bool pointer = false;
	u8 offset = 0, seg = 0;
	unsigned int i = 0;
	int ret;

	if (num < 1 || num > 3)
		return -EOPNOTSUPP;

	/*
	 * A pointer write followed by a read is one transaction here, not two:
	 * the offset and the segment the master has to emit are fields of the
	 * same configuration register. That covers the shapes drm_do_probe_ddc_edid
	 * sends -- write 0x30 then the segment byte for EDID blocks past the
	 * second, write the start offset, then read.
	 */
	if (num == 3 && msgs[0].addr == DDC_SEGMENT_ADDR &&
	    !(msgs[0].flags & I2C_M_RD) && msgs[0].len == 1) {
		seg = msgs[0].buf[0];
		msgs++;
		num--;
	}
	if (num == 2 && !(msgs[0].flags & I2C_M_RD) && msgs[0].len == 1 &&
	    (msgs[1].flags & I2C_M_RD) && msgs[0].addr == msgs[1].addr) {
		offset = msgs[0].buf[0];
		pointer = true;
	}

	mutex_lock(&hdmi->ddc.lock);

	ret = histb_hdmi_ddc_arb(hdmi, true);
	if (ret)
		goto unlock;

	ret = histb_hdmi_ddc_idle(hdmi);
	if (ret)
		goto release;

	if (pointer) {
		ret = histb_hdmi_ddc_msg(hdmi, msgs[1].addr << 1, seg, offset,
					 seg ? PWD_CMD_READ_SEGMENT :
					       PWD_CMD_READ_MULTI,
					 msgs[1].buf, msgs[1].len, true);
		if (ret < 0)
			goto release;
		i = 2;
	}

	for (; i < num; i++) {
		if (msgs[i].flags & I2C_M_RD)
			ret = histb_hdmi_ddc_msg(hdmi, msgs[i].addr << 1, 0, 0,
						 PWD_CMD_READ_MULTI,
						 msgs[i].buf, msgs[i].len, true);
		else
			ret = histb_hdmi_ddc_msg(hdmi, msgs[i].addr << 1, 0, 0,
						 PWD_CMD_WRITE_MULTI,
						 msgs[i].buf, msgs[i].len, false);
		if (ret < 0)
			goto release;
	}

	ret = num;

release:
	histb_hdmi_ddc_arb(hdmi, false);
unlock:
	mutex_unlock(&hdmi->ddc.lock);

	return ret;
}

static u32 histb_hdmi_ddc_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static const struct i2c_algorithm histb_hdmi_ddc_algo = {
	.master_xfer = histb_hdmi_ddc_xfer,
	.functionality = histb_hdmi_ddc_func,
};

static int histb_hdmi_ddc_init(struct histb_hdmi *hdmi)
{
	struct i2c_adapter *adapter = &hdmi->ddc.adapter;

	mutex_init(&hdmi->ddc.lock);

	strscpy(adapter->name, "histb-hdmi-ddc", sizeof(adapter->name));
	adapter->owner = THIS_MODULE;
	adapter->algo = &histb_hdmi_ddc_algo;
	adapter->dev.parent = hdmi->dev;
	i2c_set_adapdata(adapter, hdmi);

	return devm_i2c_add_adapter(hdmi->dev, adapter);
}

static enum drm_connector_status histb_hdmi_detect(struct drm_bridge *bridge,
						   struct drm_connector *connector)
{
	struct histb_hdmi *hdmi = bridge_to_histb_hdmi(bridge);
	bool connected = histb_hdmi_read(hdmi, AON_TX_AON_STATE) & HPD_STATE;

	if (histb_hdmi_read(hdmi, AON_HOTPLUG_ST_CFG) & HPD_POLARITY_CTL)
		connected = !connected;

	return connected ? connector_status_connected :
			   connector_status_disconnected;
}

static const struct drm_edid *histb_hdmi_edid_read(struct drm_bridge *bridge,
						   struct drm_connector *connector)
{
	return drm_edid_read_ddc(connector, bridge->ddc);
}

static const struct drm_bridge_funcs histb_hdmi_bridge_funcs = {
	.attach = histb_hdmi_attach,
	.detect = histb_hdmi_detect,
	.edid_read = histb_hdmi_edid_read,
	.mode_valid = histb_hdmi_mode_valid,
	.atomic_check = histb_hdmi_atomic_check,
	.atomic_enable = histb_hdmi_atomic_enable,
	.atomic_disable = histb_hdmi_atomic_disable,
	.atomic_create_state = drm_atomic_helper_bridge_create_state,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
};

/*
 * The transmitter configures itself from the atomic paths, so its bind is the
 * empty one the component framework asks for.
 */
static int histb_hdmi_component_bind(struct device *comp, struct device *master,
				     void *master_data)
{
	return 0;
}

static void histb_hdmi_component_unbind(struct device *comp, struct device *master,
					void *master_data)
{
}

static const struct component_ops histb_hdmi_component_ops = {
	.bind = histb_hdmi_component_bind,
	.unbind = histb_hdmi_component_unbind,
};

/*
 * Audio: the transmitter's I2S receiver, channel status memory and ACR
 * generator, in the first window at 0x1000..0x1050.
 *
 * This is not a codec in the usual sense: the samples arrive over I2S from the
 * AIAO block, and the transmitter only has to open its input, describe the
 * stream in the channel status bytes, and give the ACR generator the N value
 * for this sample rate and TMDS clock -- CTS itself comes from the hardware
 * (acr_cts_hw_sw_sel stays clear).  The order below, including the pulse on the
 * afifo/acr/aud reset requests, is the one the vendor path uses, because those
 * fields are clocked by what that pulse restarts.  Tables and the extracted
 * field encodings: docs/mv200h-hdmi-audio-2026-10-22.md.
 *
 * The four I2S input enables are all raised and the fifo map is left at its
 * reset value, because which AIAO transmit port reaches which input is not
 * documented anywhere local; the vendor build does the same.
 */
#define TX_AUDIO_CTRL		0x1000
#define  AUDIO_IN_EN		BIT(0)
#define  AUDIO_MUTE_EN		BIT(1)
#define  AUDIO_LAYOUT		BIT(2)	/* clear for two channels */
#define  AUDIO_I2S_EN		GENMASK(7, 4)	/* one enable per input */
#define AUD_I2S_CTRL		0x1004
#define  I2S_HBRA_ON		BIT(0)
#define  I2S_LENGTH		GENMASK(11, 8)
#define AUD_CHST_CFG0		0x100c
#define  CHST_PROF		BIT(0)
#define  CHST_COMPRESS		BIT(1)
#define  CHST_FS		GENMASK(27, 24)
#define  CHST_CLK_ACCURACY	GENMASK(31, 28)	/* level 2, the reset value */
#define AUD_CHST_CFG1		0x1010
#define  CHST_LENGTH		GENMASK(3, 0)
#define  CHST_ORG_FS		GENMASK(7, 4)
#define AUD_FIFO_CTRL		0x1018
#define  FIFO_TEST		GENMASK(4, 0)
#define  FIFO_HBR_MASK		GENMASK(11, 8)
#define AUD_ACR_CTRL		0x1040
#define  ACR_CTS_HW_SW_SEL	BIT(1)
#define ACR_N_VAL_SW		0x1048
#define  ACR_N_MAX		GENMASK(19, 0)

#define AUDIO_SRST		(TX_AUD_SRST | TX_ACR_SRST | TX_AFIFO_SRST)

struct histb_hdmi_audio_rate {
	unsigned int hz;
	u8 fs;		/* CHST_FS, channel status byte 3 */
	u8 org;		/* CHST_ORG_FS, byte 4's high nibble */
};

/*
 * Only these seven rates have a channel status code on this part; the table the
 * register set was generated from carries no entry for 64 or 128 kHz.
 */
static const struct histb_hdmi_audio_rate histb_hdmi_audio_rates[] = {
	{  32000, 0x04, 0x0c },
	{  44100, 0x00, 0x0f },
	{  48000, 0x02, 0x0d },
	{  88200, 0x08, 0x07 },
	{  96000, 0x0a, 0x05 },
	{ 176400, 0x0c, 0x03 },
	{ 192000, 0x0e, 0x01 },
};

/* AUD_I2S_CTRL's length field and CHST_LENGTH share this encoding. */
struct histb_hdmi_audio_bits {
	unsigned int width;
	u8 code;
};

static const struct histb_hdmi_audio_bits histb_hdmi_audio_bits[] = {
	{ 16, 0x02 },
	{ 17, 0x0c },
	{ 18, 0x04 },
	{ 19, 0x08 },
	{ 20, 0x0a },
	{ 21, 0x0d },
	{ 22, 0x05 },
	{ 23, 0x09 },
	{ 24, 0x0b },
};

/*
 * N, and the CTS the hardware is expected to reach, per rate and TMDS clock.  A
 * row with tmds 0 is the fallback the vendor uses for modes it did not list.
 */
struct histb_hdmi_audio_acr {
	unsigned int hz;
	unsigned int tmds_khz;
	u32 n;
};

static const struct histb_hdmi_audio_acr histb_hdmi_audio_acr[] = {
	{  32000,      0,   4096 },
	{  32000,  25174,   4576 },
	{  32000,  25200,   4096 },
	{  32000,  27000,   4096 },
	{  32000,  27027,   4096 },
	{  32000,  54000,   4096 },
	{  32000,  54054,   4096 },
	{  32000,  74175,  11648 },
	{  32000,  74250,   4096 },
	{  32000, 148351,  11648 },
	{  32000, 148500,   4096 },
	{  32000, 296703,   5824 },
	{  32000, 297000,   3072 },
	{  32000, 593406,   5824 },
	{  32000, 594000,   3072 },
	{  44100,      0,   6272 },
	{  44100,  25174,   7007 },
	{  44100,  25200,   6272 },
	{  44100,  27000,   6272 },
	{  44100,  27027,   6272 },
	{  44100,  54000,   6272 },
	{  44100,  54054,   6272 },
	{  44100,  74175,  17836 },
	{  44100,  74250,   6272 },
	{  44100, 148351,   8918 },
	{  44100, 148500,   6272 },
	{  44100, 296703,   4459 },
	{  44100, 297000,   4704 },
	{  44100, 593406,   8918 },
	{  44100, 594000,   9408 },
	{  48000,      0,   6144 },
	{  48000,  25174,   6864 },
	{  48000,  25200,   6144 },
	{  48000,  27000,   6144 },
	{  48000,  27027,   6144 },
	{  48000,  54000,   6144 },
	{  48000,  54054,   6144 },
	{  48000,  74175,  11648 },
	{  48000,  74250,   6144 },
	{  48000, 148351,   5824 },
	{  48000, 148500,   6144 },
	{  48000, 296703,   5824 },
	{  48000, 297000,   5120 },
	{  48000, 593406,   5824 },
	{  48000, 594000,   6144 },
	{  64000,      0,   8192 },
	{  64000,  25174,   9152 },
	{  64000,  25200,   8192 },
	{  64000,  27000,   8192 },
	{  64000,  27027,   8192 },
	{  64000,  54000,   8192 },
	{  64000,  54054,   8192 },
	{  64000,  74175,  23296 },
	{  64000,  74250,   8192 },
	{  64000, 148351,  23296 },
	{  64000, 148500,   8192 },
	{  64000, 296703,  11648 },
	{  64000, 297000,   8192 },
	{  64000, 593406,  11648 },
	{  64000, 594000,   8192 },
	{  88200,      0,  12544 },
	{  88200,  25174,  14014 },
	{  88200,  25200,  12544 },
	{  88200,  27000,  12544 },
	{  88200,  27027,  12544 },
	{  88200,  54000,  12544 },
	{  88200,  54054,  12544 },
	{  88200,  74175,  35672 },
	{  88200,  74250,  12544 },
	{  88200, 148351,  17836 },
	{  88200, 148500,  12544 },
	{  88200, 296703,   8918 },
	{  88200, 297000,   9408 },
	{  88200, 593406,  17836 },
	{  88200, 594000,  18816 },
	{  96000,      0,  12288 },
	{  96000,  25174,  13728 },
	{  96000,  25200,  12288 },
	{  96000,  27000,  12288 },
	{  96000,  27027,  12288 },
	{  96000,  54000,  12288 },
	{  96000,  54054,  12288 },
	{  96000,  74175,  23296 },
	{  96000,  74250,  12288 },
	{  96000, 148351,  11648 },
	{  96000, 148500,  12288 },
	{  96000, 296703,  11648 },
	{  96000, 297000,  10240 },
	{  96000, 593406,  11648 },
	{  96000, 594000,  12288 },
	{ 128000,      0,  16384 },
	{ 128000,  25174,  18304 },
	{ 128000,  25200,  16384 },
	{ 128000,  27000,  16384 },
	{ 128000,  27027,  16384 },
	{ 128000,  54000,  16384 },
	{ 128000,  54054,  16384 },
	{ 128000,  74175,  46592 },
	{ 128000,  74250,  16384 },
	{ 128000, 148351,  46592 },
	{ 128000, 148500,  16384 },
	{ 128000, 296703,  23296 },
	{ 128000, 297000,  16384 },
	{ 128000, 593406,  23296 },
	{ 128000, 594000,  16384 },
	{ 176400,      0,  25088 },
	{ 176400,  25174,  28028 },
	{ 176400,  25200,  25088 },
	{ 176400,  27000,  25088 },
	{ 176400,  27027,  25088 },
	{ 176400,  54000,  25088 },
	{ 176400,  54054,  25088 },
	{ 176400,  74175,  71344 },
	{ 176400,  74250,  25088 },
	{ 176400, 148351,  35672 },
	{ 176400, 148500,  25088 },
	{ 176400, 296703,  17836 },
	{ 176400, 297000,  18816 },
	{ 176400, 593406,  35672 },
	{ 176400, 594000,  27632 },
	{ 192000,      0,  24576 },
	{ 192000,  25174,  27456 },
	{ 192000,  25200,  24576 },
	{ 192000,  27000,  24576 },
	{ 192000,  27027,  24576 },
	{ 192000,  54000,  24576 },
	{ 192000,  54054,  24576 },
	{ 192000,  74175,  46592 },
	{ 192000,  74250,  24576 },
	{ 192000, 148351,  23296 },
	{ 192000, 148500,  24576 },
	{ 192000, 296703,  23296 },
	{ 192000, 297000,  20480 },
	{ 192000, 593406,  23296 },
	{ 192000, 594000,  24576 },
};

static void histb_hdmi_audio_rmw(struct histb_hdmi *hdmi, u32 offset, u32 mask,
				  u32 val)
{
	u32 cur = histb_hdmi_read(hdmi, offset);

	histb_hdmi_write(hdmi, offset, (cur & ~mask) | val);
}

static const struct histb_hdmi_audio_rate *histb_hdmi_audio_rate(unsigned int hz)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(histb_hdmi_audio_rates); i++)
		if (histb_hdmi_audio_rates[i].hz == hz)
			return &histb_hdmi_audio_rates[i];

	return NULL;
}

static u32 histb_hdmi_audio_n(unsigned int hz, unsigned int tmds_khz)
{
	const struct histb_hdmi_audio_acr *fallback = NULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(histb_hdmi_audio_acr); i++) {
		const struct histb_hdmi_audio_acr *row = &histb_hdmi_audio_acr[i];

		if (row->hz != hz)
			continue;
		if (row->tmds_khz == tmds_khz)
			return row->n;
		if (!row->tmds_khz)
			fallback = row;
	}

	return fallback ? fallback->n : 0;
}

static u8 histb_hdmi_audio_bits_code(unsigned int width)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(histb_hdmi_audio_bits); i++)
		if (histb_hdmi_audio_bits[i].width == width)
			return histb_hdmi_audio_bits[i].code;

	return 0xff;
}

static int histb_hdmi_audio_setup(struct histb_hdmi *hdmi, unsigned int rate,
				   unsigned int width, unsigned int channels)
{
	const struct histb_hdmi_audio_rate *ar = histb_hdmi_audio_rate(rate);
	u8 bits = histb_hdmi_audio_bits_code(width);
	u32 mask = AUDIO_IN_EN | AUDIO_MUTE_EN | AUDIO_I2S_EN;
	u32 n;

	if (!ar || bits == 0xff)
		return -EINVAL;

	n = histb_hdmi_audio_n(rate, hdmi->audio_tmds_khz);
	if (!n)
		return -EINVAL;

	/* Off and muted while the fields are rewritten. */
	histb_hdmi_audio_rmw(hdmi, TX_AUDIO_CTRL, mask, AUDIO_MUTE_EN);

	histb_hdmi_audio_rmw(hdmi, TX_PWD_RST_CTRL, AUDIO_SRST, AUDIO_SRST);
	usleep_range(500, 600);
	histb_hdmi_audio_rmw(hdmi, TX_PWD_RST_CTRL, AUDIO_SRST, 0);

	histb_hdmi_audio_rmw(hdmi, TX_AUDIO_CTRL, AUDIO_LAYOUT,
			     channels > 2 ? AUDIO_LAYOUT : 0);
	histb_hdmi_audio_rmw(hdmi, AUD_I2S_CTRL, I2S_LENGTH | I2S_HBRA_ON,
			     FIELD_PREP(I2S_LENGTH, bits));
	histb_hdmi_audio_rmw(hdmi, AUD_CHST_CFG0,
			     CHST_FS | CHST_PROF | CHST_COMPRESS,
			     FIELD_PREP(CHST_FS, ar->fs));
	histb_hdmi_audio_rmw(hdmi, AUD_CHST_CFG1, CHST_LENGTH | CHST_ORG_FS,
			     FIELD_PREP(CHST_LENGTH, bits) |
			     FIELD_PREP(CHST_ORG_FS, ar->org));
	histb_hdmi_audio_rmw(hdmi, AUD_FIFO_CTRL, FIFO_TEST | FIFO_HBR_MASK, 0);
	histb_hdmi_audio_rmw(hdmi, AUD_ACR_CTRL, ACR_CTS_HW_SW_SEL, 0);
	histb_hdmi_write(hdmi, ACR_N_VAL_SW, n & ACR_N_MAX);

	histb_hdmi_audio_rmw(hdmi, TX_AUDIO_CTRL, mask,
			     AUDIO_I2S_EN | AUDIO_IN_EN);

	return 0;
}

static void histb_hdmi_audio_disable(struct histb_hdmi *hdmi)
{
	histb_hdmi_audio_rmw(hdmi, TX_AUDIO_CTRL,
			     AUDIO_IN_EN | AUDIO_MUTE_EN | AUDIO_I2S_EN,
			     AUDIO_MUTE_EN);
}

static int histb_hdmi_audio_prepare(struct device *dev, void *data,
				     struct hdmi_codec_daifmt *fmt,
				     struct hdmi_codec_params *hparms)
{
	struct histb_hdmi *hdmi = data;

	if (fmt->fmt != HDMI_I2S)
		return -EINVAL;

	return histb_hdmi_audio_setup(hdmi, hparms->sample_rate,
				      hparms->sample_width,
				      hparms->channels);
}

static void histb_hdmi_audio_shutdown(struct device *dev, void *data)
{
	histb_hdmi_audio_disable(data);
}

static int histb_hdmi_audio_mute(struct device *dev, void *data,
				 bool enable, int direction)
{
	struct histb_hdmi *hdmi = data;

	if (direction != SNDRV_PCM_STREAM_PLAYBACK)
		return -EINVAL;

	histb_hdmi_audio_rmw(hdmi, TX_AUDIO_CTRL, AUDIO_MUTE_EN,
			     enable ? AUDIO_MUTE_EN : 0);

	return 0;
}

static const struct hdmi_codec_ops histb_hdmi_codec_ops = {
	.prepare = histb_hdmi_audio_prepare,
	.audio_shutdown = histb_hdmi_audio_shutdown,
	.mute_stream = histb_hdmi_audio_mute,
	/*
	 * No get_eld: the ELD lives on the connector, which the VDP master
	 * creates over the bridge chain, so handing it out is a display-side
	 * step and not this device's to invent.
	 */
};

static void histb_hdmi_audio_unregister(void *data)
{
	platform_device_unregister(data);
}

static int histb_hdmi_audio_register(struct histb_hdmi *hdmi)
{
	struct hdmi_codec_pdata pdata = {
		.ops = &histb_hdmi_codec_ops,
		.data = hdmi,
		.i2s = 1,
		.max_i2s_channels = 2,
	};
	struct platform_device *pdev;

	pdev = platform_device_register_data(hdmi->dev, HDMI_CODEC_DRV_NAME,
					     PLATFORM_DEVID_AUTO, &pdata,
					     sizeof(pdata));
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);

	/*
	 * The codec device must not carry our of_node: ASoC reaches it from the
	 * sound card's phandle through soc_component_to_node(), which falls back
	 * to the parent device's node, and setting it here made our own
	 * of_match_table bind the codec (-EINVAL "invalid resource (null)"
	 * measured on the board).
	 */
	return devm_add_action_or_reset(hdmi->dev, histb_hdmi_audio_unregister,
					pdev);
}

static int histb_hdmi_parse_dt(struct histb_hdmi *hdmi)
{
	struct platform_device *pdev = to_platform_device(hdmi->dev);
	struct device *dev = &pdev->dev;

	hdmi->regs = devm_platform_ioremap_resource_byname(pdev, "tx");
	if (IS_ERR(hdmi->regs))
		return PTR_ERR(hdmi->regs);

	hdmi->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(hdmi->phy))
		return PTR_ERR(hdmi->phy);

	/*
	 * The two divider fields and the resets live in the clock generator,
	 * whose node is a syscon. Those fields are not modelled as clocks: both
	 * are chosen from the mode, and the factor that turns the pixel clock
	 * into the PHY's reference clock -- the interpolation ratio -- has no
	 * field in the clock generator at all, so no rate relation could be
	 * reported honestly for them.
	 */
	hdmi->crg = syscon_regmap_lookup_by_phandle(dev->of_node,
						    "hisilicon,crg");
	if (IS_ERR(hdmi->crg))
		return dev_err_probe(dev, PTR_ERR(hdmi->crg), "no crg regmap\n");

	hdmi->clk_pix = devm_clk_get(dev, "pix");
	if (IS_ERR(hdmi->clk_pix))
		return dev_err_probe(dev, PTR_ERR(hdmi->clk_pix), "no pixel clock\n");

	hdmi->clk_tmds = devm_clk_get(dev, "tmds");
	if (IS_ERR(hdmi->clk_tmds))
		return dev_err_probe(dev, PTR_ERR(hdmi->clk_tmds), "no TMDS clock\n");

	hdmi->resets = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(hdmi->resets))
		return dev_err_probe(dev, PTR_ERR(hdmi->resets), "no resets\n");

	return 0;
}

static int histb_hdmi_probe(struct platform_device *pdev)
{
	struct histb_hdmi *hdmi;
	int ret;

	/*
	 * The bridge must come from devm_drm_bridge_alloc(): that is where its
	 * refcount, list head and container pointer are set up, and
	 * drm_bridge_add() faults without them.  The macro takes the enclosing
	 * struct, so the bridge stays embedded here.
	 */
	hdmi = devm_drm_bridge_alloc(&pdev->dev, struct histb_hdmi, bridge,
				     &histb_hdmi_bridge_funcs);
	if (IS_ERR(hdmi))
		return PTR_ERR(hdmi);

	hdmi->dev = &pdev->dev;
	INIT_DELAYED_WORK(&hdmi->fdet_work, histb_hdmi_fdet_work);
	platform_set_drvdata(pdev, hdmi);

	ret = histb_hdmi_parse_dt(hdmi);
	if (ret)
		return ret;

	ret = histb_hdmi_ddc_init(hdmi);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to add the DDC adapter\n");

	hdmi->bridge.of_node = pdev->dev.of_node;
	hdmi->bridge.type = DRM_MODE_CONNECTOR_HDMIA;
	hdmi->bridge.ops = DRM_BRIDGE_OP_DETECT | DRM_BRIDGE_OP_EDID;
	hdmi->bridge.ddc = &hdmi->ddc.adapter;

	ret = devm_drm_bridge_add(&pdev->dev, &hdmi->bridge);
	if (ret)
		return ret;

	ret = histb_hdmi_audio_register(hdmi);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "no audio codec\n");

	/*
	 * Membership of the VDP's aggregate is what makes the master bind at
	 * all -- it reads its own of-graph endpoint to build the match list --
	 * and it is what takes the DRM device down before this driver's clocks,
	 * regmap and MMIO are released.  Bind has nothing left to do: the
	 * bridge is on the global list by then.
	 */
	return component_add(&pdev->dev, &histb_hdmi_component_ops);
}

static void histb_hdmi_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &histb_hdmi_component_ops);
}

static const struct of_device_id histb_hdmi_of_table[] = {
	{ .compatible = "hisilicon,hi3798mv200-hdmi", },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_hdmi_of_table);

static struct platform_driver histb_hdmi_driver = {
	.probe = histb_hdmi_probe,
	.remove = histb_hdmi_remove,
	.driver = {
		.name = "histb-hdmi",
		.of_match_table = histb_hdmi_of_table,
	},
};
module_platform_driver(histb_hdmi_driver);

MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_DESCRIPTION("HiSilicon Hi3798MV200 HDMI transmitter driver");
MODULE_LICENSE("GPL");
