// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hisilicon HiP04 clock driver
 *
 * Copyright (c) 2013-2014 Hisilicon Limited.
 * Copyright (c) 2013-2014 Linaro Limited.
 *
 * Author: Haojian Zhuang <haojian.zhuang@linaro.org>
 */

#include <linux/kernel.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>

#include <dt-bindings/clock/hip04-clock.h>

#include "clk.h"

/* fixed rate clocks */
static struct hisi_fixed_rate_clock hip04_fixed_rate_clks[] __initdata = {
	{ HIP04_OSC50M,   "osc50m",   NULL, 0, 50000000, },
	{ HIP04_CLK_50M,  "clk50m",   NULL, 0, 50000000, },
	{ HIP04_CLK_168M, "clk168m",  NULL, 0, 168750000, },
};

static const struct hisi_clocks hip04_clks = {
	.fixed_rate_clks = hip04_fixed_rate_clks,
	.fixed_factor_clks_num = ARRAY_SIZE(hip04_fixed_rate_clks),
};

static const struct of_device_id hip04_clk_match_table[] = {
	{ .compatible = "hisilicon,hip04-clock",
	  .data = &hip04_clks },
	{ }
};
MODULE_DEVICE_TABLE(of, hip04_clk_match_table);

static struct platform_driver hip04_clk_driver = {
	.probe = hisi_clk_probe,
	.remove_new = hisi_clk_remove,
	.driver		= {
		.name	= "hip04-clock",
		.of_match_table = hip04_clk_match_table,
	},
};

module_platform_driver(hip04_clk_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HiSilicon HiP04 Clock Driver");
