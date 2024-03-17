// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hisilicon clock driver
 *
 * Copyright (c) 2012-2013 Hisilicon Limited.
 * Copyright (c) 2012-2013 Linaro Limited.
 * Copyright (c) 2023 David Yang
 *
 * Author: Haojian Zhuang <haojian.zhuang@linaro.org>
 *	   Xin Li <li.xin@linaro.org>
 */

#include <linux/kernel.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "clk.h"

static DEFINE_SPINLOCK(hisi_clk_lock);

struct hisi_clock_data *hisi_clk_init(struct device_node *np, size_t nr)
{
	void __iomem *base;
	struct hisi_clock_data *data;
	int ret;
	int i;

	base = of_iomap(np, 0);
	if (!base) {
		pr_err("%s: failed to map clock registers\n", __func__);
		return NULL;
	}

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return NULL;

	data->clk_data = kzalloc(sizeof(*data->clk_data) + nr * sizeof(data->clk_data->hws[0]),
				 GFP_KERNEL);
	if (!data->clk_data)
		goto err_data;

	ret = of_clk_add_hw_provider(np, of_clk_hw_onecell_get, data->clk_data);
	if (ret)
		goto err_clk;

	data->base = base;
	data->clks = NULL;
	data->clk_data->num = nr;
	for (i = 0; i < nr; i++)
		data->clk_data->hws[i] = ERR_PTR(-EPROBE_DEFER);

	return data;

err_clk:
	kfree(data->clk_data);
err_data:
	kfree(data);
	return NULL;
}
EXPORT_SYMBOL_GPL(hisi_clk_init);

