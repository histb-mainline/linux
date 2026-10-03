// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/*
 * Regulator driver for the PWM controlled voltage regulators of HiSilicon STB
 * SoCs
 *
 * May work on Hi3716mv410/420 Hi3796mv100/200 Hi3798cv200/mv100/mv200 but not
 * thoroughly tested
 *
 * Copyright (c) 2026 David Yang
 */

#include <linux/bitfield.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

/*
 * Duty cycle + 1 and period + 1 are counted in periods of the oscillator clock
 * (usually 24 MHz), and a larger duty cycle means a lower output voltage.
 */
#define HISTB_DUTY		GENMASK(31, 16)
#define HISTB_PERIOD		GENMASK(15, 0)

/* 24 MHz / 240 = 100 kHz, arbitrarily chosen but should be enough */
#define HISTB_PWM_PERIOD	240

struct histb_regulator {
	struct regulator_desc desc;
	void __iomem *base;
	int min_uV;
	int max_uV;
};

static int histb_regulator_get_voltage(struct regulator_dev *rdev)
{
	struct histb_regulator *priv = rdev_get_drvdata(rdev);
	u32 val;
	u32 period;
	u32 duty;
	u64 uV;

	val = readl_relaxed(priv->base);
	period = FIELD_GET(HISTB_PERIOD, val);
	duty = FIELD_GET(HISTB_DUTY, val);

	/* an empty counter range leaves the output high */
	if (duty <= 1 || period <= 1)
		return priv->max_uV;

	duty--;
	period--;
	if (duty >= period)
		return priv->min_uV;

	uV = (u64)(priv->max_uV - priv->min_uV) * duty;

	return priv->max_uV - DIV_ROUND_CLOSEST_ULL(uV, period);
}

static int
histb_regulator_set_voltage(struct regulator_dev *rdev, int min_uV, int max_uV,
			    unsigned int *selector)
{
	struct histb_regulator *priv = rdev_get_drvdata(rdev);
	unsigned int duty;
	u32 val;

	if (min_uV >= priv->max_uV)
		duty = 0;
	else if (min_uV <= priv->min_uV)
		duty = HISTB_PWM_PERIOD;
	else
		/* rounding down keeps the output at or above the request */
		duty = DIV_ROUND_DOWN_ULL((u64)(priv->max_uV - min_uV) *
					  HISTB_PWM_PERIOD,
					  priv->max_uV - priv->min_uV);

	val = FIELD_PREP(HISTB_DUTY, duty + 1) |
	      FIELD_PREP(HISTB_PERIOD, HISTB_PWM_PERIOD + 1);
	writel_relaxed(val, priv->base);

	return 0;
}

static const struct regulator_ops histb_regulator_ops = {
	.get_voltage = histb_regulator_get_voltage,
	.set_voltage = histb_regulator_set_voltage,
};

static int histb_regulator_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_regulator *priv;
	struct regulator_init_data *init_data;
	struct regulator_config config = {};
	struct regulator_dev *rdev;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	init_data = of_get_regulator_init_data(dev, dev->of_node, &priv->desc);
	if (!init_data)
		return -ENOMEM;

	priv->min_uV = init_data->constraints.min_uV;
	priv->max_uV = init_data->constraints.max_uV;
	if (priv->min_uV <= 0 || priv->max_uV <= priv->min_uV) {
		dev_err(dev, "invalid voltage range (%d..%d uV)\n",
			priv->min_uV, priv->max_uV);
		return -EINVAL;
	}

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->desc.name = dev_name(dev);
	priv->desc.type = REGULATOR_VOLTAGE;
	priv->desc.ops = &histb_regulator_ops;
	priv->desc.owner = THIS_MODULE;
	priv->desc.continuous_voltage_range = true;

	config.dev = dev;
	config.init_data = init_data;
	config.driver_data = priv;
	config.of_node = dev->of_node;

	rdev = devm_regulator_register(dev, &priv->desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev),
				     "failed to register %s\n",
				     priv->desc.name);

	return 0;
}

static const struct of_device_id histb_regulator_of_match[] = {
	{ .compatible = "hisilicon,hi3798cv200-regulator" },
	{}
};
MODULE_DEVICE_TABLE(of, histb_regulator_of_match);

static struct platform_driver histb_regulator_driver = {
	.probe = histb_regulator_probe,
	.driver = {
		.name = "hi3798cv200-regulator",
		.of_match_table = histb_regulator_of_match,
	},
};
module_platform_driver(histb_regulator_driver);

MODULE_DESCRIPTION("HiSilicon STB regulator driver");
MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_LICENSE("Dual MIT/GPL");
