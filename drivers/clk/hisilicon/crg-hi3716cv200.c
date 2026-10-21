// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSilicon Hi3716CV200 Clock and Reset Generator driver
 *
 * Copyright (c) 2016-2017 HiSilicon Technologies Co., Ltd.
 */

#include <dt-bindings/clock/hisilicon,hi3716cv200-crg.h>
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "clk.h"
#include "crg.h"
#include "reset.h"

/*
 * Fixed sources. The first three are declared by the 3.10 machine code for this
 * chip (mach-s40/cpu/hi3716cv200/hi3716cv200.c:38-49): clk_uart runs at 83.3
 * MHz, clk_uart1 at 3 MHz, and the crystal behind the SoC timers at 24 MHz
 * (core.c:371-379, matching the vendor boot's "sched_clock: 32 bits at 24MHz").
 * The 100 MHz behind the I2C gates is what the vendor bus driver divides:
 * std_i2c/drv_i2c.h:13 names it I2C_DFT_SYSCLK for this chip.
 *
 * These are published by the provider instead of declared in the device tree
 * because a bit group reaches its source through clk_hw, so only a clock this
 * driver registered can be one -- and a source named in both places makes the
 * second registration fail with -EEXIST, which aborts the probe and leaves every
 * CRG consumer deferred.
 */
static const struct hisi_fixed_rate_clock hi3716cv200_fixed_rate_clks[] = {
	{ HI3716CV200_CLK_OSC24M, "clk_osc24m", NULL, 0, 24000000, },
	{ HI3716CV200_CLK_3M, "clk_3m", NULL, 0, 3000000, },
	{ HI3716CV200_CLK_83P3M, "clk_83p3m", NULL, 0, 83300000, },
	{ HI3716CV200_CLK_I2C100M, "clk_i2c100m", NULL, 0, 100000000, },
	/*
	 * The two rates the SDIO hosts are left running at, from the selector table in
	 * himci_s40.c:28-31 read against the board's own CRG39/CRG40 words (clk_sel 0 and
	 * 2). dw_mmc divides this rate for the card clock and refuses to probe at 0 Hz, so
	 * naming it is what makes the host usable; a gate whose selector is left outside its
	 * mask has to record the rate that choice implies.
	 */
	{ HI3716CV200_CLK_SDIO75M, "clk_sdio75m", NULL, 0, 75000000, },
	{ HI3716CV200_CLK_SDIO50M, "clk_sdio50m", NULL, 0, 50000000, },
	/*
	 * The rate CRG24's nandc_clk_sel code 4 stands for (hinfc610_s40.c:22), which is
	 * what the board's own vendor system has the NAND controller running at.
	 */
	{ HI3716CV200_CLK_NAND200M, "clk_nand200m", NULL, 0, 200000000, },
};

struct hi3716cv200_clk_bits {
	struct clk_hw	hw;
	void __iomem	*reg;
	u32		mask;
	u32		value;
	/*
	 * clk_init_data wants an array of parents, so the one parent a bit group
	 * may have is kept here and handed over as a one-element array. It is a
	 * pointer to a sibling clock rather than a name because a name would be
	 * looked up through clkdev, which only knows clocks someone registered a
	 * lookup alias for -- and a silent miss there leaves the gate reporting
	 * 0 Hz, which is what the note on recalc_rate below is about.
	 */
	const struct clk_hw	*parent_hw;
};

#define to_hi3716cv200_clk_bits(_hw) \
	container_of(_hw, struct hi3716cv200_clk_bits, hw)

/*
 * Some CRG registers hold the whole set of a device's gates next to clock
 * selects whose rates are documented nowhere, so the group is raised as a unit
 * rather than split into gates and a mux. This is also what the vendor does:
 * SI_HW_ResetCtrl(0) in hdmi_1_4/si_hdmitx.c:115-155 writes 0x3f to CRG67, the
 * value the board itself reports.
 */
