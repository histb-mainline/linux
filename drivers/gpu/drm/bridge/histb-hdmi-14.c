// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023 David Yang
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/string.h>

#include "sii9134.h"

struct histb_hdmi_14_priv {
	void __iomem *base;

	struct device *dev;

	struct clk *clk;
	struct reset_control *rst;
	struct clk *clk_sii9134;
	struct reset_control *rst_sii9134;
	struct clk *clk_phy;
	struct reset_control *rst_phy;

	struct sii9134 sii9134;
};

static const struct regmap_config histb_hdmi_14_sii9134_regmap_config = {
	.reg_bits = 8,
	.reg_shift = 2,
	.val_bits = 8,
};

static int histb_hdmi_14_runtime_suspend(struct device *dev)
{
	struct histb_hdmi_14_priv *priv = dev_get_drvdata(dev);

	clk_disable_unprepare(priv->clk_phy);
	clk_disable_unprepare(priv->clk_sii9134);
	clk_disable_unprepare(priv->clk);

	return 0;
}

static int histb_hdmi_14_runtime_resume(struct device *dev)
{
	struct histb_hdmi_14_priv *priv = dev_get_drvdata(dev);

	return reset_control_assert(priv->rst_phy) ?:
	       reset_control_assert(priv->rst_sii9134) ?:
	       reset_control_assert(priv->rst) ?:
	       clk_prepare_enable(priv->clk) ?:
	       clk_prepare_enable(priv->clk_sii9134) ?:
	       clk_prepare_enable(priv->clk_phy) ?:
	       reset_control_deassert(priv->rst) ?:
	       reset_control_deassert(priv->rst_sii9134) ?:
	       reset_control_deassert(priv->rst_phy);
}

static const struct dev_pm_ops histb_hdmi_14_pm_ops = {
	.runtime_suspend = histb_hdmi_14_runtime_suspend,
	.runtime_resume = histb_hdmi_14_runtime_resume,
};

static void histb_hdmi_14_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_hdmi_14_priv *priv = platform_get_drvdata(pdev);

	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	clk_disable_unprepare(priv->clk);
}

static int histb_hdmi_14_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_hdmi_14_priv *priv;
	int irq;
	int ret;

	/* acquire resources */
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return PTR_ERR(priv->clk);
	priv->rst = devm_reset_control_get_optional(dev, NULL);
	if (IS_ERR(priv->rst))
		return PTR_ERR(priv->rst);

	priv->clk_sii9134 = devm_clk_get(dev, "sii9134");
	if (IS_ERR(priv->clk_sii9134))
		return PTR_ERR(priv->clk_sii9134);
	priv->rst_sii9134 = devm_reset_control_get_optional(dev, "sii9134");
	if (IS_ERR(priv->rst_sii9134))
		return PTR_ERR(priv->rst_sii9134);

	priv->clk_phy = devm_clk_get(dev, "phy");
	if (IS_ERR(priv->clk_phy))
		return PTR_ERR(priv->clk_phy);
	priv->rst_phy = devm_reset_control_get_optional(dev, "phy");
	if (IS_ERR(priv->rst_phy))
		return PTR_ERR(priv->rst_phy);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);
	dev_set_drvdata(dev, priv);

	/* bring up device */
	ret = histb_hdmi_14_runtime_resume(dev);
	if (ret)
		return ret;

	priv->sii9134.map = devm_regmap_init_mmio(dev, priv->base,
					&histb_hdmi_14_sii9134_regmap_config);
	if (IS_ERR(priv->sii9134.map))
		return PTR_ERR(priv->sii9134.map);

	priv->sii9134.map1 = devm_regmap_init_mmio(dev, priv->base + 0x400,
					&histb_hdmi_14_sii9134_regmap_config);
	if (IS_ERR(priv->sii9134.map1))
		return PTR_ERR(priv->sii9134.map1);

	priv->sii9134.dev = dev;
	ret = sii9134_probe(&priv->sii9134, irq);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, MSEC_PER_SEC);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	return 0;
}

static const struct of_device_id histb_hdmi_14_of_match[] = {
	{ .compatible = "hisilicon,histb-hdmi-1-4", },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_hdmi_14_of_match);

static struct platform_driver histb_hdmi_14_driver = {
	.probe = histb_hdmi_14_probe,
	.remove_new = histb_hdmi_14_remove,
	.driver = {
		.name = "histb-hdmi-14",
		.of_match_table = histb_hdmi_14_of_match,
		.pm = &histb_hdmi_14_pm_ops,
	},
};

module_platform_driver(histb_hdmi_14_driver);

MODULE_DESCRIPTION("HiSilicon STB HDMI 1.4 Tx");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
