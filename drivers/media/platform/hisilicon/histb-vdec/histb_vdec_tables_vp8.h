/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VP8 hardware probability-table layout for the Hi3798MV100 VDM.
 *
 * The VDM consumes a 2752-byte probability image through the picture message
 * table-address slot (D39). Unlike the H.264/HEVC CABAC tables there is NO
 * constant image to mirror: the vendor firmware only declares an array of this
 * size in its decoder parameter block and builds it at run time from the
 * frame's current probabilities, in its own syntax-parsing layer. This header
 * therefore documents the reconstructed image layout; histb_vdec_vp8.c packs it
 * from V4L2_CID_STATELESS_VP8_FRAME.
 *
 * Reconstructed layout, transcribed from the store patterns of the vendor
 * firmware's message-assembly code. The firmware hands this whole region to the
 * engine as one block, so the layout is the firmware's own and not a documented
 * hardware format:
 *
 *   [8]         prob_skip_false
 *   [9..14]     mb_segment_tree_probs[3], prob_intra, prob_last, prob_gf,
 *               stored as one run from a pointer to the [8] byte
 *   [24..27]    y_mode_probs[4] and [28..30] uv_mode_probs[3], written
 *               after a 16-byte zeroing of that block
 *   [712..775]  mv_probs[2][19] in four 16-byte chunks, the 19-entry array
 *               split {1,0,2..8} then {1,9..18}
 *   [776..2311] coefficient probabilities, 16 bytes per group of 11
 *               probabilities plus 5 zero bytes, group order
 *               [plane][band][context]; the firmware's loop covers 1536
 *               bytes from 776, which is exactly the four VP8 plane types
 *               times 24 groups
 *
 * What is not covered by the frame control - the bytes between the groups and
 * beyond 2311 up to the 2752-byte upload - has no identified source in the
 * vendor tables, so this driver leaves those bytes zero.
 */
#ifndef HISTB_VDEC_TABLES_VP8_H
#define HISTB_VDEC_TABLES_VP8_H

#define HISTB_VDEC_VP8_TAB_SIZE		2752

/*
 * Offset of the VP8 table inside the per-device pool table region, after the
 * H.264/HEVC CABAC tables, the tile info table and the PPFD buffer.
 */
#define HISTB_TABLE_OFF_VP8_TAB		(8192 + 210944)

/*
 * Probability-image regions inside the 2752-byte table. The vendor firmware's
 * syntax-parsing code addresses these regions relative to its decoder parameter
 * block, while the image it uploads to the engine starts 8 bytes into that
 * block (its message-assembly code copies the whole 2752 bytes from there).
 * Every region therefore sits 8 bytes below the offset the firmware's stores
 * use: prob_skip_false lands on table[0].
 */
#define HISTB_VP8_TAB_MODE2_OFF		0	/* skip, seg[3], intra/last/gf */
#define HISTB_VP8_TAB_MODE_OFF		16	/* y_mode[4] then uv_mode[3] */
#define HISTB_VP8_TAB_MV_OFF		704	/* two planes, 32 bytes each */
#define HISTB_VP8_TAB_COEF_OFF		768	/* 4 planes * 24 groups * 16 */
#define HISTB_VP8_TAB_COEF_STRIDE	16
#define HISTB_VP8_TAB_COEF_PROBS	11
#define HISTB_VP8_TAB_COEF_GROUPS	24	/* bands * contexts per plane */
#define HISTB_VP8_TAB_COEF_PLANES	4	/* real VP8 plane types */

#endif /* HISTB_VDEC_TABLES_VP8_H */
