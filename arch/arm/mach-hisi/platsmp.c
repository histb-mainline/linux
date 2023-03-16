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

/*
 * hisi_pen_release controls the release of CPUs from the holding
 * pen in headsmp.S, which exists because we are not always able to
 * control the release of individual CPUs from the board firmware.
 */
volatile int hisi_pen_release = -1;

/*
 * Write hisi_write_pen_release in a way that is guaranteed to be visible to
 * all observers, irrespective of whether they're taking part in coherency
 * or not.  This is necessary for the hotplug code to work reliably.
 */
static void hisi_write_pen_release(int val)
{
	hisi_pen_release = val;
	smp_wmb();
	sync_cache_w(&hisi_pen_release);
}

/*
 * hisi_lock exists to avoid running the loops_per_jiffy delay loop
 * calibrations on the secondary CPU while the requesting CPU is using
 * the limited-bandwidth bus - which affects the calibration value.
 */
static DEFINE_RAW_SPINLOCK(hisi_lock);

static void hisi_pen_secondary_init(unsigned int cpu)
{
	/*
	 * let the primary processor know we're out of the
	 * pen, then head off into the C entry point
	 */
	hisi_write_pen_release(-1);

	/*
	 * Synchronise with the boot thread.
	 */
	raw_spin_lock(&hisi_lock);
	raw_spin_unlock(&hisi_lock);
}


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
				  __pa_symbol(hisi_secondary_startup));
}

static int hi3798_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned long timeout;

	/*
	 * Set synchronisation state between this boot processor
	 * and the secondary one
	 */
	raw_spin_lock(&hisi_lock);

	hi3798_set_cpu(cpu, true);

	/*
	 * This is really belt and braces; we hold unintended secondary
	 * CPUs in the holding pen until we're ready for them.  However,
	 * since we haven't sent them a soft interrupt, they shouldn't
	 * be there.
	 */
	hisi_write_pen_release(cpu);

	/*
	 * Send the secondary CPU a soft interrupt, thereby causing
	 * the boot monitor to read the system wide flags register,
	 * and branch to the address found there.
	 */
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));

	timeout = jiffies + (1 * HZ);
	while (time_before(jiffies, timeout)) {
		smp_rmb();
		if (hisi_pen_release == -1)
			break;

		udelay(10);
	}

	/*
	 * now the secondary core is starting up let it run its
	 * calibrations, then wait for it to finish
	 */
	raw_spin_unlock(&hisi_lock);

	return hisi_pen_release != -1 ? -ENOSYS : 0;
}

static const struct smp_operations hi3798_smp_ops __initconst = {
	.smp_prepare_cpus	= hi3798_smp_prepare_cpus,
	.smp_secondary_init	= hisi_pen_secondary_init,
	.smp_boot_secondary	= hi3798_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= hi3798_cpu_die,
	.cpu_kill		= hi3798_cpu_kill,
#endif
};


CPU_METHOD_OF_DECLARE(hi3xxx_smp, "hisilicon,hi3620-smp", &hi3xxx_smp_ops);
CPU_METHOD_OF_DECLARE(hix5hd2_smp, "hisilicon,hix5hd2-smp", &hix5hd2_smp_ops);
CPU_METHOD_OF_DECLARE(hip01_smp, "hisilicon,hip01-smp", &hip01_smp_ops);
CPU_METHOD_OF_DECLARE(hi3798_smp, "hisilicon,hi3798-smp", &hi3798_smp_ops);
