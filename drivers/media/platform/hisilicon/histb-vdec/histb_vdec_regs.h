/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Register definitions for the HiSilicon Hi3798MV100 video decoder (VDH,
 * HiVDHV300R001 generation). Derived from the vendor firmware's register
 * description headers and cross-checked against the firmware's own assembly and
 * the hardware.
 */
#ifndef HISTB_VDEC_REGS_H
#define HISTB_VDEC_REGS_H

#include <linux/bits.h>

/*
 * VDM register window, 4 KiB plus the SED block at 0xF8C30000. Offsets are
 * register indices as used by the vendor firmware. The syntax-error statistics
 * at 0x1000 belong to the same hardware block (the firmware reads them as its
 * syntax-error record pair), so the mapping covers them while the luminance
 * histogram past 0x8100 stays outside.
 */
#define VDM_START			0x000	/* doorbell: write 0, 1, 0 */
#define VDM_REPAIR			0x004	/* RealVideo repair engine start */
#define VDM_CFG0			0x008
#define VDM_CFG0_MBAMT			GENMASK(19, 0)
#define VDM_CFG0_MEM_CG			BIT(20)	/* memory clock gating */
#define VDM_CFG0_MOD_CG			BIT(21)	/* module clock gating */
#define VDM_CFG0_MARKER_DETECT	BIT(22)
#define VDM_CFG0_ACLAST_DETECT	BIT(23)
#define VDM_CFG0_COEFIDX_DETECT	BIT(24)
#define VDM_CFG0_VOPTYPE_DETECT	BIT(25)
#define VDM_CFG0_LD_QMATRIX		BIT(30)
#define VDM_CFG1			0x00c
#define VDM_CFG1_VID_STD		GENMASK(3, 0)
#define VDM_CFG1_DDR_STRIDE		GENMASK(13, 4)	/* stride >> 6 */
#define VDM_CFG1_FST_SLC_GRP		BIT(14)
#define VDM_CFG1_MV_OUT_EN		BIT(15)
#define VDM_CFG1_MAX_SLCGRP_NUM		GENMASK(27, 16)
#define VDM_CFG1_LINE_NUM_OUT_EN	BIT(28)
#define VDM_CFG1_DBLK_1D_EN		BIT(29)
#define VDM_CFG1_COMPRESS_EN		BIT(30)
#define VDM_CFG1_PPFD_EN			BIT(31)
#define VDM_AVM_ADDR			0x010	/* downstream msg pool */
#define VDM_VAM_ADDR			0x014	/* upstream msg pool */
#define VDM_STREAM_BASE			0x018
#define VDM_STATE			0x01c
#define VDM_STATE_SLICE_NUM		GENMASK(16, 0)
#define VDM_STATE_DEC_OVER		BIT(17)
#define VDM_STATE_DEC_ERR		BIT(18)
#define VDM_STATE_VERSION		GENMASK(26, 19)
#define VDM_INT_STATE			0x020	/* write 1 to clear */
#define VDM_INT_MASK			0x024	/* write 1 to mask */
#define VDM_INT_DEC_OVER		BIT(0)
#define VDM_CTRL_STATE			0x028	/* == 1 means idle */
#define VDM_EMAR_ID			0x030
#define VDM_RPT_CNT			0x034
#define VDM_TO(n)			(0x038 + 4 * (n)) /* 8 module timeouts */

/* The firmware programs the seven module timeout windows 0x3c..0x54 with one
 * common value and never touches VDM_RPT_CNT / VDM_TO(0).
 */
#define VDM_TIMEOUT_MOD			0x00300c03
#define VDM_TIMEOUT_HEVC			0x00300003

