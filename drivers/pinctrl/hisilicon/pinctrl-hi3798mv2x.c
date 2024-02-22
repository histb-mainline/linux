// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * IOCONFIG driver for Hi3798MV2x SoCs
 *
 * Copyright 2024 (r) Yang Xiwen <forbidden405@outlook.com>
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>

#include "../core.h"
#include "../pinconf.h"
#include "../pinmux.h"

#include "pinctrl-histb.h"

// The sequence is important!
enum hi3798mv2x_ioconfig_pins {
	INVALID = -1,
	Y19,
	W19,
	AA20,
	Y20,
	V19,
	Y21,
	W20,
	W21,
	V20,
	V21,
	U20,
	U21,
	T18,
	T19,
	T20,
	R20,
	R21,
	P20,
	R19,
	K18,
	J17,
	J18,
	H17,
	H18,
	K20,
	K19,
	J20,
	J19,
	H21,
	H20,
	H19,
	G20,
	G19,
	F21,
	F20,
	F19,
	E20,
	E19,
	D21,
	E18,
	C20,
	D19,
	B21,
	B18,
	C17,
	B17,
	A17,
	C16,
	B16,
	B4,
	C4,
	A3,
	B3,
	A2,
	B2,
	A6,
	C6,
	C5,
	C3,
	D4,
	D3,
	B1,
	C2,
	C1,
	A5,
	D5,
};

enum hi3798mv2x_pin_funcs {
	HI3798MV2X_INVALID = -1,
	HI3798MV2X_GPIO = HISTB_PIN_FUNCTION_GPIO,
	HI3798MV2X_BOOT_SEL,
	HI3798MV2X_CLKOUT_25M,
	HI3798MV2X_EMMC,
	/* Fast Ethernet PHY LED */
	HI3798MV2X_FE_LED,
	HI3798MV2X_HDMITX,
	HI3798MV2X_I2C_SCL,
	HI3798MV2X_I2C_SDA,
	HI3798MV2X_I2S_CLK,
	HI3798MV2X_I2S_DATA,
	HI3798MV2X_I2S_WS,
	HI3798MV2X_JTAG,
	HI3798MV2X_NAND,
	HI3798MV2X_PMC,
	HI3798MV2X_RGMII,
	HI3798MV2X_RMII,
	HI3798MV2X_SATA,
	HI3798MV2X_SDIO,
	HI3798MV2X_SIM,
	HI3798MV2X_SPDIF,
	HI3798MV2X_SPI,
	HI3798MV2X_SPI_FLASH,
	/*
	 * workaround for pin E19
	 *
	 * This pin can be either D0 or D1,
	 * so the common property is not adequate to select a function
	 */
	HI3798MV2X_TSI0_D0,
	HI3798MV2X_TSI0_D1,
	HI3798MV2X_TSI0_DATA,
	HI3798MV2X_TSI1_DATA,
	HI3798MV2X_TSI2_DATA,
	HI3798MV2X_TSI3_DATA,
	HI3798MV2X_TSO,
	HI3798MV2X_TSI_CLK,
	HI3798MV2X_TSI_SYNC,
	HI3798MV2X_TSI_VALID,
	HI3798MV2X_UART,
	HI3798MV2X_FUNCTION_COUNT,
};

