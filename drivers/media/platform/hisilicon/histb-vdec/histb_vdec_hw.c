// SPDX-License-Identifier: GPL-2.0-only
/*
 * VDM hardware sequencing for the HiSilicon Hi3798MV100 video decoder.
 *
 * The VDM is started by a doorbell register and reports completion through a
 * single level-triggered interrupt; the message pool it reads and writes is
 * a plain DMA region prepared by the codec layers.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/reset.h>

#include "histb_vdec.h"
#include "histb_vdec_tables_h264.h"
#include "histb_vdec_tables_hevc.h"

/**
 * histb_vdec_hw_init() - prepare the VDM register interface
 * @dev: decoder device
 *
 * Masks every interrupt source and drops anything latched across the reset.
 * Runs once at probe, after the reset is released and before the interrupt
 * handler is registered; the per-frame start path unmasks the frame-done
 * source again.
 *
 * Return: 0 on success, a negative error code otherwise.
 */
int histb_vdec_hw_init(struct histb_vdec_dev *dev)
{
	vdm_write(dev, VDM_INT_MASK, 0xffffffff);
	vdm_write(dev, VDM_INT_STATE, 0xffffffff);

	return 0;
}

/**
 * histb_vdec_hw_reset() - pulse the VDM main FSM reset
 * @dev: decoder device
 *
 * Asserts the decoder reset line for a short pulse and re-applies the
 * all-masked interrupt state, which the reset clears. Used to recover from
 * a wedged frame.
 */
void histb_vdec_hw_reset(struct histb_vdec_dev *dev)
{
	reset_control_assert(dev->rst);
	usleep_range(10, 20);
	reset_control_deassert(dev->rst);

	vdm_write(dev, VDM_INT_MASK, 0xffffffff);
	vdm_write(dev, VDM_INT_STATE, 0xffffffff);
}

/**
 * histb_vdec_hw_kick() - start decoding of the programmed frame
 * @dev: decoder device
 * @std: protocol of the frame about to be decoded
 *
 * Selects the protocol's clock gating mode in the SCD block's global
 * register window (0 for HEVC, 3 for every other protocol) and rings the
 * VDM doorbell with the 0-1-0 write sequence the start logic expects.
 */
void histb_vdec_hw_kick(struct histb_vdec_dev *dev, enum histb_vdec_vidstd std)
{
	writel(std == HISTB_VIDSTD_HEVC ? VDH_CK_GT_HEVC : VDH_CK_GT_OTHER,
	       dev->scd + SCD_VDH_CK_GT);

	/* Drain the message pool writes before ringing the doorbell. */
	wmb();

	vdm_write(dev, VDM_START, 0);
	vdm_write(dev, VDM_START, 1);
	vdm_write(dev, VDM_START, 0);
}

/**
 * histb_vdec_hw_init_tables() - program the constant protocol tables
 * @dev: decoder device
 *
 * Copies the CABAC M/N tables of H.264 and HEVC into the table area of the
 * message pool, from where the VDM fetches them by DMA. The VDM expects
 * them at fixed offsets once per probe; they are read-only afterwards.
 */
void histb_vdec_hw_init_tables(struct histb_vdec_dev *dev)
{
	void *tables = histb_vdec_tables_base(&dev->pool);
	unsigned int i;

	BUILD_BUG_ON(HISTB_TABLE_OFF_CABAC_MN_H264 +
		     HISTB_VDEC_CABAC_MN_H264_SIZE >
		     HISTB_TABLE_OFF_CABAC_MN_HEVC);
	BUILD_BUG_ON(HISTB_TABLE_OFF_CABAC_MN_HEVC +
		     HISTB_VDEC_CABAC_MN_HEVC_SIZE >
		     HISTB_TABLE_OFF_TILE_INFO);
	BUILD_BUG_ON(HISTB_TABLE_OFF_TILE_INFO + HISTB_VDEC_TILE_INFO_SIZE >
		     HISTB_TABLE_OFF_PPFD);
	BUILD_BUG_ON(HISTB_TABLE_OFF_PPFD + HISTB_VDEC_PPFD_SIZE >
		     HISTB_TABLE_OFF_SCRATCH_BASE);

	memcpy(tables + HISTB_TABLE_OFF_CABAC_MN_H264,
	       histb_vdec_cabac_mn_h264, HISTB_VDEC_CABAC_MN_H264_SIZE);

	/*
	 * The HEVC table is stored as readable (m, n) pairs; the vendor
	 * loader finishes with a byte-swap pass that interleaves every pair
	 * as (n, m) in memory, which is what the hardware fetcher expects.
	 * The H.264 loader does not swap.
	 */
	for (i = 0; i < HISTB_VDEC_CABAC_MN_HEVC_SIZE / 4; i++) {
		u32 v = histb_vdec_cabac_mn_hevc[i];

		v = ((v & 0xff00ff00) >> 8) | ((v & 0x00ff00ff) << 8);
		((u32 *)tables)[HISTB_TABLE_OFF_CABAC_MN_HEVC / 4 + i] = v;
	}

	/*
	 * Every HEVC picture message points at the tile info image, but untiled
	 * pictures decode with it all zero and the HEVC run path refuses tiled
	 * ones, so it is programmed once here rather than per frame.
	 */
	memset(tables + HISTB_TABLE_OFF_TILE_INFO, 0,
	       HISTB_VDEC_TILE_INFO_SIZE);
}

