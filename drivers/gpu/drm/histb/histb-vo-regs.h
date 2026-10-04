/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register definitions for the HiSTB (hi3798mv100) VO / VDP v2 display
 * controller, extracted from the vendor SDK:
 *   HiSTBLinuxV100R005C00SPC050
 *     source/common/drv/include/hi3798mv100/hi_reg_vdp.h
 *     source/msp/drv/vo/vdp_v2_0/hal/3798m/{vdp_define.h,vdp_driver.c}
 *
 * The VO block sits at 0xf8cc0000 on hi3798mv100 (DISP_BASE_ADDRESS in
 * vdp_define.h).  Two display channels (DHD0/DHD1) exist, spaced 0x400
 * apart; this driver drives DHD0, the HD channel wired to the HDMI TX.
 *
 * The offsets below were computed mechanically by walking the global
 * register struct in hi_reg_vdp.h (typedef starting at line 9971), adding
 * 4 bytes per U_* union / unsigned int member and 4 bytes per array
 * element, *including* the "unsigned int Reserved_N[...]" members.  The
 * checksum HDATE_VERSION == 0xf000 validates the walk.
 */

#ifndef _HISTB_VO_REGS_H_
#define _HISTB_VO_REGS_H_

#include <linux/bitops.h>

/* ------------------------------------------------------------------ */
/* Global (offsets from VO base)                                       */
/* ------------------------------------------------------------------ */
#define VO_CTRL			0x0000
#define   VO_CTRL_CK_GT_EN	BIT(31)		/* vo_ck_gt_en */
#define VO_MUX			0x0100	/* output routing: sddate[0] hddate[1]
					 * vga[2] hdmi[3] digital[5:4];
					 * hdmi_sel selects which DHD channel
					 * feeds the HDMI interface (SDK
					 * vdp_v3_0 mv410: hdmi_sel = hd_id)
					 */
#define   VO_MUX_SDDATE_SEL	BIT(0)
#define   VO_MUX_HDDATE_SEL	BIT(1)
#define   VO_MUX_VGA_SEL	BIT(2)
#define   VO_MUX_HDMI_SEL	BIT(3)
#define   VO_MUX_DIGITAL	GENMASK(5, 4)
#define VO_MUX_TESTSYNC		0x0108	/* vo_test_en[31] + test dv/hs/vs/field
					 * polarity bits; boot leaves
					 * 0x12123434 (test disabled)
					 */
#define VO_MUX_TESTDATA		0x010c	/* test_data[29:0]; boot leaves
					 * 0x04040000
					 */
/*
 * 0x140 is reserved in hi_reg_vdp.h on both mv100 and mv200 and no SDK code
 * writes it; the state that scans out the boot logo reads it back as 0.  It is
 * listed here only so the register dump lines up with the SDK's naming.
 */
#define VO_DAC_MUX		0x0140
#define VO_INTSTA		0x0004
#define VO_MSKINTSTA		0x0008	/* write 1 to clear int status */
#define VO_INTMSK		0x000c	/* 1 = masked */

#define VO_INT_DHD0_VTTHD1	BIT(0)	/* DHD0 vblank-ish threshold int */
#define VO_INT_DHD0_VTTHD2	BIT(1)
#define VO_INT_DHD0_VTTHD3	BIT(2)

#define VOAXICTRL		0x0034
#define   VO_AXI_M0_RD0_OSTD GENMASK(3, 0)	/* read outstanding, bus 0 */
#define   VO_AXI_M0_RD1_OSTD GENMASK(7, 4)	/* read outstanding, bus 1 */
#define   VO_AXI_M0_WR_OSTD GENMASK(11, 8)	/* write outstanding */

/*
 * Second display channel ("SD"/DSD), addressed by the SDK as
 * DHD0_<reg> + CHN_OFFSET (3798m/vdp_driver.c:9838-9858 VDP_DSD_DEFAULT).
 * hi_reg_vdp.h models no members in this range -- 0xc400.. is a Reserved hole
 * of the mv100 struct -- so the names below mirror the DHD0 layout rather
 * than vendor register names.
 */
