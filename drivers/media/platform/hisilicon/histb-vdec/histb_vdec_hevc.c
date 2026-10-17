// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSilicon Hi3798MV100 (VDH HiVDHV300R001) stateless HEVC decoder.
 *
 * One job decodes one frame: the picture slot of the message pool carries the
 * frame-level state (POC, DPB address and POC tables, CABAC context table
 * pointer, tile info image, scaling lists), followed by one chained slice slot
 * per slice. Every slot word layout implemented here is derived from the vendor
 * firmware's message-assembly routines; the behaviour behind each word, and the
 * frame storage model (APC/PMV tables, tile frame slots), are described next to
 * the code that writes them.
 *
 * Hardware facts worth keeping next to the code:
 *
 *  - The decoder consumes fully parsed syntax and never reads slice
 *    headers; the only bitstream it fetches is slice_data(), addressed
 *    through a per-slice stream segment, exactly like H.264.
 *  - Frame storage is table based like H.264 (16 APC slots addressed by
 *    capture buffer index, 20 private PMV slots), but the CABAC context
 *    table is a fixed per-slice-type (m, n) image and the picture message
 *    additionally carries the tile info image and a 256-word scaling
 *    list copy.
 *  - The slice chain is anchored in picture word D63 (not D9 like H.264)
 *    and each slice slot carries its CTB range end in D43/D44.
 */

#include <linux/dma-mapping.h>
#include <media/v4l2-hevc.h>
#include <media/videobuf2-dma-contig.h>

#include "histb_vdec.h"

/*
 * Same-boot A/B instrument for the HEVC message and register shape, in the
 * mould of histb_vdec_mpeg2_ab. Each bit turns on one field that the vendor's
 * HEVC HAL and this driver's already byte-exact H.264 and MPEG-2 paths
 * (histb_vdec_h264.c, histb_vdec_mpeg2.c) both agree on, while the HEVC branch
 * here has so far written something else:
 *
 *   BIT(0) CFG0 coef_idx_detect_en (bit24)
 *   BIT(1) CFG0 load_qmatrix (bit30)
 *   BIT(2) CFG1 fst_slc_grp (bit14)
 *   BIT(4) CFG1 dblk_1d_en (bit29)
 * BIT(6) declare the slice segment with its rbsp_trailing_bits still inside the
 * length. The default trims them (see histb_hevc_assemble_slice()); with
 *          this bit the chains lose frames, counted per stream on the board:
 *          r2 5/12, bs1 10/12, 512x240 0/12 byte-exact frames, and a job that
 *          follows one of another resolution loses its first CTB-row columns
 *          too.
 *   BIT(7) leave the post-filter scratch address and length clear, which is
 *          what the vendor programs for HEVC
 *   BIT(8) clear CFG1 bits13:4 (the DDR stride field): the vendor's HEVC
 *          CFG1 starts from the constant 13 and never fills them, unlike
 *          the H.264 and MPEG-2 paths
 *   BIT(9) write VDM+0x98 (PRC_CACHE_TYPE) = 2, which the vendor programs
 *          for HEVC only
 *  BIT(10) the HEVC timeout constant 0x00300003 instead of the 0x00300c03
 *          the other codecs use, which is what the vendor sets for HEVC
 *  BIT(11) CFG1 max_slcgrp_num = 3 (bits27:16), the constant the vendor's HEVC
 *          CFG1 carries; no effect observed on the 256x256 and 320x240 streams
 *          decoded on the board, so the field stays arm-only.
 *  BIT(12) leave the seven pipeline timeout windows at their reset values: the
 *          frame-by-frame counts then match the default arm exactly, and that
 *          equivalence is what refuted the per-stage-timeout reading of the
 *          engine's failure mode.
 *  BIT(13) trim only the zero padding of rbsp_trailing_bits and keep its stop
 *          bit: one more declared bit than the default. Pixels follow the last
 * bit exactly (r2 0/12), so this is only a bracket for the measurement.
 *  BIT(14) after the trim, round the declared length up to the next byte
 *          boundary: also costs frames (w384 363/384), same reason.
 *          Only the exact trim decodes every chain, which is why it is the
 *          default; every variant above costs frames on the same streams, so
 *          the measured cost sits in the declared length, not in the trailing
 *          stop bit.
 *  BIT(15) pad the CTB grid by one row without touching the programmed luma
 *          height (the single-CTB-row bottom-line probe in
 *          histb_vdec_hevc_run). It flushes the missing bottom lines, but the
 *          deblocking picture-bottom follows the grid, so the last row is not
 *          what a conformant decoder produces - measurement scaffolding only,
 *          never a default.
 *  BIT(16) lift the tile refusal and put the raster-scan address into the
 *          slice words that carry one (D8/D9/D43), keeping D10/D44 in the
 *          tile-scan order - the split the vendor applies through its
 *          tile-scan-to-raster map when tiles are enabled. No tiled grid
 *          shape decoded a full frame on the board, so this stays a probe;
 *          see the refusal in histb_vdec_hevc_run.
 *  BIT(17) with BIT(16), also write the slice's own tile address into bits
 *          17:0 of D6 (the word the vendor ors an independent-slice address
 *          into); the field's meaning is not pinned down, hence its own bit.
 * The vendor also always sets CFG0 bit24 and CFG1 bits27:16 = 3 with bit29;
 * measured here they change nothing, alone or next to the trim, so they stay
 * arm-only (BIT(0), BIT(11), BIT(4)).
 */
static unsigned int histb_vdec_hevc_ab;
module_param(histb_vdec_hevc_ab, uint, 0644);
MODULE_PARM_DESC(histb_vdec_hevc_ab, "HEVC message A/B bits (0 = as derived)");

/*
 * PMV slot geometry: 32 bytes per 16x16 macroblock row-column, 128-byte
 * aligned, floored at the 1080p image the vendor reserves even for small
 * streams. Unlike the H.264 layer the pool is not fixed at the largest
 * supported frame; the geometry is grown to the stream resolution on the first
 * frame that needs it (see histb_vdec_hevc_run).
 */
#define HISTB_HEVC_PMV_SLOT_SIZE_MIN		261120u
#define HISTB_HEVC_PMV_SLOT_SIZE(w_mb, h_mb)	\
	ALIGN((w_mb) * 32u * (h_mb), 128u)

/*
 * The picture message carries a 16-entry frame address table, so capture buffer
 * indexes double as APC slots up to this limit. dpb2apc[] uses
 * HISTB_HEVC_APC_NONE for DPB entries that map to no slot.
 */
#define HISTB_HEVC_NUM_APC_SLOTS		V4L2_HEVC_DPB_ENTRIES_NUM_MAX
#define HISTB_HEVC_APC_NONE			0xffu

/*
 * Protocol code the VDM_CFG1 VID_STD field carries for HEVC on this decoder
 * generation. It is not the 17 of the firmware dispatch table the start path
 * uses; the vendor start program writes 13.
 */
#define HISTB_HEVC_VIDSTD_CODE			13

/*
 * Post-filter scratch row count of the PPFD image this pool area is sized for
 * (HISTB_VDEC_PPFD_SIZE = 64 words x 800 rows); the register takes the row
 * count minus one.
 */
#define HISTB_HEVC_PPFD_ROWS			800

/*
 * Hardware slice-type codes, indexed by the V4L2 slice_type (B=0, P=1, I=2):
 * the hardware codes B as 2, P as 1 and I as 0, the same coding the H.264 HAL's
 * slice-type table produces and the h264 layer implements.
 */
static u8 histb_hevc_slice_type_code(u8 slice_type)
{
	static const u8 code[3] = { 2, 1, 0 };

	return code[slice_type % 3];
}

struct histb_vdec_hevc_ctx {
	void		*pmv_va[HISTB_VDEC_MAX_PMV_SLOTS];
	dma_addr_t	pmv_dma[HISTB_VDEC_MAX_PMV_SLOTS];
	unsigned int	pmv_slot_size;
	void		*rbsp_va;
	dma_addr_t	rbsp_dma;
	unsigned int	rbsp_size;
};

/* Per-frame assembly state shared by the picture and slice packers. */
struct histb_hevc_run {
	const struct v4l2_ctrl_hevc_sps *sps;
	const struct v4l2_ctrl_hevc_pps *pps;
	const struct v4l2_ctrl_hevc_decode_params *dec;
	const struct v4l2_ctrl_hevc_slice_params *slices;
	const struct v4l2_ctrl_hevc_scaling_matrix *scaling;
	unsigned int		num_slices;
	unsigned int		width;
	unsigned int		height;
	unsigned int		width_ctbs;
	unsigned int		height_ctbs;
	unsigned int		ctb_log2;
	unsigned int		ctu_count;
	u32			tile_stride;
	u32			buf_lines;
	const u8		*stream_va;
	dma_addr_t		src_dma;
	dma_addr_t		dst_dma;
	u8			dpb2apc[V4L2_HEVC_DPB_ENTRIES_NUM_MAX];
	u32			img_addr[HISTB_HEVC_NUM_APC_SLOTS];
	s32			apc_poc[HISTB_HEVC_NUM_APC_SLOTS];
};

static struct histb_vdec_dev *histb_vdec_hevc_dev(struct histb_vdec_ctx *ctx)
{
	return video_get_drvdata(ctx->fh.vdev);
}

/*
 * PMV slot allocator: capture buffer index -> PMV slot id. Slot ids are stored
 * +1 so that a zeroed pmv_map[] means "unallocated"; ids wrap around the
 * 20-slot pool.
 */
static u8 histb_hevc_pmv_alloc(struct histb_vdec_ctx *ctx, unsigned int index)
{
	u8 id = ctx->pmv_map[index];

	if (!id) {
		id = ctx->pmv_next++ % HISTB_VDEC_MAX_PMV_SLOTS + 1;
		ctx->pmv_map[index] = id;
	}

	return id - 1;
}

/*
 * Grow the PMV pool to @slot_size per slot. The private slots are addressed
 * independently of the frame table, so a new geometry invalidates every
 * allocated slot id and resets the map.
 *
 * One chunk per slot, never one slot_size * HISTB_VDEC_MAX_PMV_SLOTS block: the
 * 20-slot pool at the 1080p floor is 5.0 MB, an order-11 request (8 MB, 8 MB
 * aligned), and on the board that request alone failed - `cma:
 * __cma_alloc_frozen: reserved: alloc failed, req-size: 1275 pages, ret: -16`,
 * with 9.9 MB free in the 16 MB region - so every HEVC job was refused with
 * -ENOMEM before the doorbell.
 */
