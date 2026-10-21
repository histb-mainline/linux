// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013 Linaro Ltd.
 * Copyright (c) 2013 HiSilicon Limited.
 * Based on arch/arm/mach-vexpress/platsmp.c, Copyright (C) 2002 ARM Ltd.
 */
#include <linux/smp.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/delay.h>

#include <asm/cacheflush.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>
#include <asm/mach/map.h>

#include "core.h"

#define HIX5HD2_BOOT_ADDRESS		0xffff0000

static void __iomem *ctrl_base;

void hi3xxx_set_cpu_jump(int cpu, void *jump_addr)
{
	cpu = cpu_logical_map(cpu);
	if (!cpu || !ctrl_base)
		return;
	writel_relaxed(__pa_symbol(jump_addr), ctrl_base + ((cpu - 1) << 2));
}

int hi3xxx_get_cpu_jump(int cpu)
{
	cpu = cpu_logical_map(cpu);
	if (!cpu || !ctrl_base)
		return 0;
	return readl_relaxed(ctrl_base + ((cpu - 1) << 2));
}

static void __init hisi_enable_scu_a9(void)
{
	unsigned long base = 0;
	void __iomem *scu_base = NULL;

	if (scu_a9_has_base()) {
		base = scu_a9_get_base();
		scu_base = ioremap(base, SZ_4K);
		if (!scu_base) {
			pr_err("ioremap(scu_base) failed\n");
			return;
		}
		scu_enable(scu_base);
		iounmap(scu_base);
	}
}

static void __init hi3xxx_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np = NULL;
	u32 offset = 0;

	hisi_enable_scu_a9();
	if (!ctrl_base) {
		np = of_find_compatible_node(NULL, NULL, "hisilicon,sysctrl");
		if (!np) {
			pr_err("failed to find hisilicon,sysctrl node\n");
			return;
		}
		ctrl_base = of_iomap(np, 0);
		if (!ctrl_base) {
			of_node_put(np);
			pr_err("failed to map address\n");
			return;
		}
		if (of_property_read_u32(np, "smp-offset", &offset) < 0) {
			of_node_put(np);
			pr_err("failed to find smp-offset property\n");
			return;
		}
		ctrl_base += offset;
		of_node_put(np);
	}
}

static int hi3xxx_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	hi3xxx_set_cpu(cpu, true);
	hi3xxx_set_cpu_jump(cpu, secondary_startup);
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));
	return 0;
}

static const struct smp_operations hi3xxx_smp_ops __initconst = {
	.smp_prepare_cpus	= hi3xxx_smp_prepare_cpus,
	.smp_boot_secondary	= hi3xxx_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= hi3xxx_cpu_die,
	.cpu_kill		= hi3xxx_cpu_kill,
#endif
};

static void __init hisi_common_smp_prepare_cpus(unsigned int max_cpus)
{
	hisi_enable_scu_a9();
}

static void hix5hd2_set_scu_boot_addr(phys_addr_t start_addr, phys_addr_t jump_addr)
{
	void __iomem *virt;

	virt = ioremap(start_addr, PAGE_SIZE);

	writel_relaxed(0xe51ff004, virt);	/* ldr pc, [pc, #-4] */
	writel_relaxed(jump_addr, virt + 4);	/* pc jump phy address */
	iounmap(virt);
}

static int hix5hd2_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	phys_addr_t jumpaddr;

	jumpaddr = __pa_symbol(secondary_startup);
	hix5hd2_set_scu_boot_addr(HIX5HD2_BOOT_ADDRESS, jumpaddr);
	hix5hd2_set_cpu(cpu, true);
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));
	return 0;
}


static const struct smp_operations hix5hd2_smp_ops __initconst = {
	.smp_prepare_cpus	= hisi_common_smp_prepare_cpus,
	.smp_boot_secondary	= hix5hd2_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= hix5hd2_cpu_die,
#endif
};


#define SC_SCTL_REMAP_CLR      0x00000100
#define HIP01_BOOT_ADDRESS     0x80000000
#define REG_SC_CTRL            0x000

static void hip01_set_boot_addr(phys_addr_t start_addr, phys_addr_t jump_addr)
{
	void __iomem *virt;

	virt = phys_to_virt(start_addr);

	writel_relaxed(0xe51ff004, virt);
	writel_relaxed(jump_addr, virt + 4);
}

static int hip01_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	phys_addr_t jumpaddr;
	unsigned int remap_reg_value = 0;
	struct device_node *node;


	jumpaddr = __pa_symbol(secondary_startup);
	hip01_set_boot_addr(HIP01_BOOT_ADDRESS, jumpaddr);

	node = of_find_compatible_node(NULL, NULL, "hisilicon,hip01-sysctrl");
	if (WARN_ON(!node))
		return -1;
	ctrl_base = of_iomap(node, 0);
	of_node_put(node);

	/* set the secondary core boot from DDR */
	remap_reg_value = readl_relaxed(ctrl_base + REG_SC_CTRL);
	barrier();
	remap_reg_value |= SC_SCTL_REMAP_CLR;
	barrier();
	writel_relaxed(remap_reg_value, ctrl_base + REG_SC_CTRL);

	hip01_set_cpu(cpu, true);

	return 0;
}

static const struct smp_operations hip01_smp_ops __initconst = {
	.smp_prepare_cpus       = hisi_common_smp_prepare_cpus,
	.smp_boot_secondary     = hip01_boot_secondary,
};

