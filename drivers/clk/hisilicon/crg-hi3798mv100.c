// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSilicon Hi3798MV100 Clock and Reset Generator driver
 *
 * Copyright (c) 2016 HiSilicon Technologies Co., Ltd.
 */

#include <dt-bindings/clock/hisilicon,hi3798mv100-crg.h>
#include <dt-bindings/clock/hisilicon,hi3798mv100-sysctrl.h>
#include <linux/clk-provider.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "clk.h"
#include "crg.h"
#include "reset.h"

/*
 * Clocks which only serve as the parent of another clock of the same provider
 * are not exposed to the device tree, so their ids come after the exposed ones.
 */
enum hi3798mv100_inner_clk {
	HI3798MV100_FIXED_3M = HI3798MV100_CRG_CLK_COUNT,
	HI3798MV100_FIXED_12M,
	HI3798MV100_FIXED_15M,
	HI3798MV100_FIXED_24M,
	HI3798MV100_FIXED_25M,
	HI3798MV100_FIXED_48M,
	HI3798MV100_FIXED_50M,
	HI3798MV100_FIXED_60M,
	HI3798MV100_FIXED_75M,
	HI3798MV100_FIXED_83P3M,
	HI3798MV100_FIXED_100M,
	HI3798MV100_FIXED_150M,
	HI3798MV100_FIXED_200M,
	HI3798MV100_FIXED_250M,
	HI3798MV100_MMC_MUX,
	HI3798MV100_SDIO0_MUX,
	HI3798MV100_HD_PIX,

	HI3798MV100_CRG_NR_CLKS,
};

/* fixed sources: this CRG cannot change these rates */
static const struct hisi_fixed_rate_clock hi3798mv100_fixed_rate_clks[] = {
	{ HI3798MV100_OSC_CLK, "clk_osc", NULL, 0, 24000000, },
	{ HI3798MV100_APB_CLK, "clk_apb", NULL, 0, 100000000, },
	{ HI3798MV100_AHB_CLK, "clk_ahb", NULL, 0, 200000000, },
	{ HI3798MV100_FIXED_3M, "3m", NULL, 0, 3000000, },
	{ HI3798MV100_FIXED_12M, "12m", NULL, 0, 12000000, },
	{ HI3798MV100_FIXED_15M, "15m", NULL, 0, 15000000, },
	{ HI3798MV100_FIXED_24M, "24m", NULL, 0, 24000000, },
	{ HI3798MV100_FIXED_25M, "25m", NULL, 0, 25000000, },
	{ HI3798MV100_FIXED_48M, "48m", NULL, 0, 48000000, },
	{ HI3798MV100_FIXED_50M, "50m", NULL, 0, 50000000, },
	{ HI3798MV100_FIXED_60M, "60m", NULL, 0, 60000000, },
	{ HI3798MV100_FIXED_75M, "75m", NULL, 0, 75000000, },
	{ HI3798MV100_FIXED_83P3M, "83p3m", NULL, 0, 83300000, },
	{ HI3798MV100_FIXED_100M, "100m", NULL, 0, 100000000, },
	{ HI3798MV100_FIXED_150M, "150m", NULL, 0, 150000000, },
	{ HI3798MV100_FIXED_200M, "200m", NULL, 0, 200000000, },
	{ HI3798MV100_FIXED_250M, "250m", NULL, 0, 250000000, },
};

/* CRG39 and CRG40 share the same selector encoding */
static const char *const hi3798mv100_mmc_mux_p[] = {
	"75m", "100m", "50m", "15m",
};

static const u32 hi3798mv100_mmc_mux_table[] = { 0, 1, 2, 3 };

static const struct hisi_mux_clock hi3798mv100_mux_clks[] = {
	/* CRG40, which is the SDIO1 instance hosting the eMMC */
	{ HI3798MV100_MMC_MUX, "mmc_mux", hi3798mv100_mmc_mux_p,
	  ARRAY_SIZE(hi3798mv100_mmc_mux_p), CLK_SET_RATE_PARENT, 0xa0, 8, 2,
	  0, hi3798mv100_mmc_mux_table, },
	/* CRG39 */
	{ HI3798MV100_SDIO0_MUX, "sdio0_mux", hi3798mv100_mmc_mux_p,
	  ARRAY_SIZE(hi3798mv100_mmc_mux_p), CLK_SET_RATE_PARENT, 0x9c, 8, 2,
	  0, hi3798mv100_mmc_mux_table, },
};

