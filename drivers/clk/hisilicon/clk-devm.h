// SPDX-License-Identifier: GPL-2.0-or-later

#include "clk.h"

int devm_hisi_register_fixed_rate(struct device *dev, const struct hisi_fixed_rate_clock
		*fixed_rate, int nums, struct hisi_clock_data *data);
int devm_hisi_register_fixed_factor(struct device *dev, const struct hisi_fixed_factor_clock
		*fixed_factor, int nums, struct hisi_clock_data *data);
int devm_hisi_register_mux(struct device *dev, const struct hisi_mux_clock
		*mux, int nums, struct hisi_clock_data *data);
int devm_hisi_register_divider(struct device *dev, const struct hisi_divider_clock
		*divider, int nums, struct hisi_clock_data *data);
int devm_hisi_register_gate(struct device *dev, const struct hisi_gate_clock
		*gate, int nums, struct hisi_clock_data *data);
