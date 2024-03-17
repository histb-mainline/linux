/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Hisilicon Hi3620 clock gate driver
 *
 * Copyright (c) 2012-2013 Hisilicon Limited.
 * Copyright (c) 2012-2013 Linaro Limited.
 * Copyright (c) 2023 David Yang
 *
 * Author: Haojian Zhuang <haojian.zhuang@linaro.org>
 *	   Xin Li <li.xin@linaro.org>
 */

#ifndef	__HISI_CLK_H
#define	__HISI_CLK_H

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/spinlock.h>

struct platform_device;
struct hisi_clocks;

/*
 * (Virtual) fixed clocks, often depended by crucial peripherals, require
 * early initialization before device probing, thus cannot use devm APIs.
 * Otherwise, kernel will defer those peripherals, causing boot failure.
 *
 * fixed_rate and fixed_factor clocks are driver-managed. They are freed by
 * `hisi_clk_free` altogether.
 *
 * Other clocks are devm-managed.
 */
struct hisi_clock_data {
	struct clk_hw_onecell_data	*clk_data;
	void __iomem			*base;
	const struct hisi_clocks	*clks;
};

struct hisi_fixed_rate_clock {
	unsigned int		id;
	char			*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		fixed_rate;
};

struct hisi_fixed_factor_clock {
	unsigned int		id;
	char			*name;
	const char		*parent_name;
	unsigned long		mult;
	unsigned long		div;
	unsigned long		flags;
};

struct hisi_mux_clock {
	unsigned int		id;
	const char		*name;
	const char		*const *parent_names;
	u8			num_parents;
	unsigned long		flags;
	unsigned long		offset;
	u8			shift;
	u8			width;
	u8			mux_flags;
	const u32		*table;
	const char		*alias;
};

struct hisi_phase_clock {
	unsigned int		id;
	const char		*name;
	const char		*parent_names;
	unsigned long		flags;
	unsigned long		offset;
	u8			shift;
	u8			width;
	u32			*phase_degrees;
	u32			*phase_regvals;
	u8			phase_num;
};

struct hisi_divider_clock {
	unsigned int		id;
	const char		*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		offset;
	u8			shift;
	u8			width;
	u8			div_flags;
	struct clk_div_table	*table;
	const char		*alias;
};

struct hi6220_divider_clock {
	unsigned int		id;
	const char		*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		offset;
	u8			shift;
	u8			width;
	u32			mask_bit;
	const char		*alias;
};

struct hisi_gate_clock {
	unsigned int		id;
	const char		*name;
	const char		*parent_name;
	unsigned long		flags;
	unsigned long		offset;
	u8			bit_idx;
	u8			gate_flags;
	const char		*alias;
};

struct hisi_clocks {
	/* if 0, sum all *_num */
	size_t nr;

	int (*prologue)(struct device *dev, struct hisi_clock_data *data);

	const struct hisi_fixed_rate_clock *fixed_rate_clks;
	size_t fixed_rate_clks_num;

	const struct hisi_fixed_factor_clock *fixed_factor_clks;
	size_t fixed_factor_clks_num;

	const struct hisi_mux_clock *mux_clks;
	size_t mux_clks_num;

	const struct hisi_phase_clock *phase_clks;
	size_t phase_clks_num;

	const struct hisi_divider_clock *divider_clks;
	size_t divider_clks_num;

	const struct hisi_gate_clock *gate_clks;
	size_t gate_clks_num;

	const struct hisi_gate_clock *gate_sep_clks;
	size_t gate_sep_clks_num;

	const void *customized_clks;
	size_t customized_clks_num;
	int (*clk_register_customized)(struct device *dev, const void *clks,
				       size_t num, struct hisi_clock_data *data);
};

struct clk_hw *
devm_clk_hw_register_hisi_phase(struct device *dev, const struct hisi_phase_clock *clks,
				void __iomem *base, spinlock_t *lock);
struct clk_hw *
devm_clk_hw_register_hisi_gate_sep(struct device *dev, const char *name,
				   const char *parent_name, unsigned long flags,
				   void __iomem *reg, u8 bit_idx,
				   u8 clk_gate_flags, spinlock_t *lock);
struct clk_hw *
devm_clk_hw_register_hi6220_divider(struct device *dev, const char *name,
				    const char *parent_name, unsigned long flags,
				    void __iomem *reg, u8 shift,
				    u8 width, u32 mask_bit, spinlock_t *lock);

struct hisi_clock_data *hisi_clk_init(struct device_node *np, size_t nr);
void hisi_clk_free(struct device_node *np, struct hisi_clock_data *data);

int hisi_clk_register_fixed_rate(const struct hisi_fixed_rate_clock *clks,
				 size_t num, struct hisi_clock_data *data);
int hisi_clk_register_fixed_factor(const struct hisi_fixed_factor_clock *clks,
				   size_t num, struct hisi_clock_data *data);

int hisi_clk_register_mux(struct device *dev, const struct hisi_mux_clock *clks,
			  size_t num, struct hisi_clock_data *data);
int hisi_clk_register_phase(struct device *dev,
			    const struct hisi_phase_clock *clks,
			    size_t num, struct hisi_clock_data *data);
int hisi_clk_register_divider(struct device *dev,
			      const struct hisi_divider_clock *clks,
			      size_t num, struct hisi_clock_data *data);
int hisi_clk_register_gate(struct device *dev,
			   const struct hisi_gate_clock *clks,
			   size_t num, struct hisi_clock_data *data);
int hisi_clk_register_gate_sep(struct device *dev,
			       const struct hisi_gate_clock *clks,
			       size_t num, struct hisi_clock_data *data);
int hi6220_clk_register_divider(struct device *dev,
				const struct hi6220_divider_clock *clks,
				size_t num, struct hisi_clock_data *data);

/* helper functions for platform driver */

int hisi_clk_early_init(struct device_node *np, const struct hisi_clocks *clks);
int hisi_clk_probe(struct platform_device *pdev);
void hisi_clk_remove(struct platform_device *pdev);

#endif	/* __HISI_CLK_H */
