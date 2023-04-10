/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * HiSilicon Clock and Reset Driver Header
 *
 * Copyright (c) 2016 HiSilicon Limited.
 */

#ifndef __HISI_CRG_H
#define __HISI_CRG_H

struct hisi_clock_data;
struct hisi_reset_controller;

struct hisi_crg_dev {
	struct hisi_clock_data *clk_data;
	struct hisi_reset_controller *rstc;
};

/* helper functions for platform driver */

int hisi_crg_probe(struct platform_device *pdev);
void hisi_crg_remove(struct platform_device *pdev);

#endif	/* __HISI_CRG_H */