#define VDM_PART_DEC_OVER_LEVEL		0x05c
#define VDM_YSTADDR_1D			0x060	/* current output frame */
#define VDM_YSTRIDE_1D			0x064
#define VDM_UVOFFSET_1D			0x068
#define VDM_HEAD_INF_OFFSET		0x06c
#define VDM_PRCMEM1			0x070
#define VDM_PRCMEM2			0x074	/* line number table */
#define VDM_PPFD_BUF_ADDR		0x080
#define VDM_PPFD_BUF_LEN		0x084
#define VDM_PRC_CACHE_TYPE		0x098	/* HEVC: the vendor programs 2 there */
#define VDM_DNR_MBINFO_STADDR		0x090
#define VDM_REF_PIC_TYPE		0x094	/* 16 x 2-bit frame types */
#define VDM_FF_APT_EN			0x098

/* Per-picture performance counters, updated by the VDM at frame end. */
#define VDM_DEC_CYCLE_PER_PIC		0x0b0
#define VDM_RD_BDWIDTH_PER_PIC		0x0b4
#define VDM_WR_BDWIDTH_PER_PIC		0x0b8
#define VDM_RD_REQ_PER_PIC		0x0bc
#define VDM_WR_REQ_PER_PIC		0x0c0
#define VDM_MB0_QP_IN_CURR_PIC		0x0d0
#define VDM_SWITCH_ROUNDING		0x0d4
#define VDM_AXI_TEST_ST			0x0e0
#define VDM_AXI_TEST_STA		0x0f0

/*
 * Syntax error detection status. These live past the first 4 KiB of the block
 * and are where the vendor firmware reads its syntax-error record pair from
 * (the non-low-delay path looks only at 0x1000 and 0x1014). The block runs to
 * the end of the mapped window, and the firmware's low-delay path walks it as
 * 16-byte per-slice records (status at +0, record end at +8, slice index
 * 1..200), so the dump covers all of it.
 */
#define VDM_SED_STA			0x1000
#define VDM_SED_END0			0x1014
#define VDM_SED_LAST			0x10f0

/*
 * VDEC global registers inside the SCD block at 0xF8C3C000. The decoder data
 * path does not use the SCD, but the firmware pokes these three registers on
 * every VDM start; we keep the same behaviour.
 */
#define SCD_AVS_FLAG			0x000
#define SCD_EMAR_ID			0x004
/*
 * VDH select/reset. The vendor firmware's decoder start path writes 1 here
 * before every picture; no live path in this driver touches the offset, so it
 * is only ever reachable through the MPEG-2 A/B instrument.
 */
#define SCD_VDH_SELRST			0x008
#define SCD_VDH_CK_GT			0x00c

/* CRG window offsets: gate/reset/clk_sel of the VDH and its ack window. The
 * soft-reset request is bit 5 of CRG30 and the reset acknowledge is bit 1 of
 * CRG93, matching the CRG register descriptions the vendor firmware programs
 * against.
 */
#define VDH_CRG30				0x078
#define VDH_CRG93				0x174

/* Per-protocol value written to SCD_VDH_CK_GT before each VDM start. */
#define VDH_CK_GT_HEVC			0x0
#define VDH_CK_GT_OTHER			0x3

/*
 * Firmware channel type codes (dispatch table); the VDM_CFG1_VID_STD register
 * field carries its own per-protocol codes, which each codec source defines
 * separately (they disagree for HEVC and VP8).
 */
enum histb_vdec_vidstd {
	HISTB_VIDSTD_H264	= 0,
	HISTB_VIDSTD_VC1	= 1,
	HISTB_VIDSTD_MPEG4	= 2,
	HISTB_VIDSTD_MPEG2	= 3,
	HISTB_VIDSTD_H263	= 4,
	HISTB_VIDSTD_DIVX3	= 5,
	HISTB_VIDSTD_AVS	= 6,
	HISTB_VIDSTD_JPEG	= 7,
	HISTB_VIDSTD_REAL8	= 8,
	HISTB_VIDSTD_REAL9	= 9,
	HISTB_VIDSTD_VP6F	= 10,
	HISTB_VIDSTD_VP6	= 11,
	HISTB_VIDSTD_VP6A	= 12,
	HISTB_VIDSTD_VP8	= 13,
	HISTB_VIDSTD_HEVC	= 17,
};

#endif /* HISTB_VDEC_REGS_H */
