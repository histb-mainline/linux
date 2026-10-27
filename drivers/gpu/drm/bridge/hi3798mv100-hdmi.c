// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023 David Yang
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "sii9134.h"

struct hi3798mv100_hdmi_map {
	void __iomem *base;
	unsigned int max_offset;
};

struct hi3798mv100_hdmi_priv {
	void __iomem *base;

	struct clk *clk_ctrl;
	struct reset_control *rst;
	struct reset_control *rst_sii9134;
	struct clk *clk_phy;
	struct reset_control *rst_phy;

	struct hi3798mv100_hdmi_map map0;
	struct hi3798mv100_hdmi_map map1;
	struct hi3798mv100_hdmi_map map_phy;

	struct sii9134 sii9134;
};

/*
 * One config per bank: regmap debugfs is keyed on device+name. Register numbers
 * are byte offsets into the window (SiI9134 register N sits at base + 4 * N);
 * the custom bus below performs the access width the wrapper actually decodes.
 */
static const struct regmap_config hi3798mv100_hdmi_bank0_regmap_config = {
	.reg_bits = 32,
	.val_bits = 8,
	.name = "bank0",
	.max_register = 0x3fc,
	.cache_type = REGCACHE_NONE,
};

static const struct regmap_config hi3798mv100_hdmi_bank1_regmap_config = {
	.reg_bits = 32,
	.val_bits = 8,
	.name = "bank1",
	.max_register = 0x3fc,
	.cache_type = REGCACHE_NONE,
};

/* on-chip "Taiyan" TMDS PHY (hi3798mv100): 32-bit regs at base + 0x1800 */
static const struct regmap_config hi3798mv100_hdmi_phy_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.name = "phy",
	.max_register = 0x38,
	.cache_type = REGCACHE_NONE,
};

/*
 * The TX wrapper only answers full-width accesses: byte strobes are ignored, so
 * an 8-bit value needs a 32-bit read of the word and a 32-bit write whose low
 * byte carries the register value. regmap-mmio cannot express that -- it
 * demands reg_stride >= 4 for 32-bit values, and the core then rejects every
 * register number that is not a multiple of the stride.
 */
static int hi3798mv100_hdmi_regmap_read(void *context, const void *reg, size_t reg_len,
				  void *val, size_t val_len)
{
	struct hi3798mv100_hdmi_map *map = context;
	unsigned int offset = get_unaligned_le32(reg);
	u32 word;

	if (reg_len != 4 || (val_len != 1 && val_len != 4))
		return -EIO;

	if (offset > map->max_offset) {
		pr_warn_once("hi3798mv100-hdmi: read out of window: off=%#x len=%zu\n",
			     offset, val_len);
		return -EINVAL;
	}

	word = readl(map->base + offset);
	if (val_len == 1)
		*(u8 *)val = word & 0xff;
	else
		put_unaligned_le32(word, val);

	return 0;
}

static int hi3798mv100_hdmi_regmap_write(void *context, const void *data, size_t count)
{
	struct hi3798mv100_hdmi_map *map = context;
	const u8 *buf = data;
	unsigned int offset = get_unaligned_le32(buf);
	size_t val_len = count - 4;
	u32 word;

	if (count < 5 || val_len > 4)
		return -EIO;

	if (offset > map->max_offset) {
		pr_warn_once("hi3798mv100-hdmi: write out of window: off=%#x len=%zu\n",
			     offset, val_len);
		return -EINVAL;
	}

	word = val_len == 4 ? get_unaligned_le32(buf + 4) : buf[4];
	writel(word, map->base + offset);

	return 0;
}