#define SD_CTRL			0xc400
#define SD_VSYNC		0xc404
#define SD_HSYNC1		0xc408
#define SD_HSYNC2		0xc40c
#define SD_VPLUS		0xc410
#define SD_PWR			0xc414
#define SD_VTTHD3		0xc418
#define SD_VTTHD		0xc41c
#define SD_SYNC_INV		0xc420
#define SD_DATA_SEL		0xc42c

/* ------------------------------------------------------------------ */
/* G0 graphics (RGB fetch) layer                                       */
/* ------------------------------------------------------------------ */
/* graphics layers are one 0x800-wide block each: G0..G4 (GFX_MAX=5) */
#define GFX_LAYER_STRIDE		0x800
#define G0_CTRL			0x6000
#define   G0_CTRL_IFMT GENMASK(7, 0)
#define   G0_CTRL_BITEXT GENMASK(9, 8)	/* bitext, U_G0_CTRL bits[9..8] */
#define     G0_BITEXT_3RD	3	/* VDP_GFX_BITEXTEND_3RD */
#define   G0_CTRL_FLIP_EN BIT(24)	/* flip_en, U_G0_CTRL bit24 */
#define   G0_CTRL_READ_MODE BIT(26)	/* read_mode; 1 = VDP_RMODE_PROGRESSIVE */
#define   G0_CTRL_UPD_MODE BIT(27)	/* upd_mode;  1 = VDP_RMODE_PROGRESSIVE */
#define   G0_CTRL_SURFACE_EN BIT(31)
#define G0_UPD			0x6004
#define   G0_UPD_REGUP BIT(0)
#define G0_ADDR			0x6010
#define G0_STRIDE		0x601c
#define   G0_STRIDE_MASK GENMASK(15, 0)	/* unit: 16 bytes */
#define G0_IRESO		0x6020
#define   G0_IRESO_IW GENMASK(11, 0)
#define   G0_IRESO_IH GENMASK(23, 12)
#define G0_CBMPARA		0x6030
#define   G0_CBMPARA_GALPHA GENMASK(7, 0)
#define   G0_CBMPARA_PALPHA_EN BIT(12)	/* take alpha from the pixel, not from galpha */
#define   G0_CBMPARA_KEY_EN	BIT(14)	/* colour-key the surface */
#define   G0_CBMPARA_KEY_MODE	BIT(15)
#define G0_PARAUP		0x6044
#define G0_DFPOS		0x6080
#define   G0_DFPOS_X GENMASK(11, 0)
#define   G0_DFPOS_Y GENMASK(23, 12)
#define G0_DLPOS		0x6084
#define   G0_DLPOS_X GENMASK(11, 0)
#define   G0_DLPOS_Y GENMASK(23, 12)
#define G0_VFPOS		0x6088
#define G0_VLPOS		0x608c

/*
 * Input pixel format (VDP_GFX_IFMT_E in the SDK, vdp_define.h:370-385).
 * The alpha byte is *consumed* by the A-prefixed codes: a DRM_FORMAT_XRGB8888
 * framebuffer (which carries 0x00 there) fed in as ARGB_8888 composites as
 * fully transparent, so XRGB has to use the keyed code that ignores byte 3.
 * Measured on the board: filling the boot logo's ARGB8888 buffer with
 * 0x00ffffff blanks the panel, 0xffffffff makes it white.
 */
#define G0_IFMT_KRGB888		0x60	/* VDP_GFX_IFMT_KRGB_888, 32bpp, no alpha */
#define G0_IFMT_ARGB8888	0x68	/* VDP_GFX_IFMT_ARGB_8888 */
#define G0_IFMT_RGBA8888	0xe8	/* VDP_GFX_IFMT_RGBA_8888 */