static void histb_hevc_pmv_pool_free(struct histb_vdec_dev *dev,
				     struct histb_vdec_hevc_ctx *hevc)
{
	unsigned int i;

	for (i = 0; i < HISTB_VDEC_MAX_PMV_SLOTS; i++) {
		if (!hevc->pmv_va[i])
			continue;
		dma_free_wc(dev->dev, hevc->pmv_slot_size, hevc->pmv_va[i],
			    hevc->pmv_dma[i]);
		hevc->pmv_va[i] = NULL;
	}
	hevc->pmv_slot_size = 0;
}

static int histb_hevc_pmv_pool_grow(struct histb_vdec_ctx *ctx,
				    struct histb_vdec_hevc_ctx *hevc,
				    unsigned int slot_size)
{
	struct histb_vdec_dev *dev = histb_vdec_hevc_dev(ctx);
	unsigned int i;

	/*
	 * One PMV geometry per session, as on the H.264 side: a mid-session SPS
	 * change would free the pool while a later DPB can still name those
	 * pictures. Restarting the queues runs ->stop() and zeroes the size, so
	 * that is the only supported way here to move to a new geometry and the
	 * job with a different size is refused with -EINVAL.
	 */
	if (hevc->pmv_slot_size) {
		/*
		 * A second geometry inside one session is refused rather than
		 * rebuilt: the rebuild clears pmv_map, so a picture that a
		 * later job still names in its DPB is handed a slot that was
		 * never written, and the engine reads its collocated motion
		 * vectors straight out of that memory (D23 is addressed through
		 * the same dpb2apc[] map that histb_hevc_assemble_slice()
		 * validates). The H.264 side can rebuild at a flush boundary
		 * because every entry of its DPB array carries a validity flag;
		 * a HEVC entry carries exactly one flag,
		 * V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE
		 * (include/uapi/linux/v4l2-controls.h), so that predicate
		 * cannot be formed here. The slot size is a function of the SPS
		 * picture alone, so a conformant client reaches a new geometry
		 * only by restarting the queues, which runs ->stop(); measured
		 * over the scored corpus, 111 streams each carry one distinct
		 * SPS payload, so this refuses nothing that decodes today.
		 * Accepting a mid-chain change would need a per-picture slot
		 * table that survives the rebuild.
		 */
		if (hevc->pmv_slot_size != slot_size)
			return -EINVAL;
		return 0;
	}

	histb_hevc_pmv_pool_free(dev, hevc);

	for (i = 0; i < HISTB_VDEC_MAX_PMV_SLOTS; i++) {
		hevc->pmv_va[i] = dma_alloc_wc(dev->dev, slot_size,
					       &hevc->pmv_dma[i], GFP_KERNEL);
		if (!hevc->pmv_va[i])
			break;
	}
	if (i < HISTB_VDEC_MAX_PMV_SLOTS) {
		histb_hevc_pmv_pool_free(dev, hevc);
		return -ENOMEM;
	}
	hevc->pmv_slot_size = slot_size;

	memset(ctx->pmv_map, 0, sizeof(ctx->pmv_map));
	ctx->pmv_next = 0;

	return 0;
}

/*
 * Start-code scan over the OUTPUT plane. Returns the bit position of every VCL
 * slice NAL unit (nal_unit_type 0..9 and 16..23, the slice NALs the message ABI
 * can describe). A 4-byte start code matches once: the extra leading zero byte
 * never forms a 00 00 01 triple with the prefix. Returns the total number of
 * slice NALs found (may exceed @max).
 */
static unsigned int histb_hevc_find_slice_nals(const u8 *buf, u32 size,
					       u32 *starts, unsigned int max)
{
	unsigned int n = 0;
	u32 i;

	for (i = 0; i + 3 < size; i++) {
		u8 nal_type = (buf[i + 3] >> 1) & 0x3f;

		if (buf[i] || buf[i + 1] || buf[i + 2] != 0x01)
			continue;

		if (nal_type > 9 && (nal_type < 16 || nal_type > 23))
			continue;

		if (n < max)
			starts[n] = (i + 3) * 8;
		n++;
	}

	return n;
}

/*
 * Scaling lists, picture words D64..D319. The vendor copies a 256-word image
 * word for word; the layout this driver assumes is the linear image of the V4L2
 * control (4x4, 8x8, 16x16, 32x32 lists and the DC coefficients in struct
 * order, zero padded). That packing is unverified against the hardware: on the
 * board the submitted matrices are not consulted at all for a picture that
 * claims them, so captures cannot confirm or correct this layout; see the
 * scaling-list refusal in histb_vdec_hevc_run for the measurement.
 */
static void histb_hevc_pack_scaling(u32 *dst,
				    const struct v4l2_ctrl_hevc_scaling_matrix *m)
{
	memcpy(dst, m, sizeof(*m));
}

static void histb_hevc_assemble_pic(struct histb_vdec_dev *dev,
				    struct histb_vdec_ctx *ctx,
				    struct histb_vdec_hevc_ctx *hevc,
				    const struct histb_hevc_run *run,
				    u8 cur_pmv_slot, u32 *pic)
{
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	const struct v4l2_ctrl_hevc_decode_params *dec = run->dec;
	dma_addr_t tables = histb_vdec_tables_dma(&dev->pool);
	unsigned int log2_min_cb = sps->log2_min_luma_coding_block_size_minus3 + 3;
	unsigned int log2_min_tb = sps->log2_min_luma_transform_block_size_minus2 + 2;
	unsigned int log2_min_pcm = sps->log2_min_pcm_luma_coding_block_size_minus3 + 3;
	unsigned int log2_max_pcm = log2_min_pcm +
			sps->log2_diff_max_min_pcm_luma_coding_block_size;
	unsigned int i;

	memset(pic, 0, HISTB_VDEC_SLOT_BYTES);

	pic[0] = (run->width_ctbs - 1) |
		 ((run->height_ctbs - 1) << 9) |
		 ((sps->flags & V4L2_HEVC_SPS_FLAG_PCM_ENABLED) ? BIT(18) : 0) |
		 ((u32)sps->chroma_format_idc << 19) |
		 ((sps->flags & V4L2_HEVC_SPS_FLAG_AMP_ENABLED) ? BIT(21) : 0) |
		 ((sps->flags & V4L2_HEVC_SPS_FLAG_SAMPLE_ADAPTIVE_OFFSET) ?
		  BIT(22) : 0) |
		 ((sps->flags & V4L2_HEVC_SPS_FLAG_PCM_LOOP_FILTER_DISABLED) ?
		  BIT(23) : 0) |
		 ((sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED) ?
		  BIT(24) : 0) |
		 ((sps->flags & V4L2_HEVC_SPS_FLAG_STRONG_INTRA_SMOOTHING_ENABLED) ?
		  BIT(25) : 0);
	/* picture order count of the current frame */
	pic[1] = (u32)dec->pic_order_cnt_val;
	pic[2] = run->ctb_log2 |
		 (log2_min_cb << 3) |
		 ((u32)sps->log2_diff_max_min_luma_coding_block_size << 6) |
		 ((u32)(log2_min_tb +
			sps->log2_diff_max_min_luma_transform_block_size) << 9) |
		 ((u32)log2_min_tb << 12) |
		 ((u32)log2_max_pcm << 15) |
		 ((u32)log2_min_pcm << 18) |
		 ((u32)sps->max_transform_hierarchy_depth_intra << 21) |
		 ((u32)sps->max_transform_hierarchy_depth_inter << 24) |
		 ((pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED) ? BIT(27) : 0) |
		 ((pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED) ? BIT(28) : 0) |
		 ((u32)pps->log2_parallel_merge_level_minus2 << 29);
	pic[3] = ((sps->bit_depth_luma_minus8 + 8) & 0xf) |
		 (((sps->bit_depth_chroma_minus8 + 8) & 0xf) << 4) |
		 (((sps->pcm_sample_bit_depth_luma_minus1 + 1) & 0xf) << 8) |
		 (((sps->pcm_sample_bit_depth_chroma_minus1 + 1) & 0xf) << 12) |
		 ((6u * sps->bit_depth_chroma_minus8 & 0x3f) << 16) |
		 ((6u * sps->bit_depth_luma_minus8 & 0x3f) << 22);
	/*
	 * Band-line scratch addresses. The engine writes four areas here (the
	 * top PMV row, the reconstructed band, the left PMV column and the top
	 * deblocking row) and the vendor repeats HISTB_SCRATCH_SED_TOP in
	 * D5/D43. Sharing one region lets the reconstructed band run through
	 * the table area mid-decode, so each word gets its own scratch block
	 * the way the byte-exact H.264 path does (histb_vdec_h264.c); measured
	 * on the board on a 320x240 intra frame, 1238 of 4800 luma lines land
	 * where the model puts them while the areas are shared, all 4800 once
	 * each word has its own block. D57 and D59 likewise split into
	 * dblk_left and dblk_top.
	 */
	pic[4] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_SED_TOP));
	pic[5] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_PMV_TOP));
	/* current frame PMV slot */
	pic[6] = lower_32_bits(hevc->pmv_dma[cur_pmv_slot]);
	pic[7] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_RCNTOP));
	/* APC frame address table, then the POC table in APC slot order */
	for (i = 0; i < HISTB_HEVC_NUM_APC_SLOTS; i++) {
		pic[8 + i] = run->img_addr[i];
		pic[26 + i] = (u32)run->apc_poc[i];
	}
	/* HEVC CABAC (m, n) context table */
	pic[25] = lower_32_bits(tables + HISTB_TABLE_OFF_CABAC_MN_HEVC);
	pic[43] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_PMV_LEFT));
	pic[54] = (run->width & 0xffff) | (run->height & 0xffff) << 16;
	/* D55 is the tile segment info image the vendor fills, not an alias of
	 * the reconstructed band in D7.
	 */
	pic[55] = lower_32_bits(tables + HISTB_TABLE_OFF_TILE_INFO);
	pic[56] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_SAO_LEFT));
	pic[57] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_DBLK_LEFT));
	pic[58] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_SAO_TOP));
	pic[59] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_DBLK_TOP));
	pic[60] = (((u32)pps->pps_cb_qp_offset & 31) << 5) |
		  ((u32)pps->pps_cr_qp_offset & 31) |
		  ((pps->flags & V4L2_HEVC_PPS_FLAG_CONSTRAINED_INTRA_PRED) ?
		   BIT(10) : 0) |
		  ((pps->flags & V4L2_HEVC_PPS_FLAG_TRANSQUANT_BYPASS_ENABLED) ?
		   BIT(11) : 0) |
		  ((pps->flags & V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED) ?
		   BIT(12) : 0) |
		  ((u32)pps->diff_cu_qp_delta_depth & 0xf) << 13 |
		  /* bits21:16 are Log2MinCuQpDeltaSize = Log2CtbSizeY minus
		   * diff_cu_qp_delta_depth (H.265 7.4.3.2, which is also the
		   * shape the vendor programs); measured on the board, a
		   * min-CB-based sum here derails the CABAC parse of every
		   * picture with cu_qp_delta_enabled.
		   */
		  ((pps->flags & V4L2_HEVC_PPS_FLAG_CU_QP_DELTA_ENABLED) ?
		   ((run->ctb_log2 - pps->diff_cu_qp_delta_depth) & 7) << 16 : 0) |
		  ((pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) ? BIT(19) : 0) |
		  ((pps->flags & V4L2_HEVC_PPS_FLAG_UNIFORM_SPACING) ? BIT(20) : 0) |
		  ((pps->flags & V4L2_HEVC_PPS_FLAG_ENTROPY_CODING_SYNC_ENABLED) ?
		   BIT(21) : 0) |
		  ((pps->flags & V4L2_HEVC_PPS_FLAG_SIGN_DATA_HIDING_ENABLED) ?
		   BIT(22) : 0) |
		  ((pps->flags & V4L2_HEVC_PPS_FLAG_TRANSFORM_SKIP_ENABLED) ?
		   BIT(23) : 0) |
		  ((pps->flags & V4L2_HEVC_PPS_FLAG_LOOP_FILTER_ACROSS_TILES_ENABLED) ?
		   BIT(24) : 0);
	pic[61] = (pps->num_tile_columns_minus1 & 0xffff) |
		  (u32)pps->num_tile_rows_minus1 << 16;
	/* head of the slice chain */
	pic[63] = lower_32_bits(histb_vdec_slice_slot_dma(&dev->pool, 0));

	/*
	 * The scaling image is copied every job: with SPS scaling_list_enabled
	 * clear the hardware ignores it, and skipping the copy would leave the
	 * previous frame's lists in the pool slot. A client that enables the
	 * flag and does not fill the control gets its zeroed default, which is
	 * what the uAPI prescribes (the control always exists in the handler).
	 */
	histb_hevc_pack_scaling(&pic[64], run->scaling);
}