static const struct regmap_bus hi3798mv100_hdmi_regmap_bus = {
	.read = hi3798mv100_hdmi_regmap_read,
	.write = hi3798mv100_hdmi_regmap_write,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

/*
 * Take the transmitter through a reset of our own instead of trusting the state
 * it was handed over in, and do it in a split order: hold controller and PHY,
 * release the PHY so the bridge core can configure it, and only then release
 * the controller with 10 ms of settling for the TMDS PLL. A narrow
 * assert/deassert pair with nothing in between does not reset anything, so the
 * delays are part of the contract.
 *
 * Both halves are clocked before this runs: the gates of PERI_CRG67[5:0] and
 * PERI_CRG68[0] are the clocks "ctrl" and "phy", enabled from
 * hi3798mv100_hdmi_runtime_resume(), because a reset request only reaches a
 * block while it is clocked. Whether the block answers afterwards is visible in
 * the register dump that follows in sii9134_probe(): an unclocked or still-held
 * transmitter reads back zero.
 */
static void hi3798mv100_hdmi_reset_hold(struct sii9134 *sii)
{
	struct hi3798mv100_hdmi_priv *priv =
		container_of(sii, struct hi3798mv100_hdmi_priv, sii9134);

	reset_control_assert(priv->rst_phy);
	reset_control_assert(priv->rst_sii9134);
	reset_control_assert(priv->rst);
	usleep_range(1000, 1200);

	reset_control_deassert(priv->rst_phy);
	usleep_range(1000, 1200);
}

static void hi3798mv100_hdmi_reset_release(struct sii9134 *sii)
{
	struct hi3798mv100_hdmi_priv *priv =
		container_of(sii, struct hi3798mv100_hdmi_priv, sii9134);

	reset_control_deassert(priv->rst_sii9134);
	reset_control_deassert(priv->rst);
	/*
	 * releasing the controller reset waits 10 ms for the TMDS PLL to lock
	 */
	usleep_range(10000, 12000);
}

/* PHY ("Taiyan" PHY of hi3798mv100: 32-bit registers at base + 0x1800) */
#define PHY_OE_ADDR		0x0	/* reg 0x00 */
#define RG_TX_RSTB		0x01
#define PHY_PWD_ADDR		0x4	/* reg 0x01 */
#define RG_TX_EN		0x01
#define PHY_AUD_ADDR		0x8	/* reg 0x02 */
#define PHY_PLL1_ADDR		0xc	/* reg 0x03 */
#define PHY_PLL2_ADDR		0x10	/* reg 0x04 */
#define PHY_DRV_ADDR		0x14	/* reg 0x05 */
#define PHY_CLK_ADDR		0x18	/* reg 0x06 */

static int hi3798mv100_hdmi_phy_write(struct sii9134 *sii, unsigned int reg, u32 val)
{
	return regmap_write(sii->map_phy, reg, val);
}

static int hi3798mv100_hdmi_phy_read(struct sii9134 *sii, unsigned int reg, u32 *val)
{
	return regmap_read(sii->map_phy, reg, val);
}

static int hi3798mv100_hdmi_phy_update(struct sii9134 *sii, unsigned int reg,
			      u32 mask, u32 val)
{
	unsigned int tmp;
	int ret;

	ret = hi3798mv100_hdmi_phy_read(sii, reg, &tmp);
	if (ret)
		return ret;

	tmp &= ~mask;
	tmp |= val;

	return hi3798mv100_hdmi_phy_write(sii, reg, tmp);
}

/* ------------------------------------------------------------------ */
/* PHY                                                                  */
/* ------------------------------------------------------------------ */

static void hi3798mv100_hdmi_phy_init(struct sii9134 *sii)
{
	/*
	 * PHY bring-up sequence for the integrated ("Taiyan") PHY; these values
	 * are per SoC family and are not in the SiI9134 datasheet.
	 */
	hi3798mv100_hdmi_phy_write(sii, PHY_OE_ADDR, 0x00);
	/*
	 * audio clock select: the I2S path uses a fixed source, so 0x02 is
	 * always right.
	 */
	hi3798mv100_hdmi_phy_write(sii, PHY_AUD_ADDR, 0x02);
	hi3798mv100_hdmi_phy_write(sii, PHY_PLL1_ADDR, 0x02);
	hi3798mv100_hdmi_phy_write(sii, PHY_PLL2_ADDR, 0x09);
	hi3798mv100_hdmi_phy_write(sii, PHY_DRV_ADDR, 0x01);
	hi3798mv100_hdmi_phy_write(sii, PHY_CLK_ADDR, 0x00);
	/* power up last, improves PLL stability */
	hi3798mv100_hdmi_phy_write(sii, PHY_PWD_ADDR, RG_TX_EN);
}

static void hi3798mv100_hdmi_phy_power(struct sii9134 *sii, bool on)
{
	if (on)
		hi3798mv100_hdmi_phy_update(sii, PHY_OE_ADDR, RG_TX_RSTB, RG_TX_RSTB);
	else
		hi3798mv100_hdmi_phy_update(sii, PHY_OE_ADDR, RG_TX_RSTB, 0);
}

/*
 * Output swing / PLL drive strength depends on the TMDS clock rate:
 * <= 27MHz -> 0x00, <= 165MHz -> 0x01, above that -> 0x03.
 * @tmds_khz is the TMDS clock in kHz (equal to the pixel clock at
 * 8 bits per channel).
 */
static void hi3798mv100_hdmi_phy_swing(struct sii9134 *sii, unsigned int tmds_khz)
{
	unsigned int mhz = tmds_khz / 1000;
	unsigned int val;

	hi3798mv100_hdmi_phy_read(sii, PHY_PLL1_ADDR, &val);
	val &= ~0x03;

	if (mhz <= 27)
		val |= 0x00;
	else if (mhz <= 165)
		val |= 0x01;
	else
		val |= 0x03;

	hi3798mv100_hdmi_phy_write(sii, PHY_PLL1_ADDR, val);
}

static const struct sii9134_phy_ops hi3798mv100_hdmi_phy_ops = {
	.init = hi3798mv100_hdmi_phy_init,
	.power = hi3798mv100_hdmi_phy_power,
	.swing = hi3798mv100_hdmi_phy_swing,
	.dvi_enc_in_ctrl = true,
};

/*
 * Hi3716CV200 PHY.
 *
 * Same register file at base + 0x1800, word-strided like the bank windows, but
 * the bit assignment and the whole bring-up belong to a different generation:
 * the vendor keeps them in two #if branches of the same functions
 * (hdmi_1_4/hdmi_drv/si_phy.c), and the branch for this part writes ten
 * registers the mv100 branch never touches -- including the PLL control and the
 * bias generator -- and then resets the PLL by asserting power-down. Reusing the
 * mv100 values here is what makes the transmitter measure its own input timing
 * (hres/vres nonzero) while the TMDS clock never locks.
 *
 * The mv100 names above describe this part's numbering differently, so these
 * macros are indexed by the vendor's own register number and named after the
 * comment the vendor puts next to it.
 */
#define CV200_PHY_PLL_CTRL	(0x02 * 4)	/* deep colour in bits 1..0 */
#define CV200_PHY_OE		(0x05 * 4)	/* oe bit 5, power-down bit 4 */
#define CV200_PHY_CTRL2		(0x06 * 4)	/* rising time in bits 7..6 */
#define CV200_PHY_CTRL3		(0x07 * 4)
#define CV200_PHY_CTRL4		(0x08 * 4)
#define CV200_PHY_CTRL5		(0x09 * 4)
#define CV200_PHY_BIAS1		(0x0a * 4)
#define CV200_PHY_BIAS2		(0x0b * 4)
#define CV200_PHY_ENC_BYPASS	(0x0d * 4)
#define CV200_PHY_TERM_EN	(0x0e * 4)

#define CV200_RG_OE		BIT(5)
#define CV200_RG_PWR_UP	BIT(4)

static void hi3716cv200_hdmi_phy_init(struct sii9134 *sii)
{
	/*
	 * si_phy.c:193-232. Register 0x09 depends on the die revision: the
	 * vendor branches on bits 15..8 of 0xf8a9007c -- 0x41 is the "EC" part
	 * whose PHY is the 19 m one and gets 0x00, 0x40 is the original and gets
	 * 0x1a, and an unknown tag falls back to the original
	 * (boot/product/driver/hdmi/hdmi_1_4/phy.c:9-32). This board's tag was
	 * measured as 0x40 from its own boot system, so 0x1a is what the vendor
	 * would write here.
	 */
	hi3798mv100_hdmi_phy_write(sii, CV200_PHY_CTRL2, 0x89);
	hi3798mv100_hdmi_phy_write(sii, CV200_PHY_TERM_EN, 0x00);
	hi3798mv100_hdmi_phy_write(sii, CV200_PHY_CTRL3, 0x81);
	hi3798mv100_hdmi_phy_write(sii, CV200_PHY_CTRL5, 0x1a);
	hi3798mv100_hdmi_phy_write(sii, CV200_PHY_BIAS1, 0x07);
	hi3798mv100_hdmi_phy_write(sii, CV200_PHY_BIAS2, 0x51);
	hi3798mv100_hdmi_phy_write(sii, CV200_PHY_PLL_CTRL, 0x24);
	/*
	 * oe stays off at bring-up (bit 5 clear in 0x12); bit 4 is the power-up
	 * that the reset below takes away again on purpose.
	 */
	hi3798mv100_hdmi_phy_write(sii, CV200_PHY_OE, 0x12);
	hi3798mv100_hdmi_phy_write(sii, CV200_PHY_CTRL4, 0x40);
	hi3798mv100_hdmi_phy_write(sii, CV200_PHY_ENC_BYPASS, 0x00);

	/* "after cfg phy ok, we need reset phy pll" (si_phy.c:231-233) */
	hi3798mv100_hdmi_phy_update(sii, CV200_PHY_OE, CV200_RG_PWR_UP, 0);
	mdelay(1);
}

static void hi3716cv200_hdmi_phy_power(struct sii9134 *sii, bool on)
{
	/*
	 * The PHY's power-up is bit 4 and its output enable bit 5 of one word
	 * (si_phy.c:262-308 and :146-179), so releasing the reset has to come
	 * first: enabling the output while the PLL is still powered down is the
	 * one order this part does not accept.
	 */
	if (on) {
		hi3798mv100_hdmi_phy_update(sii, CV200_PHY_OE, CV200_RG_PWR_UP,
					    CV200_RG_PWR_UP);
		hi3798mv100_hdmi_phy_update(sii, CV200_PHY_OE, CV200_RG_OE,
					    CV200_RG_OE);
	} else {
		hi3798mv100_hdmi_phy_update(sii, CV200_PHY_OE, CV200_RG_OE, 0);
		hi3798mv100_hdmi_phy_update(sii, CV200_PHY_OE, CV200_RG_PWR_UP, 0);
	}
}

/*
 * The swing/divider helper for this PHY generation is empty: si_phy.c:453-468
 * has no body in the cv200 branch (the drive strength is fixed by the bias
 * registers set at bring-up), and the rate-dependent code below it belongs to
 * the mv100 "taiyan" PHY. Writing the mv100 PLL1 bits here would touch a
 * register the vendor leaves alone.
 */
static void hi3716cv200_hdmi_phy_swing(struct sii9134 *sii, unsigned int tmds_khz)
{
}

static const struct sii9134_phy_ops hi3716cv200_hdmi_phy_ops = {
	.init = hi3716cv200_hdmi_phy_init,
	.power = hi3716cv200_hdmi_phy_power,
	.swing = hi3716cv200_hdmi_phy_swing,
	.edge_falling = true,
	/*
	 * No .dvi_enc_in_ctrl here: this part owns the encoder on the PHY side, so
	 * TEST_TX.DVI_ENC_BYPASS has to stay as reset left it. The vendor SDK never
	 * writes TEST_TX on cv200 -- SI_TX_CTRL_DVIEncoder(), the only writer of that
	 * bit, compiles its body for the mv100 family only (si_hdmitx.c:767-793), and
	 * InitTestCtrlReg() returns before its #if 0 block (si_hdmitx.c:510-525) --
	 * and its live page-1 TEST_TX reads 0x08 (BIT_DVI_ENC_BYPASS,
	 * si_defstx.h:268) while it displays. Measured on the board: with the bit
	 * cleared, as copying the mv100 flag did, the sink saw nothing but the
	 * transmitter's own timing; setting it back by hand changed every frame the
	 * capture card produced in that window.
	 */
};

static int hi3798mv100_hdmi_runtime_suspend(struct device *dev)
{
	struct hi3798mv100_hdmi_priv *priv = dev_get_drvdata(dev);

	clk_disable_unprepare(priv->clk_phy);
	clk_disable_unprepare(priv->clk_ctrl);

	return 0;
}

static int hi3798mv100_hdmi_runtime_resume(struct device *dev)
{
	struct hi3798mv100_hdmi_priv *priv = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(priv->clk_ctrl);
	if (ret)
		return ret;

	ret = clk_prepare_enable(priv->clk_phy);
	if (ret) {
		clk_disable_unprepare(priv->clk_ctrl);
		return ret;
	}

	return 0;
}

static const struct dev_pm_ops hi3798mv100_hdmi_pm_ops = {
	RUNTIME_PM_OPS(hi3798mv100_hdmi_runtime_suspend,
		       hi3798mv100_hdmi_runtime_resume, NULL)
};

static void hi3798mv100_hdmi_pm_cleanup(void *data)
{
	struct device *dev = data;
	struct hi3798mv100_hdmi_priv *priv = dev_get_drvdata(dev);

	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);	/* balance the always-on reference */
	clk_disable_unprepare(priv->clk_phy);
	clk_disable_unprepare(priv->clk_ctrl);
}

