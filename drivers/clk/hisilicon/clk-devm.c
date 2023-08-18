// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hisilicon clock driver (devm version)
 *
 */
#include <linux/device.h>

#include "clk-devm.h"

#define DEFINE_HISI_REGISTER_CLK_PARAMS(_variant) \
	struct hisi_register_##_variant##_clock_params { \
		const struct hisi_##_variant##_clock *_variant; \
		int nums; \
		struct hisi_clock_data *data; \
	}

DEFINE_HISI_REGISTER_CLK_PARAMS(fixed_rate);
DEFINE_HISI_REGISTER_CLK_PARAMS(fixed_factor);
DEFINE_HISI_REGISTER_CLK_PARAMS(mux);
DEFINE_HISI_REGISTER_CLK_PARAMS(divider);
DEFINE_HISI_REGISTER_CLK_PARAMS(gate);

#define DEFINE_HISI_RELEASE_CLOCK(_variant) \
	static void hisi_release_##_variant##_clock(struct device *dev, void *res) \
{ \
	struct hisi_register_##_variant##_clock_params *params = res; \
	hisi_clk_unregister_##_variant(params->_variant, params->nums, params->data); \
} \

DEFINE_HISI_RELEASE_CLOCK(fixed_rate);
DEFINE_HISI_RELEASE_CLOCK(fixed_factor);
DEFINE_HISI_RELEASE_CLOCK(mux);
DEFINE_HISI_RELEASE_CLOCK(divider);
DEFINE_HISI_RELEASE_CLOCK(gate);

#define DEFINE_DEVM_HISI_CLK_REGISTER(_variant) int \
	devm_hisi_clk_register_##_variant(struct device *dev, \
					  const struct hisi_##_variant##_clock *_variant, \
					  int nums, \
					  struct hisi_clock_data *data) \
{ \
	struct hisi_register_##_variant##_clock_params *params; \
	int ret; \
	params = devres_alloc(hisi_release_##_variant##_clock, sizeof(*params), GFP_KERNEL); \
	if (!params) \
		return -ENOMEM; \
	ret = hisi_clk_register_##_variant(_variant, nums, data); \
	if (ret) { \
		devres_free(params); \
		return ret; \
	} \
	params->_variant = _variant; \
	params->nums = nums; \
	params->data = data; \
	devres_add(dev, params); \
	return 0; \
} \
EXPORT_SYMBOL_GPL(devm_hisi_clk_register_##_variant);

DEFINE_DEVM_HISI_CLK_REGISTER(fixed_rate);
DEFINE_DEVM_HISI_CLK_REGISTER(fixed_factor);
DEFINE_DEVM_HISI_CLK_REGISTER(mux);
DEFINE_DEVM_HISI_CLK_REGISTER(divider);
DEFINE_DEVM_HISI_CLK_REGISTER(gate);