/*
 * Hi3716CV200 (2x Cortex-A9): no PSCI and no power controller firmware, so the
 * kernel releases the secondary core itself. Its reset vector is the first word of
 * the last SRAM page, and the vendor boot code leaves a two-word trampoline there
 * -- "ldr pc, [pc, #-4]" followed by the address to jump to -- which is the same
 * trick hix5hd2 above uses. Powering the core up is a PMC sequence and then one
 * CRG bit clears its reset, both taken from mach-s40/hotplug.c
 * (s40_scu_power_up()) and platform.h for this chip.
 */
#define HI3716CV200_SRAM_BOOT_ADDR		0xffff0000
#define HI3716CV200_PMC_BASE			0xf8a23000
#define  PMC_CTRL				0x00
#define  PMC_CORE1_MTCMOS			BIT(0)
#define  PMC_CORE1_PD_REQ			BIT(3)
#define  PMC_CORE1_WAIT_MTCMOS_ACK		BIT(8)
#define  PMC_CORE1_ISO				0x04
#define  PMC_CORE1_CLAMP			0x0c
#define HI3716CV200_CRG_CPU_RST			0xf8a22050
#define  HI3716CV200_CORE1_RESET		BIT(17)

static void __iomem *hi3716cv200_pmc;
static void __iomem *hi3716cv200_crg_rst;

static void __init hi3716cv200_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np;
	void __iomem *scu;

	np = of_find_compatible_node(NULL, NULL, "arm,cortex-a9-scu");
	scu = np ? of_iomap(np, 0) : NULL;
	if (np)
		of_node_put(np);
	if (scu)
		scu_enable(scu);
	else
		pr_warn("hisi-smp: no A9 SCU mapped, the cluster stays non-coherent\n");

	hi3716cv200_pmc = ioremap(HI3716CV200_PMC_BASE, SZ_4K);
	hi3716cv200_crg_rst = ioremap(HI3716CV200_CRG_CPU_RST, SZ_4K);
	if (!hi3716cv200_pmc || !hi3716cv200_crg_rst) {
		pr_err("hisi-smp: no PMC or CPU reset window, secondary cpus stay down\n");
		return;
	}

	/* The vendor writes these two before ever touching the core bits. */
	writel(0x3, hi3716cv200_pmc + PMC_CORE1_CLAMP);
	writel(0xffff88, hi3716cv200_pmc + PMC_CORE1_ISO);
}

static int hi3716cv200_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	void __iomem *virt;
	u32 val;

	if (cpu != 1) {
		pr_err("hisi-smp: can only release cpu1 on this chip\n");
		return -EINVAL;
	}

	virt = ioremap(HI3716CV200_SRAM_BOOT_ADDR, PAGE_SIZE);
	if (!virt)
		return -ENOMEM;
	writel(0xe51ff004, virt);	/* ldr pc, [pc, #-4] */
	writel(__pa_symbol(secondary_startup), virt + 4);
	iounmap(virt);

	val = readl(hi3716cv200_pmc + PMC_CTRL);
	val &= ~(PMC_CORE1_PD_REQ | PMC_CORE1_WAIT_MTCMOS_ACK);
	val |= PMC_CORE1_MTCMOS;
	writel(val, hi3716cv200_pmc + PMC_CTRL);

	writel(readl(hi3716cv200_crg_rst) & ~HI3716CV200_CORE1_RESET,
	       hi3716cv200_crg_rst);

	arch_send_wakeup_ipi_mask(cpumask_of(cpu));
	return 0;
}

static const struct smp_operations hi3716cv200_smp_ops __initconst = {
	.smp_prepare_cpus	= hi3716cv200_smp_prepare_cpus,
	.smp_boot_secondary	= hi3716cv200_boot_secondary,
};


static void hi3798_smp_prepare_cpus(unsigned int max_cpus)
{
	unsigned int i;
	unsigned int l2ctlr;
	unsigned int ncores;

	asm ("mrc p15, 1, %0, c9, c0, 2\n" : "=r" (l2ctlr));
	ncores = ((l2ctlr >> 24) & 0x3) + 1;

	pr_info("smp: %u cores detected\n", ncores);
	if (ncores > max_cpus) {
		pr_warn("smp: %u cores greater than maximum (%u), clipping\n",
			ncores, max_cpus);
		ncores = max_cpus;
	}
	for (i = 0; i < ncores; i++)
		set_cpu_possible(i, true);

	/* Put the boot address in this magic register */
	hix5hd2_set_scu_boot_addr(HIX5HD2_BOOT_ADDRESS,
				  __pa_symbol(secondary_startup));
}

static int hi3798_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	hi3798_set_cpu(cpu, true);
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));
	return 0;
}

static const struct smp_operations hi3798_smp_ops __initconst = {
	.smp_prepare_cpus	= hi3798_smp_prepare_cpus,
	.smp_boot_secondary	= hi3798_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= hi3798_cpu_die,
	.cpu_kill		= hi3798_cpu_kill,
#endif
};

CPU_METHOD_OF_DECLARE(hi3716cv200_smp, "hisilicon,hi3716cv200-smp", &hi3716cv200_smp_ops);
CPU_METHOD_OF_DECLARE(hi3xxx_smp, "hisilicon,hi3620-smp", &hi3xxx_smp_ops);
CPU_METHOD_OF_DECLARE(hix5hd2_smp, "hisilicon,hix5hd2-smp", &hix5hd2_smp_ops);
CPU_METHOD_OF_DECLARE(hip01_smp, "hisilicon,hip01-smp", &hip01_smp_ops);
CPU_METHOD_OF_DECLARE(hi3798_smp, "hisilicon,hi3798-smp", &hi3798_smp_ops);