/*
 * Binding to the aggregate is what publishes the bridge: the master's .bind()
 * runs component_bind_all() before it looks for its downstream bridge through
 * the device tree, so the bridge is on the global list exactly while some DRM
 * device can attach to it. Unbinding takes it away again after the master has
 * stopped using it.
 */
static int hi3798mv100_hdmi_component_bind(struct device *comp,
					struct device *master,
					void *master_data)
{
	struct hi3798mv100_hdmi_priv *priv = dev_get_drvdata(comp);

	sii9134_bridge_add(&priv->sii9134);
	return 0;
}

static void hi3798mv100_hdmi_component_unbind(struct device *comp,
					   struct device *master,
					   void *master_data)
{
	struct hi3798mv100_hdmi_priv *priv = dev_get_drvdata(comp);

	sii9134_bridge_remove(&priv->sii9134);
}

static const struct component_ops hi3798mv100_hdmi_component_ops = {
	.bind	= hi3798mv100_hdmi_component_bind,
	.unbind	= hi3798mv100_hdmi_component_unbind,
};

static int hi3798mv100_hdmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi3798mv100_hdmi_priv *priv;
	int irq;
	int ret;

	/* acquire resources */
	priv = devm_drm_bridge_alloc(dev, struct hi3798mv100_hdmi_priv,
				     sii9134.bridge, &sii9134_bridge_funcs);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	/*
	 * "ctrl" is the PERI_CRG67[5:0] gate group and "phy" PERI_CRG68[0];
	 * both have to be running before the resets below shift, so none of
	 * these is optional. The resets are CRG67[9] (transmitter core),
	 * CRG67[8] (its bus) and CRG68[4] (PHY).
	 */
	priv->clk_ctrl = devm_clk_get(dev, "ctrl");
	if (IS_ERR(priv->clk_ctrl))
		return dev_err_probe(dev, PTR_ERR(priv->clk_ctrl),
				     "no ctrl clock\n");

	priv->rst = devm_reset_control_get(dev, "base");
	if (IS_ERR(priv->rst))
		return dev_err_probe(dev, PTR_ERR(priv->rst), "no base reset\n");

	priv->rst_sii9134 = devm_reset_control_get(dev, "bus");
	if (IS_ERR(priv->rst_sii9134))
		return dev_err_probe(dev, PTR_ERR(priv->rst_sii9134),
				     "no bus reset\n");

	priv->clk_phy = devm_clk_get(dev, "phy");
	if (IS_ERR(priv->clk_phy))
		return dev_err_probe(dev, PTR_ERR(priv->clk_phy),
				     "no phy clock\n");

	priv->rst_phy = devm_reset_control_get(dev, "phy");
	if (IS_ERR(priv->rst_phy))
		return dev_err_probe(dev, PTR_ERR(priv->rst_phy),
				     "no phy reset\n");

	/*
	 * The SiI9134 INT output is level and storms while a sink holds HPD, so
	 * the driver polls instead of using interrupts.
	 */
	irq = -1;

	platform_set_drvdata(pdev, priv);
	dev_set_drvdata(dev, priv);

	/* bring up device */
	ret = hi3798mv100_hdmi_runtime_resume(dev);
	if (ret)
		return ret;

	/*
	 * Keep the device powered for its whole probe/remove lifetime: the
	 * connector poll work, EDID reads and the codec DAI callbacks all reach
	 * these registers without holding a runtime PM reference of their own.
	 * hi3798mv100_hdmi_runtime_resume() is therefore called directly from
	 * probe below; neither idle gating nor pm_runtime_force_suspend() at
	 * system sleep is in place yet.
	 */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_get(dev);
	ret = devm_add_action_or_reset(dev, hi3798mv100_hdmi_pm_cleanup, dev);
	if (ret)
		return ret;

	priv->map0.base = priv->base;
	priv->map0.max_offset = 0x3fc;
	priv->map1.base = priv->base + 0x400;
	priv->map1.max_offset = 0x3fc;
	priv->map_phy.base = priv->base + 0x1800;
	priv->map_phy.max_offset = 0x38;

	priv->sii9134.map = devm_regmap_init(dev, &hi3798mv100_hdmi_regmap_bus,
					     &priv->map0,
					     &hi3798mv100_hdmi_bank0_regmap_config);
	if (IS_ERR(priv->sii9134.map))
		return PTR_ERR(priv->sii9134.map);

	priv->sii9134.map1 = devm_regmap_init(dev, &hi3798mv100_hdmi_regmap_bus,
					      &priv->map1,
					      &hi3798mv100_hdmi_bank1_regmap_config);
	if (IS_ERR(priv->sii9134.map1))
		return PTR_ERR(priv->sii9134.map1);

	priv->sii9134.map_phy = devm_regmap_init(dev, &hi3798mv100_hdmi_regmap_bus,
						 &priv->map_phy,
						 &hi3798mv100_hdmi_phy_regmap_config);
	if (IS_ERR(priv->sii9134.map_phy))
		return PTR_ERR(priv->sii9134.map_phy);

	priv->sii9134.dev = dev;
	priv->sii9134.reset_hold = hi3798mv100_hdmi_reset_hold;
	priv->sii9134.reset_release = hi3798mv100_hdmi_reset_release;
	/*
	 * The PHY is the only part whose programming differs between the two
	 * supported parts; the SiI9134 controller side is shared.
	 */
	if (of_device_is_compatible(dev->of_node, "hisilicon,hi3716cv200-hdmi"))
		priv->sii9134.phy = &hi3716cv200_hdmi_phy_ops;
	else
		priv->sii9134.phy = &hi3798mv100_hdmi_phy_ops;
	ret = sii9134_probe(&priv->sii9134, irq);
	if (ret)
		return ret;

	return component_add(dev, &hi3798mv100_hdmi_component_ops);
}

