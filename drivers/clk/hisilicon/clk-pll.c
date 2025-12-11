// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PLL driver for HiSilicon SoCs
 *
 * Copyright 2024 (c) Yang Xiwen <forbidden405@outlook.com>
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/module.h>

#include "clk.h"

/* PLL has two conf regs in total */
#define HISI_PLL_CFG(n)		((n) * 4)

/* reg 0 definitions */
#define HISI_PLL_FRAC		GENMASK(23, 0)
#define HISI_PLL_POSTDIV1	GENMASK(26, 24)
#define HISI_PLL_POSTDIV2	GENMASK(30, 28)

/* reg 1 definitions */
#define HISI_PLL_FBDIV		GENMASK(11, 0)
#define HISI_PLL_REFDIV		GENMASK(17, 12)
#define HISI_PLL_PD		BIT(20)
#define HISI_PLL_FOUTVCOPD	BIT(21)
#define HISI_PLL_FOUT4PHASEPD	BIT(22)
#define HISI_PLL_FOUTPOSTDIVPD	BIT(23)
#define HISI_PLL_DACPD		BIT(24)
#define HISI_PLL_DSMPD		BIT(25)
#define HISI_PLL_BYPASS		BIT(26)

/*
 * Datasheet said the maximum is 3.2GHz,
 * but tests show it can be very high
 *
 * Leave some margin here (8 GHz should be fine)
 */
#define HISI_PLL_FOUTVCO_MAX_RATE	8000000000
/* 800 MHz */
#define HISI_PLL_FOUTVCO_MIN_RATE	800000000

struct hisi_pll {
	struct clk_hw	hw;
	void __iomem	*base;
	u8		postdiv1, postdiv2, refdiv;
	u32		divisor;
};

#define to_hisi_pll(_hw) container_of(_hw, struct hisi_pll, hw)

static int hisi_pll_prepare(struct clk_hw *hw)
{
	struct hisi_pll *pll = to_hisi_pll(hw);
	u32 reg;

	reg = readl(pll->base + HISI_PLL_CFG(0));
	pll->postdiv1 = FIELD_GET(HISI_PLL_POSTDIV1, reg);
	pll->postdiv2 = FIELD_GET(HISI_PLL_POSTDIV2, reg);
	// We don't use frac, clear it
	reg &= ~HISI_PLL_FRAC;
	writel(reg, pll->base + HISI_PLL_CFG(0));

	reg = readl(pll->base + HISI_PLL_CFG(1));
	pll->refdiv = FIELD_GET(HISI_PLL_REFDIV, reg);

	pll->divisor = pll->refdiv * pll->postdiv1 * pll->postdiv2;

	// return -EINVAL if boot loader does not init PLL correctly
	if (pll->divisor == 0) {
		pr_err("%s: PLLs are not initialized by boot loader correctly!\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static int hisi_pll_set_rate(struct clk_hw *hw, ulong rate, ulong parent_rate)
{
	struct hisi_pll *pll = to_hisi_pll(hw);
	u64 fbdiv = rate * pll->divisor;
	u32 reg;

	do_div(fbdiv, parent_rate);

	reg = readl(pll->base + HISI_PLL_CFG(1));
	reg &= ~HISI_PLL_FBDIV;
	reg |= FIELD_PREP(HISI_PLL_FBDIV, fbdiv);
	writel(reg, pll->base + HISI_PLL_CFG(1));

	/* TODO: wait for PLL lock? */

	return 0;
}

static int hisi_pll_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	struct hisi_pll *pll = to_hisi_pll(hw);
	u64 vco, ref_rate = req->best_parent_rate;

	if (ref_rate == 0)
		return -EINVAL;

	do_div(ref_rate, pll->refdiv);
	vco = clamp(req->rate * (pll->postdiv1 * pll->postdiv2),
		    HISI_PLL_FOUTVCO_MIN_RATE, HISI_PLL_FOUTVCO_MAX_RATE);
	vco = rounddown(vco, ref_rate);
	if (vco < HISI_PLL_FOUTVCO_MIN_RATE)
		vco += ref_rate;

	do_div(vco, pll->postdiv1 * pll->postdiv2);
	req->rate = vco;

	return 0;
}

static ulong hisi_pll_recalc_rate(struct clk_hw *hw, ulong parent_rate)
{
	struct hisi_pll *pll = to_hisi_pll(hw);
	u32 reg, fbdiv;

	reg = readl(pll->base + HISI_PLL_CFG(1));
	fbdiv = FIELD_GET(HISI_PLL_FBDIV, reg);
	parent_rate *= fbdiv;
	do_div(parent_rate, pll->divisor);

	return parent_rate;
}

static const struct clk_ops hisi_pll_ops = {
	.prepare	= hisi_pll_prepare,
	.set_rate	= hisi_pll_set_rate,
	.determine_rate	= hisi_pll_determine_rate,
	.recalc_rate	= hisi_pll_recalc_rate,
};

/*
 * devm_hisi_pll_register - register a HiSilicon PLL
 *
 * @dev: clk provider
 * @name: clock name
 * @parent_name: parent clock, usually 24MHz OSC
 * #flags: CCF common flags
 * @reg: register address
 */
struct clk *devm_clk_register_hisi_pll(struct device *dev, const char *name, const char *parent,
				       unsigned int flags, void __iomem *reg)
{
	struct hisi_pll *pll;
	struct clk_init_data init;

	pll = devm_kzalloc(dev, sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return ERR_PTR(-ENOMEM);

	if (!parent)
		return ERR_PTR(-EINVAL);

	init.name = name;
	init.ops = &hisi_pll_ops;
	init.flags = flags;
	init.parent_names = &parent;
	init.num_parents = 1;

	pll->base = reg;
	pll->hw.init = &init;

	return devm_clk_register(dev, &pll->hw);
}
EXPORT_SYMBOL_GPL(devm_clk_register_hisi_pll);
