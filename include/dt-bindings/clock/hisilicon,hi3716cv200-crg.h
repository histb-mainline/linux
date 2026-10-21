/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Clocks provided by the core CRG of the HiSilicon Hi3716CV200 SoC.
 *
 * The ids are ordered by (register offset, field low bit) and every field comes
 * from the vendor chip header common/drv/include/hi3716cv200/hi_reg_crg.h; the
 * generator that reads those unions is scripts/cv200-crg-fields.py.  Do not
 * renumber by hand.
 *
 * Only fields whose consumer is actually driven here are published.  The one PLL
 * that is modelled (HI3716CV200_CLK_VO_HD) is a lookup over the vendor's own
 * mode table rather than arithmetic on the ctrl0/ctrl1 fields, because those
 * fields have no local authority on this part and no Hi3716CV200 user guide
 * exists in this workspace; every other gate is given a parent only where the
 * vendor states the rate outright (the I2C and SDIO sources) and is otherwise
 * registered without one instead of inventing a rate.
 */

#ifndef __DT_BINDINGS_CLOCK_HI3716CV200_CRG_H
#define __DT_BINDINGS_CLOCK_HI3716CV200_CRG_H

/* Sources with no CRG register of their own. */
#define HI3716CV200_CLK_OSC24M		0
#define HI3716CV200_CLK_3M		1
#define HI3716CV200_CLK_83P3M		2

/* CRG26 (0x068) */
#define HI3716CV200_UART1_CLK		3
#define HI3716CV200_UART2_CLK		4
#define HI3716CV200_UART3_CLK		5
#define HI3716CV200_UART4_CLK		6

/* CRG41 (0x0a4) */
#define HI3716CV200_DMAC_CLK		7

/* CRG67 (0x10c): the six transmitter control gates the vendor raises as one */
#define HI3716CV200_HDMITX_CTRL_CLK	8

/* CRG68 (0x110) */
#define HI3716CV200_HDMITX_PHY_CLK	9

/* CRG69 (0x114) */
#define HI3716CV200_ADAC_CLK		10

/* CRG70 (0x118) */
#define HI3716CV200_AIAO_CLK		11


/*
 * CRG27 (0x06c). Each adapter's gate is followed immediately by its own
 * srst_req bit, and the bus driver that claims these blocks (i2c-hix5hd2.c)
 * never asks the reset framework, so the gate is registered as the (cken, srst)
 * pair and preparing it also releases the reset.
 */
#define HI3716CV200_I2C0_CLK		12
#define HI3716CV200_I2C1_CLK		13
#define HI3716CV200_I2C2_CLK		14
#define HI3716CV200_I2C3_CLK		15
#define HI3716CV200_I2C4_CLK		16

/*
 * Appended: the 100 MHz the I2C gates hang off. No DT consumer names it, but the
 * provider stores every clock by id, so it needs one. Ids are append-only.
 */
#define HI3716CV200_CLK_I2C100M		17

/* CRG94 (0x178): wdg0_cken, the counting clock of the SP805 that restarts the board */
#define HI3716CV200_WDT0_CLK		18

/* CRG51 (0xcc): gsf_ge0_cken / gsf_ge1_cken, the two gigabit MAC ports */
#define HI3716CV200_GMAC0_CLK		19
#define HI3716CV200_GMAC1_CLK		20

/* CRG53 (0xd4): gpu_cken, plus the four PP gates and the reset it feeds */
#define HI3716CV200_GPU_CLK		21

/*
 * CRG49 (0xc4): sha_cken plus the reset beside it. This is the clock the hardware
 * random number generator at 0xf8005000 runs on -- the vendor's cipher header names
 * REG_RNG_BASE_ADDR 0xF8005000 with NUMBER at +4 and STAT at +8, and no other gate
 * feeds that window. histb-rng takes no clock of its own, so no DT consumer names it;
 * the id exists because the provider stores every clock by id.
 */
#define HI3716CV200_SHA_CLK		22

/*
 * CRG46 (0xb8) and CRG47 (0xbc): the USB2 block. Two groups because the vendor
 * raises them as two words: CRG46 carries bus/ohci48m/ohci12m/otg_utmi/hst_phy and
 * utmi0..2 ckeys (bits 0..7) with their srst_req bits at 12..17, CRG47 carries the
 * three per-PHY reference clocks (bits 0..2) with srst_req/treq at 8..13
 * (hi_reg_crg.h:737-751, 766-770). What the vendor system leaves behind is
 * CRG46 = 0x1FF and CRG47 = 0x7 -- every cken set, every reset released -- which is
 * the value these two groups aim for.
 */