#define hisi_clk_unregister_fn(type) \
static void hisi_clk_unregister_##type(struct hisi_clock_data *data) \
{ \
	for (int i = 0; i < data->clks->type##_clks_num; i++) { \
		struct clk_hw *clk = data->clk_data->hws[data->clks->type##_clks[i].id]; \
\
		if (clk && !IS_ERR(clk)) \
			clk_hw_unregister_##type(clk); \
	} \
}

hisi_clk_unregister_fn(fixed_rate)
hisi_clk_unregister_fn(fixed_factor)

void hisi_clk_free(struct device_node *np, struct hisi_clock_data *data)
{
	if (data->clks) {
		if (data->clks->fixed_rate_clks_num)
			hisi_clk_unregister_fixed_rate(data);
		if (data->clks->fixed_factor_clks_num)
			hisi_clk_unregister_fixed_factor(data);
	}

	of_clk_del_provider(np);
	kfree(data->clk_data);
	kfree(data);
}
EXPORT_SYMBOL_GPL(hisi_clk_free);

int hisi_clk_register_fixed_rate(const struct hisi_fixed_rate_clock *clks,
				 size_t num, struct hisi_clock_data *data)
{
	struct clk_hw *clk;
	int i;

	for (i = 0; i < num; i++) {
		const struct hisi_fixed_rate_clock *p_clk = &clks[i];

		clk = clk_hw_register_fixed_rate(NULL, p_clk->name, p_clk->parent_name,
			p_clk->flags, p_clk->fixed_rate);

		if (IS_ERR(clk)) {
			pr_err("%s: failed to register clock %s\n",
			       __func__, p_clk->name);
			goto err;
		}

		data->clk_data->hws[p_clk->id] = clk;
	}

	return 0;

err:
	while (i--)
		clk_hw_unregister_fixed_rate(data->clk_data->hws[clks[i].id]);
	return PTR_ERR(clk);
}
EXPORT_SYMBOL_GPL(hisi_clk_register_fixed_rate);

int hisi_clk_register_fixed_factor(const struct hisi_fixed_factor_clock *clks,
				   size_t num, struct hisi_clock_data *data)
{
	struct clk_hw *clk;
	int i;

	for (i = 0; i < num; i++) {
		const struct hisi_fixed_factor_clock *p_clk = &clks[i];

		clk = clk_hw_register_fixed_factor(NULL, p_clk->name, p_clk->parent_name,
			p_clk->flags, p_clk->mult, p_clk->div);

		if (IS_ERR(clk)) {
			pr_err("%s: failed to register clock %s\n",
			       __func__, p_clk->name);
			goto err;
		}

		data->clk_data->hws[p_clk->id] = clk;
	}

	return 0;

err:
	while (i--)
		clk_hw_unregister_fixed_rate(data->clk_data->hws[clks[i].id]);
	return PTR_ERR(clk);
}
EXPORT_SYMBOL_GPL(hisi_clk_register_fixed_factor);

/*
 * We ARE function creater. Commit message from checkpatch:
 *   Avoid warning on macros that use argument concatenation as
 *   those macros commonly create another function
 */
#define hisi_clk_register_fn(fn, type, stmt) \
int fn(struct device *dev, const struct type *clks, \
	size_t num, struct hisi_clock_data *data) \
{ \
	void __iomem *base = data->base; \
\
	for (int i = 0; i < num; i++) { \
		const struct type *p_clk = &clks[i]; \
		struct clk_hw *clk = stmt; \
\
		if (IS_ERR(clk)) { \
			pr_err("%s: failed to register clock %s\n", \
			       __func__, p_clk->name); \
			return PTR_ERR(clk); \
		} \
\
		if (p_clk->alias) \
			clk_hw_register_clkdev(clk, p_clk->alias, NULL); \
\
		data->clk_data->hws[p_clk->id] = clk; \
	} \
\
	return 0; \
} \
EXPORT_SYMBOL_GPL(fn);

hisi_clk_register_fn(hisi_clk_register_mux, hisi_mux_clock,
	__devm_clk_hw_register_mux(dev, NULL, p_clk->name,
		p_clk->num_parents, p_clk->parent_names, NULL, NULL,
		p_clk->flags, base + p_clk->offset, p_clk->shift, BIT(p_clk->width) - 1,
		p_clk->mux_flags, p_clk->table, &hisi_clk_lock))

int hisi_clk_register_phase(struct device *dev,
			    const struct hisi_phase_clock *clks,
			    size_t num, struct hisi_clock_data *data)
{
	void __iomem *base = data->base;

	for (int i = 0; i < num; i++) {
		const struct hisi_phase_clock *p_clk = &clks[i];
		struct clk_hw *clk = devm_clk_hw_register_hisi_phase(dev,
			p_clk, base, &hisi_clk_lock);

		if (IS_ERR(clk)) {
			pr_err("%s: failed to register clock %s\n", __func__,
			       p_clk->name);
			return PTR_ERR(clk);
		}

		data->clk_data->hws[p_clk->id] = clk;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(hisi_clk_register_phase);

hisi_clk_register_fn(hisi_clk_register_divider, hisi_divider_clock,
	devm_clk_hw_register_divider_table(dev, p_clk->name, p_clk->parent_name,
		p_clk->flags, base + p_clk->offset, p_clk->shift, p_clk->width,
		p_clk->div_flags, p_clk->table, &hisi_clk_lock))
hisi_clk_register_fn(hisi_clk_register_gate, hisi_gate_clock,
	devm_clk_hw_register_gate(dev, p_clk->name, p_clk->parent_name,
		p_clk->flags, base + p_clk->offset, p_clk->bit_idx,
		p_clk->gate_flags, &hisi_clk_lock))
hisi_clk_register_fn(hisi_clk_register_gate_sep, hisi_gate_clock,
	devm_clk_hw_register_hisi_gate_sep(dev, p_clk->name, p_clk->parent_name,
		p_clk->flags, base + p_clk->offset, p_clk->bit_idx,
		p_clk->gate_flags, &hisi_clk_lock))
hisi_clk_register_fn(hi6220_clk_register_divider, hi6220_divider_clock,
	devm_clk_hw_register_hi6220_divider(dev, p_clk->name, p_clk->parent_name,
		p_clk->flags, base + p_clk->offset, p_clk->shift, p_clk->width,
		p_clk->mask_bit, &hisi_clk_lock))

static size_t hisi_clocks_get_nr(const struct hisi_clocks *clks)
{
	if (clks->nr)
		return clks->nr;

	return clks->fixed_rate_clks_num + clks->fixed_factor_clks_num +
		clks->mux_clks_num + clks->phase_clks_num +
		clks->divider_clks_num + clks->gate_clks_num +
		clks->gate_sep_clks_num + clks->customized_clks_num;
}

int hisi_clk_early_init(struct device_node *np, const struct hisi_clocks *clks)
{
	struct hisi_clock_data *data;
	int ret;

	data = hisi_clk_init(np, hisi_clocks_get_nr(clks));
	if (!data)
		return -ENOMEM;
	data->clks = clks;

	ret = hisi_clk_register_fixed_rate(clks->fixed_rate_clks,
					   clks->fixed_rate_clks_num, data);
	if (ret)
		goto err;

	ret = hisi_clk_register_fixed_factor(clks->fixed_factor_clks,
					     clks->fixed_factor_clks_num, data);
	if (ret)
		goto err;

	np->data = data;
	return 0;

err:
	hisi_clk_free(np, data);
	return ret;
}
EXPORT_SYMBOL_GPL(hisi_clk_early_init);

static int hisi_clk_register(struct device *dev, const struct hisi_clocks *clks,
			     struct hisi_clock_data *data)
{
	int ret;

#define do_hisi_clk_register(type) do { \
	if (clks->type##_clks_num) { \
		ret = hisi_clk_register_##type(dev, clks->type##_clks, \
					       clks->type##_clks_num, data); \
		if (ret) \
			return ret; \
	} \
} while (0)

	do_hisi_clk_register(mux);
	do_hisi_clk_register(phase);
	do_hisi_clk_register(divider);
	do_hisi_clk_register(gate);
	do_hisi_clk_register(gate_sep);

	if (clks->clk_register_customized && clks->customized_clks_num) {
		ret = clks->clk_register_customized(dev, clks->customized_clks,
						    clks->customized_clks_num, data);
		if (ret)
			return ret;
	}

	return 0;
}

int hisi_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	const struct hisi_clocks *clks;
	struct hisi_clock_data *data;
	int ret;

	clks = of_device_get_match_data(dev);
	if (!clks)
		return -ENOENT;

	if (!np->data) {
		ret = hisi_clk_early_init(np, clks);
		if (ret)
			return ret;
	}

	data = np->data;
	np->data = NULL;

	if (clks->prologue) {
		ret = clks->prologue(dev, data);
		if (ret)
			goto err;
	}

	ret = hisi_clk_register(dev, clks, data);
	if (ret)
		goto err;

	platform_set_drvdata(pdev, data);
	return 0;

err:
	hisi_clk_free(np, data);
	return ret;
}
EXPORT_SYMBOL_GPL(hisi_clk_probe);

void hisi_clk_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct hisi_clock_data *data = platform_get_drvdata(pdev);

	hisi_clk_free(np, data);
}
EXPORT_SYMBOL_GPL(hisi_clk_remove);