/* ------------------------------------------------------------------ */
/* GP0 graphics post block (G0 -> GP0 -> CBM mixer -> DHD0)            */
/* ------------------------------------------------------------------ */
#define GP0_CTRL		0x9000
#define   GP0_CTRL_READ_MODE	BIT(31)	/* U_GP0_CTRL read_mode; the boot's live
					 * logo configuration has it set
					 * (0x80001203)
					 */
/*
 * Whole-register values of the ip orders VDP_GP_SetIpOrder() can select
 * (3798m/vdp_driver.c:5427-5467): ORDER_NULL 0x1032, ORDER_CSC 0x1230,
 * ORDER_ZME_CSC 0x1203.
 */
#define GP0_ORDER_NULL		0x00001032
#define GP0_ORDER_CSC		0x00001230
#define GP0_ORDER_ZME_CSC	0x00001203
#define   GP0_CTRL_MUX1_SEL GENMASK(1, 0)
#define   GP0_CTRL_MUX2_SEL GENMASK(5, 4)
#define   GP0_CTRL_AOUT_SEL GENMASK(9, 8)
#define   GP0_CTRL_BOUT_SEL GENMASK(13, 12)
#define GP0_UPD			0x9004
#define GP0_GALPHA		0x9020
#define GP0_HCOEFAD		0x9010	/* ZME horizontal coefficient window */
#define GP0_VCOEFAD		0x9014	/* ZME vertical coefficient window */
#define GP0_PARAUP		0x9018	/* gdc_paraup, U_GP0_PARAUP */
#define GP0_DFPOS		0x9100
#define GP0_DLPOS		0x9104
#define GP0_BK			0x9110
#define GP0_ALPHA		0x9114
#define   GP0_UPD_REGUP BIT(0)
#define GP0_ORESO		0x9008
#define   GP0_ORESO_OW GENMASK(11, 0)
#define   GP0_ORESO_OH GENMASK(23, 12)
#define GP0_IRESO		0x900c
#define   GP0_IRESO_IW GENMASK(11, 0)
#define   GP0_IRESO_IH GENMASK(23, 12)
#define GP0_DFPOS		0x9100
#define GP0_DLPOS		0x9104
#define GP0_VFPOS		0x9108
#define GP0_VLPOS		0x910c
/*
 * GP0 CSC stage (offset verified with gcc offsetof against the SDK's
 * hi_reg_vdp.h, not by walking the header).  Only the enable bit is written:
 * the interface runs in RGB, so the stage stays bypassed and the transmitter
 * does the colour conversion.
 */
#define GP0_CSC_IDC		0x9120	/* cscidc0/1[21:0] csc_en[22] */

/*
 * GP0 zoom engine step registers: hratio/vratio are the source pixels consumed
 * per destination pixel, in 4.20 and 4.12 fixed point (horizontal precision
 * from the sibling IP: vpss_hal_3798cv200.h `VPSS_ZME_HPREC (1<<20)`).  They
 * are shadowed like the rest of the block, so a driver that sets GP0_IRESO /
 * GP0_ORESO without them keeps scaling by whatever the bootloader asked for.
 */
#define GP0_ZME_HSP		0x9140
#define   GP0_ZME_HSP_RATIO	GENMASK(23, 0)
#define   GP0_ZME_HSP_HFIR_ORDER BIT(24)
#define   GP0_ZME_HSP_HAFIR_EN	BIT(25)
#define   GP0_ZME_HSP_HFIR_EN	BIT(26)
#define   GP0_ZME_HSP_HCHMID_EN	BIT(28)
#define   GP0_ZME_HSP_HLMID_EN	BIT(29)
#define   GP0_ZME_HSP_HAMID_EN	BIT(30)
#define   GP0_ZME_HSP_HSC_EN	BIT(31)
#define   GP0_ZME_HPREC		BIT(20)
#define GP0_ZME_HLOFFSET	0x9144
#define GP0_ZME_HCOFFSET	0x9148
#define GP0_ZME_VSP		0x914c	/* filter/tap enables only; ratio is in VSR */
#define GP0_ZME_VSR		0x9150
#define   GP0_ZME_VSR_RATIO	GENMASK(15, 0)
#define   GP0_ZME_VPREC		BIT(12)
#define GP0_ZME_VOFFSET		0x9154