/*
 * The HD display clock: PERI_CRG_PLL10/11 configure the HD PLL and CRG54's
 * vo_hd_clk_div halves its output for the channel, so the channel clock is half
 * the PLL output. Nothing documents the loop settings in PLL11 or relates them
 * to a rate, so this clock is driven by a table of register pairs, one per
 * display format. Each rate below is what that entry's own timing adds up to,
 * e.g. 1024x768 with htotal 1344 and vtotal 806 at 60Hz is 65.12MHz.
 *
 * PLL10 bits 26:24 hold the post divider code and either bit 28 or bit 29 is
 * set in every entry; which one depends on the entry, nothing clears them, and
 * they are always written together with the divider, so they are carried as
 * part of the value instead of being modelled as a gate. Every entry uses
 * divider code 0; only 4K50/60 would need another code and that is above the
 * transmitter's range.
 */
#define CRG54				0xd8
#define  CRG54_VO_HD_CLK_DIV		GENMASK(19, 18)
#define  CRG54_CLK_DIV_2		0

struct hi3798_hd_pix_cfg {
	unsigned long	rate;
	u32		pll10;
	u32		pll11;
};

static const struct hi3798_hd_pix_cfg hi3798mv100_hd_pix_cfgs[] = {
	/* CEA: one loop setting covers the whole family */
	{  74250000, 0x14000000, 0x02002063, }, /* 720p50/60, 1080p24-30, 1080i */
	{ 148500000, 0x12000000, 0x02002063, }, /* 1080p50/60 */
	/*
	 * 4K: 297MHz is half the 594MHz the PLL runs at here, the same divider
	 * as every other entry. All three of 4K@24/25/30 add up to exactly
	 * 297MHz (5500x2250x24, 5280x2250x25, 4400x2250x30), so one row covers
	 * them the way the CEA family has one. 4K@50/60 would need a different
	 * post divider and is out of range for this port.
	 */
	{ 297000000, 0x11000000, 0x02002063, }, /* 3840x2160@24/25/30 */
	{ 296703000, 0x11E6A7EF, 0x00002062, }, /* 3840x2160@23.976/29.97 */
	/* VESA */
	{  25200000, 0x24000000, 0x20050a8, }, /* 640x480@60 */
	{  39805440, 0x25000000, 0x201967a, }, /* 800x600@60 */
	{  65122560, 0x15000000, 0x201954a, }, /* 1024x768@60 */
	{  83764800, 0x13000000, 0x2019417, }, /* 1280x800@60 */
	{  85478400, 0x13000000, 0x2032859, }, /* 1360x768@60 */
	{  85770240, 0x13000000, 0x200a1ad, }, /* 1366x768@60 */
	{  88896000, 0x14000000, 0x2006163, }, /* 1440x900@60 reduced blanking */
	{  97785600, 0x14000000, 0x2006187, }, /* 1600x900@60 reduced blanking */
	{ 106755840, 0x12000000, 0x201e42b, }, /* 1440x900@60 */
	{ 107855040, 0x15000000, 0x20198c9, }, /* 1280x1024@60 */
	{ 121717440, 0x12000000, 0x20050cb, }, /* 1400x1050@60 */
	{ 146217600, 0x12000000, 0x2018493, }, /* 1680x1050@60 */
	{ 156750000, 0x12000000, 0x200309d, }, /* 2048x1152@60 */
	{ 162000000, 0x12000000, 0x2001036, }, /* 1600x1200@60 */
	{ 234000000, 0x11000000, 0x2001027, }, /* 1920x1440@60 */
	/*
	 * The 2560-class rows are both above 165MHz and both marked as
	 * unsupported in the format list these pairs come from, so they are
	 * carried as they stand and never selected by a mode this driver
	 * accepts.
	 */
	{ 241500000, 0x12000000, 0x20020a1, }, /* 2560x1440@60 reduced blanking */
	{ 268500000, 0x12000000, 0x20020b3, }, /* 2560x1600@60 reduced blanking */
};

struct hi3798_hd_pix_clock {
	struct clk_hw	hw;
	void __iomem	*pll10;
	void __iomem	*pll11;
	void __iomem	*crg54;
};

#define to_hi3798_hd_pix_clock(_hw) \
	container_of(_hw, struct hi3798_hd_pix_clock, hw)

