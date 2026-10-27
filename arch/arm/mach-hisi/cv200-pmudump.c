// SPDX-License-Identifier: GPL-2.0
/*
 * Bench instrument for the Hi3716CV200 CPU performance monitor.
 *
 * perf_event_open(2) says the armv7 PMU exists (7 counters announced) and the
 * pmu node now has its interrupt wired, yet the cycle and instruction counters
 * both read 0 across a busy loop and the arm-pmu interrupt count never moves.
 * That combination is not visible from EL0 at all, so this dumps the CP15 side
 * of it from EL1: whether the global enable and the per-counter enable and
 * interrupt-enable bits are actually set in the hardware, whether PMCCNTR ticks
 * in kernel context, and what the implementation-defined control registers (A9
 * auxiliary control, secure debug enable, coprocessor access) say.
 *
 * The encodings are the ones drivers/perf/arm_v7_pmu.c uses: PMCR c9,c12,0,
 * PMCNTENSET c9,c12,1, PMOVSR c9,c12,3, PMSELR c9,c12,5, PMCCNTR c9,c13,0,
 * PMXEVTYPER c9,c13,1, PMINTENSET c9,c14,1.
 *
 * Not a driver: debugfs only, no binding, deleted before any submission.
 * Reading /sys/kernel/debug/cv200_pmu dumps every online CPU; writing "1"
 * enables the manual arm on subsequent reads, writing "0" turns it off, and
 * writing any other hexadecimal value ORs it into the auxiliary control
 * register of every CPU.
 */
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/seq_file.h>
#include <linux/smp.h>
#include <linux/types.h>

/*
 * The distributor's view of the INTID the pmu node names, per CPU. INTID 16..31
 * (the private range) is banked per core in this GIC, so the words have to be
 * read from the core they describe -- which is what pmu_take_sample is: it runs
 * as an IPI-served function on every online CPU. This is what separates "the
 * PMI is asserted and never taken" from "nobody enabled INTID 22 here".
 * ICC_IAR is deliberately not read: reading it acknowledges an interrupt.
 */
#define GICD_ISENABLER0		0x100
#define GICD_ICENABLER0		0x180
#define GICD_ISPENDR0		0x200
#define GICD_ISACTIVER0		0x300
#define GICD_INTID22		BIT(22)
#define ICC_PMR			0x04
#define ICC_BPR			0x08

static void __iomem *gicd;
static void __iomem *gicc;

struct pmu_sample {
	unsigned int	cpu;
	unsigned int	pmcr;
	unsigned int	ens;	/* PMCNTENSET  */
	unsigned int	intens;	/* PMINTENSET  */
	unsigned int	ovf;	/* PMOVSR      */
	unsigned int	cc0;	/* PMCCNTR before the loop */
	unsigned int	cc1;	/* PMCCNTR after  the loop */
	unsigned int	evt;	/* PMXEVTYPER after selecting counter 0 */
	unsigned int	actlr;	/* c1,c0,1: A9 auxiliary control */
	unsigned int	sder;	/* c1,c1,1: secure debug enable  */
	unsigned int	cpacr;	/* c1,c0,2: coprocessor access   */
	unsigned int	man_pmcr;	/* PMCR right after the manual arm */
	unsigned int	man_ens;	/* PMCNTENSET after the manual arm  */
	unsigned int	man_cc0;	/* PMCCNTR before the manual loop   */
	unsigned int	man_cc1;	/* PMCCNTR after  the manual loop   */
	unsigned int	gd_en;		/* GICD_ISENABLER0   */
	unsigned int	gd_pend;	/* GICD_ISPENDR0     */
	unsigned int	gd_act;		/* GICD_ISACTIVER0   */
	unsigned int	ic_pmr;		/* ICC_PMR           */
	unsigned int	ic_bpr;		/* ICC_BPR           */
	unsigned int	en22;		/* INTID 22 written then read back */
};

static DEFINE_PER_CPU(struct pmu_sample, pmu_sample);

/* set by writing 1 to the file: run the manual arm on each read */
static bool manual;

/* set by writing 2: enable INTID 22 and read the bit back on every CPU;
 * writing 3 clears it again.
 */
static bool int22;

static unsigned int rd_pmcr(void)
{
	unsigned int v;

	asm volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(v));

	return v;
}

static unsigned int rd_ens(void)
{
	unsigned int v;

	asm volatile("mrc p15, 0, %0, c9, c12, 1" : "=r"(v));

	return v;
}

static unsigned int rd_ovf(void)
{
	unsigned int v;

	asm volatile("mrc p15, 0, %0, c9, c12, 3" : "=r"(v));

	return v;
}

static unsigned int rd_cc(void)
{
	unsigned int v;

	asm volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));

	return v;
}

static unsigned int rd_evt0(void)
{
	unsigned int zero = 0, v;

	asm volatile("mcr p15, 0, %0, c9, c12, 5" : : "r"(zero));
	asm volatile("isb");
	asm volatile("mrc p15, 0, %0, c9, c13, 1" : "=r"(v));

	return v;
}

