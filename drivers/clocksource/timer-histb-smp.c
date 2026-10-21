// SPDX-License-Identifier: GPL-2.0
/*
 * HiSilicon Hi3716CV200 per-CPU SP804 local timers
 *
 * Copyright (c) 2016-2017 HiSilicon Technologies Co., Ltd.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_clk.h>
#include <linux/of_irq.h>

#include "timer-sp.h"

/*
 * Hi3716CV200 has no usable ARM local timer: the Cortex-A9 private
 * timer/watchdog block and the global timer both keep the value written and
 * never count (measured on the board by enabling the block at 0xf8a00620 with
 * the same control the running SP804 clocksource uses, and reading the counter
 * back across a 3 s window -- 0xFFFFFFFF before and after, 2026-10-21). So a
 * clocks-less arm,cortex-a9-twd-timer node cannot even self-calibrate here,
 * and the tick has to come from somewhere else.
 *
 * The vendor build for this chip takes the tick from one SP804 channel per
 * CPU: mach-s40/timer.c indexes hi_timer[(cpu * 2) + 4], which is cell 4 at
 * 0xf8a2a000 for cpu0 with SPI 26 and cell 6 at 0xf8a2b000 for cpu1 with
 * SPI 27, and configs/hi3716cv200_defconfig has HAVE_SP804_LOCAL_TIMER=y with
 * HAVE_ARM_LOCAL_TIMER unset. Each channel is a plain 32-bit SP804 timer with
 * the standard register layout, so the access pattern is the one
 * drivers/clocksource/timer-sp804.c already uses; what differs is that these
 * are per-CPU event devices instead of the single broadcast device, which is
 * what buys one-shot clock events and hrtimers.
 *
 * The channel rate is measured, not assumed: with cell 4 enabled next to the
 * running clocksource cell, the two counted down at the same rate (257814107
 * versus 257813964 counts between two samples, ratio 1.0000006), so both are
 * fed the 24 MHz crystal -- the same value the vendor's fixed sp804_clk
 * declares (mach-s40/core.c:371).
 */

#define MAX_CPUS	2

struct histb_timer {
	void __iomem			*base;
	struct clock_event_device	evt;
	unsigned int			irq;
	unsigned int			cpu;
};

static struct histb_timer histb_timers[MAX_CPUS];
static unsigned long histb_rate;
static unsigned long histb_reload;

#define histb_evt_to_timer(evt)	container_of(evt, struct histb_timer, evt)

static void histb_timer_disable(struct histb_timer *timer)
{
	writel_relaxed(0, timer->base + TIMER_CTRL);
	writel_relaxed(1, timer->base + TIMER_INTCLR);
}

static int histb_timer_shutdown(struct clock_event_device *evt)
{
	histb_timer_disable(histb_evt_to_timer(evt));

	return 0;
}

static int histb_timer_set_periodic(struct clock_event_device *evt)
{
	struct histb_timer *timer = histb_evt_to_timer(evt);

	histb_timer_disable(timer);
	writel_relaxed(histb_reload, timer->base + TIMER_LOAD);
	writel_relaxed(TIMER_CTRL_32BIT | TIMER_CTRL_IE | TIMER_CTRL_PERIODIC |
		       TIMER_CTRL_ENABLE, timer->base + TIMER_CTRL);

	return 0;
}

static int histb_timer_set_next_event(unsigned long next,
				      struct clock_event_device *evt)
{
	struct histb_timer *timer = histb_evt_to_timer(evt);

	writel_relaxed(next, timer->base + TIMER_LOAD);
	writel_relaxed(TIMER_CTRL_32BIT | TIMER_CTRL_IE | TIMER_CTRL_ONESHOT |
		       TIMER_CTRL_ENABLE, timer->base + TIMER_CTRL);

	return 0;
}

static irqreturn_t histb_timer_interrupt(int irq, void *dev_id)
{
	struct histb_timer *timer = dev_id;

	writel_relaxed(1, timer->base + TIMER_INTCLR);
	timer->evt.event_handler(&timer->evt);

	return IRQ_HANDLED;
}

