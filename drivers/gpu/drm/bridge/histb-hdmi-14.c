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

struct histb_hdmi_map {
	void __iomem *base;
	unsigned int max_offset;
};

struct histb_hdmi_14_priv {
	void __iomem *base;

	struct clk *clk_ctrl;
	struct reset_control *rst;
	struct reset_control *rst_sii9134;
	struct clk *clk_phy;
	struct reset_control *rst_phy;

	struct histb_hdmi_map map0;
	struct histb_hdmi_map map1;
	struct histb_hdmi_map map_phy;

	struct sii9134 sii9134;
};

/*
 * One config per bank: regmap debugfs is keyed on device+name.  Register
 * numbers are byte offsets into the window (SiI9134 register N sits at
 * base + 4 * N); the custom bus below performs the access width the wrapper
 * actually decodes.
 */
static const struct regmap_config histb_hdmi_14_bank0_regmap_config = {
	.reg_bits = 32,
	.val_bits = 8,
	.name = "bank0",
	.max_register = 0x3fc,
	.cache_type = REGCACHE_NONE,
};

static const struct regmap_config histb_hdmi_14_bank1_regmap_config = {
	.reg_bits = 32,
	.val_bits = 8,
	.name = "bank1",
	.max_register = 0x3fc,
	.cache_type = REGCACHE_NONE,
};

/* on-chip HDMI PHY ("Taiyan", hi3798mv100): 32-bit regs at base + 0x1800 */
static const struct regmap_config histb_hdmi_14_phy_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.name = "phy",
	.max_register = 0x38,
	.cache_type = REGCACHE_NONE,
};

/*
 * The TX wrapper only answers full-width accesses: byte strobes are ignored,
 * so an 8-bit value needs a 32-bit read of the word and a 32-bit write whose
 * low byte carries the register value (exactly what the SDK's
 * DRV_ReadByte_8BA()/DRV_WriteByte_8BA() do).  regmap-mmio cannot express
 * that -- it demands reg_stride >= 4 for 32-bit values, and the core then
 * rejects every register number that is not a multiple of the stride.
 */
