// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hi3798MV200 Clock and Reset Generator Driver
 *
 * Copyright (c) 2024 Yang Xiwen <forbidden405@outlook.com>
 * Copyright (c) 2016 HiSilicon Technologies Co., Ltd.
 */

#include <dt-bindings/clock/hisilicon,hi3798mv200-crg.h>
#include <dt-bindings/clock/hisilicon,hi3798mv200-sysctrl.h>
#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include "clk.h"
#include "crg.h"
#include "reset.h"

/* hi3798MV200 core CRG */
enum hi3798mv200_crg_inner_clk {
	HI3798MV200_FIXED_3M = HI3798MV200_CRG_CLK_COUNT,
	HI3798MV200_FIXED_6M,
	HI3798MV200_FIXED_12M,
	HI3798MV200_FIXED_24M,
	HI3798MV200_FIXED_25M,
	HI3798MV200_FIXED_27M,
	HI3798MV200_FIXED_48M,
	HI3798MV200_FIXED_50M,
	HI3798MV200_FIXED_54M,
	HI3798MV200_FIXED_60M,
	HI3798MV200_FIXED_75M,
	HI3798MV200_FIXED_100M,
	HI3798MV200_FIXED_125M,
	HI3798MV200_FIXED_150M,
	HI3798MV200_FIXED_200M,
	HI3798MV200_FIXED_250M,
	HI3798MV200_FIXED_300M,
	HI3798MV200_FIXED_333M,
	HI3798MV200_FIXED_400M,
	HI3798MV200_FIXED_450M,
	HI3798MV200_FIXED_600M,
	HI3798MV200_FIXED_800M,
	HI3798MV200_FIXED_1200M,
	HI3798MV200_FIXED_1350M,
	HI3798MV200_MMC_MUX,
	HI3798MV200_SDIO0_MUX,
	HI3798MV200_SDIO1_MUX,
	HI3798MV200_COMBPHY_MUX,
	HI3798MV200_FEMAC_MUX,
	HI3798MV200_GMAC_MUX,
	HI3798MV200_CRG_NR_CLKS,
};

static const struct hisi_fixed_rate_clock hi3798mv200_fixed_rate_clks[] = {
	{ HI3798MV200_OSC_CLK, "clk_osc", NULL, 0, 24000000, },
	{ HI3798MV200_APB_CLK, "clk_apb", NULL, 0, 100000000, },
	{ HI3798MV200_AHB_CLK, "clk_ahb", NULL, 0, 200000000, },
	{ HI3798MV200_FIXED_3M, "3m", NULL, 0, 3000000, },
	/*
	 * DOZE mode runs the always-on domain off a divided crystal, which the
	 * user guide gives as 6 MHz (SC_CTRL mcu_bus_clk_sel == 2).
	 */
	{ HI3798MV200_FIXED_6M, "6m", NULL, 0, 6000000, },
	{ HI3798MV200_FIXED_12M, "12m", NULL, 0, 12000000, },
	{ HI3798MV200_FIXED_24M, "24m", NULL, 0, 24000000, },
	{ HI3798MV200_FIXED_25M, "25m", NULL, 0, 25000000, },
	{ HI3798MV200_FIXED_27M, "27m", NULL, 0, 27000000, },
	{ HI3798MV200_FIXED_48M, "48m", NULL, 0, 48000000, },
	{ HI3798MV200_FIXED_50M, "50m", NULL, 0, 50000000, },
	{ HI3798MV200_FIXED_54M, "54m", NULL, 0, 54000000, },
	{ HI3798MV200_FIXED_60M, "60m", NULL, 0, 60000000, },
	{ HI3798MV200_FIXED_75M, "75m", NULL, 0, 75000000, },
	{ HI3798MV200_FIXED_100M, "100m", NULL, 0, 100000000, },
	{ HI3798MV200_FIXED_125M, "125m", NULL, 0, 125000000, },
	{ HI3798MV200_FIXED_150M, "150m", NULL, 0, 150000000, },
	{ HI3798MV200_FIXED_200M, "200m", NULL, 0, 200000000, },
	/*
	 * The user guide names these sources only by frequency: PERI_CRG54 bit
	 * 27 switches the VDP root between 333 MHz and 250 MHz, and PERI_CRG22
	 * lets the media0 and media2 bus clocks pick 300 MHz or 450 MHz.
	 */
	{ HI3798MV200_FIXED_250M, "250m", NULL, 0, 250000000, },
	{ HI3798MV200_FIXED_300M, "300m", NULL, 0, 300000000, },
	{ HI3798MV200_FIXED_333M, "333m", NULL, 0, 333000000, },
	{ HI3798MV200_FIXED_400M, "400m", NULL, 0, 400000000, },
	{ HI3798MV200_FIXED_450M, "450m", NULL, 0, 450000000, },
	{ HI3798MV200_FIXED_600M, "600m", NULL, 0, 600000000, },
	{ HI3798MV200_FIXED_800M, "800m", NULL, 0, 800000000, },
	{ HI3798MV200_FIXED_1200M, "1200m", NULL, 0, 1200000000, },
	{ HI3798MV200_FIXED_1350M, "1350m", NULL, 0, 1350000000, },
};

static const char *const sdio_mux_p[] = { "100m", "50m", "150m", "25m" };
static u32 sdio_mux_table[] = {0, 1, 2, 3};

static const char *const mmc_mux_p[] = { "100m", "50m", "25m", "200m", "150m" };
static u32 mmc_mux_table[] = {0, 1, 2, 3, 6};

static const char *const cpu_mux_p[] = { "apll", "200m", "800m", "1350m",
	"24m", "1200m", "400m", "600m" };
static u32 cpu_mux_table[] = {0, 1, 2, 3, 4, 5, 6, 7};

static const char *const comphy_mux_p[] = { "25m", "100m"};
static const char *const femac_mux_p[] = { "54m", "27m" };
static const char *const gmac_mux_p[] = { "125m", "75m" };
static const char *const ext_netphy_mux_p[] = { "25m", "50m" };
static const char *const aiao_mclk_mux_p[] = { "epll", "vpll" };
static const char *const mde1_bus_mux_p[] = { "24m", "200m" };
static const char *const mde3_bus_mux_p[] = { "24m", "400m" };
static u32 mux_table_1bit[] = {0, 1};

static const char *const core_bus_mux_p[] = { "24m", "200m", "250m" };
static const char *const mde0_bus_mux_p[] = { "24m", "300m", "400m" };
static const char *const mde2_bus_mux_p[] = { "24m", "400m", "450m" };
static u32 mux_table_2bit_pattern1[] = {0, 1, 2};

static const char *const vdp_sel_mux_p[] = { "333m", "250m" };
static const char *const vdp_hd_ppc_div_mux_p[] = { "clk_vdp_ini_2d",
	"clk_vdp_ini_4d" };
static const char *const vdp_sd_ppc_div_mux_p[] = { "clk_vdp_ini_2d",
	"clk_vdp_ini_4d" };
static const char *const vdp_hd_ppc_mux_p[] = { "clk_vdp_ini",
	"clk_vdp_hd_ppc_div" };
static const char *const vdp_sd_ppc_mux_p[] = { "clk_vdp_ini",
	"clk_vdp_sd_ppc_div" };