static unsigned int rd_intens(void)
{
	unsigned int v;

	asm volatile("mrc p15, 0, %0, c9, c14, 1" : "=r"(v));

	return v;
}

static unsigned int rd_actlr(void)
{
	unsigned int v;

	asm volatile("mrc p15, 0, %0, c1, c0, 1" : "=r"(v));

	return v;
}

static unsigned int rd_sder(void)
{
	unsigned int v;

	asm volatile("mrc p15, 0, %0, c1, c1, 1" : "=r"(v));

	return v;
}

static unsigned int rd_cpacr(void)
{
	unsigned int v;

	asm volatile("mrc p15, 0, %0, c1, c0, 2" : "=r"(v));

	return v;
}

static void wr_pmcr(unsigned int v)
{
	asm volatile("mcr p15, 0, %0, c9, c12, 0" : : "r"(v));
	asm volatile("isb");
}

static void wr_cc(unsigned int v)
{
	asm volatile("mcr p15, 0, %0, c9, c13, 0" : : "r"(v));
	asm volatile("isb");
}

static void wr_ens(unsigned int v)
{
	asm volatile("mcr p15, 0, %0, c9, c12, 1" : : "r"(v));
	asm volatile("isb");
}

/* PMCNTENSET is write-1-to-enable; writing 0 there does nothing, so clearing
 * needs the separate PMCNTENCLR (c9,c12,2). */
static void wr_clr(unsigned int v)
{
	asm volatile("mcr p15, 0, %0, c9, c12, 2" : : "r"(v));
	asm volatile("isb");
}

/*
 * Manual arm: take the PMU out of reset by hand -- global enable with the
 * by-64 prescaler off, PMCCNTR zeroed, the cycle counter's own enable bit set
 * -- then measure it across a loop. This is the one measurement that separates
 * "the PMU cannot count on this part" from "the perf driver's enable does not
 * reach the hardware", and it is deliberately not the driver's code path.
 */
static void pmu_measure_manual(struct pmu_sample *s)
{
	unsigned int i;
	unsigned int pmcr;

	wr_cc(0);
	pmcr = rd_pmcr() & ~(BIT(3) | BIT(5));		/* clear D and DP */
	wr_pmcr(pmcr | BIT(0));				/* E=1 */
	s->man_pmcr = rd_pmcr();

	wr_ens(BIT(31));				/* PMCNTENSET.CC */
	s->man_ens = rd_ens();
	s->man_cc0 = rd_cc();

	preempt_disable();
	for (i = 0; i < 2000000; i++)
		asm volatile("" ::: "memory");
	s->man_cc1 = rd_cc();
	preempt_enable();

	/* leave the PMU as the driver expects to find it: everything off */
	wr_clr(BIT(31));
	wr_pmcr(rd_pmcr() & ~BIT(0));
}

static void pmu_take_sample(void *info)
{
	struct pmu_sample *s = this_cpu_ptr(&pmu_sample);
	unsigned int i;

	s->cpu = smp_processor_id();
	s->pmcr = rd_pmcr();
	s->ens = rd_ens();
	s->intens = rd_intens();
	s->ovf = rd_ovf();
	s->evt = rd_evt0();
	s->cc0 = rd_cc();

	/*
	 * Preemption off, not interrupts: this runs as an IPI-served function
	 * (on_each_cpu with wait=1), and flipping the interrupt state there is
	 * what made tick/irq_work BUG -- twice, at 115 s and at 21 s, both
	 * blamed on the board before they were traced here. 2M iterations is a
	 * few milliseconds on an A9, so the loop stays on this CPU anyway.
	 */
	preempt_disable();
	for (i = 0; i < 2000000; i++)
		asm volatile("" ::: "memory");
	s->cc1 = rd_cc();
	preempt_enable();

	s->actlr = rd_actlr();
	s->sder = rd_sder();
	s->cpacr = rd_cpacr();

	if (gicd) {
		s->gd_en = readl_relaxed(gicd + GICD_ISENABLER0);
		s->gd_pend = readl_relaxed(gicd + GICD_ISPENDR0);
		s->gd_act = readl_relaxed(gicd + GICD_ISACTIVER0);
		/*
		 * The one question the read-only dumps cannot answer is whether
		 * the enable bit itself is writable: "reads 0" is also the shape
		 * of a RAZ/WI bit, and an unimplemented PPI input looks the same
		 * from here as a driver that never enabled it. Writing and reading
		 * back separates them.
		 */
		if (int22) {
			writel_relaxed(GICD_INTID22, gicd + GICD_ISENABLER0);
			s->en22 = readl_relaxed(gicd + GICD_ISENABLER0) & GICD_INTID22;
		}
	}
	if (gicc) {
		s->ic_pmr = readl_relaxed(gicc + ICC_PMR);
		s->ic_bpr = readl_relaxed(gicc + ICC_BPR);
	}

	if (manual)
		pmu_measure_manual(s);
}