static int histb_hdmi_regmap_read(void *context, const void *reg, size_t reg_len,
				  void *val, size_t val_len)
{
	struct histb_hdmi_map *map = context;
	unsigned int offset = get_unaligned_le32(reg);
	u32 word;

	if (reg_len != 4 || (val_len != 1 && val_len != 4))
		return -EIO;

	if (offset > map->max_offset) {
		pr_warn_once("histb-hdmi: read out of window: off=%#x len=%zu\n",
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

static int histb_hdmi_regmap_write(void *context, const void *data, size_t count)
{
	struct histb_hdmi_map *map = context;
	const u8 *buf = data;
	unsigned int offset = get_unaligned_le32(buf);
	size_t val_len = count - 4;
	u32 word;

	if (count < 5 || val_len > 4)
		return -EIO;

	if (offset > map->max_offset) {
		pr_warn_once("histb-hdmi: write out of window: off=%#x len=%zu\n",
			     offset, val_len);
		return -EINVAL;
	}

	word = val_len == 4 ? get_unaligned_le32(buf + 4) : buf[4];
	writel(word, map->base + offset);

	return 0;
}

static const struct regmap_bus histb_hdmi_regmap_bus = {
	.read = histb_hdmi_regmap_read,
	.write = histb_hdmi_regmap_write,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

/*
 * Take the transmitter through a reset of our own instead of trusting the
 * state the boot handed over, in the SDK's split order
 * (SI_HW_ResetHDMITX(), si_hdmitx.c:164-200): hold controller and PHY,
 * release the PHY so the bridge core can configure it, and only then release
 * the controller with 10 ms of settling for the TMDS PLL.  A narrow
 * assert/deassert pair with nothing in between does not reset anything, so
 * the delays are part of the contract.
 *
 * Both halves are clocked before this runs: the gates of PERI_CRG67[5:0] and
 * PERI_CRG68[0] are the clocks "ctrl" and "phy", enabled from
 * histb_hdmi_14_runtime_resume(), because a reset request only reaches a
 * block while it is clocked and the vendor's USB boot path hands the
 * transmitter over gated (CRG67 = 0x300, CRG68 = 0x10).  Whether the block
 * answers afterwards is visible in the register dump that follows in
 * sii9134_probe(): an unclocked or still-held transmitter reads back zero.
 */
static void histb_hdmi_14_reset_hold(struct sii9134 *sii)
{
	struct histb_hdmi_14_priv *priv =
		container_of(sii, struct histb_hdmi_14_priv, sii9134);

	reset_control_assert(priv->rst_phy);
	reset_control_assert(priv->rst_sii9134);
	reset_control_assert(priv->rst);
	usleep_range(1000, 1200);

	reset_control_deassert(priv->rst_phy);
	usleep_range(1000, 1200);
}

static void histb_hdmi_14_reset_release(struct sii9134 *sii)
{
	struct histb_hdmi_14_priv *priv =
		container_of(sii, struct histb_hdmi_14_priv, sii9134);

	reset_control_deassert(priv->rst_sii9134);
	reset_control_deassert(priv->rst);
	/* SI_HW_ResetCtrl(0) waits 10 ms for the TMDS PLL to lock */
	usleep_range(10000, 12000);
}

static int histb_hdmi_14_runtime_suspend(struct device *dev)
{
	struct histb_hdmi_14_priv *priv = dev_get_drvdata(dev);

	clk_disable_unprepare(priv->clk_phy);
	clk_disable_unprepare(priv->clk_ctrl);

	return 0;
}

static int histb_hdmi_14_runtime_resume(struct device *dev)
{
	struct histb_hdmi_14_priv *priv = dev_get_drvdata(dev);
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

static const struct dev_pm_ops histb_hdmi_14_pm_ops = {
	RUNTIME_PM_OPS(histb_hdmi_14_runtime_suspend,
		       histb_hdmi_14_runtime_resume, NULL)
};

static void histb_hdmi_14_pm_cleanup(void *data)
{
	struct device *dev = data;
	struct histb_hdmi_14_priv *priv = dev_get_drvdata(dev);

	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);	/* balance the always-on reference */
	clk_disable_unprepare(priv->clk_phy);
	clk_disable_unprepare(priv->clk_ctrl);
}

/*
 * Binding to the aggregate is what publishes the bridge: the master's
 * .bind() runs component_bind_all() before it looks for its downstream
 * bridge through the device tree, so the bridge is on the global list
 * exactly while some DRM device can attach to it.  Unbinding takes it away
 * again after the master has stopped using it.
 */
static int histb_hdmi_14_component_bind(struct device *comp,
					struct device *master,
					void *master_data)
{
	struct histb_hdmi_14_priv *priv = dev_get_drvdata(comp);

	sii9134_bridge_add(&priv->sii9134);
	return 0;
}

static void histb_hdmi_14_component_unbind(struct device *comp,
					   struct device *master,
					   void *master_data)
{
	struct histb_hdmi_14_priv *priv = dev_get_drvdata(comp);

	sii9134_bridge_remove(&priv->sii9134);
}

static const struct component_ops histb_hdmi_14_component_ops = {
	.bind	= histb_hdmi_14_component_bind,
	.unbind	= histb_hdmi_14_component_unbind,
};

static int histb_hdmi_14_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_hdmi_14_priv *priv;
	int irq;
	int ret;

	/* acquire resources */
	priv = devm_drm_bridge_alloc(dev, struct histb_hdmi_14_priv,
				     sii9134.bridge, &sii9134_bridge_funcs);
	if (IS_ERR(priv))
		return PTR_ERR(priv);

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	/*
	 * "ctrl" is the PERI_CRG67[5:0] gate group and "phy" PERI_CRG68[0];
	 * both have to be running before the resets below shift, so none of
	 * these is optional.  The resets are CRG67[9] (transmitter core),
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
	 * The SiI9134 INT output is level and storms while a sink holds
	 * HPD, so the driver polls instead of using interrupts (same as
	 * the SDK's production path).
	 */
	irq = -1;

	platform_set_drvdata(pdev, priv);
	dev_set_drvdata(dev, priv);

	/* bring up device */
	ret = histb_hdmi_14_runtime_resume(dev);
	if (ret)
		return ret;

	/*
	 * Keep the device powered for its whole probe/remove lifetime: the
	 * connector poll work, EDID reads and the codec DAI callbacks all
	 * reach these registers without holding a runtime PM reference of
	 * their own.  histb_hdmi_14_runtime_resume() is therefore called
	 * directly from probe below; neither idle gating nor
	 * pm_runtime_force_suspend() at system sleep is in place yet.
	 */
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_get(dev);
	ret = devm_add_action_or_reset(dev, histb_hdmi_14_pm_cleanup, dev);
	if (ret)
		return ret;

	priv->map0.base = priv->base;
	priv->map0.max_offset = 0x3fc;
	priv->map1.base = priv->base + 0x400;
	priv->map1.max_offset = 0x3fc;
	priv->map_phy.base = priv->base + 0x1800;
	priv->map_phy.max_offset = 0x38;

	priv->sii9134.map = devm_regmap_init(dev, &histb_hdmi_regmap_bus,
					     &priv->map0,
					     &histb_hdmi_14_bank0_regmap_config);
	if (IS_ERR(priv->sii9134.map))
		return PTR_ERR(priv->sii9134.map);

	priv->sii9134.map1 = devm_regmap_init(dev, &histb_hdmi_regmap_bus,
					      &priv->map1,
					      &histb_hdmi_14_bank1_regmap_config);
	if (IS_ERR(priv->sii9134.map1))
		return PTR_ERR(priv->sii9134.map1);

	priv->sii9134.map_phy = devm_regmap_init(dev, &histb_hdmi_regmap_bus,
						 &priv->map_phy,
						 &histb_hdmi_14_phy_regmap_config);
	if (IS_ERR(priv->sii9134.map_phy))
		return PTR_ERR(priv->sii9134.map_phy);

	priv->sii9134.dev = dev;
	priv->sii9134.reset_hold = histb_hdmi_14_reset_hold;
	priv->sii9134.reset_release = histb_hdmi_14_reset_release;
	ret = sii9134_probe(&priv->sii9134, irq);
	if (ret)
		return ret;

	return component_add(dev, &histb_hdmi_14_component_ops);
}

static void histb_hdmi_14_remove(struct platform_device *pdev)
{
	struct histb_hdmi_14_priv *priv = platform_get_drvdata(pdev);

	/*
	 * component_del() first: it brings the DRM device that attached this
	 * bridge down.  Then the bridge core goes, and the devm action
	 * (histb_hdmi_14_pm_cleanup) releases the always-on clocks, runtime PM
	 * and the devm-managed bridge behind it.
	 */
	component_del(&pdev->dev, &histb_hdmi_14_component_ops);
	sii9134_remove(&priv->sii9134);
}

static const struct of_device_id histb_hdmi_14_of_match[] = {
	{ .compatible = "hisilicon,histb-hdmi-1-4", },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_hdmi_14_of_match);

static struct platform_driver histb_hdmi_14_driver = {
	.probe = histb_hdmi_14_probe,
	.remove = histb_hdmi_14_remove,
	.driver = {
		.name = "histb-hdmi-14",
		.of_match_table = histb_hdmi_14_of_match,
		.pm = pm_ptr(&histb_hdmi_14_pm_ops),
	},
};

module_platform_driver(histb_hdmi_14_driver);

MODULE_DESCRIPTION("HiSilicon STB HDMI 1.4 Tx");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