static struct hisi_mux_clock hi3798mv200_mux_clks[] = {
	{ HI3798MV200_CORE_BUS_CLK, "clk_core_bus", core_bus_mux_p, ARRAY_SIZE(core_bus_mux_p),
		0, 0x58, 0, 2, 0, mux_table_2bit_pattern1, },
	{ HI3798MV200_MDE0_BUS_CLK, "clk_mde0_bus", mde0_bus_mux_p, ARRAY_SIZE(mde0_bus_mux_p),
		0, 0x58, 2, 2, 0, mux_table_2bit_pattern1, },
	{ HI3798MV200_MDE1_BUS_CLK, "clk_mde1_bus", mde1_bus_mux_p, ARRAY_SIZE(mde1_bus_mux_p),
		0, 0x58, 4, 1, 0, mux_table_1bit, },
	{ HI3798MV200_MDE2_BUS_CLK, "clk_mde2_bus", mde2_bus_mux_p, ARRAY_SIZE(mde2_bus_mux_p),
		0, 0x58, 8, 2, 0, mux_table_2bit_pattern1, },
	{ HI3798MV200_MDE3_BUS_CLK, "clk_mde3_bus", mde3_bus_mux_p, ARRAY_SIZE(mde3_bus_mux_p),
		0, 0x58, 10, 1, 0, mux_table_1bit, },
	{ HI3798MV200_CPU_CLK, "clk_cpu", cpu_mux_p, ARRAY_SIZE(cpu_mux_p),
		CLK_SET_RATE_PARENT, 0x48, 0, 3, CLK_MUX_ROUND_CLOSEST, cpu_mux_table },
	{ HI3798MV200_MMC_MUX, "mmc_mux", mmc_mux_p, ARRAY_SIZE(mmc_mux_p),
		0, 0xa0, 8, 3, CLK_MUX_ROUND_CLOSEST, mmc_mux_table, },
	{ HI3798MV200_COMBPHY_MUX, "combphy_mux", comphy_mux_p,
		ARRAY_SIZE(comphy_mux_p), 0, 0x188, 3, 1, 0, mux_table_1bit, },
	{ HI3798MV200_SDIO0_MUX, "sdio0_mux", sdio_mux_p, ARRAY_SIZE(sdio_mux_p),
		0, 0x9c, 8, 2, CLK_MUX_ROUND_CLOSEST, sdio_mux_table, },
	{ HI3798MV200_SDIO1_MUX, "sdio1_mux", sdio_mux_p, ARRAY_SIZE(sdio_mux_p),
		0, 0x28c, 8, 2, CLK_MUX_ROUND_CLOSEST, sdio_mux_table, },
	{ HI3798MV200_FEMAC_MUX, "femac_mux", femac_mux_p, ARRAY_SIZE(femac_mux_p),
		0, 0xd0, 2, 1, 0, mux_table_1bit, },
	{ HI3798MV200_GMAC_MUX, "gmac_mux", gmac_mux_p, ARRAY_SIZE(gmac_mux_p),
		0, 0xcc, 7, 1, 0, mux_table_1bit, },
	{ HI3798MV200_EXT_NETPHY_CLK, "ext_netphy_mux", ext_netphy_mux_p, ARRAY_SIZE(ext_netphy_mux_p),
		0, 0xcc, 6, 1, 0, mux_table_1bit, },
	/*
	 * The master clock the AIAO ports divide.  PERI_CRG70 bit 21 picks the
	 * source and the ports turn it into mclk with their own 27-bit fractional
	 * divider, so the divider cannot be modelled here and the consumer only
	 * ever reads this clock's rate.  Bit 21 is left as the boot loader set
	 * it, because nothing here moves it.  The rate the ports actually divide
	 * is half of this, which clk_aiao_mclk below describes.
	 */
	{ HI3798MV200_AIAO_MCLK_SEL_CLK, "clk_aiao_mclk_sel", aiao_mclk_mux_p,
		ARRAY_SIZE(aiao_mclk_mux_p), 0, 0x118, 21, 1, 0, mux_table_1bit, },
	/*
	 * The VDP root and its two pixel-processor work clocks.  CRG54 bit 27
	 * picks the root between a 333 MHz and a 250 MHz source, and CRG211 bits
	 * 28:24 then divide that choice by (N+1) to make clk_vdp_ini.  Each
	 * pipe then takes either that root directly or a divided copy of it:
	 * the divide *by-two-or-four* choice is one bit (CRG211 bit 20 for
	 * hd_ppc, bit 17 for sd_ppc) and the *root-or-divided* choice is another
	 * (bit 22 and bit 21), and the two are not adjacent, so each pipe needs
	 * two muxes in series rather than one two-bit field.  The vendor's own
	 * clock helper spells the three resulting settings as ALL (select bit
	 * clear), HALF (select set, divide bit clear) and QUARTER (both set).
	 */
	{ HI3798MV200_VDP_SEL_CLK, "clk_vdp_sel", vdp_sel_mux_p,
		ARRAY_SIZE(vdp_sel_mux_p), 0, 0xd8, 27, 1, 0, mux_table_1bit, },
	{ HI3798MV200_VDP_HD_PPC_DIV_CLK, "clk_vdp_hd_ppc_div",
		vdp_hd_ppc_div_mux_p, ARRAY_SIZE(vdp_hd_ppc_div_mux_p),
		0, 0x34c, 20, 1, 0, mux_table_1bit, },
	{ HI3798MV200_VDP_SD_PPC_DIV_CLK, "clk_vdp_sd_ppc_div",
		vdp_sd_ppc_div_mux_p, ARRAY_SIZE(vdp_sd_ppc_div_mux_p),
		0, 0x34c, 17, 1, 0, mux_table_1bit, },
	{ HI3798MV200_VDP_HD_PPC_CLK, "clk_vdp_hd_ppc", vdp_hd_ppc_mux_p,
		ARRAY_SIZE(vdp_hd_ppc_mux_p), 0, 0x34c, 22, 1, 0, mux_table_1bit, },
	{ HI3798MV200_VDP_SD_PPC_CLK, "clk_vdp_sd_ppc", vdp_sd_ppc_mux_p,
		ARRAY_SIZE(vdp_sd_ppc_mux_p), 0, 0x34c, 21, 1, 0, mux_table_1bit, },
};

static u32 mmc_phase_regvals[] = {0, 1, 2, 3, 4, 5, 6, 7};
static u32 mmc_phase_degrees[] = {0, 45, 90, 135, 180, 225, 270, 315};

/*
 * The stage between the root select and the pixel processors: CRG211 bits
 * 28:24 divide by (N+1), so the five-bit field covers one through
 * thirty-two without a table.  The vendor's bring-up writes 0x3, a divide by
 * four, for the 1080p30-class formats, and the data sheet gives the reset
 * value of the word as 0x01007fcf, which has 0x1 in this field.  Both
 * readings are recorded in the port ledger section 85.
 */
static struct hisi_divider_clock hi3798mv200_divider_clks[] = {
	{ HI3798MV200_VDP_INI_CLK, "clk_vdp_ini", "clk_vdp_sel", 0,
		0x34c, 24, 5, 0, NULL, NULL, },
};

/*
 * The AIAO port dividers sit behind a halving stage: a port produces half the
 * byte rate that the CRG70 source mux and the port divider word predict.
 * Which stage does it is unresolved -- the manual lists an epll_fout1 output
 * that the aiao_mclk_sel description does not mention -- but the ratio is the
 * same on hi3798mv100, whose vendor divider tables solve against half the
 * 491.52 MHz audio PLL.
 */
static struct hisi_fixed_factor_clock hi3798mv200_fixed_factor_clks[] = {
	{ HI3798MV200_AIAO_MCLK_CLK, "clk_aiao_mclk", "clk_aiao_mclk_sel",
		1, 2, 0, },
	/*
	 * The divided copies each pixel processor can be switched to
	 * (CRG211 bit 20 and bit 17 pick between these two).
	 */
	{ HI3798MV200_VDP_INI_2D_CLK, "clk_vdp_ini_2d", "clk_vdp_ini",
		1, 2, 0, },
	{ HI3798MV200_VDP_INI_4D_CLK, "clk_vdp_ini_4d", "clk_vdp_ini",
		1, 4, 0, },
};