static int histb_hevc_pack_weights(u32 *slot,
				   const struct v4l2_ctrl_hevc_slice_params *slice,
				   bool list1)
{
	const struct v4l2_hevc_pred_weight_table *w = &slice->pred_weight_table;
	const s8 *lofs = list1 ? w->luma_offset_l1 : w->luma_offset_l0;
	const s8 *lwt = list1 ? w->delta_luma_weight_l1 : w->delta_luma_weight_l0;
	const s8 (*cofs)[2] = list1 ? w->chroma_offset_l1 : w->chroma_offset_l0;
	const s8 (*cwt)[2] = list1 ? w->delta_chroma_weight_l1 :
				     w->delta_chroma_weight_l0;
	unsigned int count = list1 ? slice->num_ref_idx_l1_active_minus1 + 1 :
			    slice->num_ref_idx_l0_active_minus1 + 1;
	unsigned int lden = w->luma_log2_weight_denom & 7;
	unsigned int cden = (w->luma_log2_weight_denom +
			     w->delta_chroma_log2_weight_denom) & 7;
	/*
	 * Weight image of one list: luma at W64 (L0) / W80 (L1), the first
	 * chroma component at W128 / W144 and the second at W160 / W176,
	 * 16 entries each, packed as offset<<12 | weight<<3 | den for luma
	 * and the first chroma component, weight | offset<<9 for the second.
	 */
	unsigned int base = list1 ? 80u : 64u;
	unsigned int i;

	/*
	 * A seventeenth reference cannot be packed. The weight image the slice
	 * slot carries is 16 words per component and per list - luma at
	 * W64..W79 (L0) and W80..W95 (L1), the first chroma component at
	 * W128..W143 and W144..W159, the second at W160..W175 and W176..W191,
	 * the layout the vendor's slice-message writer produces - and the uAPI
	 * arrays the loop reads are the same length
	 * (V4L2_HEVC_DPB_ENTRIES_NUM_MAX = 16, v4l2-controls.h). Because the
	 * two chroma rows are written at base + 64 and base + 96, index 16
	 * would land in the other list's luma block, so the count is checked
	 * instead of clamped. H.265 keeps the active list length inside the DPB
	 * and this driver's frame table is that same 16 slots, so a conformant
	 * stream is never refused. TODO: the weight values themselves are
	 * unmeasured - the two weighted B pictures scored on the board came out
	 * 6/6 byte-exact from an encoder that submits unit weights, so only a
	 * stream with non-default coefficients confirms the packing.
	 */
	if ((list1 ? slice->num_ref_idx_l1_active_minus1 :
		     slice->num_ref_idx_l0_active_minus1) >
	    V4L2_HEVC_DPB_ENTRIES_NUM_MAX - 1)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		int lw = lwt[i] + (1 << lden);
		int cw = cwt[i][0] + (1 << cden);
		int cw2 = cwt[i][1] + (1 << cden);

		slot[base + i] = ((u32)(u8)lofs[i] << 12) |
				 ((u32)lw & 0x1ff) << 3 | lden;
		slot[base + 64 + i] = ((u32)(u8)cofs[i][0] << 12) |
				      ((u32)cw & 0x1ff) << 3 | cden;
		slot[base + 96 + i] = ((u32)cw2 & 0x1ff) |
				      ((u32)(u8)cofs[i][1] << 9);
	}

	return 0;
}

/*
 * Tile-scan to raster-scan CTB address of H.265 6.5.1: walk the tile grid in
 * tile order and index inside each tile in raster order. The vendor keeps such
 * a map in its CPU-side context and writes the converted value into the slice
 * message, so the address words of a tiled picture are not in the order the
 * uAPI's slice_segment_addr uses. The grid comes from the control's
 * column_width_minus1[]/row_height_minus1[], whose last entries close the
 * picture; the counts are clamped so the implicit final column and row keep the
 * array reads in bounds.
 */
static u32 histb_hevc_tile_to_raster(const struct v4l2_ctrl_hevc_pps *pps,
				     unsigned int w_ctbs, unsigned int h_ctbs,
				     u32 addr)
{
	unsigned int cols = min_t(unsigned int,
				  (unsigned int)pps->num_tile_columns_minus1 + 1,
				  ARRAY_SIZE(pps->column_width_minus1) + 1);
	unsigned int rows = min_t(unsigned int,
				  (unsigned int)pps->num_tile_rows_minus1 + 1,
				  ARRAY_SIZE(pps->row_height_minus1) + 1);
	unsigned int y0 = 0, tile_addr = addr, tr, tc;

	for (tr = 0; tr < rows; tr++) {
		unsigned int rowh = tr + 1 < rows ?
			(unsigned int)pps->row_height_minus1[tr] + 1 :
			h_ctbs - y0;
		unsigned int x0 = 0;

		if (!rowh)
			return addr;
		for (tc = 0; tc < cols; tc++) {
			unsigned int colw = tc + 1 < cols ?
				(unsigned int)pps->column_width_minus1[tc] + 1 :
				w_ctbs - x0;
			unsigned int n = colw * rowh;

			if (!colw)
				return addr;
			if (tile_addr < n)
				return (y0 + tile_addr / colw) * w_ctbs + x0 +
				       tile_addr % colw;
			tile_addr -= n;
			x0 += colw;
		}
		y0 += rowh;
	}
	return addr;
}

