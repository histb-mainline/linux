// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IOCONFIG pinctrl driver for HiSTB SoCs
 *
 * Copyright 2024 (r) Yang Xiwen <forbidden405@outlook.com>
 */

#include <asm/io.h>
#include <linux/bitfield.h>
#include <linux/dev_printk.h>
#include <linux/kernel.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/types.h>

#include "../core.h"
#include "../pinconf.h"
#include "../pinmux.h"

#include "pinctrl-histb.h"

static int histb_pinctrl_pinmux_set(struct pinctrl_dev *pctldev, unsigned int func_selector,
				    unsigned int group_selector)
{
	const struct pin_desc *pin = pin_desc_get(pctldev, group_selector);
	const struct histb_pin_data *data = pin->drv_data;
	const struct histb_pin_mux_desc *mux = data->func_tbl;
	u32 reg;
	void __iomem *pin_reg = (u32 *)pinctrl_dev_get_drvdata(pctldev) + group_selector;
	bool found = false;

	while (mux->func != HISTB_PIN_FUNCTION_EOF) {
		if (mux->func == func_selector) {
			reg = readl(pin_reg);
			reg &= ~HISTB_PIN_FUNC_MASK;
			reg |= mux->bits;
			writel(reg, pin_reg);

			found = true;
			break;
		}
		mux++;
	}

	if (!found) {
		dev_err(pctldev->dev, "Unable to set pin %s to the given function %s\n",
			pin_get_name(pctldev, group_selector),
			pinmux_generic_get_function_name(pctldev, func_selector));
		return -ENOENT;
	}

	return 0;
}

static int histb_pinctrl_gpio_request(struct pinctrl_dev *pctldev,
				      struct pinctrl_gpio_range *range,
				      unsigned int pin)
{
	return histb_pinctrl_pinmux_set(pctldev, HISTB_PIN_FUNCTION_GPIO, pin);
}

enum histb_pinctrl_bias_status {
	BIAS_DISABLE = 0,
	BIAS_PULL_DOWN = BIT(0),
	BIAS_PULL_UP = BIT(1),
	// if both pull up and pull down are detected
	BIAS_INVALID = BIT(0) | BIT(1),
};

static int histb_pinctrl_set_bias(struct pinctrl_dev *pctldev, unsigned int selector,
				  enum histb_pinctrl_bias_status status)
{
	struct histb_pin_data *priv = pin_desc_get(pctldev, selector)->drv_data;
	void __iomem *pin_reg = (u32 *)pinctrl_dev_get_drvdata(pctldev) + selector;
	u32 reg;
	bool not_supported;

	switch (status) {
	case BIAS_DISABLE:
		not_supported = false;
		break;
	case BIAS_PULL_DOWN:
		not_supported = priv->flags & HISTB_PIN_FLAG_NOPD;
		break;
	case BIAS_PULL_UP:
		not_supported = priv->flags & HISTB_PIN_FLAG_NOPU;
		break;
	case BIAS_INVALID:
		not_supported = true;
		break;
	}

	if (not_supported)
		return -ENOTSUPP;

	reg = readl(pin_reg);
	reg &= ~(HISTB_PIN_PULLDOWN | HISTB_PIN_PULLUP);

	switch (status) {
	case BIAS_DISABLE:
		break;
	case BIAS_PULL_DOWN:
		reg |= HISTB_PIN_PULLDOWN;
		break;
	case BIAS_PULL_UP:
		reg |= HISTB_PIN_PULLUP;
		break;
	case BIAS_INVALID:
		// Can't reach here.
		break;
	}

	writel(reg, pin_reg);

	return 0;
}

static enum histb_pinctrl_bias_status
histb_pinctrl_get_bias_status(struct pinctrl_dev *pctldev, unsigned int selector)
{
	void __iomem *pin_reg = (u32 *)pinctrl_dev_get_drvdata(pctldev) + selector;
	u32 reg = readl(pin_reg);
	enum histb_pinctrl_bias_status ret = BIAS_DISABLE;

	if (reg & HISTB_PIN_PULLDOWN)
		ret = BIAS_PULL_DOWN;

	if (reg & HISTB_PIN_PULLUP)
		ret |= BIAS_PULL_UP;

	return ret;
}

static int histb_pinctrl_set_slew_rate(struct pinctrl_dev *pctldev, unsigned int selector,
				       unsigned long argument)
{
	struct histb_pin_data *priv = pin_desc_get(pctldev, selector)->drv_data;
	void __iomem *pin_reg = (u32 *)pinctrl_dev_get_drvdata(pctldev) + selector;
	u32 reg;

	if (priv->flags & HISTB_PIN_FLAG_NOSR || argument > 1)
		return -ENOTSUPP;

	reg = readl(pin_reg);
	if (argument)
		reg |= HISTB_PIN_SLEWRATE;
	else
		reg &= ~HISTB_PIN_SLEWRATE;

	writel(reg, pin_reg);

	return 0;
}

static bool histb_pinctrl_get_slew_rate(struct pinctrl_dev *pctldev, unsigned int selector)
{
	void __iomem *pin_reg = (u32 *)pinctrl_dev_get_drvdata(pctldev) + selector;

	return readl(pin_reg) & HISTB_PIN_SLEWRATE;
}

static int histb_pinctrl_endisable_schmitt(struct pinctrl_dev *pctldev, unsigned int selector,
					   bool enable)
{
	struct histb_pin_data *priv = pin_desc_get(pctldev, selector)->drv_data;
	void __iomem *pin_reg = (u32 *)pinctrl_dev_get_drvdata(pctldev) + selector;
	u32 reg;

	if (priv->flags & HISTB_PIN_FLAG_SCHMITT)
		return -ENOTSUPP;

	reg = readl(pin_reg);
	if (enable)
		reg |= HISTB_PIN_SCHMITT;
	else
		reg &= ~HISTB_PIN_SCHMITT;

	writel(reg, pin_reg);

	return 0;
}