static struct hisi_phase_clock hi3798mv200_phase_clks[] = {
	{ HI3798MV200_SDIO0_SAMPLE_CLK, "sdio0_sample", "clk_sdio0_ciu",
		0, 0x9c, 12, 3, mmc_phase_degrees,
		mmc_phase_regvals, ARRAY_SIZE(mmc_phase_regvals) },
	{ HI3798MV200_SDIO0_DRV_CLK, "sdio0_drive", "clk_sdio0_ciu",
		0, 0x9c, 16, 3, mmc_phase_degrees,
		mmc_phase_regvals, ARRAY_SIZE(mmc_phase_regvals) },
	{ HI3798MV200_SDIO1_SAMPLE_CLK, "sdio1_sample", "clk_sdio1_ciu",
		0, 0x28c, 12, 3, mmc_phase_degrees,
		mmc_phase_regvals, ARRAY_SIZE(mmc_phase_regvals) },
	{ HI3798MV200_SDIO1_DRV_CLK, "sdio1_drive", "clk_sdio1_ciu",
		0, 0x28c, 16, 3, mmc_phase_degrees,
		mmc_phase_regvals, ARRAY_SIZE(mmc_phase_regvals) },
	{ HI3798MV200_MMC_SAMPLE_CLK, "mmc_sample", "clk_mmc_ciu",
		0, 0xa0, 12, 3, mmc_phase_degrees,
		mmc_phase_regvals, ARRAY_SIZE(mmc_phase_regvals) },
	{ HI3798MV200_MMC_DRV_CLK, "mmc_drive", "clk_mmc_ciu",
		0, 0xa0, 16, 3, mmc_phase_degrees,
		mmc_phase_regvals, ARRAY_SIZE(mmc_phase_regvals) },
};

static const struct hisi_gate_clock hi3798mv200_gate_clks[] = {
	/* UART */
	{ HI3798MV200_UART2_CLK, "clk_uart2", "75m",
		CLK_SET_RATE_PARENT, 0x68, 4, 0, },
	{ HI3798MV200_UART3_CLK, "clk_uart3", "75m",
		CLK_SET_RATE_PARENT, 0x68, 6, 0, },
	/* I2C */
	{ HI3798MV200_I2C0_CLK, "clk_i2c0", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6c, 4, 0, },
	{ HI3798MV200_I2C1_CLK, "clk_i2c1", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6c, 8, 0, },
	{ HI3798MV200_I2C2_CLK, "clk_i2c2", "clk_apb",
		CLK_SET_RATE_PARENT, 0x6c, 12, 0, },
	/* SPI */
	{ HI3798MV200_SPI0_CLK, "clk_spi0", "clk_apb",
		CLK_SET_RATE_PARENT, 0x70, 0, 0, },
	/* SCI */
	{ HI3798MV200_SCI0_CLK, "clk_sci0", "clk_apb",
		CLK_SET_RATE_PARENT, 0x74, 0, 0, },
	{ HI3798MV200_SCI1_CLK, "clk_sci1", "clk_apb",
		CLK_SET_RATE_PARENT, 0x74, 2, 0, },
	/* SDIO */
	{ HI3798MV200_SDIO0_BIU_CLK, "clk_sdio0_biu", "clk_ahb",
		CLK_SET_RATE_PARENT, 0x9c, 0, 0, },
	{ HI3798MV200_SDIO0_CIU_CLK, "clk_sdio0_ciu", "sdio0_mux",
		CLK_SET_RATE_PARENT, 0x9c, 1, 0, },
	{ HI3798MV200_SDIO1_BIU_CLK, "clk_sdio1_biu", "clk_ahb",
		CLK_SET_RATE_PARENT, 0x28c, 0, 0, },
	{ HI3798MV200_SDIO1_CIU_CLK, "clk_sdio1_ciu", "sdio1_mux",
		CLK_SET_RATE_PARENT, 0x28c, 1, 0, },
	/* EMMC */
	{ HI3798MV200_MMC_BIU_CLK, "clk_mmc_biu", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xa0, 0, 0, },
	{ HI3798MV200_MMC_CIU_CLK, "clk_mmc_ciu", "mmc_mux",
		CLK_SET_RATE_PARENT, 0xa0, 1, 0, },
	/*
	 * Ethernet.  CRG51 pairs each clock enable with a soft-reset request
	 * bit (gsf_srst_req[0] / gsf_cken[1] and gmac_if_srst_req[2] /
	 * gmac_if_cken[3]), so the gates are bits 1 and 3 -- the same split the
	 * vendor clock table makes, where HIGMAC_MAC0_CLK carries mask 0x2 and
	 * HIGMAC_MAC_IF0_CLK mask 0x8 (hi_reg_crg.h:819-827, clk-hi3798mv200.c:72-73).
	 * The three reset request bits belong to the reset provider, which the
	 * MAC node asks for by name.
	 */
	{ HI3798MV200_GMAC_CLK, "clk_gmac", "gmac_mux",
		CLK_SET_RATE_PARENT, 0xcc, 1, 0, },
	{ HI3798MV200_GSF_CLK, "clk_gmacif", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xcc, 3, 0, },
	{ HI3798MV200_ETH_CLK, "clk_femac", "femac_mux",
		CLK_SET_RATE_PARENT, 0xd0, 1, 0, },
	{ HI3798MV200_ETH_BUS_CLK, "clk_femacif", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xd0, 0, 0, },
	{ HI3798MV200_FEPHY_CLK, "clk_fephy", "25m",
		CLK_SET_RATE_PARENT, 0x388, 0, 0, },
	/* COMBPHY */
	{ HI3798MV200_COMBPHY_CLK, "clk_combphy", "combphy_mux",
		CLK_SET_RATE_PARENT, 0x188, 0, 0, },
	/* USB2 */
	{ HI3798MV200_USB2_BUS_CLK, "clk_u2_bus", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xb8, 0, 0, },
	{ HI3798MV200_USB2_HST_PHY_CLK, "clk_u2_phy", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 4, 0, },
	{ HI3798MV200_USB2_12M_CLK, "clk_u2_12m", "12m",
		CLK_SET_RATE_PARENT, 0xb8, 2, 0 },
	{ HI3798MV200_USB2_48M_CLK, "clk_u2_48m", "48m",
		CLK_SET_RATE_PARENT, 0xb8, 1, 0 },
	{ HI3798MV200_USB2_UTMI0_CLK, "clk_u2_utmi0", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 5, 0 },
	{ HI3798MV200_USB2_UTMI1_CLK, "clk_u2_utmi1", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 6, 0 },
	{ HI3798MV200_USB2_OTG_UTMI_CLK, "clk_u2_otg_utmi", "60m",
		CLK_SET_RATE_PARENT, 0xb8, 3, 0 },
	{ HI3798MV200_USB2_PHY1_REF_CLK, "clk_u2_phy1_ref", "24m",
		CLK_SET_RATE_PARENT, 0xbc, 0, 0 },
	{ HI3798MV200_USB2_PHY2_REF_CLK, "clk_u2_phy2_ref", "24m",
		CLK_SET_RATE_PARENT, 0xbc, 2, 0 },
	/* USB3 bus */
	{ HI3798MV200_USB3_GM_CLK, "clk_u3_gm", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xb0, 6, 0 },
	{ HI3798MV200_USB3_GS_CLK, "clk_u3_gs", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xb0, 5, 0 },
	{ HI3798MV200_USB3_BUS_CLK, "clk_u3_bus", "clk_ahb",
		CLK_SET_RATE_PARENT, 0xb0, 0, 0 },
	/* USB3 ctrl */
	{ HI3798MV200_USB3_SUSPEND_CLK, "clk_u3_suspend", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 2, 0 },
	{ HI3798MV200_USB3_PIPE_CLK, "clk_u3_pipe", NULL,
		CLK_SET_RATE_PARENT, 0xb0, 3, 0 },
	{ HI3798MV200_USB3_REF_CLK, "clk_u3_ref", "125m",
		CLK_SET_RATE_PARENT, 0xb0, 1, 0 },
	{ HI3798MV200_USB3_UTMI_CLK, "clk_u3_utmi", "60m",
		CLK_SET_RATE_PARENT, 0xb0, 4, 0 },
	/* Watchdog */
	{ HI3798MV200_WDG0_CLK, "clk_wdg0", "clk_osc",
		CLK_SET_RATE_PARENT, 0x178, 0, 0 },
	/*
	 * GPU.  The block enable is CRG53 bit 0, and the two pixel processors
	 * have their own gates in CRG53 bits 9:8; the vendor's enable sets all
	 * three with one write, and lima is handed only two clock specifiers,
	 * so the PP gates are chained as each other's parents and enabling
	 * "core" pulls PP1 then PP0.  CRG73 bits[2:0] selects the GPU PLL
	 * frequency and stays as the boot loader left it; moving it requires
	 * the PMC power-good sequence, so this clock has no set_rate.
	 */
	{ HI3798MV200_GPU_CLK, "clk_gpu", "200m",
		0, 0xd4, 0, 0 },
	{ HI3798MV200_GPU_PP0_CLK, "clk_gpu_pp0", "clk_gpu_pp1",
		0, 0xd4, 8, 0 },
	{ HI3798MV200_GPU_PP1_CLK, "clk_gpu_pp1", "200m",
		0, 0xd4, 9, 0 },
	/*
	 * AIAO audio.  CRG70 bit 0 gates the block's working clock, bit 4 is its
	 * soft reset and reaches the block only while it is clocked, and bit 22
	 * picks that clock between 100 and 50 MHz.  Bit 22 is left as the boot
	 * loader set it and is therefore not modelled: the gate is hung on the
	 * fixed clock it selects, the way the GPU gate is hung on its boot
	 * selector value.
	 */
	{ HI3798MV200_AIAO_CLK, "clk_aiao", "50m", 0, 0x118, 0, 0 },
};