#define HI3716CV200_USB_BUS_CLK		23
#define HI3716CV200_USB_PHY_CLK		24

/*
 * CRG42 (0xa8) and CRG43 (0xac): the SATA host. CRG42 carries sata_bus/rx0/cko_alive/
 * mpll_dword/refclk ckeys at bits 0..4 with bus and cko_alive resets at 8 and 9; CRG43
 * carries the PHY reference clock at bit 0 (its refclk_sel at bit 1 stays as the vendor
 * left it, 0). What the vendor system leaves running is CRG42 = 0x1F, CRG43 = 0x1.
 */
#define HI3716CV200_SATA_BUS_CLK	25
#define HI3716CV200_SATA_PHY_CLK	26

/*
 * CRG39 (0x09c) and CRG40 (0x0a0): the two SDIO hosts the vendor describes for this
 * chip (`himciv200.SD@0xf9820000` and `himciv200.MMC@0xf9830000`). Each register carries
 * bus_cken at bit 0, the card-clock cken at bit 1 and srst_req at bit 4, with a
 * sdio_clk_sel pair at bits 8..9 that stays as the bootloader left it
 * (hi_reg_crg.h's PERI_CRG39_SDIO0 = 0x009c / PERI_CRG40_SDIO1 = 0x00a0 -- the vendor's
 * clock ids are register offsets, so these are the same window this driver maps).
 */
#define HI3716CV200_SDIO0_CLK		27
#define HI3716CV200_SDIO1_CLK		28

/*
 * The card-clock rate each SDIO host is left running at. The vendor table for the
 * selector is outright about the four rates (himci_s40.c:28-31 and 61-64: 00 is 75 MHz,
 * 01 is 100 MHz, 10 is 50 MHz, 11 is 24 MHz), and the board's own live words say which
 * one is in effect -- CRG39 reads clk_sel=0, CRG40 reads clk_sel=2. These two ids are
 * sources rather than fields, but they are appended after the gates because a published
 * id is an ABI and only grows at the end.
 */
#define HI3716CV200_CLK_SDIO75M		29
#define HI3716CV200_CLK_SDIO50M		30

/*
 * CRG54 (0x0d8): the display channel's gates. hi_reg_crg.h's U_PERI_CRG54 names
 * vo_bus/vo/vo_sd/vo_sdate/vo_hd/vo_hdate_cken at bits 0..5, four vdac ckeys at
 * 6..9 and vo_clkout_cken at 10, with vou_srst_req alone at bit 30; the rate and
 * divider selectors at bits 12..29 belong to HI3716CV200_CLK_VO_HD below, so they
 * stay outside this mask. Both the running vendor system and a boot straight out of
 * fastboot leave bits 0..10 set (CRG54 = 0x05E107FF), which is the value this group
 * aims for.
 */
#define HI3716CV200_VO_CLK		31

/*
 * The HD channel pixel clock: CRG10/CRG11's hpll_ctrl0/ctrl1 word pair plus the
 * CRG54 vo_hd_clk_sel/vo_hd_clk_div selectors. This is the one clock on this part
 * that is modelled as a rate rather than a gate, because the vendor publishes the
 * rate as data, not as arithmetic: s_stDispFormatParam in msp/drv/vo/vdp_v1_0/
 * hal/cv200/hal_specific_config.c pairs a raw (ctrl0, ctrl1) word pair with every
 * mode, and the mode's own horizontal/vertical totals times its refresh give the
 * clock that pair has to produce. That is also why no PLL bit field is interpreted
 * anywhere here -- there is no local authority for those fields on this chip.
 */
#define HI3716CV200_CLK_VO_HD		32

/*
 * CRG28 (0x70): the two SSP gates with their reset bits, ssp0_cken/ssp0_srst_req
 * at 0/1 and ssp1_cken/ssp1_srst_req at 4/5 (hi_reg_crg.h, dumped with
 * scripts/cv200-crg-fields.py 28). Registered without a parent: CRG28 has no
 * selector or divider, and nothing in this workspace or the vendor tree states
 * the SSPCLK rate -- the vendor SPI driver only writes CPSR and never names its
 * input clock (msp/drv/spi/drv_spi.c:632-659) -- so the rate is left unclaimed
 * instead of invented. (Method to settle it: drive SCLK with a known CPSR/SCR
 * and count cycles against the 24 MHz reference.)
 *
 * The vendor system leaves CRG28 at 0x00000022: both gates low with the two
 * srst_req bits set. With the pair raised the way this provider does it the
 * board measures 0x00000011 (both gates on, both srst_req bits low) and the
 * ports work, so for these bits low is the released state.
 */
