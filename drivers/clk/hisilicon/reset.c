// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hisilicon Reset Controller Driver
 *
 * Copyright (c) 2015-2016 HiSilicon Technologies Co., Ltd.
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "clk.h"
#include "crg.h"
#include "reset.h"

#define	HISI_RESET_BIT_MASK	0x1f
#define	HISI_RESET_OFFSET_SHIFT	8
#define	HISI_RESET_OFFSET_MASK	0xffff00

struct hisi_reset_controller {
	spinlock_t	lock;
	void __iomem	*membase;
	struct reset_controller_dev	rcdev;
};


#define to_hisi_reset_controller(rcdev)  \
	container_of(rcdev, struct hisi_reset_controller, rcdev)

static int hisi_reset_of_xlate(struct reset_controller_dev *rcdev,
			const struct of_phandle_args *reset_spec)
{
	u32 offset;
	u8 bit;

	offset = (reset_spec->args[0] << HISI_RESET_OFFSET_SHIFT)
		& HISI_RESET_OFFSET_MASK;
	bit = reset_spec->args[1] & HISI_RESET_BIT_MASK;

	return (offset | bit);
}

static int hisi_reset_assert(struct reset_controller_dev *rcdev,
			      unsigned long id)
{
	struct hisi_reset_controller *rstc = to_hisi_reset_controller(rcdev);
	unsigned long flags;
	u32 offset, reg;
	u8 bit;

	offset = (id & HISI_RESET_OFFSET_MASK) >> HISI_RESET_OFFSET_SHIFT;
	bit = id & HISI_RESET_BIT_MASK;

	spin_lock_irqsave(&rstc->lock, flags);

	reg = readl(rstc->membase + offset);
	writel(reg | BIT(bit), rstc->membase + offset);

	spin_unlock_irqrestore(&rstc->lock, flags);

	return 0;
}

static int hisi_reset_deassert(struct reset_controller_dev *rcdev,
				unsigned long id)
{
	struct hisi_reset_controller *rstc = to_hisi_reset_controller(rcdev);
	unsigned long flags;
	u32 offset, reg;
	u8 bit;

	offset = (id & HISI_RESET_OFFSET_MASK) >> HISI_RESET_OFFSET_SHIFT;
	bit = id & HISI_RESET_BIT_MASK;

	spin_lock_irqsave(&rstc->lock, flags);

	reg = readl(rstc->membase + offset);
	writel(reg & ~BIT(bit), rstc->membase + offset);

	spin_unlock_irqrestore(&rstc->lock, flags);

	return 0;
}

static const struct reset_control_ops hisi_reset_ops = {
	.assert		= hisi_reset_assert,
	.deassert	= hisi_reset_deassert,
};

struct hisi_reset_controller *hisi_reset_init(struct platform_device *pdev)
{
	struct hisi_reset_controller *rstc;

	rstc = devm_kmalloc(&pdev->dev, sizeof(*rstc), GFP_KERNEL);
	if (!rstc)
		return NULL;

	rstc->membase = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rstc->membase))
		return NULL;

	spin_lock_init(&rstc->lock);
	rstc->rcdev.owner = THIS_MODULE;
	rstc->rcdev.ops = &hisi_reset_ops;
	rstc->rcdev.of_node = pdev->dev.of_node;
	rstc->rcdev.of_reset_n_cells = 2;
	rstc->rcdev.of_xlate = hisi_reset_of_xlate;
	reset_controller_register(&rstc->rcdev);

	return rstc;
}
EXPORT_SYMBOL_GPL(hisi_reset_init);

void hisi_reset_exit(struct hisi_reset_controller *rstc)
{
	reset_controller_unregister(&rstc->rcdev);
}
EXPORT_SYMBOL_GPL(hisi_reset_exit);

int hisi_crg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hisi_crg_dev *crg;
	int ret;

	crg = devm_kmalloc(dev, sizeof(*crg), GFP_KERNEL);
	if (!crg)
		return -ENOMEM;

	ret = hisi_clk_probe(pdev);
	if (ret)
		return ret;

	crg->rstc = hisi_reset_init(pdev);
	if (!crg->rstc) {
		ret = -ENOMEM;
		goto err;
	}

	platform_set_drvdata(pdev, crg);
	return 0;

err:
	hisi_clk_remove(pdev);
	return ret;
}
EXPORT_SYMBOL_GPL(hisi_crg_probe);

void hisi_crg_remove(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);

	hisi_reset_exit(crg->rstc);
	hisi_clk_remove(pdev);
}
EXPORT_SYMBOL_GPL(hisi_crg_remove);