static int hi3716cv200_clk_bits_prepare(struct clk_hw *hw)
{
	struct hi3716cv200_clk_bits *clk = to_hi3716cv200_clk_bits(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	writel_relaxed((val & ~clk->mask) | clk->value, clk->reg);

	return 0;
}

static void hi3716cv200_clk_bits_unprepare(struct clk_hw *hw)
{
	struct hi3716cv200_clk_bits *clk = to_hi3716cv200_clk_bits(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	writel_relaxed(val & ~clk->mask, clk->reg);
}

static int hi3716cv200_clk_bits_is_prepared(struct clk_hw *hw)
{
	struct hi3716cv200_clk_bits *clk = to_hi3716cv200_clk_bits(hw);

	return (readl_relaxed(clk->reg) & clk->mask) == clk->value;
}

/*
 * A bit group here is a gate, not a divider: the consumer gets whatever its
 * source runs at. Leaving recalc_rate out makes the framework report 0 Hz, and
 * that is not harmless -- the console showed it as "base_baud = 0", and the i2c
 * bus driver clamped both SCL dividers to 0xffff (read back on the board), which
 * is why every transfer timed out even though its interrupt reached the CPU.
 */
static unsigned long hi3716cv200_clk_bits_recalc(struct clk_hw *hw,
						 unsigned long parent_rate)
{
	return parent_rate;
}

static const struct clk_ops hi3716cv200_clk_bits_ops = {
	.prepare = hi3716cv200_clk_bits_prepare,
	.unprepare = hi3716cv200_clk_bits_unprepare,
	.is_prepared = hi3716cv200_clk_bits_is_prepared,
	.recalc_rate = hi3716cv200_clk_bits_recalc,
};

struct hi3716cv200_bits_clock {
	unsigned int	id;
	const char	*name;
	unsigned long	offset;
	u32		mask;
	u32		value;
	unsigned long	flags;
	/*
	 * The rate of a bit group is whatever its source runs at, and the fields
	 * that pick a rate are not modelled here; naming a source id is what lets
	 * a consumer read a rate instead of zero. -1 leaves the group with no
	 * parent, which is enough for a consumer that only ever enables and
	 * disables it -- the drivers bound here for dmac, hdmi and aiao do.
	 */
	/*
	 * Every gate below carries CLK_IS_CRITICAL: the fastboot leaves the
	 * display pipeline running through CRG67/68, so if the clock core
	 * dropped an unclaimed gate, clk_disable_unused would take the HDMI TX
	 * clock away under a live VDP and the fabric deadlocks -- observed
	 * 2026-10-21 as a freeze at ~1.9 s after which no software path (panic,
	 * sysrq, console) reached the board any more. What freed it was a reset
	 * this kernel does not control: measured later the same day, the board
	 * returns on its own at ~4 minutes regardless of the SP805's own timeout,
	 * so do not count on the watchdog as the way out of that state. A driver
	 * that owns a block clears the flag by claiming it.
	 */
	int		parent_id;
};

static const struct hi3716cv200_bits_clock hi3716cv200_bits_clks[] = {
	/*
	 * CRG26: uart:0 has no gate at all, its clock is hard-wired. Each other port
	 * is a cken bit with its srst_req immediately above it (hi_reg_crg.h:399-409),
	 * so the group covers both: prepare raises the gate and releases the reset,
	 * unprepare drops both. uart1 runs off the 3 MHz source the 3.10 machine code
	 * declares for it, the rest off 83.3 MHz -- that rate is what a consumer's
	 * baud divisor gets computed from.
	 */
	{ HI3716CV200_UART1_CLK, "clk_uart1", 0x68, BIT(0) | BIT(1), BIT(0),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_3M, },
	{ HI3716CV200_UART2_CLK, "clk_uart2", 0x68, BIT(4) | BIT(5), BIT(4),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_83P3M, },
	{ HI3716CV200_UART3_CLK, "clk_uart3", 0x68, BIT(8) | BIT(9), BIT(8),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_83P3M, },
	{ HI3716CV200_UART4_CLK, "clk_uart4", 0x68, BIT(12) | BIT(13), BIT(12),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_83P3M, },
	/*
	 * CRG31[0] jpgd_cken with CRG31[4] jpgd_srst_req, and CRG36[0] jpge_cken
	 * with CRG36[4] jpge_srst_req (hi3716cv200/hi_reg_crg.h:518,520,594,596).
	 * The source muxes next to them (CRG31[8], CRG36[9:8], CRG36[10]) stay
	 * unmodelled: the vendor only ever writes 0 into CRG31[8]
	 * (jpeg_drv_osr.c:274-283, its comment gives 0 as 200 MHz), and the encoder
	 * pair has no caller at all, so the rate this gate reports is the parent's.
	 */
	{ HI3716CV200_JPGD_CLK, "clk_jpgd", 0x7c, BIT(0) | BIT(4), BIT(0),
	  0, -1, },
	{ HI3716CV200_JPGE_CLK, "clk_jpge", 0x90, BIT(0) | BIT(4), BIT(0),
	  0, -1, },
	/*
	 * CRG30, the video decoder, and CRG35, the video encoder. Same shape as
	 * the two JPEG rows above: the group covers the cken bit with the
	 * subsystem reset right above it, so preparing the clock leaves the block
	 * out of reset, and unprepare puts it back. The codec driver still holds
	 * its own `resets` cells and pulses them per its bring-up order; the
	 * decoder's second line, CRG30[6] vdh_mfd_srst_req, belongs to that alone.
	 * CRG30[9:8] vdh_clk_sel and CRG35[8:7] venc_clk_sel stay at the value the
	 * boot hands over: the vendor writes 0 into the decoder's
	 * (product/Hi3716CV200/vfmw.c:203) and no source touches the encoder's on
	 * this part.
	 */
	{ HI3716CV200_VDH_CLK, "clk_vdh", 0x78, BIT(0) | BIT(4), BIT(0),
	  0, -1, },
	{ HI3716CV200_VENC_CLK, "clk_venc", 0x8c, BIT(0) | BIT(4), BIT(0),
	  0, -1, },
	/* CRG41[0] dmac_cken; the block resets through CRG41[4] */
	{ HI3716CV200_DMAC_CLK, "clk_dmac", 0xa4, BIT(0), BIT(0),
	  CLK_IS_CRITICAL, -1, },
	/* CRG67[5:0]: bus, cec, id, mhl, os and as gates of the transmitter */
	{ HI3716CV200_HDMITX_CTRL_CLK, "clk_hdmitx_ctrl", 0x10c,
	  GENMASK(5, 0), GENMASK(5, 0), CLK_IS_CRITICAL, -1, },
	/* CRG68[0]: the transmitter PHY's bus gate; bit 4 is its reset */
	{ HI3716CV200_HDMITX_PHY_CLK, "clk_hdmitx_phy", 0x110, BIT(0), BIT(0),
	  CLK_IS_CRITICAL, -1, },
	/* CRG69[0] adac_cken (the on-chip analog codec) */
	{ HI3716CV200_ADAC_CLK, "clk_adac", 0x114, BIT(0), BIT(0),
	  CLK_IS_CRITICAL, -1, },
	/* CRG70[0] aiao_cken; the block resets through CRG70[4] */
	{ HI3716CV200_AIAO_CLK, "clk_aiao", 0x118, BIT(0), BIT(0),
	  CLK_IS_CRITICAL, -1, },
	/*
	 * CRG94: wdg0..3_cken at 0/1/2/3 with the matching srst_req at 4/5/6/7
	 * (hi_reg_crg.h), so the pair is claimed the same way every other row in
	 * this table claims its (cken, srst) pair: the mask covers both, the value
	 * raises only the gate. Bit4 belongs in the mask because srst_req set means
	 * the block is held in reset, which is what the uart/i2c/ssp/sha rows in
	 * this same table assume; sp805_wdt.c requests no reset control, so the
	 * `resets` cell this row used to defer to was inert and is dropped from the
	 * binding. It is NOT the explanation for the stalled counter below: a boot
	 * with this row in place read CRG94=0x0000000F (clocked, reset released) and
	 * WDTVALUE still stayed 0x00000000 across 35 samples at 2 s, RIS/MIS clear,
	 * no bite in 200 s of uptime. What is measured is that a bare WDTLOAD write
	 * into an enabled block does start the counter (0x2AEA53FF -> 0x2AE93BF1 in
	 * one read, and a 2 s WDTLOAD with CONTROL=0x3 reset the board in ~4 s), so
	 * the stall comes from the order of writes in sp805_wdt.c's own feed
	 * sequence, not from the gate or the reset -- see the wdtkick bench.
	 */
	{ HI3716CV200_WDT0_CLK, "clk_wdt0", 0x178, BIT(0) | BIT(4), BIT(0),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_OSC24M, },
	/*
	 * CRG27: i2c0..4_cken at 4/8/12/16/20 with srst_req at 5/9/13/17/21
	 * (hi_reg_crg.h). The board reads this register as 0x00222222 right after
	 * fastboot hands over -- every gate off and every reset held -- so raising
	 * the pair is what actually makes an adapter usable. The bus driver
	 * divides its 100 MHz source by the SCL high/low registers, so these five
	 * need a parent with a rate.
	 */
	{ HI3716CV200_I2C0_CLK, "clk_i2c0", 0x6c, BIT(4) | BIT(5), BIT(4),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_I2C100M, },
	{ HI3716CV200_I2C1_CLK, "clk_i2c1", 0x6c, BIT(8) | BIT(9), BIT(8),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_I2C100M, },
	{ HI3716CV200_I2C2_CLK, "clk_i2c2", 0x6c, BIT(12) | BIT(13), BIT(12),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_I2C100M, },
	{ HI3716CV200_I2C3_CLK, "clk_i2c3", 0x6c, BIT(16) | BIT(17), BIT(16),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_I2C100M, },
	{ HI3716CV200_I2C4_CLK, "clk_i2c4", 0x6c, BIT(20) | BIT(21), BIT(20),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_I2C100M, },
	{ HI3716CV200_I2C5_CLK, "clk_i2c5", 0x6c, BIT(0) | BIT(1), BIT(0),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_I2C100M, },
	/*
	 * CRG28: ssp0_cken/ssp0_srst_req at 0/1 and ssp1 at 4/5 (hi_reg_crg.h).
	 * The parent is the 24 MHz crystal as an *assumption*, not as local
	 * authority: CRG28 has no selector or divider and nothing here or in the
	 * vendor tree names the SSPCLK rate (its driver only writes CPSR,
	 * msp/drv/spi/drv_spi.c:632-659). spi-pl022 has to have a rate to program
	 * any divisor at all -- measured: with no parent the master probes, and
	 * every device setup fails with "Max speed that can be programmed is 0 Hz"
	 * plus the WARN at spi-pl022.c:1548 -- so leaving the rate unclaimed would
	 * make the port unusable rather than merely unquantified. The number only
	 * sets the SCLK waveform, which this board cannot observe; what the loopback
	 * arm proves is independent of it. Method to settle the rate: drive SCLK
	 * with a known CPSR/SCR and count cycles against the crystal.
	 */
	{ HI3716CV200_SSP0_CLK, "clk_ssp0", 0x70, BIT(0) | BIT(1), BIT(0),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_OSC24M, },
	{ HI3716CV200_SSP1_CLK, "clk_ssp1", 0x70, BIT(4) | BIT(5), BIT(4),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_OSC24M, },
	/*
	 * CRG51 (hi_reg_crg.h:842-872): gsf_ge0_cken/gsf_ge1_cken at bits 3/4 with
	 * gsf_mac0_srst_req/gsf_mac1_srst_req at 10/11. The fastboot leaves the
	 * register at 0x7f -- all seven gates on, all four resets released -- so
	 * raising the pair changes no hardware state here either; it is what lets a
	 * driver claim the port instead of depending on that inheritance. No parent
	 * with a rate: hix5hd2_gmac only prepares and enables this clock, it never
	 * asks its rate, and the gsf_clk_sel source bit (6) stays unowned.
	 */
	{ HI3716CV200_GMAC0_CLK, "clk_gmac0", 0xcc, BIT(3) | BIT(10), BIT(3),
	  CLK_IS_CRITICAL, -1, },
	{ HI3716CV200_GMAC1_CLK, "clk_gmac1", 0xcc, BIT(4) | BIT(11), BIT(4),
	  CLK_IS_CRITICAL, -1, },
	/*
	 * CRG53 (hi_reg_crg.h region for the GPU): gpu_cken bit 0 with the four
	 * per-Pipeline-Processor gates at 8..11, and gpu_srst_req bit 4. The value
	 * has to raise all five clock bits together, because the vendor does the same
	 * for this chip (msp/drv/gpu/mali4xx/platform/mali400/mali4xx_clk.c:267) and
	 * the PP and their MMUs sit behind those four: with only bit 0 set, lima
	 * reaches the L2 cache but its first MMU self-test fails
	 * ("ppmmu0 dte write test fail", measured 2026-10-21). So they are one group
	 * with one owner here rather than five separate ids.
	 */
	{ HI3716CV200_GPU_CLK, "clk_gpu", 0xd4,
	  BIT(0) | BIT(4) | BIT(8) | BIT(9) | BIT(10) | BIT(11),
	  BIT(0) | BIT(8) | BIT(9) | BIT(10) | BIT(11),
	  CLK_IS_CRITICAL, -1, },
	/*
	 * CRG49: the SHA/TRNG group. The hardware RNG at 0xf8005000 has no clock of
	 * its own to request (histb-rng never calls clk_get), so this entry exists to
	 * make the gate a property of the kernel rather than of whatever the fastboot
	 * left behind; clearing bit 0 by hand is expected to stall RNG_STAT.
	 */
	{ HI3716CV200_SHA_CLK, "clk_sha", 0xc4, BIT(0) | BIT(4), BIT(0),
	  CLK_IS_CRITICAL, -1, },
	/*
	 * CRG46/CRG47: the USB2 host. The vendor brings the block up as two words --
	 * measured on this board's own vendor system, CRG46 = 0x1FF (bus, the two OHCI
	 * reference rates, otg_utmi, hst_phy and utmi0..2 clocked, srst_req 12..17 all
	 * clear) and CRG47 = 0x7 (the three PHY reference clocks on, their resets
	 * clear) -- so both groups are registered. CRG47's refclk_sel bits 16..18 are
	 * deliberately outside the mask: the vendor leaves them 0 and nothing here
	 * needs to move them.
	 */
	{ HI3716CV200_USB_BUS_CLK, "clk_usb_bus", 0xb8,
	  GENMASK(7, 0) | GENMASK(17, 12), GENMASK(7, 0),
	  CLK_IS_CRITICAL, -1, },
	{ HI3716CV200_USB_PHY_CLK, "clk_usb_phy", 0xbc,
	  GENMASK(2, 0) | BIT(8) | BIT(10) | BIT(12), GENMASK(2, 0),
	  CLK_IS_CRITICAL, -1, },
	/*
	 * CRG42/CRG43: the SATA host, again two words. CRG42's reset bits are
	 * inside the mask so prepare releases them, and sata_mpll_dword_cken is
	 * included because the vendor has it set whenever the block is up. CRG43
	 * holds only sata_phy_ref_cken -- bits 7..1 are reserved there, so its
	 * mask stops at bit 0 and keeps them out of the read-modify-write.
	 */
	{ HI3716CV200_SATA_BUS_CLK, "clk_sata_bus", 0xa8,
	  GENMASK(4, 0) | BIT(8) | BIT(9), GENMASK(4, 0),
	  CLK_IS_CRITICAL, -1, },
	{ HI3716CV200_SATA_PHY_CLK, "clk_sata_phy", 0xac,
	  BIT(0), BIT(0),
	  CLK_IS_CRITICAL, -1, },
	/*
	 * CRG39/CRG40: the SDIO hosts. The value bits are both ckeys, so preparing
	 * the clock raises the bus and the card clock together and releases the reset
	 * held by bit 4; the clk_sel pair is left alone because the vendor system's
	 * own live words decide it and nothing here needs to move it. Because it is
	 * left alone, the parent has to name the rate that choice puts on the block:
	 * a consumer that divides the CIU clock (dw_mmc) reads 0 Hz otherwise and
	 * refuses to probe.
	 */
	{ HI3716CV200_SDIO0_CLK, "clk_sdio0", 0x9c,
	  BIT(0) | BIT(1) | BIT(4), BIT(0) | BIT(1),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_SDIO75M, },
	{ HI3716CV200_SDIO1_CLK, "clk_sdio1", 0xa0,
	  BIT(0) | BIT(1) | BIT(4), BIT(0) | BIT(1),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_SDIO50M, },
	/*
	 * CRG24 (0x60): the NAND flash controller. Only the gate is claimed: bit 4 is
	 * nf_srst_req and belongs to hinfc610, which requests it through the node's
	 * `resets` cell and pulses it before configuring, and leaving it out of this
	 * mask is what keeps a gate operation from holding the block in reset. The
	 * nandc_clk_sel triple at bits 10:8 is left as the board has it (the vendor
	 * system reads CRG24 = 0x00000401, code 4 = 200 MHz), which is the state the
	 * asynchronous path runs in. The selector moves only when a synchronous card
	 * gets clocked, and nothing here does that -- hinfc610_s40.c:30-55 is the only
	 * writer, and it is reached just for the Toggle/ONFI modes this board's CON
	 * word says it is not in (bits 16:15 are 0, and BOOT_CFG bit 12 -- the sync
	 * boot pad -- is 0 too). Because the selector is left alone, the parent names
	 * the rate that choice puts on the block.
	 */
	{ HI3716CV200_NAND_CLK, "clk_nand", 0x60, BIT(0), BIT(0),
	  CLK_IS_CRITICAL, HI3716CV200_CLK_NAND200M, },
};

static int hi3716cv200_clk_register_bits(struct device *dev,
					 const struct hi3716cv200_bits_clock *clks,
					 int nums, struct hisi_clock_data *data)
{
	int i;

	for (i = 0; i < nums; i++) {
		struct hi3716cv200_clk_bits *bits;
		struct clk_init_data init = {};
		struct clk *clk;

		bits = devm_kzalloc(dev, sizeof(*bits), GFP_KERNEL);
		if (!bits)
			return -ENOMEM;

		init.name = clks[i].name;
		init.ops = &hi3716cv200_clk_bits_ops;
		init.flags = clks[i].flags;

		bits->reg = data->base + clks[i].offset;
		bits->mask = clks[i].mask;
		bits->value = clks[i].value;
		if (clks[i].parent_id >= 0) {
			struct clk *parent = data->clk_data.clks[clks[i].parent_id];

			bits->parent_hw = __clk_get_hw(parent);
			init.parent_hws = &bits->parent_hw;
			init.num_parents = 1;
		}
		bits->hw.init = &init;

		clk = devm_clk_register(dev, &bits->hw);
		if (IS_ERR(clk))
			return dev_err_probe(dev, PTR_ERR(clk),
					     "failed to register clock %s\n",
					     clks[i].name);

		data->clk_data.clks[clks[i].id] = clk;
	}

	return 0;
}

/*
 * CRG70's audio clock divider feeds the AIAO block's incoming clock. It is
 * programmed once per power-up: loaden low, skipcfg = 0x10, loaden high to
 * latch. The shared aiao_v1_1 HAL does exactly this on this chip too
 * (msp/drv/aiao/aiao_v1_1/common/src/hal_aiao_priv.c:604-614), and with
 * skipcfg left at its reset 0 a transmit port can be enabled and still never
 * advance its read pointer, so it is done here rather than in a consumer.
 */
#define CRG70_AIAO			0x118
#define  CRG70_AIAO_SKIPCFG		GENMASK(16, 12)
#define  CRG70_AIAO_LOADEN		BIT(17)
#define  CRG70_AIAO_SKIPCFG_100M	0x10

static void hi3716cv200_setup_aiaoclk(void __iomem *base)
{
	u32 val;

	val = readl(base + CRG70_AIAO) & ~CRG70_AIAO_LOADEN;
	writel(val, base + CRG70_AIAO);

	val = (val & ~CRG70_AIAO_SKIPCFG) |
	      FIELD_PREP(CRG70_AIAO_SKIPCFG, CRG70_AIAO_SKIPCFG_100M);
	writel(val, base + CRG70_AIAO);

	writel(val | CRG70_AIAO_LOADEN, base + CRG70_AIAO);
}

static struct hisi_clock_data *hi3716cv200_clk_register(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hisi_clock_data *clk_data;
	int ret;

	clk_data = hisi_clk_alloc(pdev, HI3716CV200_CRG_CLK_COUNT);
	if (!clk_data)
		return ERR_PTR(-ENOMEM);

	ret = hisi_clk_register_fixed_rate(hi3716cv200_fixed_rate_clks,
					   ARRAY_SIZE(hi3716cv200_fixed_rate_clks),
					   clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = hi3716cv200_clk_register_bits(dev, hi3716cv200_bits_clks,
					    ARRAY_SIZE(hi3716cv200_bits_clks),
					    clk_data);
	if (ret)
		return ERR_PTR(ret);

	hi3716cv200_setup_aiaoclk(clk_data->base);

	ret = of_clk_add_provider(pdev->dev.of_node, of_clk_src_onecell_get,
				  &clk_data->clk_data);
	if (ret)
		return ERR_PTR(ret);

	return clk_data;
}

static void hi3716cv200_clk_unregister(struct platform_device *pdev)
{
	of_clk_del_provider(pdev->dev.of_node);
}

static const struct hisi_crg_funcs hi3716cv200_crg_funcs = {
	.register_clks = hi3716cv200_clk_register,
	.unregister_clks = hi3716cv200_clk_unregister,
};

static const struct of_device_id hi3716cv200_crg_of_match[] = {
	{ .compatible = "hisilicon,hi3716cv200-crg",
	  .data = &hi3716cv200_crg_funcs, },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, hi3716cv200_crg_of_match);

static int hi3716cv200_crg_probe(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg;

	crg = devm_kmalloc(&pdev->dev, sizeof(*crg), GFP_KERNEL);
	if (!crg)
		return -ENOMEM;

	crg->funcs = of_device_get_match_data(&pdev->dev);
	if (!crg->funcs)
		return -ENOENT;

	crg->rstc = hisi_reset_init(pdev);
	if (!crg->rstc)
		return -ENOMEM;

	crg->clk_data = crg->funcs->register_clks(pdev);
	if (IS_ERR(crg->clk_data)) {
		hisi_reset_exit(crg->rstc);
		return PTR_ERR(crg->clk_data);
	}

	platform_set_drvdata(pdev, crg);

	return 0;
}

static void hi3716cv200_crg_remove(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);

	hisi_reset_exit(crg->rstc);
	crg->funcs->unregister_clks(pdev);
}

static struct platform_driver hi3716cv200_crg_driver = {
	.probe = hi3716cv200_crg_probe,
	.remove = hi3716cv200_crg_remove,
	.driver = {
		.name = "hi3716cv200-crg",
		.of_match_table = hi3716cv200_crg_of_match,
	},
};

/*
 * The clock tree of this SoC has to be up before the peripherals which use it
 * probe, and before the watchdog's gate is needed by anything.
 */
static int __init hi3716cv200_crg_init(void)
{
	return platform_driver_register(&hi3716cv200_crg_driver);
}
core_initcall(hi3716cv200_crg_init);

MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_DESCRIPTION("HiSilicon Hi3716CV200 CRG driver");
MODULE_LICENSE("GPL");