#define HI3716CV200_SSP0_CLK		33
#define HI3716CV200_SSP1_CLK		34

/*
 * CRG24 (0x60): the NAND flash controller. The vendor's own glue for this chip
 * names nandc_cken at bit 0 and an nandc_clk_sel triple at bits 10:8 whose table
 * documents only four of the eight codes -- 0 is 24 MHz, 4 is 200 MHz, 5 is 150
 * MHz, 6 is 125 MHz, 7 is 100 MHz (hinfc610_s40.c:18-25) -- and it moves the
 * selector only to clock a synchronous (Toggle/ONFI) card, never for the
 * asynchronous path. What the board's own vendor system reads is CRG24 =
 * 0x00000401: gated, selector 4, so 200 MHz.
 */
#define HI3716CV200_NAND_CLK		35

/* The rate CRG24's selector code 4 puts on the NAND controller. */
#define HI3716CV200_CLK_NAND200M	36

/*
 * Appended: CRG27's sixth gate pair, cken at bit 0 with srst_req at bit 1. The
 * hix5hd2 clock table in the vendor tree puts its sixth adapter exactly there
 * (drivers/clk/hisilicon/clk-hix5hd2.c:124 "clk_i2c5" at 0x6c bit 0), and the value
 * this board hands over in CRG27 -- 0x00222222 -- has bit 1 set together with
 * 5/9/13/17/21, i.e. the boot holds this sixth reset like the other five. On this
 * package the adapter behind that pair is the one the SDK calls QAMI2C, at
 * 0xF8B17000 with INTID 76 (msp/drv/i2c/std_i2c/drv_i2c.h:104,113).
 */
#define HI3716CV200_I2C5_CLK		37

/*
 * The JPEG codec blocks. CRG31[0] is jpgd_cken with CRG31[4] jpgd_srst_req
 * (hi_reg_crg.h:518,520), CRG36[0]/[4] are the encoder pair (hi_reg_crg.h:594,596).
 * CRG31[8] jpgd_clk_sel is the decoder's source mux -- the vendor driver writes 0
 * for it, which its own comment reads as 200 MHz against 150 MHz
 * (msp/drv/jpeg/src/jpeg_drv_osr.c:274-283) -- and CRG36[9:8]/[10] are the
 * encoder's, which no caller sets. Neither mux is modelled here: the gate only
 * covers the cken/reset pair.
 */
#define HI3716CV200_JPGD_CLK		38
#define HI3716CV200_JPGE_CLK		39

/*
 * The video codec blocks. CRG30[0] vdh_cken with CRG30[4] vdh_all_srst_req,
 * and CRG35[0] venc_cken with CRG35[4] venc_srst_req (hi_reg_crg.h:487-510 and
 * :565-586, dumped with scripts/cv200-crg-fields.py 30 35). The vendor's own
 * decoder power-on writes that pair plus [6] vdh_mfd_srst_req and [9:8]
 * vdh_clk_sel = 0
 * (msp/drv/vfmw/vfmw_v4.0/firmware/product/Hi3716CV200/vfmw.c:199-205).
 * Two further CRG30 bits have vendor writers but no part of this driver's
 * path: [1] vdhdsp_cken (DSP_OpenHardware) and [5] vdh_scd_srst_req
 * (SCD_OpenHardware), both at vfmw.c:215-284. The Hi3798MV100 node leaves
 * those two alone and
 * decodes there, so this part claims the same set -- the gate here, [4] and [6]
 * through the decoder's own `resets` cells.
 * This part also has a second SCD gate the Hi3798MV100 register file does not
 * model: CRG59 (0xec) scd_cken[0] with scd_srst_req[4]. No SDK source names
 * either field (method: whole-tree search of every .c and .S for the field
 * names), so it stays unmodelled until a board test shows the syntax-detection
 * path needing a clock of its own.
 */
#define HI3716CV200_VDH_CLK		40
#define HI3716CV200_VENC_CLK		41

#define HI3716CV200_CRG_CLK_COUNT	42

#endif /* __DT_BINDINGS_CLOCK_HI3716CV200_CRG_H */