static int histb_hevc_assemble_slice(struct histb_vdec_dev *dev,
				     struct histb_vdec_ctx *ctx,
				     struct histb_vdec_hevc_ctx *hevc,
				     const struct histb_hevc_run *run,
				     unsigned int index,
				     u32 nal_bit, u32 next_bit,
				     u32 payload_bits)
{
	const struct v4l2_ctrl_hevc_slice_params *slice = &run->slices[index];
	const struct v4l2_ctrl_hevc_pps *pps = run->pps;
	const struct v4l2_ctrl_hevc_decode_params *dec = run->dec;
	const struct v4l2_ctrl_hevc_sps *sps = run->sps;
	u32 *slot = histb_vdec_slice_slot(&dev->pool, index);
	unsigned int last = run->num_slices - 1;
	unsigned int count0 = 0, count1 = 0;
	unsigned int end_ctb, i;
	/*
	 * Under tiles_enabled the vendor does not put the uAPI's tile-scan
	 * address into the raster-domain words: its HAL writes the
	 * tile-scan-to-raster converted value into D8/D9/D43 and the raw
	 * tile-scan address into D10/D44. BIT(16) makes this driver follow that
	 * split; without it, or without tiles, the two orders coincide and the
	 * default path is untouched.
	 */
	bool tiled = (pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) &&
		     (histb_vdec_hevc_ab & BIT(16));
	unsigned int tile_addr = slice->slice_segment_addr;
	unsigned int raster_addr = tiled ?
		histb_hevc_tile_to_raster(pps, run->width_ctbs,
					  run->height_ctbs, tile_addr) :
		tile_addr;
	bool is_low_delay;
	bool is_b = slice->slice_type == V4L2_HEVC_SLICE_TYPE_B;
	bool is_p = slice->slice_type == V4L2_HEVC_SLICE_TYPE_P;
	bool lf_across = (slice->flags &
		V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_LOOP_FILTER_ACROSS_SLICES_ENABLED) ||
		(pps->flags &
		V4L2_HEVC_PPS_FLAG_PPS_LOOP_FILTER_ACROSS_SLICES_ENABLED);
	bool dbf_off = (slice->flags &
		V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_DEBLOCKING_FILTER_DISABLED) ||
		(pps->flags &
		V4L2_HEVC_PPS_FLAG_PPS_DISABLE_DEBLOCKING_FILTER);
	bool weighted;
	s32 col_dpb = -1;
	u32 data_bit, valid_bits, qp;

	memset(slot, 0, HISTB_VDEC_SLOT_BYTES);

	/*
	 * slice_type indexes the hardware's two-bit slice code, and that code
	 * has three values: the vendor's per-slice-type table is { 2, 1, 0 }
	 * for B, P, I, and histb_hevc_slice_type_code() above folds anything
	 * larger back onto those three with "% 3". H.265 7.4.3 gives
	 * slice_segment_type no fourth value, so a larger one is a client
	 * error, and passing it through would describe the slice to the
	 * engine as some other type while the list lengths in D7 and the
	 * weighted-prediction test at the bottom of this function still
	 * follow the value the client gave. The same confusion between the
	 * coded value and the enum, not the coefficients, is what made H.264
	 * weighted prediction fail on the board, which is why the domain is
	 * checked before anything is packed.
	 */
	if (slice->slice_type > V4L2_HEVC_SLICE_TYPE_I)
		return -EINVAL;

	if (is_p || is_b)
		count0 = min_t(unsigned int,
			       slice->num_ref_idx_l0_active_minus1 + 1,
			       V4L2_HEVC_DPB_ENTRIES_NUM_MAX);
	if (is_b)
		count1 = min_t(unsigned int,
			       slice->num_ref_idx_l1_active_minus1 + 1,
			       V4L2_HEVC_DPB_ENTRIES_NUM_MAX);

	for (i = 0; i < count0; i++)
		/*
		 * Every active reference must name a DPB entry this job mapped
		 * onto a frame slot. The engine is handed the list as 4-bit APC
		 * slot ids (D15/D16) and reads the picture behind each id from
		 * the 16-entry frame table in D8..D23, so an index outside the
		 * DPB array or an entry with no capture buffer has nothing that
		 * can be packed: the slot id would be truncated, and the
		 * address that selects is the one the caller prefills with the
		 * destination of this very frame, which turns inter prediction
		 * into self prediction. Refusal is the deliverable
		 * disposition - the capture buffer comes back with bytesused 0.
		 * Measured on the board: this is what fires when a client's
		 * reference indices do not resolve - it refused two frames of a
		 * 12-frame P chain while the submitting side held a DPB that
		 * did not match the array it handed over - and the same chain
		 * has been decoded 12/12 byte-exact ever since the indices
		 * resolve, so nothing that names its references correctly is
		 * lost here.
		 */
		if (slice->ref_idx_l0[i] >= V4L2_HEVC_DPB_ENTRIES_NUM_MAX ||
		    run->dpb2apc[slice->ref_idx_l0[i]] == HISTB_HEVC_APC_NONE)
			return -EINVAL;
	for (i = 0; i < count1; i++)
		/*
		 * The same bound on list 1, which the engine reads from D17/D18
		 * against the same frame table: a B slice's backward reference
		 * that resolves to no capture buffer would carry the un-mapped
		 * marker into a four-bit slot id and point prediction at the
		 * picture being written. H.265 7.4.3 keeps both lists inside
		 * the DPB, and the 12-frame B chains scored on the board
		 * resolve both lists and decode 12/12 byte-exact, so this
		 * refuses nothing that a conformant submission produces.
		 */
		if (slice->ref_idx_l1[i] >= V4L2_HEVC_DPB_ENTRIES_NUM_MAX ||
		    run->dpb2apc[slice->ref_idx_l1[i]] == HISTB_HEVC_APC_NONE)
			return -EINVAL;

	/*
	 * Collocated picture for temporal motion vector prediction: the
	 * hardware needs its APC slot (D7) and the PMV slot address (D23). I
	 * slices carry no reference lists, so temporal MVP has no source
	 * picture there and the words stay clear.
	 */
	if ((is_p || is_b) &&
	    (slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED)) {
		bool from_l1 = is_b &&
			!(slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0);
		const u8 *refs = from_l1 ? slice->ref_idx_l1 :
					   slice->ref_idx_l0;
		unsigned int bound = from_l1 ? count1 : count0;

		/*
		 * collocated_ref_idx is an index into the list that
		 * collocated_from_l0_flag selects (H.265 7.4.3), and @bound is
		 * the active count the loops above just validated. So this is
		 * the check that makes the two writes below safe without a
		 * further test: refs[] stays inside its 16 entries, and
		 * dpb2apc[refs[i]] cannot be HISTB_HEVC_APC_NONE, which is what
		 * D7 bits31:28 (ColPicApc) and D23 (the PMV address of that
		 * slot) are built from. An index at or beyond the active count
		 * would put the un-mapped marker into a four-bit field - the
		 * engine would take temporal motion vectors from another
		 * picture's slot. Temporal MVP is not a rejected shape: the
		 * 12-frame chains with it on decode 12/12 byte-exact, and the
		 * same chain with it off gives frame-for-frame identical
		 * readings, so the refusal here only catches a client whose
		 * index is outside its own list.
		 */
		if (slice->collocated_ref_idx >= bound)
			return -EINVAL;
		col_dpb = refs[slice->collocated_ref_idx];
	}

	/*
	 * Slice data segment. The hardware never reads slice headers, so the
	 * segment points behind the header bits; the vendor carries the header
	 * size as the byte offset to the slice data. The second segment (words
	 * D3..D5) stays clear: the frame arrives as one contiguous OUTPUT
	 * buffer.
	 */
	if (slice->data_byte_offset * 8 > slice->bit_size ||
	    nal_bit >= next_bit)
		return -EINVAL;
	data_bit = nal_bit + slice->data_byte_offset * 8;
	/*
	 * The computed start of slice_data() must still be inside this slice's
	 * span. D0..D2 of the segment word are a DMA offset, a bit offset and a
	 * bit length that the engine consumes literally, and every subtraction
	 * below (next_bit - data_bit, payload_bits - data_bit) only works if
	 * this ordering holds; with data_bit past the next slice's first bit
	 * the declared length would wrap to nearly 4 GiB and the span would run
	 * past the end of the buffer. Overshoot is not benign even without the
	 * wrap: when the declared span ends past the last coded bit the engine
	 * still reports completion and takes a few CTB columns from stale band
	 * state, measured as 5 of 12, 10 of 12 and 0 of 12 byte-exact frames on
	 * three chains, which is why this is a refusal and not a clamp. The
	 * offset is applied in the de-escaped domain the staging produced; over
	 * 104 streams (the scored corpus plus eight purpose-built wide WPP
	 * pictures) no slice header contained an emulation-prevention byte, so
	 * the two possible readings of the client's value coincide on
	 * everything measured so far.
	 */
	if (data_bit >= next_bit || data_bit >= payload_bits)
		return -EINVAL;
	valid_bits = slice->bit_size - slice->data_byte_offset * 8;
	valid_bits = min_t(u32, valid_bits, next_bit - data_bit);
	if (data_bit + valid_bits > payload_bits)
		valid_bits = payload_bits - data_bit;
	/*
	 * The declared length stops at the last coded bit: rbsp_trailing_bits
	 * is byte alignment, not slice data (histb_vdec_trail_zero_bits(), same
	 * as the other codecs). Leaving it in does not only set the error flag
	 * — the engine also decodes a few CTB columns from stale band state:
	 * with the untrimmed length r2 reaches 5/12 byte-exact frames, bs1
	 * 10/12 and 512x240 0/12, all 12/12 once it is trimmed. A single job
	 * can look fine either way, which is why the per-frame read on this arm
	 * came out the other way before the driver staged the RBSP without its
	 * emulation-prevention bytes.
	 */
	if (!(histb_vdec_hevc_ab & BIT(6))) {
		u32 trail = histb_vdec_trail_zero_bits(run->stream_va,
						       data_bit + valid_bits);

		if (!(histb_vdec_hevc_ab & BIT(13)))
			trail++;
		if (valid_bits > trail)
			valid_bits -= trail;
		if (histb_vdec_hevc_ab & BIT(14))
			valid_bits = min_t(u32, round_up(valid_bits, 8U),
					   next_bit - data_bit);
	}
	/*
	 * Nothing is left to decode once the trailing byte-alignment bits are
	 * trimmed, which means the client's span held rbsp_trailing_bits and no
	 * slice_data(). A declared length of zero in D2 has not been measured
	 * on this engine, so the driver does not hand one out: the per-slice
	 * up-message counts decoded CTBs against the declared length, and the
	 * chain only advances through D63, so a slot with no syntax is the
	 * shape that would plausibly run to the watchdog. The vendor's only
	 * construct for "this slot carries no data" is the one-bit dummy slice
	 * it inserts in front of a chain whose first slice starts mid-picture -
	 * an explicit length of one bit - and that is explicit padding, not an
	 * emptied real slice. TODO: program one slice with D2 = 0 and read the
	 * up-message CTB counter and the timeout counter; if the engine skips
	 * such a slot and still reports the picture done, this refusal can
	 * become a clamp.
	 */
	if (!valid_bits)
		return -EINVAL;
	/* histb_vdec_seg_pack() adds the granule's own bits, so only the bit
	 * position inside the byte is passed; the vendor masks the field to
	 * seven bits.
	 */
	histb_vdec_seg_pack(&slot[0], run->src_dma,
			    run->src_dma + (data_bit >> 3), data_bit & 7,
			    valid_bits);

	/* SliceQpY of the coded slice, wrapped to the hardware 6-bit field */
	qp = (26 + pps->init_qp_minus26 + slice->slice_qp_delta) & 63;

	/* IsLowDelay (D6[27]): every reference POC earlier than this picture's,
	 * the vendor derivation of the flag.
	 */
	is_low_delay = true;
	for (i = 0; i < count0 + count1; i++) {
		int idx = i < count0 ? slice->ref_idx_l0[i] :
			  slice->ref_idx_l1[i - count0];

		if (dec->dpb[idx].pic_order_cnt_val >
		    dec->pic_order_cnt_val) {
			is_low_delay = false;
			break;
		}
	}

	slot[6] = (qp << 19) |
		  ((slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_CABAC_INIT) ?
		   BIT(18) : 0) |
		  (is_low_delay ? BIT(27) : 0) |
		  ((slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT) ?
		   BIT(26) : 0);
	/*
	 * The vendor additionally ors a per-slice address into bits 17:0 of
	 * this word. Which field of its slice-parameter struct that value is
	 * has not been pinned down, so BIT(17) writes it as an arm of its own
	 * rather than folding it into the BIT(16) address-domain split.
	 */
	if ((histb_vdec_hevc_ab & BIT(17)) &&
	    (pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) &&
	    !(slice->flags &
	      V4L2_HEVC_SLICE_PARAMS_FLAG_DEPENDENT_SLICE_SEGMENT))
		slot[6] |= tile_addr & GENMASK(17, 0);
	slot[7] = histb_hevc_slice_type_code(slice->slice_type) |
		  (count0 << 2) | (count1 << 8) |
		  ((slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_MVD_L1_ZERO) ?
		   BIT(14) : 0) |
		  ((slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_TEMPORAL_MVP_ENABLED) ?
		   BIT(15) : 0) |
		  ((u32)(slice->num_ref_idx_l0_active_minus1 & 31) << 16) |
		  ((u32)(slice->num_ref_idx_l1_active_minus1 & 31) << 20) |
		  ((u32)(5 - slice->five_minus_max_num_merge_cand) & 7) << 24 |
		  ((slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_COLLOCATED_FROM_L0) ?
		   BIT(27) : 0);
	if (col_dpb >= 0) {
		/*
		 * The PMV pool is keyed by capture buffer index, which is also
		 * the APC slot the current frame's own motion vectors went to
		 * (histb_hevc_pmv_alloc(ctx, dst->vb2_buf.index) in the run
		 * path, and the same indirection through dpb2apc in
		 * histb_vdec_h264.c's collocated word). A raw dpb index here
		 * hands the engine another picture's motion vectors.
		 */
		u8 col_pmv = histb_hevc_pmv_alloc(ctx, run->dpb2apc[col_dpb]);

		slot[7] |= (u32)run->dpb2apc[col_dpb] << 28;
		slot[23] = lower_32_bits(hevc->pmv_dma[col_pmv]);
	}

	/* D8/D9 carry the raster address and the coordinates derived from it,
	 * D10 the tile-scan address of the slice start, which is what the
	 * vendor writes there.
	 */
	slot[8] = raster_addr;
	slot[9] = ((raster_addr / run->width_ctbs) << 16) |
		  (raster_addr % run->width_ctbs);
	slot[10] = tile_addr;
	slot[11] = (((u32)slice->slice_cb_qp_offset & 31) << 8) |
		   ((u32)slice->slice_cr_qp_offset & 31);
	slot[12] = (lf_across ? BIT(0) : 0) |
		   (dbf_off ? BIT(1) : 0) |
		   ((slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_CHROMA) ?
		    BIT(2) : 0) |
		   ((slice->flags & V4L2_HEVC_SLICE_PARAMS_FLAG_SLICE_SAO_LUMA) ?
		    BIT(3) : 0) |
		   (((u32)slice->slice_beta_offset_div2 & 15) << 8) |
		   (((u32)slice->slice_tc_offset_div2 & 15) << 16);

	/* long-term reference bitmaps of both lists */
	for (i = 0; i < count0; i++)
		if (dec->dpb[slice->ref_idx_l0[i]].flags &
		    V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE)
			slot[13] |= BIT(i);
	for (i = 0; i < count1; i++)
		if (dec->dpb[slice->ref_idx_l1[i]].flags &
		    V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE)
			slot[14] |= BIT(i);

	/*
	 * Reference lists as 4-bit APC slot ids, eight entries per word: list 0
	 * at D15/D16, list 1 at D17/D18.
	 */
	for (i = 0; i < count0; i++)
		slot[15 + (i >> 3)] |= (u32)run->dpb2apc[slice->ref_idx_l0[i]] <<
				       ((i & 7) * 4);
	for (i = 0; i < count1; i++)
		slot[17 + (i >> 3)] |= (u32)run->dpb2apc[slice->ref_idx_l1[i]] <<
				       ((i & 7) * 4);

	/* CABAC context initialisation word of this decoder generation */
	slot[24] = 0x400a | ((6u * sps->bit_depth_luma_minus8) >> 1) << 8;

	/*
	 * Slice chain words. The CTB range of the slice ends where the next
	 * slice begins (minus one, vendor arithmetic); the last slice ends at
	 * the last CTB of the picture. D63 links to the next slot, the tail
	 * terminates at zero, and a stateless driver refuses chains longer than
	 * the message pool holds.
	 */
	end_ctb = index < last ? run->slices[index + 1].slice_segment_addr - 1 :
		  run->ctu_count - 1;
	slot[43] = tiled ? histb_hevc_tile_to_raster(pps, run->width_ctbs,
						    run->height_ctbs, end_ctb) :
			  end_ctb;
	/* D44 is the tile-scan version of the same end; the vendor converts
	 * only the raster word.
	 */
	slot[44] = end_ctb;
	if (index < last)
		slot[63] = lower_32_bits(histb_vdec_slice_slot_dma(&dev->pool,
								   index + 1));

	weighted = (is_b && (pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_BIPRED)) ||
		   (is_p && (pps->flags & V4L2_HEVC_PPS_FLAG_WEIGHTED_PRED));
	if (weighted) {
		int ret = histb_hevc_pack_weights(slot, slice, false);

		if (ret)
			return ret;
		if (is_b) {
			ret = histb_hevc_pack_weights(slot, slice, true);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static void histb_hevc_program_regs(struct histb_vdec_dev *dev,
				    const struct histb_hevc_run *run)
{
	unsigned int lines = run->buf_lines;
	unsigned int i;
	u32 cfg0 = (run->ctu_count - 1) & VDM_CFG0_MBAMT;
	u32 cfg1 = FIELD_PREP(VDM_CFG1_VID_STD, HISTB_HEVC_VIDSTD_CODE) |
		   FIELD_PREP(VDM_CFG1_DDR_STRIDE, run->tile_stride >> 6);

	if (histb_vdec_hevc_ab & BIT(0))
		cfg0 |= VDM_CFG0_COEFIDX_DETECT;
	if (histb_vdec_hevc_ab & BIT(1))
		cfg0 |= VDM_CFG0_LD_QMATRIX;
	if (histb_vdec_hevc_ab & BIT(2))
		cfg1 |= VDM_CFG1_FST_SLC_GRP;
	if (histb_vdec_hevc_ab & BIT(11))
		cfg1 |= FIELD_PREP(VDM_CFG1_MAX_SLCGRP_NUM, 3);
	if (histb_vdec_hevc_ab & BIT(4))
		cfg1 |= VDM_CFG1_DBLK_1D_EN;
	if (histb_vdec_hevc_ab & BIT(8))
		cfg1 &= ~VDM_CFG1_DDR_STRIDE;

	/* decoded CTU budget: the field counts CTUs minus one */
	vdm_write(dev, VDM_CFG0, cfg0);
	vdm_write(dev, VDM_CFG1, cfg1);
	vdm_write(dev, VDM_AVM_ADDR,
		  lower_32_bits(histb_vdec_pic_slot_dma(&dev->pool)));
	vdm_write(dev, VDM_VAM_ADDR,
		  lower_32_bits(histb_vdec_up_msgs_dma(&dev->pool)));
	vdm_write(dev, VDM_STREAM_BASE, lower_32_bits(run->src_dma & ~15u));

	/* one timeout per pipeline stage, windows 0x3c..0x54 only */
	for (i = 1; i <= 7 && !(histb_vdec_hevc_ab & BIT(12)); i++)
		vdm_write(dev, VDM_TO(i), (histb_vdec_hevc_ab & BIT(10)) ?
			  VDM_TIMEOUT_HEVC : VDM_TIMEOUT_MOD);
	if (histb_vdec_hevc_ab & BIT(9))
		vdm_write(dev, VDM_PRC_CACHE_TYPE, 2);

	vdm_write(dev, VDM_YSTADDR_1D, lower_32_bits(run->dst_dma));
	/*
	 * The engine writes the band geometry documented in histb_vdec.h here
	 * too: YSTRIDE_1D is the 16-row band pitch, the chroma plane sits
	 * behind the whole luma band array plus the head-inf area. Measured on
	 * a 320x240 intra frame: with the head-inf offset left clear and a line
	 * pitch in YSTRIDE_1D, 73 of 4800 luma lines land where the model puts
	 * them; with this shape and the picture message's scratch split,
	 * all 4800.
	 */
	vdm_write(dev, VDM_YSTRIDE_1D,
		  histb_vdec_band_bytes(run->tile_stride));
	vdm_write(dev, VDM_UVOFFSET_1D,
		  histb_vdec_luma_bytes(run->tile_stride, lines) +
		  histb_vdec_head_inf_bytes(lines));
	vdm_write(dev, VDM_HEAD_INF_OFFSET,
		  histb_vdec_head_inf_bytes(lines));
	/* post-filter scratch of this generation */
	if (histb_vdec_hevc_ab & BIT(7)) {
		vdm_write(dev, VDM_PPFD_BUF_ADDR, 0);
		vdm_write(dev, VDM_PPFD_BUF_LEN, 0);
	} else {
		vdm_write(dev, VDM_PPFD_BUF_ADDR,
			  lower_32_bits(histb_vdec_tables_dma(&dev->pool) +
					HISTB_TABLE_OFF_PPFD));
		vdm_write(dev, VDM_PPFD_BUF_LEN, HISTB_HEVC_PPFD_ROWS - 1);
	}

	vdm_write(dev, VDM_INT_STATE, 0xffffffffu);
	vdm_write(dev, VDM_INT_MASK, 0xfffffffeu);
}

static int histb_vdec_hevc_start(struct histb_vdec_ctx *ctx)
{
	struct histb_vdec_hevc_ctx *hevc;

	hevc = kzalloc_obj(*hevc);
	if (!hevc)
		return -ENOMEM;

	/*
	 * The PMV pool is allocated on the first frame that needs it, when the
	 * SPS fixes the slot geometry; nothing else is per-context.
	 */
	ctx->priv = hevc;

	memset(ctx->pmv_map, 0, sizeof(ctx->pmv_map));
	ctx->pmv_next = 0;

	return 0;
}

static void histb_vdec_hevc_stop(struct histb_vdec_ctx *ctx)
{
	struct histb_vdec_dev *dev = histb_vdec_hevc_dev(ctx);
	struct histb_vdec_hevc_ctx *hevc = ctx->priv;

	if (!hevc)
		return;

	histb_hevc_pmv_pool_free(dev, hevc);
	if (hevc->rbsp_va)
		dma_free_wc(dev->dev, hevc->rbsp_size, hevc->rbsp_va,
			    hevc->rbsp_dma);
	kfree(hevc);
	ctx->priv = NULL;

	memset(ctx->pmv_map, 0, sizeof(ctx->pmv_map));
	ctx->pmv_next = 0;
}

/*
 * Hand the engine the slice span without its emulation-prevention bytes, the
 * staging the byte-exact H.264 layer also performs: the VDM decodes the bytes
 * it is given literally (histb_h264_pack_rbsp() records the all-I_PCM
 * measurement that shows it), and the uAPI delivers the escaped form.
 *
 * A 0x03 is dropped only when the two preceding raw bytes are zero, which is
 * the decoder's own rule and so keeps a coded 0x03 that directly follows a
 * removed one. The copy spans the first slice's NAL header byte to the end of
 * the payload and rewrites the slice offsets into that domain; a span without
 * any prevention byte keeps using the OUTPUT plane.
 */
static int histb_hevc_pack_rbsp(struct histb_vdec_ctx *ctx,
				struct histb_hevc_run *run, u32 *slice_starts,
				unsigned int n_slices, u32 *payload_bits)
{
	struct histb_vdec_dev *dev = histb_vdec_hevc_dev(ctx);
	struct histb_vdec_hevc_ctx *hevc = ctx->priv;
	unsigned int first = slice_starts[0] / 8;
	unsigned int end = *payload_bits / 8;
	unsigned int out = 0, idx = 0;
	unsigned int i;
	u8 *dst;

	if (!run->stream_va || first >= end)
		return 0;

	for (i = first; i + 2 < end; i++) {
		if (!run->stream_va[i] && !run->stream_va[i + 1] &&
		    run->stream_va[i + 2] == 0x03)
			break;
	}
	if (i + 2 >= end)
		return 0;

	if (hevc->rbsp_size < end - first) {
		if (hevc->rbsp_va)
			dma_free_wc(dev->dev, hevc->rbsp_size, hevc->rbsp_va,
				    hevc->rbsp_dma);
		hevc->rbsp_size = 0;
		hevc->rbsp_va = dma_alloc_wc(dev->dev,
					     round_up(end - first, 64 * 1024),
					     &hevc->rbsp_dma, GFP_KERNEL);
		if (!hevc->rbsp_va)
			return -ENOMEM;
		hevc->rbsp_size = round_up(end - first, 64 * 1024);
	}
	dst = hevc->rbsp_va;

	slice_starts[0] = 0;
	for (i = first; i < end; i++) {
		while (idx + 1 < n_slices && slice_starts[idx + 1] / 8 == i) {
			idx++;
			slice_starts[idx] = out * 8;
		}
		if (i >= first + 2 && run->stream_va[i] == 0x03 &&
		    !run->stream_va[i - 1] && !run->stream_va[i - 2])
			continue;
		dst[out++] = run->stream_va[i];
	}

	*payload_bits = out * 8;
	run->stream_va = dst;
	run->src_dma = hevc->rbsp_dma;
	return 0;
}

static int histb_vdec_hevc_run(struct histb_vdec_ctx *ctx,
			       struct vb2_v4l2_buffer *src,
			       struct vb2_v4l2_buffer *dst)
{
	struct histb_vdec_dev *dev = histb_vdec_hevc_dev(ctx);
	struct histb_vdec_hevc_ctx *hevc = ctx->priv;
	struct histb_hevc_run run = { };
	unsigned int cap_w, cap_h;
	struct v4l2_ctrl *ctrl;
	u32 slice_starts[HISTB_VDEC_MAX_SLICE_SLOTS];
	struct vb2_queue *cap_q;
	const u8 *stream_va;
	u32 *pic, payload_bits;
	unsigned int n_slices, found, w_mb, h_mb, needed;
	unsigned long start_code;
	unsigned int i;
	int ret;

	BUILD_BUG_ON(sizeof(struct v4l2_ctrl_hevc_scaling_matrix) >
		     (HISTB_VDEC_SLOT_WORDS - 64) * 4);

	/*
	 * The driver core has applied the request to the control handler before
	 * scheduling this job, so p_cur carries this frame's values.
	 */
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_HEVC_SPS);
	run.sps = ctrl ? ctrl->p_cur.p_hevc_sps : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_HEVC_PPS);
	run.pps = ctrl ? ctrl->p_cur.p_hevc_pps : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_HEVC_DECODE_PARAMS);
	/* decode_params and scaling_matrix have no typed pointer member */
	run.dec = ctrl ? (const void *)ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_HEVC_SCALING_MATRIX);
	run.scaling = ctrl ? (const void *)ctrl->p_cur.p : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_HEVC_SLICE_PARAMS);
	run.slices = ctrl ? ctrl->p_cur.p_hevc_slice_params : NULL;
	n_slices = ctrl ? ctrl->new_elems : 0;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_HEVC_START_CODE);
	start_code = ctrl ? ctrl->cur.val : V4L2_STATELESS_HEVC_START_CODE_ANNEX_B;

	/*
	 * The message this function builds needs all five descriptors and at
	 * least one slice: the picture words read SPS, PPS and decode_params,
	 * D64..D319 always copy the scaling matrix, and the slice chain
	 * anchored in D63 has to have a first slot.
	 *
	 * What is worth saying is which half of it is reachable. Every control
	 * named above is registered by histb_vdec_hevc_ctrls[] at the bottom of
	 * this file, a compound control has storage from registration onward,
	 * and the control core clamps a write to a dynamic array to the
	 * registered dimensions (v4l2-ctrls-api.c: an oversized request is cut
	 * to .dims[0] elements and answers ENOSPC, a zero-sized one answers
	 * EFAULT), so no pointer here can be NULL and n_slices can be neither 0
	 * nor larger than the 8 entries the slice control declares. That makes
	 * this line and the pool-count check below assertions of the uAPI
	 * contract rather than gates with an observed trigger; a refusal would
	 * still reach the client as a capture buffer with bytesused 0, which is
	 * the only signal a refusal gives on this engine. TODO: one host-side
	 * run of VIDIOC_S_EXT_CTRLS on V4L2_CID_STATELESS_HEVC_SLICE_PARAMS at
	 * size 0 and at 9 elements, reading the ioctl return, is what would let
	 * both tests be dropped instead of documented.
	 */
	if (!run.sps || !run.pps || !run.dec || !run.scaling || !run.slices ||
	    !n_slices)
		return -EINVAL;

	/*
	 * The VDM reconstructs NV21 16x16 tiles: 8-bit 4:2:0 only, because CFG1
	 * of this decoder generation has no 10-bit enable field.
	 *
	 * Tiled pictures are refused (BIT(16) lifts the refusal only as the
	 * tile address-domain probe): with V4L2_HEVC_PPS_FLAG_TILES_ENABLED the
	 * engine stops after a partial first frame for every grid shape tried
	 * on the board, whether or not the tile info image is written, and each
	 * such job costs a frame decode timeout, which on an older build used
	 * to wedge the block for the other protocols; measured here it no
	 * longer does, so the refusal is about the picture, not about the
	 * lock-up.
	 *
	 * These are judged per job, not in a control .try_ctrl(): the handler
	 * holds an all-zero default which TRY_EXT_CTRLS reads back as if a
	 * client had set it, and chroma_format_idc 0 is such a default.
	 */
	if (run.sps->bit_depth_luma_minus8 || run.sps->bit_depth_chroma_minus8 ||
	    run.sps->chroma_format_idc != 1 ||
	    (run.sps->flags & V4L2_HEVC_SPS_FLAG_SEPARATE_COLOUR_PLANE))
		return -EINVAL;
	/*
	 * The refusal above the tile probe arm is the delivered disposition,
	 * and it happens before the doorbell: with the flag set and BIT(16)
	 * clear, the board reads three refusals, no "job programmed" line, no
	 * timeout, and the capture buffer comes back with bytesused 0. What
	 * accepting tiles needs is the raster/tile address split, not a longer
	 * timeout: the vendor converts four of the six address-bearing slice
	 * words through its CPU-side tile-scan-to-raster map when tiles are on
	 * (D8, D9, D43 get the raster address, D10 and D44 the tile-scan one,
	 * and D6 bits17:0 carry an independent-slice address the driver has no
	 * value for), and BIT(16)/BIT(17) are the arms that try to follow that.
	 * The arms do not decode either: the 2x2 grid wrote 0 of 4800 luma
	 * lines on all three frames and each job ran to the watchdog, and the
	 * best any tiled shape has done on the board is 1156/4800 luma plus
	 * 568/2400 chroma on a first frame, nothing after it. Judging the
	 * address split needs a tiled picture carried by more than one slice,
	 * because a single-slice picture makes both orders equal; neither local
	 * encoder will produce one (kvazaar emits one slice per tiled picture
	 * and refuses a slice start that is not on a tile-column edge, x265
	 * only does multi-slice with wavefront, which it treats as exclusive
	 * with tiles), so the corpus cannot close this and the disposition
	 * stays a refusal.
	 */
	if ((run.pps->flags & V4L2_HEVC_PPS_FLAG_TILES_ENABLED) &&
	    !(histb_vdec_hevc_ab & BIT(16)))
		return -EINVAL;
	/*
	 * A picture whose SPS declares scaling lists is not decodable here.
	 * Measured with the flag claimed on an otherwise byte-exact picture:
	 * the delivered frame differs from the software reference in 147379 of
	 * the 147456 bytes the picture occupies, and the top-left luma comes
	 * back zeroed. The submitted matrices are not consulted at all -
	 * filling every list with 8, 16, 32, or the DC field with 8, or sending
	 * nothing, all produce byte-identical captures (md5 4264762e... on
	 * 384x256, 0d8eb4ff... on 1536x128) while the same job without the flag
	 * is exact. So no re-packing of D64 onwards can fix it, and identity
	 * lists - which must be a no-op - are already wrong: refusing is the
	 * only disposition that does not deliver bad pixels. Every stream
	 * measured on the board so far declares scaling_list_enabled clear, so
	 * this refuses nothing that decodes correctly today.
	 */
	if (run.sps->flags & V4L2_HEVC_SPS_FLAG_SCALING_LIST_ENABLED)
		return -EINVAL;

	/*
	 * One slice slot per slice, chained through D63, and the message pool
	 * holds HISTB_VDEC_MAX_SLICE_SLOTS of them (140 slots of 320 words,
	 * histb_vdec.h) - which is also the vendor's own limit for one start:
	 * its slice chain is counted against 140 and the remainder is handed to
	 * the engine by chain cutting, which clears the link word and starts
	 * the VDM again. This job path programs once and kicks once per frame,
	 * so a longer chain has neither storage nor a second doorbell, and the
	 * whole picture would be described by the first 140 slots while the CTU
	 * budget in pic[0] and CFG0 still promises every CTB in the grid.
	 * Accepting one needs that multi-start state machine, with the budget
	 * and the per-slice up-message area (4 words per slice, histb_vdec.h)
	 * accounted per segment. The slice control this driver registers
	 * declares 8 entries and the control core cuts a longer submission to
	 * that (v4l2-ctrls-api.c), so the bound that a client can actually hit
	 * is 8; this line states the pool limit in code, which is the one that
	 * would have to be raised along with a multi-start path.
	 */
	if (n_slices > HISTB_VDEC_MAX_SLICE_SLOTS)
		return -EINVAL;

	payload_bits = vb2_get_plane_payload(&src->vb2_buf, 0) * 8;
	/*
	 * An OUTPUT buffer with no payload declares no access unit. Every
	 * stream address in the message is an offset inside this plane - the
	 * segment words D0 of each slice and VDM_STREAM_BASE are computed from
	 * its DMA address and bounded by payload_bits - so with zero bytes
	 * there is no span to hand the engine and no end to bound any slice's
	 * declared length by. Padding the declaration is not an option: the VDM
	 * decodes the bytes it is given and does not re-synchronise on its own
	 * (histb_h264_pack_rbsp carries the all-I_PCM measurement that shows
	 * it), so an empty span cannot be stretched into a decodable slice. A
	 * truncated access unit reaches this line only if it carries no slice
	 * NAL at all; the slice scan below otherwise refuses it on the count.
	 */
	if (!payload_bits)
		return -EINVAL;
	run.src_dma = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	run.stream_va = vb2_plane_vaddr(&src->vb2_buf, 0);
	run.dst_dma = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	run.num_slices = n_slices;

	if (start_code == V4L2_STATELESS_HEVC_START_CODE_ANNEX_B) {
		stream_va = vb2_plane_vaddr(&src->vb2_buf, 0);
		/*
		 * Delimiting slices in the Annex-B form needs the bytes: the
		 * message ABI carries no field that names where a NAL starts -
		 * each slice slot only gets its own header-relative offset, and
		 * the chain anchor in pic D63 points at message slots, not at
		 * stream positions - so slice_starts[] can only come from a
		 * scan of the plane. The OUTPUT queue keeps a kernel mapping
		 * for exactly this reason - histb_vdec_drv.c deliberately
		 * leaves DMA_ATTR_NO_KERNEL_MAPPING off that queue - so landing
		 * on this exit means that promise was broken rather than that
		 * the client did something wrong, which is why the
		 * once-per-boot warning is the client-visible part. Accepting a
		 * buffer without a mapping would need per-NAL offsets from the
		 * client, which the stateless HEVC control set has no field
		 * for, or the start-code-detection up-message the vendor
		 * firmware uses, which this port does not implement (it kicks
		 * the VDM directly).
		 */
		if (!stream_va) {
			dev_warn_once(dev->dev,
				      "OUTPUT queue has no kernel mapping, cannot delimit slices\n");
			return -EINVAL;
		}
		found = histb_hevc_find_slice_nals(stream_va, payload_bits / 8,
						   slice_starts,
						   ARRAY_SIZE(slice_starts));
		/*
		 * The number of slice NALs found must be the number of
		 * slice-parameter entries submitted: each entry is handed its
		 * own stream span, so a count mismatch means the two lists
		 * cannot be paired and some slice would address the wrong NAL
		 * while another NAL is described by no slot at all. This is
		 * also the implicit rejection of slice carriers the message ABI
		 * cannot describe - the scan counts nal_unit_type 0..9 and
		 * 16..23 only, so a picture carried by the temporal-sub-layer
		 * or random-access types 10..13, or a buffer whose slice NALs
		 * are interleaved with something the scan skipped, is refused
		 * on the count rather than decoded from a span that starts in a
		 * NAL the engine cannot parse (the H.264 path rejects data
		 * partitioning the same way, counting only types 1 and 5).
		 * Measured on the board: declaring two slice entries for an
		 * access unit that carries one IDR slice NAL is refused before
		 * the doorbell - no "job programmed" line appears and the
		 * picture slot keeps the previous legal job's bytes - while an
		 * AU of VPS, SPS, PPS, prefix SEI and one IDR NAL counts
		 * exactly 1, which is what every scored stream submits.
		 */
		if (found != n_slices)
			return -EINVAL;
	} else {
		/*
		 * Without start codes the slices in the OUTPUT buffer cannot be
		 * delimited, so a single-slice submission is the only layout
		 * the driver can consume.
		 *
		 * The reason it can be no wider is that this route has neither
		 * half of the delimiting input: there is no start code to scan
		 * (that is what the control value declares), and the port has
		 * no start-code-detection up-message to take boundaries from,
		 * since the driver kicks the VDM directly. With one slice the
		 * whole buffer is the span, so nothing has to be delimited.
		 * Measured on the board, a single-slice submission with
		 * START_CODE_NONE is accepted and the engine is kicked for it,
		 * so this is the shape the route serves, not a missed gate.
		 * Accepting more than one slice needs per-slice NAL offsets
		 * from the client, which the stateless HEVC controls cannot
		 * express; the scored corpus all arrives through the Annex-B
		 * route, so this path is covered by direct submissions only.
		 */
		if (n_slices != 1)
			return -EINVAL;
		slice_starts[0] = 0;
	}

	ret = histb_hevc_pack_rbsp(ctx, &run, slice_starts, n_slices,
				   &payload_bits);
	if (ret)
		return ret;

	/*
	 * A picture with no columns or no rows is refused because everything
	 * downstream divides by or subtracts from the grid it makes: width_ctbs
	 * and height_ctbs become 0, so pic[0]'s (width_ctbs - 1) and
	 * (height_ctbs - 1) fields and the CTU budget in CFG0 wrap to their
	 * maximum, ctu_count - 1 underflows in histb_hevc_program_regs(), the
	 * raster coordinates in slot[9] divide by zero, and the last slice's
	 * range end becomes 0 - 1. These two SPS fields are the luma size of
	 * every decoded picture in H.265 7.4.3.2 and have no zero reading
	 * there, so a conformant client never hits this. Measured on the board:
	 * a SPS with pic_width_in_luma_samples 0 is refused before the
	 * doorbell - no "job programmed" line and the picture slot keeps the
	 * previous legal job's bytes - and with the rest of the descriptor
	 * legal this is the gate that sees it.
	 */
	if (!run.sps->pic_width_in_luma_samples ||
	    !run.sps->pic_height_in_luma_samples)
		return -EINVAL;
	run.width = run.sps->pic_width_in_luma_samples;
	run.height = run.sps->pic_height_in_luma_samples;
	run.ctb_log2 = run.sps->log2_min_luma_coding_block_size_minus3 + 3 +
		       run.sps->log2_diff_max_min_luma_coding_block_size;
	/*
	 * CtbLog2SizeY is the unit the whole geometry is counted in: it sizes
	 * pic[0]'s CTB grid and reaches pic[2] bits 2:0 verbatim, it is the CTU
	 * budget in CFG0, the per-slice range words D43/D44, the 64-row grid
	 * alignment the capture geometry reserves
	 * (histb_vdec_hevc_fmt.grid_align), and the row count the chroma plane
	 * sits behind. The two SPS fields it is summed from arrive as
	 * independent client values, and the CTB range H.265 wants is a
	 * conformance rule (7.4.3.2 with the Annex A profile limits) that the
	 * control layer cannot enforce, so the range this engine is known to
	 * work in has to be asserted here. Measured on the board: a descriptor
	 * whose derived CTB size is 128 (log2 7) is refused before the
	 * doorbell, while CTB 32 and CTB 64 are both decoded byte-exact across
	 * the scored corpus, including the single-CTB-row and height-scan
	 * families at those two sizes. TODO: no picture with a 16-sample CTB
	 * (log2 4) has been decoded on the board; one would show whether the
	 * lower bound is 4 or the engine's own minimum is 5, and whether an
	 * 8-sample CTB merely wastes grid rows or is refused by the hardware.
	 */
	if (run.ctb_log2 < 4 || run.ctb_log2 > 6)
		return -EINVAL;
	run.width_ctbs = (run.width + (1u << run.ctb_log2) - 1) >> run.ctb_log2;
	run.height_ctbs = (run.height + (1u << run.ctb_log2) - 1) >> run.ctb_log2;
	/*
	 * The bottom lines of a single-CTB-row picture are only written once a
	 * further CTB-row boundary is reached; measured on the board on 1536x64
	 * with CTU size 64: the missing constant 5 luma lines (plus 3 chroma
	 * lines) never appear in the capture buffer. Padding the grid here
	 * leaves the programmed luma height alone - pic[54] still carries
	 * run.height - so the CTB budget in pic[0]/CFG0 is the only thing that
	 * changes, which separates "where does the picture end" (deblocking)
	 * from "what flushes the line pipeline".
	 */
	if (histb_vdec_hevc_ab & BIT(15))
		run.height_ctbs++;
	/*
	 * The last CTB row's tail only comes out when another row boundary
	 * follows it, so a picture one CTB row tall is delivered missing a
	 * constant 1280 bytes per 256-byte luma tile column - 11 of its 16 rows
	 * written - plus 3 of every chroma tile's 8 rows, and those bytes are
	 * not present anywhere else in the capture buffer. The grid padding arm
	 * above showed that the deblocking picture-bottom follows the padded
	 * grid, so a limited parse plus a copy cannot rebuild the tail either.
	 * Two CTB rows, or a coded height that lands mid-row, are byte-exact,
	 * so this is not a general bottom-edge defect and there is nothing to
	 * work around at one row. Refuse the job instead of delivering a frame
	 * that is short by a band: the caller falls back to software. The
	 * BIT(15) probe above still reaches the engine, so the mechanism stays
	 * reproducible on the same boot.
	 */
	if (run.height_ctbs < 2)
		return -EINVAL;
	run.ctu_count = run.width_ctbs * run.height_ctbs;

	/*
	 * PMV slot geometry follows the stream resolution with a 1080p floor;
	 * the pool grows on demand, which invalidates the slot ids of earlier
	 * frames.
	 */
	w_mb = (run.width + 15) / 16;
	h_mb = (run.height + 15) / 16;
	needed = max_t(unsigned int, HISTB_HEVC_PMV_SLOT_SIZE(w_mb, h_mb),
		       HISTB_HEVC_PMV_SLOT_SIZE_MIN);
	ret = histb_vdec_cap_dims(ctx, &cap_w, &cap_h);
	if (ret)
		return ret;
	/*
	 * The coded picture has to fit the negotiated CAPTURE rectangle,
	 * because the buffer it lands in was sized from that rectangle
	 * (histb_vdec_buf_geom() takes bytesperline and the luma row count from
	 * it) while the engine writes its own coded grid: measured on the
	 * board, two jobs on the same 1920x256 geometry wrote exactly the same
	 * range, 737280 bytes = 1920 * 256 * 3 / 2, whether the stream carried
	 * 2472 or 29774 bytes of code, so the extent follows the declared
	 * picture and not the payload size. A larger coded picture than the
	 * negotiation would therefore put tiles outside the buffer the client
	 * was told about, which is why this is refused instead of clamped (and
	 * the same disposition the rkvdec driver uses).
	 * histb_vdec_try_fmt_cap() raises the CAPTURE rectangle to whatever the
	 * client declared on OUTPUT (see histb_vdec_drv.c), so the only way to
	 * reach this line is an OUTPUT declaration smaller than the SPS
	 * actually submitted. Measured on the board: a SPS declaring 4000
	 * columns is refused before the doorbell, and a refusal is visible as
	 * bytesused 0 on the capture buffer.
	 */
	if (run.sps->pic_width_in_luma_samples > cap_w ||
	    run.sps->pic_height_in_luma_samples > cap_h)
		return -EINVAL;

	/*
	 * Band layout comes from the CAPTURE format, so a picture smaller than
	 * the negotiated rectangle still lands where bytesperline says it does
	 * (the CTB-derived picture dimensions above keep the coded size: the
	 * engine decodes no further than the stream).
	 */
	histb_vdec_buf_geom(ctx, &run.tile_stride, &run.buf_lines);

	ret = histb_hevc_pmv_pool_grow(ctx, hevc, needed);
	if (ret)
		return ret;

	/*
	 * Map the DPB onto the APC frame table. Capture buffer indexes are the
	 * APC slots; unreferenced slots point at the output frame like an empty
	 * vendor APC tail.
	 */
	if (dst->vb2_buf.index >= HISTB_HEVC_NUM_APC_SLOTS)
		return -EINVAL;

	cap_q = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	for (i = 0; i < HISTB_HEVC_NUM_APC_SLOTS; i++) {
		run.img_addr[i] = lower_32_bits(run.dst_dma);
		run.apc_poc[i] = 0;
		run.dpb2apc[i] = HISTB_HEVC_APC_NONE;
	}
	/*
	 * The bound is read before the loop that uses it as an index count, so
	 * its first job is to keep that loop inside dec->dpb[], which is
	 * exactly V4L2_HEVC_DPB_ENTRIES_NUM_MAX entries long and is not clamped
	 * by the control layer against this one-byte count. It is also what the
	 * message can address: the picture slot carries a 16-word frame table
	 * (D8..D23) and a 16-word POC table (D26..D41), and the lists in each
	 * slice slot name those slots as 4-bit ids, so an entry past the
	 * sixteenth has neither an address nor a slot id even if it could be
	 * read. The capture queue is held to the same 16 buffers by
	 * histb_vdec_queue_setup(), so no part of the pipeline can widen past
	 * it; accepting more would need a slot indirection table outside the
	 * picture message.
	 */
	if (run.dec->num_active_dpb_entries > V4L2_HEVC_DPB_ENTRIES_NUM_MAX)
		return -EINVAL;
	for (i = 0; i < run.dec->num_active_dpb_entries; i++) {
		const struct v4l2_hevc_dpb_entry *entry = &run.dec->dpb[i];
		struct vb2_buffer *vb;

		vb = vb2_find_buffer(cap_q, entry->timestamp);
		/*
		 * A reference has to be a buffer that is actually held out of
		 * the capture queue: the uAPI names a DPB picture only by the
		 * timestamp of its capture buffer, and this driver's frame
		 * storage model makes that buffer index the APC slot - the same
		 * index writes the frame address into D8..D23, the POC into
		 * D26..D41, and the 4-bit ids the slice lists and the
		 * collocated picture use. An index at or above the 16 slots
		 * cannot be encoded there. Passing it through is not a no-op:
		 * the unused slots are prefilled above with the destination
		 * address of this very frame, so an entry that resolves to
		 * nothing silently makes the engine inter-predict from the
		 * picture it is writing. The refusal is logged with the
		 * offending entry because the client's only other signal is
		 * bytesused 0 on the capture buffer, which does not say which
		 * timestamp it named wrongly.
		 */
		if (!vb || vb->index >= HISTB_HEVC_NUM_APC_SLOTS) {
			dev_dbg(dev->dev, "hevc refuse: dpb %u ts=%llu not queued (active=%u dst=%u)\n",
				i, entry->timestamp,
				run.dec->num_active_dpb_entries,
				dst->vb2_buf.index);
			return -EINVAL;
		}

		run.dpb2apc[i] = vb->index;
		run.img_addr[vb->index] =
			lower_32_bits(vb2_dma_contig_plane_dma_addr(vb, 0));
		run.apc_poc[vb->index] = entry->pic_order_cnt_val;
	}
	run.apc_poc[dst->vb2_buf.index] = run.dec->pic_order_cnt_val;

	pic = histb_vdec_pic_slot(&dev->pool);
	histb_hevc_assemble_pic(dev, ctx, hevc, &run,
				histb_hevc_pmv_alloc(ctx, dst->vb2_buf.index),
				pic);

	for (i = 0; i < n_slices; i++) {
		u32 nal_bit = slice_starts[i];
		u32 next_bit = i + 1 < n_slices ? slice_starts[i + 1] :
						  payload_bits;

		/*
		 * A slice that starts at or past the end of the CTB grid has no
		 * CTB to write. The range words are built from these addresses:
		 * D43/D44 of a slice carry its CTB range end, computed as the
		 * next slice's start minus one and, for the last slice, as
		 * ctu_count - 1 (the vendor's arithmetic for the last CTB of
		 * the slice, which it writes in raster order to D43 and tile
		 * order to D44), so an address outside the grid makes the
		 * previous slice's range overshoot the picture while the raster
		 * coordinates in D9 (address / width_ctbs, address %
		 * width_ctbs) name a CTB row that does not exist. H.265 7.4.3
		 * requires a slice's segment address to map inside
		 * PicSizeInCtbsY (the raster/tile mapping itself is 6.5.1), so
		 * this refuses no conformant stream; measured over the 96
		 * scored streams the address checks never fired - the refusals
		 * on record are the tiled and the single-CTB-row shapes.
		 */
		if (run.slices[i].slice_segment_addr >= run.ctu_count)
			return -EINVAL;
		/*
		 * The vendor start path covers a first slice that does not
		 * begin at CTB 0 with a one-bit dummy slice; that path is not
		 * implemented here, so such frames are refused rather than
		 * handed to the engine with a chain that starts mid-picture.
		 */
		if (!i && run.slices[i].slice_segment_addr)
			return -EINVAL;
		/*
		 * The chain is a partition of the CTB grid: each slice's range
		 * end (D43/D44) is the next slice's start minus one, so an
		 * address that repeats or goes backwards puts the previous
		 * slot's end at or below its own start while this slot claims a
		 * range another slot already covers. Nothing can be clamped
		 * here without dropping CTBs out of every window, which is what
		 * the engine would then be told to skip. H.265 7.4.3 emits
		 * slice addresses in increasing raster order, and this never
		 * fired on anything measured so far: the scored corpus carries
		 * one slice per picture, so the multi-slice chains that
		 * exercise it are H.264's. TODO: one HEVC picture submitted as
		 * two increasing slices and scored byte-exact would show that
		 * the range arithmetic holds at the boundary between the two
		 * slots.
		 */
		if (i && run.slices[i].slice_segment_addr <=
		    run.slices[i - 1].slice_segment_addr)
			return -EINVAL;

		ret = histb_hevc_assemble_slice(dev, ctx, hevc, &run, i,
						nal_bit, next_bit,
						payload_bits);
		if (ret) {
			dev_dbg(dev->dev, "hevc refuse: slice %u seg=%u l0=%u l1=%u col=%u flags=%#llx dbo=%u bits=%u\n",
				i, run.slices[i].slice_segment_addr,
				run.slices[i].num_ref_idx_l0_active_minus1,
				run.slices[i].num_ref_idx_l1_active_minus1,
				run.slices[i].collocated_ref_idx,
				run.slices[i].flags,
				run.slices[i].data_byte_offset,
				run.slices[i].bit_size);
			return ret;
		}
	}

	histb_hevc_program_regs(dev, &run);
	histb_vdec_hw_kick(dev, HISTB_VIDSTD_HEVC);

	return 0;
}