static const char *const hi3798mv2x_pinctrl_function_names[] = {
	[HI3798MV2X_GPIO]	= "gpio",
	[HI3798MV2X_BOOT_SEL]	= "bootsel",
	[HI3798MV2X_CLKOUT_25M]	= "clkout",
	[HI3798MV2X_EMMC]	= "emmc",
	/* Fast Ethernet PHY LED */
	[HI3798MV2X_FE_LED]	= "fe_led",
	[HI3798MV2X_HDMITX]	= "hdmitx",
	[HI3798MV2X_I2C_SCL]	= "i2c_scl",
	[HI3798MV2X_I2C_SDA]	= "i2c_sda",
	[HI3798MV2X_I2S_CLK]	= "i2s_clk",
	[HI3798MV2X_I2S_DATA]	= "i2s_data",
	[HI3798MV2X_I2S_WS]	= "i2s_ws",
	[HI3798MV2X_JTAG]	= "jtag",
	[HI3798MV2X_NAND]	= "nand",
	[HI3798MV2X_PMC]	= "pmc",
	[HI3798MV2X_RGMII]	= "rgmii",
	[HI3798MV2X_RMII]	= "rmii",
	[HI3798MV2X_SATA]	= "sata",
	[HI3798MV2X_SDIO]	= "sdio",
	[HI3798MV2X_SIM]	= "sim",
	[HI3798MV2X_SPDIF]	= "spdif",
	[HI3798MV2X_SPI]	= "spi",
	[HI3798MV2X_SPI_FLASH]	= "spi_flash",
	/*
	 * workaround for pin E19
	 *
	 * This pin can be either D0 or D1,
	 * so the common property is not adequate to select a function
	 */
	[HI3798MV2X_TSI0_D0]	= "tsi0_d0",
	[HI3798MV2X_TSI0_D1]	= "tsi0_d1",
	/* DOCSIS */
	[HI3798MV2X_TSI0_DATA]	= "tsi0_data",
	[HI3798MV2X_TSI1_DATA]	= "tsi1_data",
	[HI3798MV2X_TSI2_DATA]	= "tsi2_data",
	[HI3798MV2X_TSI3_DATA]	= "tsi3_data",
	[HI3798MV2X_TSO]	= "tso",
	[HI3798MV2X_TSI_CLK]	= "tsi_clk",
	[HI3798MV2X_TSI_SYNC]	= "tsi_sync",
	[HI3798MV2X_TSI_VALID]	= "tsi_valid",
	[HI3798MV2X_UART]	= "uart",
};