/*
 * Clock gates of the display path.  Each one is a single bit, because the
 * binding header already publishes one id per gate and a bit with its own clock
 * is what keeps one owner per bit.  These use their own provider rather than
 * hisi_gate_clock because none of them has a parent that can be named before
 * the pixel-clock chain is modelled, and hisi_gate_clock always reports one
 * parent.
 */
struct hi3798mv200_clk_bits {
	struct clk_hw	hw;
	void __iomem	*reg;
	u32		mask;
	u32		value;
};

#define to_hi3798mv200_clk_bits(_hw) \
	container_of(_hw, struct hi3798mv200_clk_bits, hw)

static int hi3798mv200_clk_bits_prepare(struct clk_hw *hw)
{
	struct hi3798mv200_clk_bits *clk = to_hi3798mv200_clk_bits(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	writel_relaxed((val & ~clk->mask) | clk->value, clk->reg);

	return 0;
}

static void hi3798mv200_clk_bits_unprepare(struct clk_hw *hw)
{
	struct hi3798mv200_clk_bits *clk = to_hi3798mv200_clk_bits(hw);
	u32 val;

	val = readl_relaxed(clk->reg);
	writel_relaxed(val & ~clk->mask, clk->reg);
}

static int hi3798mv200_clk_bits_is_prepared(struct clk_hw *hw)
{
	struct hi3798mv200_clk_bits *clk = to_hi3798mv200_clk_bits(hw);

	return (readl_relaxed(clk->reg) & clk->mask) == clk->value;
}

static const struct clk_ops hi3798mv200_clk_bits_ops = {
	.prepare = hi3798mv200_clk_bits_prepare,
	.unprepare = hi3798mv200_clk_bits_unprepare,
	.is_prepared = hi3798mv200_clk_bits_is_prepared,
};

struct hi3798mv200_bits_clock {
	unsigned int	id;
	const char	*name;
	unsigned long	offset;
	u32		mask;
	u32		value;
	unsigned long	flags;
};

/*
 * CRG54 holds the channel gates, CRG211 the gates of the VDP's processing
 * units, CRG67/68 those of the HDMI transmitter.  Every one of them is a clock
 * of its own, so that releasing the display path can never leave a gate of the
 * same word claimed by somebody else.  The ones a boot logo already runs
 * through carry CLK_IS_CRITICAL until the display driver asks for them itself:
 * without that, clk_disable_unused() takes the picture away at the end of boot.
 * Bit 6 stays unheld because the vendor's own display bring-up clears it, and
 * the SD channel gate (bit 2) is left to its id -- an HD picture runs without
 * it, which the vendor's running system shows by holding it clear.
 *
 * Bit 4 is where the two decodes the SDK carries disagree: the Hi3798MV200 one
 * calls bits 5:4 reserved, the Hi3798MV210 one names bit 4 vo_hd_cken.  The
 * live readings settle it for this board: a system that is displaying holds
 * PERI_CRG54 = 0x400d0019, while this kernel on a fresh boot leaves 0x400d0009,
 * so bit 4 is the only display-path CRG54 bit the working system holds that we
 * do not.  It is not offered as its own gate here, because nothing would be
 * left to ask for it after the existing VO_HD id: the HD pixel clock that CRG54
 * bit 16 selects and bits 19:18 divide is modelled as
 * hi3798mv200_clk_register_hd_pix(), and that clock's prepare raises bit 4, so
 * the gate goes up before any consumer may touch the channel block.
 */
static const struct hi3798mv200_bits_clock hi3798mv200_bits_clks[] = {
	/* CRG54: the bus gate and the SDATE encoder of the HD channel. */
	{ HI3798MV200_VO_BUS, "clk_vo_bus", 0xd8, BIT(0), BIT(0), CLK_IS_CRITICAL, },
	{ HI3798MV200_VO_SD, "clk_vo_sd", 0xd8, BIT(2), BIT(2), 0, },
	{ HI3798MV200_VO_SDATE, "clk_vo_sdate", 0xd8, BIT(3), BIT(3),
	  CLK_IS_CRITICAL, },
	/*
	 * The analog DAC's gate: the boot leaves it raised and the vendor's own
	 * display bring-up clears it, so it is not one to hold on for a
	 * HDMI-out picture.
	 */
	{ HI3798MV200_VDAC_CH0_CLK, "clk_vdac_ch0", 0xd8, BIT(6), BIT(6), 0, },
	/*
	 * CRG54 bit 31, vdp_cfg_cken, is deliberately not modelled: it does not
	 * take a write from the ARM side, while the rest of the same word does,
	 * both with VDP_2 in reset and after its reset request (CRG211 bit 29)
	 * was cleared.  A clock whose .is_prepared could never report true would
	 * only make the display driver wait on something that does not exist.
	 * Its id stays published but unimplemented, like VDP_CLK.
	 */
	/*
	 * The selectors CRG54 bits 25 (hdmi_clk_sel), 26 (vo_sd_hdmi_clk_sel) and
	 * 27 (vdp_clk_sel) are left at the value the boot leaves: nobody here
	 * claims them, and the vendor's own running configuration leaves them
	 * clear too -- its PERI_CRG54 reads 0x400d0019 while displaying (the OEM
	 * oracle snapshot), i.e. nothing above bit 19 except bit 30.  Bits 24..20
	 * and 28 are reserved in this part's decode (hi3798mv200/hi_reg_crg.h:889
	 * and :893), so they stay out of the table as well.
	 * Of the remaining two, bit 29 (vo_hd_hdmi_clk_sel) belongs to the HD
	 * pixel clock below, whose prepare clears it, and bit 30 (vou_srst_req)
	 * belongs to the display driver through its syscon regmap: the OEM value
	 * has both as we set them (29 clear, 30 raised), which is the cross-check
	 * that this split matches how the block is really run.
	 */
	/*
	 * CRG211: the units of the display path.  These are raised by this driver
	 * rather than inherited: after both a cold power cycle and a warm reset the
	 * board reads the whole gate field as 0, so nothing can assume the boot
	 * left them on.
	 */
	{ HI3798MV200_VO_BP_CLK, "clk_vo_bp", 0x34c, BIT(0), BIT(0), CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_G4_CLK, "clk_vdp_g4", 0x34c, BIT(1), BIT(1), CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_V3_CLK, "clk_vdp_v3", 0x34c, BIT(2), BIT(2), CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_SD_CLK, "clk_vdp_sd", 0x34c, BIT(3), BIT(3), CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_WBC_VP_CLK, "clk_vdp_wbc_vp", 0x34c, BIT(6), BIT(6),
	  CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_WBC_GP_CLK, "clk_vdp_wbc_gp", 0x34c, BIT(7), BIT(7),
	  CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_WBC_HD_CLK, "clk_vdp_wbc_hd", 0x34c, BIT(8), BIT(8),
	  CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_G3_CLK, "clk_vdp_g3", 0x34c, BIT(9), BIT(9), CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_G1_CLK, "clk_vdp_g1", 0x34c, BIT(10), BIT(10), CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_G0_CLK, "clk_vdp_g0", 0x34c, BIT(11), BIT(11), CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_V1_CLK, "clk_vdp_v1", 0x34c, BIT(12), BIT(12), CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_V0_CLK, "clk_vdp_v0", 0x34c, BIT(13), BIT(13), CLK_IS_CRITICAL, },
	{ HI3798MV200_VDP_HD_CLK, "clk_vdp_hd", 0x34c, BIT(14), BIT(14), CLK_IS_CRITICAL, },
	/* CRG67: the transmitter controller's six gates. */
	{ HI3798MV200_HDMITX_SSC_CLK, "clk_hdmitx_ssc", 0x10c, BIT(0), BIT(0),
	  CLK_IS_CRITICAL, },
	{ HI3798MV200_HDMITX_SSC_BYPASS_CLK, "clk_hdmitx_ssc_bypass", 0x10c,
	  BIT(1), BIT(1), CLK_IS_CRITICAL, },
	{ HI3798MV200_HDMITX_CTRL_24M_CLK, "clk_hdmitx_os_24m", 0x10c,
	  BIT(2), BIT(2), CLK_IS_CRITICAL, },
	{ HI3798MV200_HDMITX_CTRL_CEC_CLK, "clk_hdmitx_cec", 0x10c,
	  BIT(3), BIT(3), CLK_IS_CRITICAL, },
	{ HI3798MV200_HDMITX_CTRL_60M_CLK, "clk_hdmitx_os_60m", 0x10c,
	  BIT(4), BIT(4), CLK_IS_CRITICAL, },
	{ HI3798MV200_HDMITX_CTRL_AS_CLK, "clk_hdmitx_as", 0x10c,
	  BIT(5), BIT(5), CLK_IS_CRITICAL, },
	/* CRG68: the transmitter PHY's TMDS gate; bits 4 and 5 are its resets. */
	{ HI3798MV200_HDMITX_PHY_TMDS_CLK, "clk_hdmitx_tmds", 0x110,
	  BIT(0), BIT(0), CLK_IS_CRITICAL, },
};

static int hi3798mv200_clk_register_bits(struct device *dev,
					const struct hi3798mv200_bits_clock *clks,
					int nums, struct hisi_clock_data *data)
{
	int i;

	for (i = 0; i < nums; i++) {
		struct hi3798mv200_clk_bits *bits;
		struct clk_init_data init = {};
		struct clk *clk;

		bits = devm_kzalloc(dev, sizeof(*bits), GFP_KERNEL);
		if (!bits)
			return -ENOMEM;

		init.name = clks[i].name;
		init.ops = &hi3798mv200_clk_bits_ops;
		init.flags = clks[i].flags;

		bits->reg = data->base + clks[i].offset;
		bits->mask = clks[i].mask;
		bits->value = clks[i].value;
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
 * The pixel clock of the HD channel.  PERI_CRG_PLL10 and PERI_CRG_PLL11
 * configure the display FPLL, CRG54 bit 16 picks which channel PLL the HD path
 * listens to (1 is the one in PLL10/PLL11, which the vendor calls
 * DISP_CLOCK_SOURCE_HD0), and CRG54 bits 19:18 divide what it gets: code 0
 * divides by two, code 1 by four, and codes 2 and 3 both pass it through.
 * What reaches the channel is therefore
 *
 *     (FBDIV + FRAC / 2^24) * FREF / REFDIV / PSTDIV1 / PSTDIV2 / 4 / div
 *
 * which is the vendor's own relation.  Its display bring-up looks for a VCO of
 * pixclk * 4 * PSTDIV1 * PSTDIV2 strictly between 800 MHz and 2400 MHz, takes
 * the first reference divider that divides both FREF and that VCO without
 * remainder, requires FBDIV strictly between 16 and 2400, and sets PLL11 bit 25
 * with the result.  The board matches it: the vendor's running system holds
 * PLL10 = 0x11800000 and PLL11 = 0x00001031, which is (49 + 1/2) * 24 MHz =
 * 1188 MHz over post dividers of 1 and 1, with CRG54 bits 19:18 = 3, and 1188
 * over four is the 297 MHz that 3840x2160@30 adds up to.
 *
 * Two things are left out on purpose.  FRAC is read back but never written:
 * bit 25 of PLL11 asks the PLL to keep to integers, an exact solution came out
 * of the same search for every mode this panel lists, and the vendor only
 * reaches for the fraction when its own ±0.03 % walk over the request fails.
 * And no lock status is waited on: CRG84 does carry a seven-bit pll_lock field
 * and the SDK defines it, but nothing in the vendor tree reads it for this PLL
 * and no document here says which of the seven bits is the display one.
 */
#define CRG_PLL10			0x28
#define  PLL10_PSTDIV1			GENMASK(30, 28)
#define  PLL10_PSTDIV2			GENMASK(27, 24)
#define  PLL10_FRAC			GENMASK(23, 0)
#define CRG_PLL11			0x2c
#define  PLL11_DSMPD			BIT(25)
#define  PLL11_REFDIV			GENMASK(17, 12)
#define  PLL11_FBDIV			GENMASK(11, 0)
#define CRG54				0xd8
#define  CRG54_VO_HD_CKEN		BIT(4)
#define  CRG54_VO_HD_CLK_SEL		BIT(16)
#define  CRG54_VO_HD_CLK_DIV		GENMASK(19, 18)
#define  CRG54_VO_HD_HDMI_CLK_SEL	BIT(29)
#define  CRG54_CLK_DIV_1		3
#define  CRG54_CLK_DIV_2		0
#define  CRG54_CLK_DIV_4		1
#define  HD_PIX_VCO_MIN			800000000
#define  HD_PIX_VCO_MAX			2400000000
#define  HD_PIX_FBDIV_MIN		16
#define  HD_PIX_FBDIV_MAX		2400
#define  HD_PIX_REFDIV_MAX		64
/* How far the search may depart from the mode, in ten-thousandths of it. */
#define  HD_PIX_DIFF_MAX		3

struct hi3798mv200_hd_pix_clock {
	struct clk_hw	hw;
	void __iomem	*pll10;
	void __iomem	*pll11;
	void __iomem	*crg54;
	unsigned long	applied_rate;
	u32		applied_pll10;
	u32		applied_pll11;
	u32		applied_div;
};

#define to_hi3798mv200_hd_pix_clock(_hw) \
	container_of(_hw, struct hi3798mv200_hd_pix_clock, hw)

struct hi3798mv200_hd_pix_cfg {
	unsigned long	rate;
	u32		pll10;
	u32		pll11;
	u32		div;
};

/* Products of two three-bit post dividers, in the order the vendor walks them. */
static const u8 hi3798mv200_hd_pix_postdivs[] = {
	1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 15, 16, 18, 20, 21, 24, 25, 28,
	30, 35, 36, 42, 49,
};

/* The channel dividers, in the order they are preferred. */
static const struct hi3798mv200_hd_pix_div {
	unsigned long	value;
	u32		code;
} hi3798mv200_hd_pix_divs[] = {
	{ 1, CRG54_CLK_DIV_1 },
	{ 2, CRG54_CLK_DIV_2 },
	{ 4, CRG54_CLK_DIV_4 },
};

static unsigned long hi3798mv200_hd_pix_div_value(u32 field)
{
	switch (FIELD_GET(CRG54_VO_HD_CLK_DIV, field)) {
	case CRG54_CLK_DIV_2:
		return 2;
	case CRG54_CLK_DIV_4:
		return 4;
	default:
		return 1;
	}
}

/*
 * Solve for one channel divider.  Only solutions that divide out exactly are
 * accepted, so the rate reported back is always the rate that was asked for.
 */
static int hi3798mv200_hd_pix_solve_div(unsigned long rate, unsigned long ref,
					unsigned long div, u32 div_code,
					struct hi3798mv200_hd_pix_cfg *cfg)
{
	unsigned int i, j, k;

	for (i = 0; i < ARRAY_SIZE(hi3798mv200_hd_pix_postdivs); i++) {
		unsigned int postdiv = hi3798mv200_hd_pix_postdivs[i];
		u64 vco = (u64)rate * 4 * div * postdiv;
		unsigned long postdiv1 = 0, postdiv2 = 0;

		if (vco <= HD_PIX_VCO_MIN || vco >= HD_PIX_VCO_MAX)
			continue;
		for (j = 1; j < HD_PIX_REFDIV_MAX; j++) {
			unsigned long step = ref / j;
			unsigned long fbdiv;
			u64 rem = vco;

			if (ref % j || do_div(rem, step))
				continue;
			fbdiv = div_u64(vco, step);
			if (fbdiv <= HD_PIX_FBDIV_MIN || fbdiv >= HD_PIX_FBDIV_MAX)
				continue;
			for (k = 1; k <= 7; k++) {
				if (!(postdiv % k) && postdiv / k <= 7) {
					postdiv1 = k;
					postdiv2 = postdiv / k;
					break;
				}
			}
			if (!postdiv1)
				continue;

			cfg->rate = rate;
			cfg->pll10 = FIELD_PREP(PLL10_PSTDIV1, postdiv1) |
				     FIELD_PREP(PLL10_PSTDIV2, postdiv2);
			cfg->pll11 = PLL11_DSMPD |
				     FIELD_PREP(PLL11_REFDIV, j) |
				     FIELD_PREP(PLL11_FBDIV, fbdiv);
			cfg->div = FIELD_PREP(CRG54_VO_HD_CLK_DIV, div_code);
			return 0;
		}
	}

	return -EINVAL;
}

static int hi3798mv200_hd_pix_try(unsigned long rate, unsigned long ref,
				  struct hi3798mv200_hd_pix_cfg *cfg)
{
	unsigned int i;

	/*
	 * Pass the PLL's output straight through first, the way the vendor's
	 * system does for every mode above 165 MHz, and only halve or quarter
	 * the channel clock when the request cannot otherwise put the VCO in
	 * range.
	 */
	for (i = 0; i < ARRAY_SIZE(hi3798mv200_hd_pix_divs); i++) {
		const struct hi3798mv200_hd_pix_div *div = &hi3798mv200_hd_pix_divs[i];

		if (!hi3798mv200_hd_pix_solve_div(rate, ref, div->value,
						  div->code, cfg))
			return 0;
	}

	return -EINVAL;
}

static int hi3798mv200_hd_pix_solve(unsigned long rate, unsigned long ref,
				    struct hi3798mv200_hd_pix_cfg *cfg)
{
	unsigned long tol;
	unsigned int off;

	if (!rate || !ref)
		return -EINVAL;

	if (!hi3798mv200_hd_pix_try(rate, ref, cfg))
		return 0;

	/*
	 * Nothing divided the request out exactly, so look for the rate nearest
	 * to it that does: a kilohertz at a time, above first and then below,
	 * for as far as the vendor's own bring-up is willing to depart from the
	 * mode -- 0.03 % of it.
	 */
	tol = rate * HD_PIX_DIFF_MAX / 10000;
	for (off = 1000; off <= tol; off += 1000) {
		if (!hi3798mv200_hd_pix_try(rate + off, ref, cfg))
			return 0;
		if (rate > off && !hi3798mv200_hd_pix_try(rate - off, ref, cfg))
			return 0;
	}

	return -EINVAL;
}

static unsigned long hi3798mv200_hd_pix_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct hi3798mv200_hd_pix_clock *clk = to_hi3798mv200_hd_pix_clock(hw);
	unsigned long postdiv1, postdiv2, refdiv;
	u32 pll10 = readl_relaxed(clk->pll10);
	u32 pll11 = readl_relaxed(clk->pll11);
	u64 num, den;

	postdiv1 = FIELD_GET(PLL10_PSTDIV1, pll10);
	postdiv2 = FIELD_GET(PLL10_PSTDIV2, pll10);
	refdiv = FIELD_GET(PLL11_REFDIV, pll11);
	if (!parent_rate || !postdiv1 || !postdiv2 || !refdiv)
		return 0;

	num = (u64)parent_rate *
	      (((u64)FIELD_GET(PLL11_FBDIV, pll11) << 24) +
	       FIELD_GET(PLL10_FRAC, pll10));
	den = (u64)refdiv * postdiv1 * postdiv2 * 4 *
	      hi3798mv200_hd_pix_div_value(readl_relaxed(clk->crg54));

	return div64_u64(num, den << 24);
}

static int hi3798mv200_hd_pix_determine_rate(struct clk_hw *hw,
					    struct clk_rate_request *req)
{
	struct hi3798mv200_hd_pix_cfg cfg;
	int ret;

	ret = hi3798mv200_hd_pix_solve(req->rate, req->best_parent_rate, &cfg);
	if (ret)
		return ret;

	req->rate = cfg.rate;

	return 0;
}

/*
 * Put one solved configuration into the clock generator.  CRG54 is
 * read-modify-written, never written whole: the same word holds the reset
 * request of the whole display unit and the gates of the channel next to this
 * one, and the vendor's own whole-word write of it cannot raise the VDP's
 * config clock back up (its bit 31 takes no write).
 */
static void hi3798mv200_hd_pix_apply(struct hi3798mv200_hd_pix_clock *clk,
				     const struct hi3798mv200_hd_pix_cfg *cfg)
{
	writel_relaxed((readl_relaxed(clk->pll10) &
			~(PLL10_PSTDIV1 | PLL10_PSTDIV2 | PLL10_FRAC)) |
		       cfg->pll10, clk->pll10);
	writel_relaxed((readl_relaxed(clk->pll11) &
			~(PLL11_DSMPD | PLL11_REFDIV | PLL11_FBDIV)) |
		       cfg->pll11, clk->pll11);

	writel_relaxed((readl_relaxed(clk->crg54) &
			~(CRG54_VO_HD_CLK_SEL | CRG54_VO_HD_CLK_DIV)) |
		       CRG54_VO_HD_CLK_SEL | cfg->div, clk->crg54);
}

static int hi3798mv200_hd_pix_set_rate(struct clk_hw *hw, unsigned long rate,
					      unsigned long parent_rate)
{
	struct hi3798mv200_hd_pix_clock *clk = to_hi3798mv200_hd_pix_clock(hw);
	struct hi3798mv200_hd_pix_cfg cfg;
	int ret;

	/*
	 * The transmitter's TMDS clock is divided out of this same FPLL, so a
	 * second request for a rate already in hardware must not pick a different
	 * valid topology: the solver prefers pass-through over the channel divider
	 * and would move the post dividers underneath the TMDS clock.  Measured on
	 * the board (docs/mv200h-femac-reboot-2026-10-21.md section 163): after one
	 * rebind CRG_PLL10/11 and CRG54's vo_hd_clk_div all differ from the first
	 * boot, and the sink stops locking.  Reproduce the stored configuration
	 * instead of solving again.
	 */
	if (clk->applied_rate == rate) {
		cfg.rate = rate;
		cfg.pll10 = clk->applied_pll10;
		cfg.pll11 = clk->applied_pll11;
		cfg.div = clk->applied_div;
		hi3798mv200_hd_pix_apply(clk, &cfg);

		return 0;
	}

	ret = hi3798mv200_hd_pix_solve(rate, parent_rate, &cfg);
	if (ret)
		return ret;

	clk->applied_rate = cfg.rate;
	clk->applied_pll10 = cfg.pll10;
	clk->applied_pll11 = cfg.pll11;
	clk->applied_div = cfg.div;
	hi3798mv200_hd_pix_apply(clk, &cfg);

	return 0;
}

/*
 * Point the channel select at the PLL this clock is built from.  The select is
 * not a detail of setting a rate: while it still names the other source, a
 * request to the display register file downstream of it is never answered, and
 * the bus keeps the transaction outstanding -- that is how this board was found
 * hanging in earlier rounds.  So it belongs to prepare, before any consumer may
 * touch that register file, and not only to set_rate.
 */
static int hi3798mv200_hd_pix_prepare(struct clk_hw *hw)
{
	struct hi3798mv200_hd_pix_clock *clk = to_hi3798mv200_hd_pix_clock(hw);
	u32 val = readl_relaxed(clk->crg54);

	/*
	 * Bit 29 is the clk_vo_hd : TMDS clock ratio and only the SD formats
	 * want 1:2.  With it set the whole VDP register file stops answering
	 * (reads 0, writes dropped); the board shows this reversibly, so the HD
	 * path pins it to 1:1 here, before any consumer reaches that file.
	 *
	 * Bit 4 goes up in the same write: the working system holds it, this
	 * kernel's fresh boot did not, and a gate of the channel clock is worth
	 * nothing raised after the channel has already been configured.
	 */
	val &= ~CRG54_VO_HD_HDMI_CLK_SEL;
	writel_relaxed(val | CRG54_VO_HD_CLK_SEL | CRG54_VO_HD_CKEN, clk->crg54);

	return 0;
}

static void hi3798mv200_hd_pix_unprepare(struct clk_hw *hw)
{
	struct hi3798mv200_hd_pix_clock *clk = to_hi3798mv200_hd_pix_clock(hw);

	writel_relaxed(readl_relaxed(clk->crg54) & ~CRG54_VO_HD_CKEN, clk->crg54);
}

static const struct clk_ops hi3798mv200_hd_pix_ops = {
	.prepare = hi3798mv200_hd_pix_prepare,
	.unprepare = hi3798mv200_hd_pix_unprepare,
	.recalc_rate = hi3798mv200_hd_pix_recalc_rate,
	.determine_rate = hi3798mv200_hd_pix_determine_rate,
	.set_rate = hi3798mv200_hd_pix_set_rate,
};

static struct clk *hi3798mv200_clk_register_hd_pix(struct device *dev,
						 void __iomem *base)
{
	struct hi3798mv200_hd_pix_clock *clk;
	static const char *const parents[] = { "clk_osc" };
	struct clk_init_data init = {
		.name = "clk_vo_hd",
		.ops = &hi3798mv200_hd_pix_ops,
		.parent_names = parents,
		.num_parents = ARRAY_SIZE(parents),
	};

	clk = devm_kzalloc(dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return ERR_PTR(-ENOMEM);

	clk->hw.init = &init;
	clk->pll10 = base + CRG_PLL10;
	clk->pll11 = base + CRG_PLL11;
	clk->crg54 = base + CRG54;

	return devm_clk_register(dev, &clk->hw);
}

static const struct hisi_pll_clock hi3798mv200_plls[] = {
	{ HI3798MV200_APLL_CLK, "apll", "clk_osc", CLK_IS_CRITICAL, 0x0 },
	/*
	 * bpll is not marked critical.  It feeds the PCIe/ComboPHY 100 MHz
	 * reference, which a boot loader may leave unprogrammed on a board that
	 * does not use those ports, and then its two CRG words read back all
	 * zero.  CLK_IS_CRITICAL makes clk_core_init() prepare it right away,
	 * hisi_pll_prepare() rejects the all-zero dividers with -EINVAL and
	 * hi3798mv200-crg fails to probe with -22 -- which takes the whole clock
	 * provider with it, so every consumer (PL011, eMMC, FEMAC) defers and
	 * the board prints nothing past "clk: Disabling unused clocks".  Left
	 * non-critical it simply has no rate until a consumer that really needs
	 * it asks, and that consumer's probe failure is then an honest, local
	 * one.
	 */
	{ HI3798MV200_BPLL_CLK, "bpll", "clk_osc", 0, 0x8 },
	{ HI3798MV200_DPLL_CLK, "dpll", "clk_osc", CLK_IS_CRITICAL, 0x10 },
	{ HI3798MV200_VPLL_CLK, "vpll", "clk_osc", CLK_IS_CRITICAL, 0x20 },
	/*
	 * PLL10/PLL11, which the SDK calls hpll, belong to the HD channel's
	 * pixel clock above rather than to the generic PLL here: the generic one
	 * freezes the two post dividers and the reference divider at whatever
	 * the boot left in them and clears the fractional part when it prepares,
	 * so it can neither report 297 MHz nor ask for it.  The HPLL id stays
	 * published, unimplemented, like the other ids of this header that no
	 * driver of ours claims.
	 */
	{ HI3798MV200_EPLL_CLK, "epll", "clk_osc", CLK_IS_CRITICAL, 0x30 },
	{ HI3798MV200_QPLL_CLK, "qpll", "clk_osc", CLK_IS_CRITICAL, 0x38 },
};

static struct hisi_clock_data *hi3798mv200_clk_register(
				struct platform_device *pdev)
{
	struct hisi_clock_data *clk_data;
	struct clk *clk;
	int ret;

	clk_data = hisi_clk_alloc(pdev, HI3798MV200_CRG_NR_CLKS);
	if (!clk_data)
		return ERR_PTR(-ENOMEM);

	/* hisi_phase_clock is resource managed */
	ret = hisi_clk_register_phase(&pdev->dev,
				hi3798mv200_phase_clks,
				ARRAY_SIZE(hi3798mv200_phase_clks),
				clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = hisi_clk_register_pll(&pdev->dev,
				    hi3798mv200_plls,
				    ARRAY_SIZE(hi3798mv200_plls),
				    clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = hisi_clk_register_fixed_rate(hi3798mv200_fixed_rate_clks,
				     ARRAY_SIZE(hi3798mv200_fixed_rate_clks),
				     clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = hisi_clk_register_mux(hi3798mv200_mux_clks,
				ARRAY_SIZE(hi3798mv200_mux_clks),
				clk_data);
	if (ret)
		goto unregister_fixed_rate;

	ret = hisi_clk_register_divider(hi3798mv200_divider_clks,
					ARRAY_SIZE(hi3798mv200_divider_clks),
					clk_data);
	if (ret)
		goto unregister_mux;

	ret = hisi_clk_register_gate(hi3798mv200_gate_clks,
				ARRAY_SIZE(hi3798mv200_gate_clks),
				clk_data);
	if (ret)
		goto unregister_divider;

	ret = hi3798mv200_clk_register_bits(&pdev->dev, hi3798mv200_bits_clks,
					    ARRAY_SIZE(hi3798mv200_bits_clks),
					    clk_data);
	if (ret)
		goto unregister_gate;

	/* The pixel clock takes CRG54's fields, so it comes after the gates. */
	clk = hi3798mv200_clk_register_hd_pix(&pdev->dev, clk_data->base);
	if (IS_ERR(clk)) {
		ret = PTR_ERR(clk);
		goto unregister_gate;
	}
	clk_data->clk_data.clks[HI3798MV200_VO_HD] = clk;

	/* Its parents are the divider and the muxes, so it comes after both. */
	ret = hisi_clk_register_fixed_factor(hi3798mv200_fixed_factor_clks,
					     ARRAY_SIZE(hi3798mv200_fixed_factor_clks),
					     clk_data);
	if (ret)
		goto unregister_gate;

	ret = of_clk_add_provider(pdev->dev.of_node,
			of_clk_src_onecell_get, &clk_data->clk_data);
	if (ret)
		goto unregister_fixed_factor;

	return clk_data;

unregister_fixed_factor:
	hisi_clk_unregister_fixed_factor(hi3798mv200_fixed_factor_clks,
					ARRAY_SIZE(hi3798mv200_fixed_factor_clks),
					clk_data);
unregister_gate:
	hisi_clk_unregister_gate(hi3798mv200_gate_clks,
				ARRAY_SIZE(hi3798mv200_gate_clks),
				clk_data);
unregister_divider:
	hisi_clk_unregister_divider(hi3798mv200_divider_clks,
				    ARRAY_SIZE(hi3798mv200_divider_clks),
				    clk_data);
unregister_mux:
	hisi_clk_unregister_mux(hi3798mv200_mux_clks,
				ARRAY_SIZE(hi3798mv200_mux_clks),
				clk_data);
unregister_fixed_rate:
	hisi_clk_unregister_fixed_rate(hi3798mv200_fixed_rate_clks,
				ARRAY_SIZE(hi3798mv200_fixed_rate_clks),
				clk_data);
	return ERR_PTR(ret);
}

static void hi3798mv200_clk_unregister(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);

	of_clk_del_provider(pdev->dev.of_node);

	hisi_clk_unregister_fixed_factor(hi3798mv200_fixed_factor_clks,
					ARRAY_SIZE(hi3798mv200_fixed_factor_clks),
					crg->clk_data);
	hisi_clk_unregister_gate(hi3798mv200_gate_clks,
				ARRAY_SIZE(hi3798mv200_gate_clks),
				crg->clk_data);
	hisi_clk_unregister_divider(hi3798mv200_divider_clks,
				    ARRAY_SIZE(hi3798mv200_divider_clks),
				    crg->clk_data);
	hisi_clk_unregister_mux(hi3798mv200_mux_clks,
				ARRAY_SIZE(hi3798mv200_mux_clks),
				crg->clk_data);
	hisi_clk_unregister_fixed_rate(hi3798mv200_fixed_rate_clks,
				ARRAY_SIZE(hi3798mv200_fixed_rate_clks),
				crg->clk_data);
}

static const struct hisi_crg_funcs hi3798mv200_crg_funcs = {
	.register_clks = hi3798mv200_clk_register,
	.unregister_clks = hi3798mv200_clk_unregister,
};

/* hi3798MV200 sysctrl CRG */

enum hi3798mv200_sysctrl_inner_clk {
	HI3798MV200_UART0_MUX = HI3798MV200_SYSCTRL_CLK_COUNT,

	HI3798MV200_SYSCTRL_NR_CLKS
};

static const char *const uart0_mux_p[] = { "3m", "75m" };

static const char *const mcu_bus_mux_p[] = { "24m", "200m", "6m" };

static const struct hisi_mux_clock hi3798mv200_sysctrl_mux_clks[] = {
	{ HI3798MV200_UART0_MUX, "uart0_mux", uart0_mux_p, ARRAY_SIZE(uart0_mux_p),
		CLK_SET_RATE_PARENT, 0x48, 29, 1, 0, mux_table_1bit, },
	{ HI3798MV200_MCU_BUS_CLK, "mcu_bus_mux", mcu_bus_mux_p, ARRAY_SIZE(mcu_bus_mux_p),
		CLK_SET_RATE_PARENT, 0x0, 0, 2, 0, mux_table_2bit_pattern1, },
};

static const struct hisi_gate_clock hi3798mv200_sysctrl_gate_clks[] = {
	{ HI3798MV200_MCE_CLK, "clk_mce", "mcu_bus_mux",
		CLK_SET_RATE_PARENT, 0x48, 0, 0, },
	{ HI3798MV200_IR_CLK, "clk_ir", "clk_osc",
		CLK_SET_RATE_PARENT, 0x48, 4, 0, },
	{ HI3798MV200_TIMER01_CLK, "clk_timer01", "clk_osc",
		CLK_SET_RATE_PARENT, 0x48, 6, 0, },
	{ HI3798MV200_UART0_CLK, "clk_uart0", "uart0_mux",
		CLK_SET_RATE_PARENT, 0x48, 12, 0, },
};

static struct hisi_clock_data *hi3798mv200_sysctrl_clk_register(
					struct platform_device *pdev)
{
	struct hisi_clock_data *clk_data;
	int ret;

	clk_data = hisi_clk_alloc(pdev, HI3798MV200_SYSCTRL_NR_CLKS);
	if (!clk_data)
		return ERR_PTR(-ENOMEM);

	ret = hisi_clk_register_mux(hi3798mv200_sysctrl_mux_clks,
				ARRAY_SIZE(hi3798mv200_sysctrl_mux_clks),
				clk_data);
	if (ret)
		return ERR_PTR(ret);

	ret = hisi_clk_register_gate(hi3798mv200_sysctrl_gate_clks,
				ARRAY_SIZE(hi3798mv200_sysctrl_gate_clks),
				clk_data);
	if (ret)
		goto unregister_mux;

	ret = of_clk_add_provider(pdev->dev.of_node,
			of_clk_src_onecell_get, &clk_data->clk_data);
	if (ret)
		goto unregister_gate;

	return clk_data;

unregister_gate:
	hisi_clk_unregister_gate(hi3798mv200_sysctrl_gate_clks,
				ARRAY_SIZE(hi3798mv200_sysctrl_gate_clks),
				clk_data);
unregister_mux:
	hisi_clk_unregister_mux(hi3798mv200_sysctrl_mux_clks,
				ARRAY_SIZE(hi3798mv200_sysctrl_mux_clks),
				clk_data);
	return ERR_PTR(ret);
}

static void hi3798mv200_sysctrl_clk_unregister(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);

	of_clk_del_provider(pdev->dev.of_node);

	hisi_clk_unregister_gate(hi3798mv200_sysctrl_gate_clks,
				ARRAY_SIZE(hi3798mv200_sysctrl_gate_clks),
				crg->clk_data);
	hisi_clk_unregister_mux(hi3798mv200_sysctrl_mux_clks,
				ARRAY_SIZE(hi3798mv200_sysctrl_mux_clks),
				crg->clk_data);
}

static const struct hisi_crg_funcs hi3798mv200_sysctrl_funcs = {
	.register_clks = hi3798mv200_sysctrl_clk_register,
	.unregister_clks = hi3798mv200_sysctrl_clk_unregister,
};

static const struct of_device_id hi3798mv200_crg_match_table[] = {
	{ .compatible = "hisilicon,hi3798mv200-crg",
		.data = &hi3798mv200_crg_funcs },
	{ .compatible = "hisilicon,hi3798mv200-sysctrl",
		.data = &hi3798mv200_sysctrl_funcs },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3798mv200_crg_match_table);

static int hi3798mv200_crg_probe(struct platform_device *pdev)
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

static void hi3798mv200_crg_remove(struct platform_device *pdev)
{
	struct hisi_crg_dev *crg = platform_get_drvdata(pdev);

	hisi_reset_exit(crg->rstc);
	crg->funcs->unregister_clks(pdev);
}

static struct platform_driver hi3798mv200_crg_driver = {
	.probe          = hi3798mv200_crg_probe,
	.remove		= hi3798mv200_crg_remove,
	.driver         = {
		.name   = "hi3798mv200-crg",
		.of_match_table = hi3798mv200_crg_match_table,
	},
};

static int __init hi3798mv200_crg_init(void)
{
	return platform_driver_register(&hi3798mv200_crg_driver);
}
core_initcall(hi3798mv200_crg_init);

static void __exit hi3798mv200_crg_exit(void)
{
	platform_driver_unregister(&hi3798mv200_crg_driver);
}
module_exit(hi3798mv200_crg_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("HiSilicon Hi3798MV200 CRG Driver");
