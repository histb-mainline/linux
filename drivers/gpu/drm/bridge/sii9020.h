// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/*
 * Copyright (c) 2023 David Yang
 */

#ifndef _SII9020_H
#define _SII9020_H

#include <linux/i2c.h>
#include <linux/interrupt.h>

struct device;
struct regmap;
struct reset_control;

#define SII9020_I2C_ADDR	0x72
#define SII9020_ALT_I2C_ADDR	0x76
#define SII9020_B_I2C_ADDR_OFFSET	8

struct sii9020_cap {
	bool deep_color : 1;
	bool rgb_2_ycbca : 1;
	bool downsample_422 : 1;
	bool range_compress : 1;
	bool range_clip : 1;
};

/**
 * struct sii9020 - SiI9020 state.
 *
 * @map: regmap of first I2C device (0x72 / 0x76), initializer
 * @map1: regmap of second I2C device (0x7a / 0x7e), initializer
 * @map1: regmap of second I2C device (0x7a / 0x7e), initializer
 * @dev: device handler, initializer
 * @rst: reset controller (optional), initializer
 * @priv: implementation-defined private data
 * @dev_id: BCD device chip model ID
 * @rev: device chip revision
 * @cap: device capabilities
 */
struct sii9020 {
	/* initializer: */
	struct regmap *map;
	struct regmap *map1;
	struct device *dev;
	struct reset_control *rst;

	/* public: */
	void *priv;
	unsigned int dev_id;
	unsigned int rev;
	struct sii9020_cap cap;

	/* private: */
	struct i2c_adapter adap;
};

int sii9020_get_blank(const struct sii9020 *ctx);
int sii9020_set_blank(const struct sii9020 *ctx, int color);

irqreturn_t sii9020_handle(int irq, void *dev_id);
int sii9020_resume(struct sii9020 *ctx);
int sii9020_probe(struct sii9020 *ctx, int irq);

#define pr_debug pr_info

#endif /* _SII9020_H */