static bool histb_pinctrl_get_schmitt(struct pinctrl_dev *pctldev, unsigned int selector)
{
	void __iomem *pin_reg = (u32 *)pinctrl_dev_get_drvdata(pctldev) + selector;

	return readl(pin_reg) & HISTB_PIN_SCHMITT;
}

static int histb_pinctrl_set_drive_strength(struct pinctrl_dev *pctldev, unsigned int selector,
					    unsigned int argument)
{
	struct histb_pin_data *priv = pin_desc_get(pctldev, selector)->drv_data;
	void __iomem *pin_reg = (u32 *)pinctrl_dev_get_drvdata(pctldev) + selector;
	int i = -1;
	u32 reg;

	if (unlikely(argument == 0 || !priv->drv_tbl))
		return -ENOTSUPP;

	// calculate the largest drive-strength that does not exceeds the given value
	// if the lowest value is still too large, use that anyway
	// TODO: use bsearch()?
	while (priv->drv_tbl[++i] > argument)
		;

	if (!priv->drv_tbl[i])
		i--;

	reg = readl(pin_reg);
	reg &= ~HISTB_PIN_DRV_MASK;
	reg |= FIELD_PREP(HISTB_PIN_DRV_MASK, i);

	writel(reg, pin_reg);

	return 0;
}

static unsigned int histb_pinctrl_get_drive_strength(struct pinctrl_dev *pctldev,
						     unsigned int selector)
{
	struct histb_pin_data *priv = pin_desc_get(pctldev, selector)->drv_data;
	void __iomem *pin_reg = (u32 *)pinctrl_dev_get_drvdata(pctldev) + selector;

	if (!priv->drv_tbl)
		return 0;

	return priv->drv_tbl[FIELD_GET(HISTB_PIN_DRV_MASK, readl(pin_reg))];
}

static int histb_pinctrl_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin,
				     unsigned long *config)
{
	enum pin_config_param param = pinconf_to_config_param(*config);
	enum histb_pinctrl_bias_status bias;
	ulong argument;

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		bias = histb_pinctrl_get_bias_status(pctldev, pin);
		argument = bias == BIAS_DISABLE;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		bias = histb_pinctrl_get_bias_status(pctldev, pin);
		argument = !!(bias & BIAS_PULL_UP);
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		bias = histb_pinctrl_get_bias_status(pctldev, pin);
		argument = !!(bias & BIAS_PULL_DOWN);
		break;
	case PIN_CONFIG_SLEW_RATE:
		argument = histb_pinctrl_get_slew_rate(pctldev, pin);
		break;
	case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
		argument = histb_pinctrl_get_schmitt(pctldev, pin);
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		argument = histb_pinctrl_get_drive_strength(pctldev, pin);
		if (argument == 0)
			return -ENOTSUPP;
		break;
	default:
		return -EINVAL;
	}

	*config = pinconf_to_config_packed(param, argument);

	return 0;
}

static int histb_pinctrl_pinconf_set_single(struct pinctrl_dev *pctldev, unsigned int pin_selector,
					    enum pin_config_param param, unsigned int argument)
{
	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		return histb_pinctrl_set_bias(pctldev, pin_selector, BIAS_DISABLE);
	case PIN_CONFIG_BIAS_PULL_UP:
		return histb_pinctrl_set_bias(pctldev, pin_selector, BIAS_PULL_UP);
	case PIN_CONFIG_BIAS_PULL_DOWN:
		return histb_pinctrl_set_bias(pctldev, pin_selector, BIAS_PULL_DOWN);
	case PIN_CONFIG_SLEW_RATE:
		return histb_pinctrl_set_slew_rate(pctldev, pin_selector, argument);
	case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
		return histb_pinctrl_endisable_schmitt(pctldev, pin_selector, argument);
	case PIN_CONFIG_DRIVE_STRENGTH:
		return histb_pinctrl_set_drive_strength(pctldev, pin_selector, argument);
	default:
		break;
	}

	dev_err(pctldev->dev, "can't handle given config %d\n", param);
	return -EINVAL;
}

static int histb_pinctrl_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
				 unsigned long *configs, unsigned int num_configs)
{
	int i, ret;

	for (i = 0; i < num_configs; i++) {
		ret = histb_pinctrl_pinconf_set_single(pctldev, pin,
						       pinconf_to_config_param(configs[i]),
						       pinconf_to_config_argument(configs[i]));
		if (ret)
			return ret;
	}

	return 0;
}

const struct pinctrl_ops histb_pinctrl_pctl_ops = {
	.get_groups_count	= pinctrl_generic_get_group_count,
	.get_group_name		= pinctrl_generic_get_group_name,
	.get_group_pins		= pinctrl_generic_get_group_pins,
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_all,
	.dt_free_map		= pinconf_generic_dt_free_map,
};

const struct pinmux_ops histb_pinctrl_pinmux_ops = {
	.get_functions_count	= pinmux_generic_get_function_count,
	.get_function_name	= pinmux_generic_get_function_name,
	.get_function_groups	= pinmux_generic_get_function_groups,
	.set_mux		= histb_pinctrl_pinmux_set,
	.gpio_request_enable	= histb_pinctrl_gpio_request,
	.strict			= true,
};

const struct pinconf_ops histb_pinctrl_pinconf_ops = {
	.is_generic			= true,
	.pin_config_get			= histb_pinctrl_pinconf_get,
	.pin_config_set			= histb_pinctrl_pinconf_set,
	.pin_config_config_dbg_show	= pinconf_generic_dump_config,
};