static void hi3798mv100_hdmi_remove(struct platform_device *pdev)
{
	struct hi3798mv100_hdmi_priv *priv = platform_get_drvdata(pdev);

	/*
	 * component_del() first: it brings the DRM device that attached this
	 * bridge down. Then the bridge core goes, and the devm action
	 * (hi3798mv100_hdmi_pm_cleanup) releases the always-on clocks, runtime
	 * PM and the devm-managed bridge behind it.
	 */
	component_del(&pdev->dev, &hi3798mv100_hdmi_component_ops);
	sii9134_remove(&priv->sii9134);
}

static const struct of_device_id hi3798mv100_hdmi_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-hdmi", },
	{ .compatible = "hisilicon,hi3716cv200-hdmi" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3798mv100_hdmi_of_match);

static struct platform_driver hi3798mv100_hdmi_driver = {
	.probe = hi3798mv100_hdmi_probe,
	.remove = hi3798mv100_hdmi_remove,
	.driver = {
		.name = "hi3798mv100-hdmi",
		.of_match_table = hi3798mv100_hdmi_of_match,
		.pm = pm_ptr(&hi3798mv100_hdmi_pm_ops),
	},
};

module_platform_driver(hi3798mv100_hdmi_driver);

MODULE_DESCRIPTION("SiI9134 HDMI 1.4 transmitter for Hi3798MV100");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