/* FIXME: use struct pinfunction instead */
static const struct {
	const char *const *pins;
	unsigned int cnt;
} hi3798mv2x_ioconfig_function_tbl[] = {
	// all pins can be set to GPIO
	[HI3798MV2X_GPIO]	= { NULL, 0 },
	[HI3798MV2X_BOOT_SEL]	= { (const char *[]){ "AA20", "C3", "B1", }, 3 },
	[HI3798MV2X_CLKOUT_25M]	= { (const char *[]){ "A5", }, 1 },
	[HI3798MV2X_EMMC]	= { (const char *[]){ "V19", "Y21", "W20", "W21", "V20", "V21", "U20", "U21", "T18", "T20", "R20", "R21", }, 12 },
	[HI3798MV2X_FE_LED]	= { (const char *[]){ "A6", "C6", }, 2 },
	[HI3798MV2X_HDMITX]	= { (const char *[]){ "B17", "A17", "C16", "B16", }, 4 },
	[HI3798MV2X_I2C_SCL]	= { (const char *[]){ "R19", "K20", "F19", "E19", "C20", "D19", }, 6 },
	[HI3798MV2X_I2C_SDA]	= { (const char *[]){ "P20", "H18", "E20", "D19", "B21", }, 5 },
	[HI3798MV2X_I2S_CLK]	= { (const char *[]){ "J20", "H21", "H20", "G19", "F21", "F20", "F19", "E19", "D21", "D19", }, 10 },
	[HI3798MV2X_I2S_DATA]	= { (const char *[]){ "H21", "H20", "H19", "G20", "F21", "C20", "B21", }, 7 },
	[HI3798MV2X_I2S_WS]	= { (const char *[]){ "J19", "H19", "G19", "E20", "E18", }, 5 },
	[HI3798MV2X_JTAG]	= { (const char *[]){ "K20", "K19", "J20", "J19", "H21", "B18", }, 6 },
	[HI3798MV2X_NAND]	= { (const char *[]){ "Y19", "W19", "AA20", "Y20", "V19", "Y21", "W20", "W21", "V20", "V21", "U20", "U21", "T18", "T19", "T20", "R20", "R21", }, 17 },
	[HI3798MV2X_PMC]	= { (const char *[]){ "P20", "R19", "D5", }, 3 },
	[HI3798MV2X_RGMII]	= { (const char *[]){ "B4", "C4", "A3", "B3", "A2", "B2", "A6", "C6", "C5", "C3", "D4", "D3", "B1", "C2", "C1", "A5", "D5", }, 17 },
	[HI3798MV2X_RMII]	= { (const char *[]){ "B4", "C4", "A3", "B3", "A2", "B2", "C5", "B1", "C2", "C1", }, 10 },
	[HI3798MV2X_SATA]	= { (const char *[]){ "K18", "J17", }, 2 },
	[HI3798MV2X_SDIO]	= { (const char *[]){ "F20", "F19", "E20", "E19", "D21", "E18", "C20", "D19", "B21", "B4", "C4", "A3", "B3", "A2", "B2", "C5", "C2", "C1", }, 18 },
	[HI3798MV2X_SIM]	= { (const char *[]){ "K18", "J17", "J18", "H17", "H18", "K20", "K19", "J20", "J19", "H21", }, 10 },
	[HI3798MV2X_SPDIF]	= { (const char *[]){ "C17", }, 1 },
	[HI3798MV2X_SPI]	= { (const char *[]){ "K20", "K19", "J20", "J19", }, 4 },
	[HI3798MV2X_SPI_FLASH]	= { (const char *[]){ "H20", "H19", "G20", "G19", "F21", "F20", }, 6 },
	[HI3798MV2X_TSI0_D0]	= { (const char *[]){ "E19", }, 1 },
	[HI3798MV2X_TSI0_D1]	= { (const char *[]){ "E19", }, 1 },
	[HI3798MV2X_TSI0_DATA]	= { (const char *[]){ "K18", "J17", "J18", "H17", "H18", "F19", "E20", "D21", "D19", }, 9 },
	[HI3798MV2X_TSI1_DATA]	= { (const char *[]){ "J17", "J18", "H19", "G20", "E18", }, 5 },
	[HI3798MV2X_TSI2_DATA]	= { (const char *[]){ "H18", }, 1 },
	[HI3798MV2X_TSI3_DATA]	= { (const char *[]){ "J18", }, 1 },
	[HI3798MV2X_TSO]	= { (const char *[]){ "H20", "H19", "G20", "G19", }, 4 },
	[HI3798MV2X_TSI_CLK]	= { (const char *[]){ "K18", "H17", "G19", "F19", "D21", "E18", "C20", "B21", }, 8 },
	[HI3798MV2X_TSI_SYNC]	= { (const char *[]){ "J17", "H19", "E19", }, 3 },
	[HI3798MV2X_TSI_VALID]	= { (const char *[]){ "J17", "H17", "H18", "F21", "D21", "E18", "C20", }, 7 },
	[HI3798MV2X_UART]	= { (const char *[]){ "K18", "J17", "J18", "H17", "K19", "J20", "J19", "H21", "A6", "C6", "D4", "D3", }, 12 },
};

/* Frequentely used drive strength table */
#define DRIVE_STRENGTH_TABLE_SAMPLE_A ((const u8[]){ 4, 3, 2, 1, 0 })
#define DRIVE_STRENGTH_TABLE_SAMPLE_B ((const u8[]){ 18, 17, 16, 15, 13, 12, 11, 10, 9, 8, 7, 6, 4, 3, 2, 1, 0 })