static const struct hi3798_hd_pix_cfg *hi3798_hd_pix_cfg(unsigned long rate)
{
	const struct hi3798_hd_pix_cfg *best = &hi3798mv100_hd_pix_cfgs[0];
	unsigned long diff = ~0UL;
	int i;

	for (i = 0; i < ARRAY_SIZE(hi3798mv100_hd_pix_cfgs); i++) {
		const struct hi3798_hd_pix_cfg *cfg = &hi3798mv100_hd_pix_cfgs[i];
		unsigned long delta = rate > cfg->rate ? rate - cfg->rate :
							     cfg->rate - rate;

		if (delta < diff) {
			diff = delta;
			best = cfg;
		}
	}

	return best;
}

static unsigned long hi3798_hd_pix_recalc_rate(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct hi3798_hd_pix_clock *clk = to_hi3798_hd_pix_clock(hw);
	u32 pll10 = readl_relaxed(clk->pll10);
	u32 pll11 = readl_relaxed(clk->pll11);
	int i;

	for (i = 0; i < ARRAY_SIZE(hi3798mv100_hd_pix_cfgs); i++) {
		const struct hi3798_hd_pix_cfg *cfg = &hi3798mv100_hd_pix_cfgs[i];

		if (cfg->pll10 == pll10 && cfg->pll11 == pll11 &&
		    FIELD_GET(CRG54_VO_HD_CLK_DIV, readl_relaxed(clk->crg54)) ==
		    CRG54_CLK_DIV_2)
			return cfg->rate;
	}

	return 0;
}

static int hi3798_hd_pix_determine_rate(struct clk_hw *hw,
					struct clk_rate_request *req)
{
	req->rate = hi3798_hd_pix_cfg(req->rate)->rate;
	return 0;
}

static int hi3798_hd_pix_set_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct hi3798_hd_pix_clock *clk = to_hi3798_hd_pix_clock(hw);
	const struct hi3798_hd_pix_cfg *cfg = hi3798_hd_pix_cfg(rate);
	u32 val;

	writel_relaxed(cfg->pll10, clk->pll10);
	writel_relaxed(cfg->pll11, clk->pll11);

	val = readl_relaxed(clk->crg54);
	val = (val & ~CRG54_VO_HD_CLK_DIV) |
	      FIELD_PREP(CRG54_VO_HD_CLK_DIV, CRG54_CLK_DIV_2);
	writel_relaxed(val, clk->crg54);

	return 0;
}

static const struct clk_ops hi3798_hd_pix_ops = {
	.recalc_rate = hi3798_hd_pix_recalc_rate,
	.determine_rate = hi3798_hd_pix_determine_rate,
	.set_rate = hi3798_hd_pix_set_rate,
};

static struct clk *hi3798_clk_register_hd_pix(struct device *dev, void __iomem *base)
{
	struct hi3798_hd_pix_clock *clk;
	static const char *const parents[] = { "clk_osc" };
	struct clk_init_data init = {
		.name = "hd_pix",
		.ops = &hi3798_hd_pix_ops,
		.parent_names = parents,
		.num_parents = ARRAY_SIZE(parents),
	};

	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	clk->hw.init = &init;
	clk->pll10 = base + 0x28;
	clk->pll11 = base + 0x2c;
	clk->crg54 = base + CRG54;

	return devm_clk_register(dev, &clk->hw);
}