/*
 * GP1: the second graphics-post block, GP0 + GP_OFFSET (0x800, from the SDK's
 * GP_MAX = 2 and its GP0_CTRL + u32Data * GP_OFFSET addressing).
 * hi_reg_vdp.h names nothing in this range, so the field layout below is
 * taken from U_GP0_CTRL and applied at the +0x800 mirror offsets.
 */
#define GP1_CTRL		0x9800
#define GP1_UPD			0x9804
#define GP1_HCOEFAD		0x9810
#define GP1_VCOEFAD		0x9814
#define GP1_PARAUP		0x9818
#define GP1_ORESO		0x9808
#define GP1_IRESO		0x980c
#define GP1_GALPHA		0x9820
#define GP1_DFPOS		0x9900
#define GP1_DLPOS		0x9904
#define GP1_VFPOS		0x9908
#define GP1_VLPOS		0x990c

/* ------------------------------------------------------------------ */
/* Mixers / CBM                                                        */
/* ------------------------------------------------------------------ */
/*
 * hi_reg_vdp.h leaves 0x60a8-0x8fff and 0x918c-0x9fff as Reserved holes, but
 * the silicon has the full set the SDK addresses by stride: GFX_MAX=5 gives
 * G0..G4 at 0x6000/0x6800/0x7000/0x7800/0x8000 and GP_MAX=2 gives GP0/GP1 at
 * 0x9000/0x9800 (vdp_define.h:40-63, applied at vdp_driver.c:4672/5414).
 * The boot logo is scanned out of G1, and both graphics slots of MIXG0 are
 * in use, so MIXG0 has to be programmed like any other mixer even though the
 * header never names it.
 */
#define MIXG0_BKG		0xb200
#define MIXG0_BKALPHA		0xb204
#define MIXG0_MIX		0xb208
#define   MIX_PRIO0 GENMASK(3, 0)
#define   MIX_PRIO1 GENMASK(7, 4)
#define CBM_BKG1		0xb400	/* DHD0 mixer background:
					 * Cr[9:0] Cb[19:10] Y[29:20]
					 */
#define   CBM_BKG_BLACK		0x04080200	/* Y=64, Cb=Cr=512 */
#define CBM_MIX1		0xb408		/* DHD0 mixer priority */
#define CBM_BKG2		0xb420		/* DHD1 mixer background */
#define CBM_MIX2		0xb428		/* DHD1 mixer priority */
#define CBM_ATTR		0xb440
#define   CBM_ATTR_SUR0 BIT(0)
#define   CBM_ATTR_SUR1 BIT(1)
#define   CBM_ATTR_SUR2 BIT(2)
#define   CBM_ATTR_SUR3 BIT(3)
#define   CBM_ATTR_SUR4 BIT(4)

/* CBM_MIX1 layer ids (values programmed are layer_id + 1, 0 = no layer) */
#define CBM_LAYER_VP0		1
#define CBM_LAYER_GP0		2

/*
 * MIXG0 is the graphics-side mixer and numbers its slots by hifb layer id
 * (drv_hifb_adp.c:1071-1073): 1 = HD_0 = G0, 2 = HD_1 = G1.  Same bit
 * positions as above, different namespace.
 */
#define MIXG0_LAYER_G0		1
#define MIXG0_LAYER_G1		2

/* ------------------------------------------------------------------ */
/* DHD0 display channel (DHD1 = base + 0x400)                          */
/* ------------------------------------------------------------------ */
#define DHD0_CTRL		0xc000
#define   DHD_CTRL_REGUP BIT(0)
#define   DHD_CTRL_IOP BIT(4)		/* 1 = progressive */
#define   DHD_CTRL_HDMI_MODE BIT(14)
#define   DHD_CTRL_CBAR_SEL BIT(29)	/* color-bar pattern select */
#define   DHD_CTRL_CBAR_EN BIT(30)	/* hardware color-bar generator */
#define   DHD_CTRL_INTF_EN BIT(31)
#define   DHD_CTRL_INTFB BIT(15)	/* reserved in the mv100 header layout;
					 * VDP_DHD_DEFAULT() keeps it set in
					 * its 0x8011 boot word but that write
					 * lands on the SD channel for mv100.
					 * Both the boot's logo state and our
					 * enabled state read it set; it has not
					 * been A/B tested on its own.
					 */
