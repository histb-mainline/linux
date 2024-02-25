// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for the integrated PWM module in HiSilicon PMC core
 *
 * Copyright 2024 (c) Yang Xiwen
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define HISI_PWM_PERIOD		GENMASK(15, 0)
#define HISI_PWM_DUTY		GENMASK(31, 16)

struct hisi_pmc_pwm {
	struct pwm_chip chip;
	void __iomem *base;
	ulong rate;
};

#define to_hisi_pmc_pwm(chip) container_of(chip, struct hisi_pmc_pwm, chip)

static int hisi_pmc_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			      const struct pwm_state *state)
{
	struct hisi_pmc_pwm *fpwm;
	u64 period_cycles, duty_cycles;
	u32 reg;

	/* Turned on by boot loader, and PWMs in PMC are always critical */
	if (!state->enabled || state->polarity == PWM_POLARITY_INVERSED)
		return -EINVAL;

	fpwm = to_hisi_pmc_pwm(chip);

	period_cycles = mul_u64_u64_div_u64(fpwm->rate, state->period, NSEC_PER_SEC);
	period_cycles = clamp(period_cycles, 0, 0xFFFF);

	duty_cycles = mul_u64_u64_div_u64(fpwm->rate, state->duty_cycle, NSEC_PER_SEC);
	duty_cycles = clamp(duty_cycles, 0, 0xFFFF);

	reg = FIELD_PREP(HISI_PWM_PERIOD, period_cycles) |
		FIELD_PREP(HISI_PWM_DUTY, duty_cycles);
	writel(reg, fpwm->base);

	return 0;
}

static int hisi_pmc_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				  struct pwm_state *state)
{
	struct hisi_pmc_pwm *fpwm;
	u16 period_cycles, duty_cycles;
	u32 reg;

	fpwm = to_hisi_pmc_pwm(chip);

	reg = readl(fpwm->base);

	period_cycles = FIELD_GET(HISI_PWM_PERIOD, reg);
	duty_cycles = FIELD_GET(HISI_PWM_DUTY, reg);

	state->enabled = true;
	state->polarity = PWM_POLARITY_NORMAL;

	state->duty_cycle = DIV64_U64_ROUND_CLOSEST((u64)duty_cycles * NSEC_PER_SEC, fpwm->rate);
	state->period = DIV64_U64_ROUND_CLOSEST((u64)period_cycles * NSEC_PER_SEC, fpwm->rate);

	return 0;
}

static const struct pwm_ops hisi_pmc_pwm_ops = {
	.apply		= hisi_pmc_pwm_apply,
	.get_state	= hisi_pmc_pwm_get_state,
};

static int hisi_pmc_pwm_probe(struct platform_device *pdev)
{
	struct hisi_pmc_pwm *fpwm;
	struct clk *clk;
	int ret;

	fpwm = devm_kzalloc(&pdev->dev, sizeof(*fpwm), GFP_KERNEL);
	if (!fpwm)
		return -ENOMEM;

	fpwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(fpwm->base))
		return PTR_ERR(fpwm->base);

	clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk), "unable to get the clock");

	/*
	 * Uses the 24MHz system clock on all existing devices, can only
	 * happen if the device tree is broken
	 *
	 * This check is done to prevent an overflow in .apply
	 */
	fpwm->rate = clk_get_rate(clk);
	if (fpwm->rate > NSEC_PER_SEC)
		return dev_err_probe(&pdev->dev, -EINVAL, "pwm clock out of range");

	fpwm->chip.dev = &pdev->dev;
	fpwm->chip.npwm = 1;
	fpwm->chip.ops = &hisi_pmc_pwm_ops;

	ret = devm_pwmchip_add(&pdev->dev, &fpwm->chip);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "unable to add pwm chip");

	return 0;
}

static const struct of_device_id hisi_pmc_pwm_of_match[] = {
	{ .compatible = "hisilicon,pmc-pwm" },
	{ .compatible = "hisilicon,hi3798mv200-pwm" },
	{ },
};
MODULE_DEVICE_TABLE(of, hisi_pmc_pwm_of_match);

static struct platform_driver hisi_pmc_pwm_driver = {
	.probe = hisi_pmc_pwm_probe,
	.driver = {
		.name = "hisi-pmc-pwm",
		.of_match_table = hisi_pmc_pwm_of_match,
	},
};
module_platform_driver(hisi_pmc_pwm_driver);

MODULE_DESCRIPTION("HiSilicon SoC PMC PWM driver");
MODULE_AUTHOR("Yang Xiwen <forbidden405@outlook.com>");
MODULE_LICENSE("GPL");