static u32 hi3798mv100_phase_regvals[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static u32 hi3798mv100_phase_degrees[] = { 0, 45, 90, 135, 180, 225, 270, 315 };

static const struct hisi_phase_clock hi3798mv100_phase_clks[] = {
	{ HI3798MV100_MMC_SAMPLE_CLK, "mmc_sample", "clk_mmc_ciu",
	  CLK_SET_RATE_PARENT, 0xa0, 12, 3, hi3798mv100_phase_degrees,
	  hi3798mv100_phase_regvals, ARRAY_SIZE(hi3798mv100_phase_regvals), },
	{ HI3798MV100_MMC_DRV_CLK, "mmc_drive", "clk_mmc_ciu",
	  CLK_SET_RATE_PARENT, 0xa0, 16, 3, hi3798mv100_phase_degrees,
	  hi3798mv100_phase_regvals, ARRAY_SIZE(hi3798mv100_phase_regvals), },
};

static const struct hisi_gate_clock hi3798mv100_gate_clks[] = {
	/* CRG26 */
	{ HI3798MV100_UART1_CLK, "clk_uart1", "3m",
	  CLK_SET_RATE_PARENT | CLK_IS_CRITICAL, 0x68, 0, 0, },
	{ HI3798MV100_UART2_CLK, "clk_uart2", "83p3m",
	  CLK_SET_RATE_PARENT, 0x68, 4, 0, },
	/* CRG27 */
	{ HI3798MV100_I2C0_CLK, "clk_i2c0", "clk_apb",
	  CLK_SET_RATE_PARENT, 0x6c, 4, 0, },
	{ HI3798MV100_I2C1_CLK, "clk_i2c1", "clk_apb",
	  CLK_SET_RATE_PARENT, 0x6c, 8, 0, },
	{ HI3798MV100_I2C2_CLK, "clk_i2c2", "clk_apb",
	  CLK_SET_RATE_PARENT, 0x6c, 12, 0, },
	/* CRG28 */
	{ HI3798MV100_SPI0_CLK, "clk_spi0", "clk_apb",
	  CLK_SET_RATE_PARENT, 0x70, 0, 0, },
	/* CRG30, the video decoder */
	{ HI3798MV100_VDH_CLK, "clk_vdh", "clk_ahb",
	  CLK_SET_RATE_PARENT, 0x78, 0, 0, },
	/* CRG31, the JPEG decoder. Its jpgd_clk_sel[8] stays unmodelled: only the
	 * gate bit is needed to power the block, and the mux's two parents are not
	 * named anywhere in the SoC's clock tree.
	 */
	{ HI3798MV100_JPGD_CLK, "clk_jpgd", "clk_ahb",
	  CLK_SET_RATE_PARENT, 0x7c, 0, 0, },
	/* CRG35, the video encoder; CRG35[4] is its reset and [8:7] its clock mux,
	 * both left to the reset controller and to the bootloader's setting.
	 */
	{ HI3798MV100_VENC_CLK, "clk_venc", "clk_ahb",
	  CLK_SET_RATE_PARENT, 0x8c, 0, 0, },
	/* CRG36, the JPEG encoder; CRG36[4] is its reset and stays unmodelled. */
	{ HI3798MV100_JPGE_CLK, "clk_jpge", "clk_ahb",
	  CLK_SET_RATE_PARENT, 0x90, 0, 0, },
	/* CRG39, SDIO0 */
	{ HI3798MV100_SDIO0_BIU_CLK, "clk_sdio0_biu", "200m",
	  CLK_SET_RATE_PARENT, 0x9c, 0, 0, },
	{ HI3798MV100_SDIO0_CIU_CLK, "clk_sdio0_ciu", "sdio0_mux",
	  CLK_SET_RATE_PARENT, 0x9c, 1, 0, },
	/* CRG40, SDIO1 which hosts the eMMC */
	{ HI3798MV100_MMC_BIU_CLK, "clk_mmc_biu", "200m",
	  CLK_SET_RATE_PARENT, 0xa0, 0, 0, },
	{ HI3798MV100_MMC_CIU_CLK, "clk_mmc_ciu", "mmc_mux",
	  CLK_SET_RATE_PARENT, 0xa0, 1, 0, },
	/* CRG41 */
	{ HI3798MV100_DMAC_CLK, "clk_dmac", "clk_ahb",
	  CLK_SET_RATE_PARENT, 0xa4, 0, 0, },
	/* CRG46 and CRG47, USB2 host controller 0 */
	{ HI3798MV100_USB20_BUS_CLK, "clk_u20_bus", "clk_ahb",
	  CLK_SET_RATE_PARENT, 0xb8, 0, 0, },
	{ HI3798MV100_USB20_48M_CLK, "clk_u20_48m", "48m",
	  CLK_SET_RATE_PARENT, 0xb8, 1, 0, },
	{ HI3798MV100_USB20_12M_CLK, "clk_u20_12m", "12m",
	  CLK_SET_RATE_PARENT, 0xb8, 2, 0, },
	{ HI3798MV100_USB20_OTG_UTMI_CLK, "clk_u20_otg_utmi", "60m",
	  CLK_SET_RATE_PARENT, 0xb8, 3, 0, },
	{ HI3798MV100_USB20_PHY_CLK, "clk_u20_phy", "60m",
	  CLK_SET_RATE_PARENT, 0xb8, 4, 0, },
	{ HI3798MV100_USB20_UTMI_CLK, "clk_u20_utmi", "60m",
	  CLK_SET_RATE_PARENT, 0xb8, 5, 0, },
	{ HI3798MV100_USB20_UTMI_CLK1, "clk_u20_utmi1", "60m",
	  CLK_SET_RATE_PARENT, 0xb8, 6, 0, },
	{ HI3798MV100_USB20_PHY1_REF_CLK, "clk_u20_phy1_ref", "24m",
	  CLK_SET_RATE_PARENT, 0xbc, 0, 0, },
	/*
	 * CRG53 gpu_cken: the bus gate of the Mali-450. The shader clock comes
	 * from CRG73, which comes up on selector 6 (200 MHz) with
	 * gpu_begin_cfg_bypass raised, so the PMC21 frequency state machine is
	 * never asked to switch and this clock has no rate to set.
	 */
	{ HI3798MV100_GPU_BUS_CLK, "clk_gpu_bus", "200m",
	  CLK_SET_RATE_PARENT, 0xd4, 0, 0, },
	/*
	 * CRG54 vo_hd_cken: the HD channel clock, which is also what the HDMI
	 * transmitter receives (hdmi_clk_sel picks vo_hd and vo_hd_hdmi_clk_sel
	 * keeps it 1:1). The firmware's own logo is scanned out through this
	 * tree, so the gate is claimed until the display driver has taken over.
	 */
	{ HI3798MV100_VO_HD_CLK, "clk_vo_hd", "hd_pix",
	  CLK_SET_RATE_PARENT | CLK_IS_CRITICAL, 0xd8, 4, 0, },
	/* CRG72, the PHY of the Fast Ethernet MAC */
	{ HI3798MV100_FEPHY_CLK, "clk_fephy", "25m",
	  CLK_SET_RATE_PARENT, 0x120, 0, 0, },
	/*
	 * CRG94[0] wdg0_cken, the SP805's counting clock. The block counts at the
	 * 24 MHz oscillator, measured on the board as 24.00000 MHz over 34 samples,
	 * while apb_pclk is the 100 MHz the driver was taking as its rate. Bit 4,
	 * wdg0_srst_req, is a reset and stays with the reset controller; the boot
	 * leaves this register at 0x3 (both watchdogs clocked, neither in reset),
	 * and critical because a watchdog whose gate can be dropped underneath it
	 * is not a safety net.
	 */
	{ HI3798MV100_WDT_CLK, "clk_wdt0", "24m",
	  CLK_IS_CRITICAL, 0x178, 0, 0, },
	/* CRG100 and CRG102, USB2 host controller 1 */
	{ HI3798MV100_USB21_BUS_CLK, "clk_u21_bus", "clk_ahb",
	  CLK_SET_RATE_PARENT, 0x198, 0, 0, },
	{ HI3798MV100_USB21_48M_CLK, "clk_u21_48m", "48m",
	  CLK_SET_RATE_PARENT, 0x198, 1, 0, },
	{ HI3798MV100_USB21_12M_CLK, "clk_u21_12m", "12m",
	  CLK_SET_RATE_PARENT, 0x198, 2, 0, },
	{ HI3798MV100_USB21_PHY_CLK, "clk_u21_phy", "60m",
	  CLK_SET_RATE_PARENT, 0x198, 4, 0, },
	{ HI3798MV100_USB21_UTMI_CLK, "clk_u21_utmi", "60m",
	  CLK_SET_RATE_PARENT, 0x198, 5, 0, },
	{ HI3798MV100_USB21_PHY1_REF_CLK, "clk_u21_phy1_ref", "24m",
	  CLK_SET_RATE_PARENT, 0x190, 0, 0, },
};

struct hi3798_clk_bits {
	struct clk_hw	hw;
	void __iomem	*reg;
	u32		mask;
	u32		value;
	/*
	 * clk_init_data names parents through an array, so the one parent a bit
	 * group may have lives at the end of this struct.
	 */
	const char	*parent_names[];
};

#define to_hi3798_clk_bits(_hw) \
	container_of(_hw, struct hi3798_clk_bits, hw)

/*
 * A few CRG registers hold the clock gates of one device next to a clock select
 * field whose rates are not documented, so the group of bits has to be enabled
 * as a unit instead of being split into gates and a mux.
 */
static int hi3798_clk_bits_prepare(struct clk_hw *hw)
{
	struct hi3798_clk_bits *clk = to_hi3798_clk_bits(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	writel_relaxed((val & ~clk->mask) | clk->value, clk->reg);

	return 0;
}

static void hi3798_clk_bits_unprepare(struct clk_hw *hw)
{
	struct hi3798_clk_bits *clk = to_hi3798_clk_bits(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	writel_relaxed(val & ~clk->mask, clk->reg);
}

static int hi3798_clk_bits_is_prepared(struct clk_hw *hw)
{
	struct hi3798_clk_bits *clk = to_hi3798_clk_bits(hw);

	return (readl_relaxed(clk->reg) & clk->mask) == clk->value;
}

static const struct clk_ops hi3798_clk_bits_ops = {
	.prepare = hi3798_clk_bits_prepare,
	.unprepare = hi3798_clk_bits_unprepare,
	.is_prepared = hi3798_clk_bits_is_prepared,
};

struct hi3798_bits_clock {
	unsigned int	id;
	const char	*name;
	unsigned long	offset;
	u32		mask;
	u32		value;
	/*
	 * The rate of a bit group is whatever its source runs at, and the
	 * fields that pick a rate are not modelled here; naming a parent is
	 * what lets a consumer read a rate instead of zero.
	 */
	const char	*parent;
};

/*
 * CRG51: the bus and MAC gates of the Fast Ethernet MAC, plus clock select 2,
 * the setting the MAC comes out of reset on. The only consumer of this clock,
 * hisi_femac, enables it without asking for a rate.
 */
static const struct hi3798_bits_clock hi3798mv100_bits_clks[] = {
	{ HI3798MV100_ETH_CLK, "clk_eth", 0xcc, 0xf, 0xb, },
	/*
	 * CRG53[10:8]: the pixel processors' gates, gpu_pp0/1/2_cken. They are
	 * raised in the same write as gpu_cken, before gpu_srst_req is pulsed,
	 * so they are one clock here; bit 4, gpu_srst_req, is a reset and
	 * belongs to the reset controller. PP2 does not exist on this MP2 part
	 * and its gate is raised anyway. The parent is the 200 MHz that CRG73
	 * comes up on: gpu_clk_mux (CRG85[7:5]) reads selector 6 and nothing
	 * here reprograms that selector.
	 */
	{ HI3798MV100_GPU_CORE_CLK, "clk_gpu_pp", 0xd4, 0x7 << 8, 0x7 << 8,
	  "200m", },
	/*
	 * CRG54: the gates of the display channel the VO runs on -- bus, core,
	 * the SD channel, its SDATE and HDATE encoders and DAC channel 0. They
	 * are raised as a unit before the VOU reset is pulsed, because a reset
	 * request only reaches a block while it is clocked. Bit 4, vo_hd_cken,
	 * is deliberately not in the mask: the HD channel clock is modelled as
	 * clk_vo_hd above, and a second owner would gate the pixel clock off
	 * whenever this clock is released. Every other gate of the register is
	 * in the mask, so releasing this clock gates the channel off instead of
	 * leaving a bit at whatever the boot set it to.
	 */
	{ HI3798MV100_VO_CLK, "clk_vo", 0xd8, 0x6f, 0x6f, },
	/*
	 * CRG67[5:0]: the transmitter's bus, CEC, ID, MHL, OS and AS gates. All
	 * six have to run while hdmitx_ctrl_srst_req is asserted, so they are
	 * one clock here; bits 8 and 9 are resets and belong to the reset
	 * controller.
	 */
	{ HI3798MV100_HDMI_CTRL_CLK, "clk_hdmi_ctrl", 0x10c, 0x3f, 0x3f, },
	/* CRG68[0]: the transmitter PHY's bus gate; bit 4 is the PHY reset. */
	{ HI3798MV100_HDMI_PHY_CLK, "clk_hdmi_phy", 0x110, BIT(0), BIT(0), },
	/* CRG70[0] aiao_cken; the block resets through CRG70[4]. */
	{ HI3798MV100_AIAO_CLK, "clk_aiao", 0x118, BIT(0), BIT(0), },
};

static int hi3798_clk_register_bits(struct device *dev,
				    const struct hi3798_bits_clock *clks,
				    int nums, struct hisi_clock_data *data)
{
	int i;

	for (i = 0; i < nums; i++) {
		struct hi3798_clk_bits *bits;
		struct clk_init_data init = {};
		struct clk *clk;

		bits = devm_kzalloc(dev,
				    struct_size(bits, parent_names, 1),
				    GFP_KERNEL);
		if (!bits)
			return -ENOMEM;

		init.name = clks[i].name;
		init.ops = &hi3798_clk_bits_ops;

		bits->reg = data->base + clks[i].offset;
		bits->mask = clks[i].mask;
		bits->value = clks[i].value;
		if (clks[i].parent) {
			bits->parent_names[0] = clks[i].parent;
			init.parent_names = bits->parent_names;
			init.num_parents = 1;
		}
		bits->hw.init = &init;

		clk = devm_clk_register(dev, &bits->hw);
		if (IS_ERR(clk))
			return dev_err_probe(dev, PTR_ERR(clk),
					     "failed to register clock %s\n",
					     clks[i].name);

		data->clk_data.clks[clks[i].id] = clk;
	}

	return 0;
}

/*
 * CRG70's audio clock divider is the AIAO block's incoming clock. It is
 * programmed once per power-up: loaden low, skipcfg = 0x10 ("aiaoclk = 100M"),
 * loaden high to latch. With skipcfg left at its reset 0 the AIAO's own I2S
 * clock generators produce no bit clock: a transmit port can be enabled and
 * still never advance its read pointer.
 */
#define CRG_AIAO_CLK			0x118
#define  CRG_AIAO_SKIPCFG		GENMASK(16, 12)
#define  CRG_AIAO_LOADEN		BIT(17)
#define  CRG_AIAO_SKIPCFG_100M		0x10

static void hi3798mv100_setup_aiaoclk(void __iomem *base)
{
	u32 val;

	val = readl(base + CRG_AIAO_CLK) & ~CRG_AIAO_LOADEN;
	writel(val, base + CRG_AIAO_CLK);

	val = (val & ~CRG_AIAO_SKIPCFG) |
	      FIELD_PREP(CRG_AIAO_SKIPCFG, CRG_AIAO_SKIPCFG_100M);
	writel(val, base + CRG_AIAO_CLK);

	writel(val | CRG_AIAO_LOADEN, base + CRG_AIAO_CLK);
}

static struct hisi_clock_data *hi3798mv100_clk_register(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hisi_clock_data *clk_data;
	struct clk *hd_pix;
	int ret;

	clk_data = hisi_clk_alloc(pdev, HI3798MV100_CRG_NR_CLKS);
	if (!clk_data)
		return ERR_PTR(-ENOMEM);

	/* the phase clocks are registered with devm, the rest is not */
	ret = hisi_clk_register_phase(dev, hi3798mv100_phase_clks,
				      ARRAY_SIZE(hi3798mv100_phase_clks),
				      clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = hisi_clk_register_fixed_rate(hi3798mv100_fixed_rate_clks,
					   ARRAY_SIZE(hi3798mv100_fixed_rate_clks),
					   clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = hisi_clk_register_mux(hi3798mv100_mux_clks,
				    ARRAY_SIZE(hi3798mv100_mux_clks), clk_data);
	if (ret)
		goto err_fixed_rate;

	/* registered with devm, so the error path only unwinds the tables */
	hd_pix = hi3798_clk_register_hd_pix(dev, clk_data->base);
	if (IS_ERR(hd_pix)) {
		ret = PTR_ERR(hd_pix);
		goto err_mux;
	}
	clk_data->clk_data.clks[HI3798MV100_HD_PIX] = hd_pix;

	ret = hisi_clk_register_gate(hi3798mv100_gate_clks,
				     ARRAY_SIZE(hi3798mv100_gate_clks),
				     clk_data);
	if (ret)
		goto err_mux;

	ret = hi3798_clk_register_bits(dev, hi3798mv100_bits_clks,
				       ARRAY_SIZE(hi3798mv100_bits_clks),
				       clk_data);
	if (ret)
		goto err_gate;

	hi3798mv100_setup_aiaoclk(clk_data->base);

	ret = of_clk_add_provider(dev->of_node, of_clk_src_onecell_get,
				  &clk_data->clk_data);
	if (ret)
		goto err_gate;

	return clk_data;

err_gate:
	hisi_clk_unregister_gate(hi3798mv100_gate_clks,
				 ARRAY_SIZE(hi3798mv100_gate_clks), clk_data);
err_mux:
	hisi_clk_unregister_mux(hi3798mv100_mux_clks,
				ARRAY_SIZE(hi3798mv100_mux_clks), clk_data);
err_fixed_rate:
	hisi_clk_unregister_fixed_rate(hi3798mv100_fixed_rate_clks,
				       ARRAY_SIZE(hi3798mv100_fixed_rate_clks),
				       clk_data);
	return ERR_PTR(ret);
}

static void hi3798mv100_clk_unregister(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);
	struct hisi_clock_data *clk_data = crg->clk_data;

	of_clk_del_provider(pdev->dev.of_node);

	hisi_clk_unregister_gate(hi3798mv100_gate_clks,
				 ARRAY_SIZE(hi3798mv100_gate_clks), clk_data);
	hisi_clk_unregister_mux(hi3798mv100_mux_clks,
				ARRAY_SIZE(hi3798mv100_mux_clks), clk_data);
	hisi_clk_unregister_fixed_rate(hi3798mv100_fixed_rate_clks,
				       ARRAY_SIZE(hi3798mv100_fixed_rate_clks),
				       clk_data);
}

static const struct hisi_crg_funcs hi3798mv100_crg_funcs = {
	.register_clks = hi3798mv100_clk_register,
	.unregister_clks = hi3798mv100_clk_unregister,
};

static const struct hisi_gate_clock hi3798mv100_sysctrl_gate_clks[] = {
	{ HI3798MV100_IR_CLK, "clk_ir", "24m",
	  CLK_SET_RATE_PARENT, 0x48, 4, 0, },
	{ HI3798MV100_TIMER01_CLK, "clk_timer01", "24m",
	  CLK_SET_RATE_PARENT, 0x48, 6, 0, },
	{ HI3798MV100_UART0_CLK, "clk_uart0", "83p3m",
	  CLK_SET_RATE_PARENT, 0x48, 12, 0, },
};

/*
 * The parents of the system controller gates are the fixed clocks of the core
 * CRG. The clock framework resolves a by-name parent when the consumer has no
 * clocks listed in its own device tree node, so the two providers can be probed
 * in either order.
 */
static struct hisi_clock_data *hi3798mv100_sysctrl_clk_register(struct platform_device *pdev)
{
	struct hisi_clock_data *clk_data;
	int ret;

	clk_data = hisi_clk_alloc(pdev, HI3798MV100_SYSCTRL_CLK_COUNT);
	if (!clk_data)
		return ERR_PTR(-ENOMEM);

	ret = hisi_clk_register_gate(hi3798mv100_sysctrl_gate_clks,
				     ARRAY_SIZE(hi3798mv100_sysctrl_gate_clks),
				     clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get,
				  &clk_data->clk_data);
	if (ret) {
		hisi_clk_unregister_gate(hi3798mv100_sysctrl_gate_clks,
					 ARRAY_SIZE(hi3798mv100_sysctrl_gate_clks),
					 clk_data);
		return ERR_PTR(ret);
	}

	return clk_data;
}

static void hi3798mv100_sysctrl_clk_unregister(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);

	of_clk_del_provider(pdev->dev.of_node);

	hisi_clk_unregister_gate(hi3798mv100_sysctrl_gate_clks,
				 ARRAY_SIZE(hi3798mv100_sysctrl_gate_clks),
				 crg->clk_data);
}

static const struct hisi_crg_funcs hi3798mv100_sysctrl_funcs = {
	.register_clks = hi3798mv100_sysctrl_clk_register,
	.unregister_clks = hi3798mv100_sysctrl_clk_unregister,
};

static const struct of_device_id hi3798mv100_crg_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-crg",
	  .data = &hi3798mv100_crg_funcs, },
	{ .compatible = "hisilicon,hi3798mv100-sysctrl",
	  .data = &hi3798mv100_sysctrl_funcs, },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, hi3798mv100_crg_of_match);

static int hi3798mv100_crg_probe(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg;

	crg = devm_kmalloc(&pdev->dev, sizeof(*crg), GFP_KERNEL);
	if (!crg)
		return -ENOMEM;

	crg->funcs = of_device_get_match_data(&pdev->dev);
	if (!crg->funcs)
		return -ENOENT;

	crg->rstc = hisi_reset_init(pdev);
	if (!crg->rstc)
		return -ENOMEM;

	crg->clk_data = crg->funcs->register_clks(pdev);
	if (IS_ERR(crg->clk_data)) {
		hisi_reset_exit(crg->rstc);
		return PTR_ERR(crg->clk_data);
	}

	platform_set_drvdata(pdev, crg);

	return 0;
}

static void hi3798mv100_crg_remove(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);

	hisi_reset_exit(crg->rstc);
	crg->funcs->unregister_clks(pdev);
}

static struct platform_driver hi3798mv100_crg_driver = {
	.probe = hi3798mv100_crg_probe,
	.remove = hi3798mv100_crg_remove,
	.driver = {
		.name = "hi3798mv100-crg",
		.of_match_table = hi3798mv100_crg_of_match,
	},
};

/*
 * The clock tree of this SoC has to be up before the peripherals which use it
 * probe, and before the SMP code needs the reset lines.
 */
static int __init hi3798mv100_crg_init(void)
{
	return platform_driver_register(&hi3798mv100_crg_driver);
}
core_initcall(hi3798mv100_crg_init);

static void __exit hi3798mv100_crg_exit(void)
{
	platform_driver_unregister(&hi3798mv100_crg_driver);
}
module_exit(hi3798mv100_crg_exit);

MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_DESCRIPTION("HiSilicon Hi3798MV100 CRG driver");
MODULE_LICENSE("GPL");