#define DHD0_VSYNC		0xc004
#define   DHD_VSYNC_VACT GENMASK(11, 0)
#define   DHD_VSYNC_VBB GENMASK(19, 12)
#define   DHD_VSYNC_VFB GENMASK(27, 20)
#define DHD0_HSYNC1		0xc008
#define   DHD_HSYNC1_HACT GENMASK(15, 0)
#define   DHD_HSYNC1_HBB GENMASK(31, 16)
#define DHD0_HSYNC2		0xc00c
#define   DHD_HSYNC2_HFB GENMASK(15, 0)
#define   DHD_HSYNC2_HMID GENMASK(31, 16)
#define DHD0_VPLUS		0xc010
#define   DHD_VPLUS_BVACT GENMASK(11, 0)
#define   DHD_VPLUS_BVBB GENMASK(19, 12)
#define   DHD_VPLUS_BVFB GENMASK(27, 20)
#define DHD0_PWR		0xc014
#define   DHD_PWR_HPW GENMASK(15, 0)
#define   DHD_PWR_VPW GENMASK(23, 16)
#define DHD0_VTTHD3		0xc018
#define DHD0_VTTHD		0xc01c
#define   DHD_VTTHD1 GENMASK(12, 0)
#define   DHD_VTTHD1_MODE BIT(15)	/* 1 = frame based */
#define DHD0_SYNC_INV		0xc020
#define   DHD_SYNC_HDMI_DV_INV BIT(8)
#define   DHD_SYNC_HDMI_HS_INV BIT(9)
#define   DHD_SYNC_HDMI_VS_INV BIT(10)
#define   DHD_SYNC_HDMI_F_INV BIT(11)
#define DHD0_DATA_SEL		0xc02c
#define DHD0_STATE		0xc0f0	/* live: vblank[1] vcnt[15:3] */
#define DHD0_DEBUG		0xc0f8	/* live: pix_h[15:0] pix_v[27:16] pix_src[30:28] */
#define DHD0_DEBUG_STATE	0xc0fc	/* live: pixel_value[29:0] at the sample point */
#define HDATE_VERSION		0xf000	/* IP version word; anchor of the offset walk */
#define HDATE_STATE		0xf010	/* hd digital port: line_len[12:0] frame_len[26:16] */
#define HDATE_EN		0xf004	/* hd_en[0]: hd digital output port enable */
#define HDATE_POLA_CTRL		0xf008	/* hs/vs/fid input polarity */
#define HDATE_VIDEO_FORMAT	0xf00c	/* video_ft[3:0] sync_add[6:4] out_ctrl[8:7] csc[11:9] */
#define HDATE_OUT_CTRL		0xf014	/* vsync_sel[1:0] hsync_sel[3:2] video3_sel[5:4]
					 * video2_sel[7:6] video1_sel[9:8] src_ctrl[11:10]
					 * sync_lpf_en[12] sd_sel[13]
					 */
/*
 * hdmi_data_sel picks the data type the DHD channel serialises on the HDMI
 * pins: 1 = RGB, 0 = YCbCr.  It is bit 3 here and the only one the VO driver
 * owns: bit 1 is vga_data_sel and bit 2 is reserved on mv100
 * (hi_reg_vdp.h:7253-7257), and VDP_DISP_SetDataSel() reads the register,
 * replaces bit 3 and writes it back (vdp_driver.c:4051-4054).
 */
#define   DHD_DATA_SEL_HDMI BIT(3)
#define DHD0_ABUFTHD		0xc034

#endif /* _HISTB_VO_REGS_H_ */
