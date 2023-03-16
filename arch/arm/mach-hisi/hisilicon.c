// SPDX-License-Identifier: GPL-2.0-only
/*
 * (HiSilicon's SoC based) flattened device tree enabled machine
 *
 * Copyright (c) 2012-2013 HiSilicon Ltd.
 * Copyright (c) 2012-2013 Linaro Ltd.
 *
 * Author: Haojian Zhuang <haojian.zhuang@linaro.org>
*/

#include <linux/clocksource.h>
#include <linux/irqchip.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#define HI3620_SYSCTRL_PHYS_BASE		0xfc802000
#define HI3620_SYSCTRL_VIRT_BASE		0xfe802000

/*
 * This table is only for optimization. Since ioremap() could always share
 * the same mapping if it's defined as static IO mapping.
 *
 * Without this table, system could also work. The cost is some virtual address
 * spaces wasted since ioremap() may be called multi times for the same
 * IO space.
 */
static struct map_desc hi3620_io_desc[] __initdata = {
	{
		/* sysctrl */
		.pfn		= __phys_to_pfn(HI3620_SYSCTRL_PHYS_BASE),
		.virtual	= HI3620_SYSCTRL_VIRT_BASE,
		.length		= 0x1000,
		.type		= MT_DEVICE,
	},
};

static void __init hi3620_map_io(void)
{
	debug_ll_io_init();
	iotable_init(hi3620_io_desc, ARRAY_SIZE(hi3620_io_desc));
}

static const char *const hi3xxx_compat[] __initconst = {
	"hisilicon,hi3620-hi4511",
	NULL,
};

DT_MACHINE_START(HI3620, "HiSilicon Hi3620 (Flattened Device Tree)")
	.map_io		= hi3620_map_io,
	.dt_compat	= hi3xxx_compat,
MACHINE_END

#define S40_IOCH1_PHYS_BASE		0xf8000000
#define S40_IOCH1_VIRT_BASE		0xf9000000
#define S40_IOCH1_SIZE			0x02000000

static struct map_desc s40_io_desc[] __initdata = {
	{
		.pfn		= __phys_to_pfn(S40_IOCH1_PHYS_BASE),
		.virtual	= S40_IOCH1_VIRT_BASE,
		.length		= S40_IOCH1_SIZE,
		.type		= MT_DEVICE,
	},
};

static void __init s40_map_io(void)
{
	debug_ll_io_init();
	iotable_init(s40_io_desc, ARRAY_SIZE(s40_io_desc));
}

static const char *const s40_compat[] __initconst = {
	"hisilicon,hi3796cv200",
	"hisilicon,hi3796mv200",
	"hisilicon,hi3798cv200",
	"hisilicon,hi3798mv200",
	"hisilicon,hi3798mv300",
	NULL,
};

DT_MACHINE_START(S40, "HiSilicon S40 (Flattened Device Tree)")
	.map_io		= s40_map_io,
	.dt_compat	= s40_compat,
MACHINE_END