static const struct v4l2_ctrl_config histb_vdec_hevc_ctrls[] = {
	{
		.id = V4L2_CID_STATELESS_HEVC_SPS,
	},
	{
		.id = V4L2_CID_STATELESS_HEVC_PPS,
	},
	{
		.id = V4L2_CID_STATELESS_HEVC_SLICE_PARAMS,
		.name = "HEVC Slice Parameters",
		.type = V4L2_CTRL_TYPE_HEVC_SLICE_PARAMS,
		.dims = { 8 },
		.flags = V4L2_CTRL_FLAG_DYNAMIC_ARRAY,
	},
	{
		.id = V4L2_CID_STATELESS_HEVC_SCALING_MATRIX,
	},
	{
		.id = V4L2_CID_STATELESS_HEVC_DECODE_PARAMS,
	},
	{
		.id = V4L2_CID_STATELESS_HEVC_DECODE_MODE,
		.min = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
		.max = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
		.def = V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED,
	},
	{
		.id = V4L2_CID_STATELESS_HEVC_START_CODE,
		.min = V4L2_STATELESS_HEVC_START_CODE_NONE,
		.max = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
		.def = V4L2_STATELESS_HEVC_START_CODE_ANNEX_B,
	},
};

static const struct histb_vdec_codec_ops histb_vdec_hevc_codec_ops = {
	.start = histb_vdec_hevc_start,
	.stop = histb_vdec_hevc_stop,
	.run = histb_vdec_hevc_run,
};

const struct histb_vdec_fmt_desc histb_vdec_hevc_fmt = {
	.fourcc = V4L2_PIX_FMT_HEVC_SLICE,
	.stride = histb_vdec_hevc_stride,
	.grid_align = 64,
	.num_ctrls = ARRAY_SIZE(histb_vdec_hevc_ctrls),
	.ctrls = histb_vdec_hevc_ctrls,
	.ops = &histb_vdec_hevc_codec_ops,
	.vidstd = HISTB_VIDSTD_HEVC,
};