static int histb_timer_starting_cpu(unsigned int cpu)
{
	struct histb_timer *timer = &histb_timers[cpu];

	/*
	 * The channel's SPI has to be taken by its own CPU only; if the affinity
	 * call fails the tick would migrate and every other CPU would wait on a
	 * counter this one is not servicing.
	 */
	if (irq_set_affinity(timer->irq, cpumask_of(timer->cpu)))
		pr_err("IRQ %u not pinned to CPU %u\n", timer->irq,
		       timer->cpu);

	clockevents_config_and_register(&timer->evt, histb_rate, 0xf,
					0xffffffff);
	enable_irq(timer->irq);

	return 0;
}

static int histb_timer_dying_cpu(unsigned int cpu)
{
	disable_irq(histb_timers[cpu].irq);

	return 0;
}

static int __init histb_local_timer_init(struct device_node *np)
{
	unsigned int i, nreg, nirq;
	struct clk *clk;
	int ret;

	nirq = of_irq_count(np);
	nreg = of_property_count_u32_elems(np, "reg") / 2;
	if (nirq <= 0 || nreg <= 0 || nirq != nreg)
		return -EINVAL;
	if (nreg > MAX_CPUS)
		nreg = MAX_CPUS;

	clk = of_clk_get(np, 0);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	ret = clk_prepare_enable(clk);
	if (ret)
		return ret;

	histb_rate = clk_get_rate(clk);
	if (!histb_rate) {
		pr_err("clock reports 0 Hz\n");
		return -EINVAL;
	}
	histb_reload = DIV_ROUND_CLOSEST(histb_rate, HZ);

	for (i = 0; i < nreg; i++) {
		struct histb_timer *timer = &histb_timers[i];

		/*
		 * reg and interrupts are listed in CPU order -- the vendor picks
		 * a core's channel by index, so a device tree that swapped the
		 * pairs would hand a CPU another CPU's counter.
		 */
		timer->base = of_iomap(np, i);
		if (!timer->base) {
			pr_err("no mapping for channel %u\n", i);
			return -ENXIO;
		}

		timer->irq = irq_of_parse_and_map(np, i);
		if (!timer->irq) {
			pr_err("no interrupt for channel %u\n", i);
			return -EINVAL;
		}

		writel_relaxed(0, timer->base + TIMER_CTRL);

		timer->cpu = i;
		timer->evt.name = "local_timer";
		timer->evt.features = CLOCK_EVT_FEAT_PERIODIC |
				      CLOCK_EVT_FEAT_ONESHOT;
		timer->evt.set_state_shutdown = histb_timer_shutdown;
		timer->evt.set_state_periodic = histb_timer_set_periodic;
		timer->evt.set_state_oneshot = histb_timer_shutdown;
		timer->evt.tick_resume = histb_timer_shutdown;
		timer->evt.set_next_event = histb_timer_set_next_event;
		timer->evt.cpumask = cpumask_of(i);
		timer->evt.irq = timer->irq;
		/*
		 * Above the arm,sp804 broadcast device (rating 300) so every CPU
		 * that owns a channel uses its own, while the broadcast one stays
		 * the clocksource and the fallback for a CPU with no channel.
		 *
		 * No CLOCK_EVT_FEAT_C3STOP: nothing here switches this block off
		 * in idle, and claiming it would keep the broadcast device in use.
		 */
		timer->evt.rating = 350;

		irq_set_status_flags(timer->irq, IRQ_PER_CPU);
		ret = request_irq(timer->irq, histb_timer_interrupt,
				  IRQF_TIMER | IRQF_NO_AUTOEN,
				  "histb-smp-timer", timer);
		if (ret) {
			pr_err("IRQ %u failed (%d)\n", timer->irq, ret);
			return ret;
		}
	}

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
				"clocksource/timer_histb_smp:online",
				histb_timer_starting_cpu,
				histb_timer_dying_cpu);
	if (ret < 0) {
		pr_err("hotplug state failed (%d)\n", ret);
		return ret;
	}

	/*
	 * cpuhp_setup_state() already ran the startup callback for every CPU that
	 * is online now, the boot CPU included, so there is nothing left to do.
	 */
	return 0;
}

TIMER_OF_DECLARE(histb_smp_timer, "hisilicon,hi3716cv200-smp-timer",
		 histb_local_timer_init);