static int pmu_show(struct seq_file *f, void *p)
{
	int cpu;

	on_each_cpu(pmu_take_sample, NULL, 1);

	for_each_online_cpu(cpu) {
		struct pmu_sample *s = per_cpu_ptr(&pmu_sample, cpu);

		seq_printf(f, "cpu%-2u PMCR=%08x (E=%u P=%u C=%u D=%u X=%u DP=%u N=%u) PMOVSR=%08x\n",
			   s->cpu, s->pmcr, (s->pmcr >> 0) & 1,
			   (s->pmcr >> 1) & 1, (s->pmcr >> 2) & 1,
			   (s->pmcr >> 3) & 1, (s->pmcr >> 4) & 1,
			   (s->pmcr >> 5) & 1,
			   (s->pmcr >> 11) & 0x1f, s->ovf);
		seq_printf(f, "cpu%-2u PMCCNTR=%08x -> %08x  delta=%u\n",
			   s->cpu, s->cc0, s->cc1,
			   (unsigned int)(s->cc1 - s->cc0));
		seq_printf(f, "cpu%-2u PMCNTENSET=%08x PMINTENSET=%08x PMXEVTYPER0=%08x\n",
			   s->cpu, s->ens, s->intens, s->evt);
		seq_printf(f, "cpu%-2u ACTLR=%08x SDER=%08x CPACR=%08x\n",
			   s->cpu, s->actlr, s->sder, s->cpacr);
		seq_printf(f, "cpu%-2u GICD en=%08x pend=%08x act=%08x  INTID22 en=%u pend=%u act=%u set-then-read=%u  ICC pmr=%08x bpr=%08x\n",
			   s->cpu, s->gd_en, s->gd_pend, s->gd_act,
			   !!(s->gd_en & GICD_INTID22),
			   !!(s->gd_pend & GICD_INTID22),
			   !!(s->gd_act & GICD_INTID22),
			   !!(s->en22 & GICD_INTID22),
			   s->ic_pmr, s->ic_bpr);
		if (manual)
			seq_printf(f, "cpu%-2u manual: PMCR=%08x (E=%u D=%u) PMCNTENSET=%08x PMCCNTR=%08x -> %08x delta=%u\n",
				   s->cpu, s->man_pmcr, (s->man_pmcr >> 0) & 1,
				   (s->man_pmcr >> 3) & 1, s->man_ens,
				   s->man_cc0, s->man_cc1,
				   (unsigned int)(s->man_cc1 - s->man_cc0));
	}

	return 0;
}

static int pmu_open(struct inode *inode, struct file *file)
{
	return single_open(file, pmu_show, NULL);
}

/*
 * OR a mask into the A9 auxiliary control register on every online CPU.
 * Deliberately only ORs: the bits this round is about are enable bits, and a
 * mask that clears one could take the machine down.
 */
static void pmu_set_actlr(void *info)
{
	unsigned int val = rd_actlr() | *(unsigned int *)info;

	asm volatile("mcr p15, 0, %0, c1, c0, 1" : : "r"(val));
	asm volatile("isb");
}

static void pmu_set_int22(void *info)
{
	bool on = *(bool *)info;

	if (!gicd)
		return;
	writel_relaxed(GICD_INTID22,
		       gicd + (on ? GICD_ISENABLER0 : GICD_ICENABLER0));
}

static ssize_t pmu_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *off)
{
	char kbuf[16];
	unsigned int mask;
	int ret;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;

	kbuf[count] = '\0';
	ret = kstrtouint(strim(kbuf), 16, &mask);
	if (ret)
		return ret;

	/* 1 turns the manual arm on, 0 turns it off; 2 enables INTID 22 on every
	 * CPU and reads the bit back in each dump, 3 clears it again; anything
	 * else is an ACTLR mask to OR in on every CPU.
	 */
	if (mask <= 1) {
		manual = mask;

		return count;
	}
	if (mask == 2 || mask == 3) {
		bool on = mask == 2;

		int22 = on;
		on_each_cpu(pmu_set_int22, &on, 1);

		return count;
	}

	on_each_cpu(pmu_set_actlr, &mask, 1);

	return count;
}

static const struct file_operations pmu_fops = {
	.owner		= THIS_MODULE,
	.open		= pmu_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
	.write		= pmu_write,
};

static int __init cv200_pmudump_init(void)
{
	/*
	 * Second mapping of the two GIC banks (the gic driver owns the first).
	 * Read-only here, and the addresses are this board's DT ones.
	 */
	gicd = ioremap(0xf8a01000, 0x1000);
	gicc = ioremap(0xf8a00100, 0x100);
	if (!gicd || !gicc)
		pr_warn("bench: cv200 PMU dump has no GIC view, INTID 22 cannot be read\n");

	debugfs_create_file("cv200_pmu", 0600, NULL, NULL, &pmu_fops);

	pr_info("bench: cv200 PMU dump at /sys/kernel/debug/cv200_pmu\n");

	return 0;
}
late_initcall(cv200_pmudump_init);