static const struct pinctrl_pin_desc hi3798mv2x_ioconfig_pins[] = {
	HISTB_PIN(Y19, "Y19", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_NAND, 0x1 }, { HISTB_PIN_FUNCTION_EOF } }),
		  HISTB_PIN_FLAG_NOPD),
	HISTB_PIN(W19, "W19", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_NAND, 0x1 }, { HISTB_PIN_FUNCTION_EOF } }),
		  HISTB_PIN_FLAG_NOPD),
	HISTB_PIN(AA20, "AA20", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_BOOT_SEL, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(Y20, "Y20", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_NAND, 0x1 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(V19, "V19", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(Y21, "Y21", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(W20, "W20", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(W21, "W21", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(V20, "V20", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(V21, "V21", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(U20, "U20", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(U21, "U21", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(T18, "T18", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(T19, "T19", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]) { { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_NAND, 0x1 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(T20, "T20", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(R20, "R20", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(R21, "R21", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_GPIO, 0x0 }, { HI3798MV2X_NAND, 0x1 },
		   { HI3798MV2X_EMMC, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(P20, "P20", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_PMC, 0x0 }, { HI3798MV2X_I2C_SDA, 0x1 },
		   { HI3798MV2X_GPIO, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(R19, "R19", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[])
		   { { HI3798MV2X_PMC, 0x0 }, { HI3798MV2X_I2C_SCL, 0x1 },
		   { HI3798MV2X_GPIO, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(K18, "K18", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]) { { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SATA, 0x1 }, { HI3798MV2X_UART, 0x2 },
		   { HI3798MV2X_TSI0_DATA, 0x3 }, { HI3798MV2X_TSI_CLK, 0x4 },
		   { HI3798MV2X_SIM, 0x5 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(J17, "J17", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]) { { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SATA, 0x1 }, { HI3798MV2X_UART, 0x2 },
		   { HI3798MV2X_TSI_SYNC, 0x3 }, { HI3798MV2X_TSI1_DATA, 0x4 },
		   { HI3798MV2X_TSI0_DATA, 0x5 }, { HI3798MV2X_TSI_VALID, 0x6 },
		   { HI3798MV2X_SIM, 0x7 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(J18, "J18", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_UART, 0x1 }, { HI3798MV2X_TSI1_DATA, 0x2 },
		   { HI3798MV2X_TSI0_DATA, 0x3 }, { HI3798MV2X_TSI3_DATA, 0x4 },
		   { HI3798MV2X_SIM, 0x5 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(H17, "H17", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_UART, 0x1 }, { HI3798MV2X_TSI_CLK, 0x2 },
		   { HI3798MV2X_TSI0_DATA, 0x3 }, { HI3798MV2X_TSI_VALID, 0x4 },
		   { HI3798MV2X_SIM, 0x5 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(H18, "H18", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_I2C_SDA, 0x1 }, { HI3798MV2X_TSI_VALID, 0x2 },
		   { HI3798MV2X_TSI0_DATA, 0x3 }, { HI3798MV2X_TSI2_DATA, 0x4 },
		   { HI3798MV2X_SIM, 0x5 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(K20, "K20", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_JTAG, 0x0 },
		   { HI3798MV2X_I2C_SCL, 0x1 }, { HI3798MV2X_SPI, 0x2 }, { HI3798MV2X_SIM, 0x3 },
		   { HI3798MV2X_GPIO, 0x4 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(K19, "K19", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_JTAG, 0x0 },
		   { HI3798MV2X_SPI, 0x1 }, { HI3798MV2X_SIM, 0x2 }, { HI3798MV2X_GPIO, 0x3 },
		   { HI3798MV2X_UART, 0x4 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(J20, "J20", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_JTAG, 0x0 },
		   { HI3798MV2X_I2S_CLK, 0x1 }, { HI3798MV2X_SPI, 0x2 },
		   { HI3798MV2X_SIM, 0x3 }, { HI3798MV2X_GPIO, 0x4 },
		   { HI3798MV2X_UART, 0x5 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(J19, "J19", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_JTAG, 0x0 },
		   { HI3798MV2X_I2S_WS, 0x1 }, { HI3798MV2X_SPI, 0x2 },
		   { HI3798MV2X_SIM, 0x3 }, { HI3798MV2X_GPIO, 0x4 },
		   { HI3798MV2X_UART, 0x5 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(H21, "H21", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_JTAG, 0x0 },
		   { HI3798MV2X_I2S_CLK, 0x1 }, { HI3798MV2X_I2S_DATA, 0x2 },
		   { HI3798MV2X_SIM, 0x3 }, { HI3798MV2X_GPIO, 0x4 },
		   { HI3798MV2X_UART, 0x5 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(H20, "H20", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_I2S_CLK, 0x1 }, { HI3798MV2X_I2S_DATA, 0x2 },
		   { HI3798MV2X_SPI_FLASH, 0x3 }, { HI3798MV2X_TSO, 0x4 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_NOPD),
	HISTB_PIN(H19, "H19", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_I2S_WS, 0x1 }, { HI3798MV2X_I2S_DATA, 0x2 },
		   { HI3798MV2X_TSI_SYNC, 0x3 }, { HI3798MV2X_TSI1_DATA, 0x4 },
		   { HI3798MV2X_SPI_FLASH, 0x5 }, { HI3798MV2X_TSO, 0x6 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(G20, "G20", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_I2S_DATA, 0x1 }, { HI3798MV2X_TSI1_DATA, 0x3 },
		   { HI3798MV2X_SPI_FLASH, 0x5 }, { HI3798MV2X_TSO, 0x6 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_NOPD),
	HISTB_PIN(G19, "G19", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_I2S_CLK, 0x1 }, { HI3798MV2X_I2S_WS, 0x2 },
		   { HI3798MV2X_TSI_CLK, 0x3 }, { HI3798MV2X_SPI_FLASH, 0x5 },
		   { HI3798MV2X_TSO, 0x6 }, { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_NOPD),
	HISTB_PIN(F21, "F21", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_I2S_DATA, 0x1 }, { HI3798MV2X_I2S_CLK, 0x2 },
		   { HI3798MV2X_TSI_VALID, 0x3 }, { HI3798MV2X_SPI_FLASH, 0x5 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(F20, "F20", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SDIO, 0x1 }, { HI3798MV2X_I2S_CLK, 0x2 },
		   { HI3798MV2X_SPI_FLASH, 0x4 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(F19, "F19", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SDIO, 0x1 }, { HI3798MV2X_I2C_SCL, 0x3 },
		   { HI3798MV2X_I2S_CLK, 0x4 }, { HI3798MV2X_TSI0_DATA, 0x5 },
		   { HI3798MV2X_TSI_CLK, 0x6 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(E20, "E20", ((const u8[]){ 18, 16, 14, 12, 5, 4, 2, 1, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SDIO, 0x1 }, { HI3798MV2X_I2S_WS, 0x4 },
		   { HI3798MV2X_TSI0_DATA, 0x5 }, { HI3798MV2X_I2C_SDA, 0x6 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(E19, "E19", ((const u8[]){ 18, 16, 14, 12, 5, 4, 2, 1, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SDIO, 0x1 }, { HI3798MV2X_TSI0_D1, 0x2 },
		   { HI3798MV2X_TSI_SYNC, 0x3 }, { HI3798MV2X_I2S_CLK, 0x4 },
		   { HI3798MV2X_TSI0_D0, 0x5 }, { HI3798MV2X_I2C_SCL, 0x6 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(D21, "D21", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SDIO, 0x1 }, { HI3798MV2X_TSI0_DATA, 0x3 },
		   { HI3798MV2X_I2S_CLK, 0x4 }, { HI3798MV2X_TSI_CLK, 0x5 },
		   { HI3798MV2X_TSI_VALID, 0x6 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(E18, "E18", ((const u8[]){ 18, 16, 14, 12, 5, 4, 2, 1, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SDIO, 0x1 }, { HI3798MV2X_TSI_CLK, 0x3 },
		   { HI3798MV2X_I2S_WS, 0x4 }, { HI3798MV2X_TSI_VALID, 0x5 },
		   { HI3798MV2X_TSI1_DATA, 0x6 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(C20, "C20", ((const u8[]){ 18, 16, 14, 12, 5, 4, 2, 1, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SDIO, 0x1 }, { HI3798MV2X_TSI_CLK, 0x2 },
		   { HI3798MV2X_TSI_VALID, 0x3 }, { HI3798MV2X_I2S_DATA, 0x4 },
		   { HI3798MV2X_I2C_SCL, 0x5 }, { HI3798MV2X_TSI_VALID, 0x6 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(D19, "D19", ((const u8[]){ 18, 16, 14, 12, 5, 4, 2, 1, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SDIO, 0x1 }, { HI3798MV2X_I2C_SCL, 0x3 },
		   { HI3798MV2X_I2S_CLK, 0x4 }, { HI3798MV2X_I2C_SDA, 0x5 },
		   { HI3798MV2X_TSI0_DATA, 0x6 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(B21, "B21", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SDIO, 0x1 }, { HI3798MV2X_I2C_SDA, 0x3 },
		   { HI3798MV2X_I2S_DATA, 0x4 }, { HI3798MV2X_TSI_CLK, 0x6 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(B18, "B18", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_JTAG, 0x0 },
		   { HI3798MV2X_GPIO, 0x1 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(C17, "C17", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_SPDIF, 0x1 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(B17, "B17", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_HDMITX, 0x1 },
		   { HI3798MV2X_GPIO, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(A17, "A17", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_HDMITX, 0x1 },
		   { HI3798MV2X_GPIO, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(C16, "C16", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_HDMITX, 0x1 },
		   { HI3798MV2X_GPIO, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(B16, "B16", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_HDMITX, 0x1 },
		   { HI3798MV2X_GPIO, 0x2 }, { HISTB_PIN_FUNCTION_EOF } }),
		  HISTB_PIN_FLAG_NOPU | HISTB_PIN_FLAG_NOPD | HISTB_PIN_FLAG_NOSR),
	HISTB_PIN(B4, "B4", ((const u8[]){ 18, 16, 14, 12, 10, 8, 6, 4, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_RMII, 0x2 },
		   { HI3798MV2X_SDIO, 0x3 }, { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(C4, "C4", ((const u8[]){ 18, 16, 14, 12, 10, 8, 6, 4, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_RMII, 0x2 }, { HI3798MV2X_SDIO, 0x3 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(A3, "A3", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_RMII, 0x2 }, { HI3798MV2X_SDIO, 0x3 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(B3, "B3", ((const u8[]){ 18, 16, 14, 12, 10, 8, 6, 4, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_RMII, 0x2 }, { HI3798MV2X_SDIO, 0x3 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(A2, "A2", ((const u8[]){ 18, 16, 14, 12, 10, 8, 6, 4, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_RMII, 0x2 }, { HI3798MV2X_SDIO, 0x3 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(B2, "B2", ((const u8[]){ 18, 16, 14, 12, 10, 8, 6, 4, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_RMII, 0x2 }, { HI3798MV2X_SDIO, 0x3 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(A6, "A6", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_UART, 0x2 }, { HI3798MV2X_FE_LED, 0x3 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(C6, "C6", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_UART, 0x2 }, { HI3798MV2X_FE_LED, 0x3 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(C5, "C5", DRIVE_STRENGTH_TABLE_SAMPLE_A,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_RMII, 0x2 }, { HI3798MV2X_SDIO, 0x3 },
		   { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(C3, "C3", ((const u8[]){ 18, 16, 14, 12, 10, 8, 6, 4, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_BOOT_SEL, 0x2 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(D4, "D4", ((const u8[]){ 18, 16, 14, 12, 10, 8, 6, 4, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_UART, 0x2 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(D3, "D3", ((const u8[]){ 18, 16, 14, 12, 10, 8, 6, 4, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_UART, 0x2 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(B1, "B1", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   { HI3798MV2X_RGMII, 0x1 }, { HI3798MV2X_RMII, 0x2 },
		   { HI3798MV2X_BOOT_SEL, 0x3 }, { HISTB_PIN_FUNCTION_EOF } }),
		  HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(C2, "C2", ((const u8[]){ 18, 16, 14, 12, 10, 8, 6, 4, 0 }),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_RGMII, 0x1 },
		   { HI3798MV2X_RMII, 0x2 }, { HI3798MV2X_SDIO, 0x3 }, { HI3798MV2X_GPIO, 0x4 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(C1, "C1", DRIVE_STRENGTH_TABLE_SAMPLE_B,
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_RGMII, 0x1 },
		   { HI3798MV2X_RMII, 0x2 }, { HI3798MV2X_SDIO, 0x3 }, { HI3798MV2X_GPIO, 0x4 },
		   { HISTB_PIN_FUNCTION_EOF } }), HISTB_PIN_FLAG_SCHMITT),
	HISTB_PIN(A5, "A5", ((const u8 *)NULL),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_GPIO, 0x0 },
		   {HI3798MV2X_CLKOUT_25M, 0x1 }, { HISTB_PIN_FUNCTION_EOF } }), 0),
	HISTB_PIN(D5, "D5", ((const u8 *)NULL),
		  ((const struct histb_pin_mux_desc[]){ { HI3798MV2X_PMC, 0x0 },
		   { HI3798MV2X_GPIO, 0x1 }, { HISTB_PIN_FUNCTION_EOF } }),
		  HISTB_PIN_FLAG_NOPU | HISTB_PIN_FLAG_NOPD | HISTB_PIN_FLAG_NOSR),
};

static int hi3798mv2x_ioconfig_probe(struct platform_device *pdev)
{
	void __iomem *base;
	struct pinctrl_desc *pctldesc;
	struct pinctrl_dev *pctl_dev;
	struct device *dev = &pdev->dev;
	int i, ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	pctldesc = devm_kzalloc(dev, sizeof(*pctldesc), GFP_KERNEL);
	if (!pctldesc)
		return -ENOMEM;

	pctldesc->name = "hi3798mv2x-ioconfig";
	pctldesc->owner = THIS_MODULE;
	pctldesc->pctlops = &histb_pinctrl_pctl_ops;
	pctldesc->pmxops = &histb_pinctrl_pinmux_ops;
	pctldesc->confops = &histb_pinctrl_pinconf_ops;
	pctldesc->pins = hi3798mv2x_ioconfig_pins;
	pctldesc->npins = ARRAY_SIZE(hi3798mv2x_ioconfig_pins);

	ret = devm_pinctrl_register_and_init(dev, pctldesc, base, &pctl_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to register pinctrl device\n");

	for (i = 0; i < pctldesc->npins; i++) {
		ret = pinctrl_generic_add_group(pctl_dev, pctldesc->pins[i].name,
						&pctldesc->pins[i].number, 1, NULL);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "Failed to register groups\n");
	}

	for (i = 0; i < HI3798MV2X_FUNCTION_COUNT; i++) {
		ret = pinmux_generic_add_function(pctl_dev, hi3798mv2x_pinctrl_function_names[i],
						  hi3798mv2x_ioconfig_function_tbl[i].pins,
						  hi3798mv2x_ioconfig_function_tbl[i].cnt, NULL);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "Failed to register functions\n");
	}

	return pinctrl_enable(pctl_dev);
}

static const struct of_device_id hi3798mv2x_pinctrl_match[] = {
	{ .compatible = "hisilicon,hi3798mv200-ioconfig", },
	{ },
};
MODULE_DEVICE_TABLE(of, hi3798mv2x_pinctrl_match);

static struct platform_driver hi3798mv2x_pinctrl_driver = {
	.probe = hi3798mv2x_ioconfig_probe,
	.driver = {
		.name = "hi3798mv2x-ioconfig",
		.of_match_table = hi3798mv2x_pinctrl_match,
	},
};

module_platform_driver(hi3798mv2x_pinctrl_driver);

MODULE_AUTHOR("Yang Xiwen <forbidden405@outlook.com>");
MODULE_DESCRIPTION("IOCONFIG pinctrl driver for Hi3798MV2x SoC");
MODULE_LICENSE("GPL");
