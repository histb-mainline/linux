// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSilicon Hi3798MV100 VDM stateless VP8 decoder (V4L2 codec layer). The
 * hardware consumes a fully parsed frame header (slot words D0-D24), three
 * reference frame addresses (D32-D35), band-line scratch pointers (D36-D40), a
 * probability table (D39) and a per-DCT-partition descriptor list stored in
 * slot words D64+ behind the D63 chain head.
 *
 * The message layout was cross-checked against the packing routine and the
 * register setup the vendor firmware runs for VP8.
 *
 * Three points that the packed message alone cannot decide are settled by the
 * vendor firmware: the bool decoder starts fresh with range 255 and count 16 at
 * one byte before the control segment, whose length is chosen so that start
 * plus length lands exactly at the end of the first-part window; the
 * probability image is the firmware's own 2752-byte table, handed to the engine
 * unmodified (so its layout is the firmware's, not a hardware format); and
 * token partitions are eight descriptor groups with the count coded as log2.
 * Still open: which bytes of that table the firmware fills beyond the
 * probabilities the control carries.
 */

#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/slab.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "histb_vdec.h"
#include "histb_vdec_tables_vp8.h"

/*
 * Band-line scratch area inside the pool table region, one 480 KiB buffer per
 * enum histb_vdec_scratch entry. VP8 references the SED/PMV/RCN/DBLK top
 * buffers in D36-D40.
 */

/*
 * Hardware protocol code in the VDM_CFG1_VID_STD field for VP8: 12. Both the
 * dispatch table and the CFG1 write in the vendor firmware use 12; the 13 in
 * enum histb_vdec_vidstd is the firmware's channel type, which the common start
 * path consumes for the clock/SCD pokes (identical behaviour for 12 and 13
 * since both fall in the non-HEVC range).
 */
#define HISTB_VP8_HW_STD		12

/*
 * A/B arms against the vendor firmware's message packer and its syntax side,
 * one bit per single-variable change, runtime writable so a whole sweep runs
 * in one boot:
 *   bit0 vendor control window: start 16 bits before the end of the
 *        uncompressed chunk (frame tag included), length up to the end of the
 *        first partition, D9 carrying range 255 with count 16, and the value
 *        seed read at that start
 *   bit2 CFG1 gate bits the vendor sets for VP8 (FST_SLC_GRP, MV_OUT_EN,
 *        DBLK_1D_EN)
 *   bit3 HEAD_INF = level * ALIGN(mb rows * 16, 32) and UVOFFSET with it
 *   bit4 last DCT partition length = the bytes left in the payload
 *   bit5 keep the probability regions 8 bytes above the upload base, the
 *        placement the vendor firmware's own table stores use, i.e. the
 *        placement this driver used before the copy source was traced
 *   bit6 D25 (the segment-id map address) points at a scratch instead of
 *        staying zero
 *   bit7 YSTRIDE/CFG1 stride as the 16-row band pitch instead of the line pitch
 *   bit8 leave the timeout registers alone (the vendor writes none for VP8)
 *   bit9 count the 3-byte frame tag in the token partition offsets
 *   bit11 read first_part_size as counting the uncompressed chunk in (the
 * frame header's own field does), which moves the token partitions and the
 *        control window's end back by 3 or 10 bytes
 *   bit12 read the D8 value window 32 bits wide instead of 24 (the vendor's
 *        live bool value is a 32-bit register refilled as the coder consumes)
 *   bit13 keep bit0's window placement but seed with count 0, i.e. the plain
 *        fresh-bool-decoder hand-off (range 255, count 0, value at the window
 *        start) instead of the vendor's 16-bit back-off
 *   bit14 assemble the D8 value little-endian (stream byte n -> bits 8n) and
 *        without shifting out the sub-byte residue - the position sweeps above
 *        cannot reach this permutation, so it is a separate degree of freedom
 *   bit15 seed D9.range from the control's coder_state.range (the live range
 *        left after the bool-coded header) instead of the fresh-decoder 255
 * bit16 seed D9.count from the control's coder_state.bit_count instead of 0/16
 *   bit17 write D8/D9/D16/D17 from the live-state formulas traced through the
 *        vendor firmware's decoder (count = 8 + ((8 - header_bits % 8) % 8),
 *        value = the 24 bits at the position its header parse stopped at with
 *        the low 16-count bits cleared, range from the control, segment
 *        back-off by that count with the +128bit/-16byte granule borrow)
 *        instead of the fresh-decoder seed the other arms use
 *   bit18 with bit17: anchor D16/D17 at the position where the vendor
 *        firmware's bitstream reader actually sits (header end + count + 8)
 *        rather than at the header's last bit
 */
static unsigned int histb_vdec_vp8_ab;
module_param(histb_vdec_vp8_ab, uint, 0644);
MODULE_PARM_DESC(histb_vdec_vp8_ab, "VP8 message/register A/B arms, one bit per vendor difference");

/*
 * Sweep knob for the boolean-decoder seed: extra bit offset applied to the
 * position D8's value window is read from (on top of the `count` back-off). The
 * vendor keeps a live 32-bit value that the uAPI's 8-bit coder_state cannot
 * express, so the exact window placement is the open item of the VP8 bring-up;
 * only a sweep can bound it.
 */
static int histb_vdec_vp8_seed_off;
module_param(histb_vdec_vp8_seed_off, int, 0644);
MODULE_PARM_DESC(histb_vdec_vp8_seed_off, "VP8 D8 bool value window shift in bits (sweep only)");

/*
 * Second sweep knob: shifts the macroblock-data window itself (D16/D17), with
 * the seed following it. The seed offset above only moves where D8's preload is
 * read from and leaves the engine's own fetch address fixed, so it cannot test
 * whether the header's trailing bits belong before the macroblock data - which
 * is what the in-tree consumer's "+8 bits" claims.
 */
static int histb_vdec_vp8_win_off;
module_param(histb_vdec_vp8_win_off, int, 0644);
MODULE_PARM_DESC(histb_vdec_vp8_win_off, "VP8 macroblock-data window shift in bits (sweep only)");

/*
 * Raw overrides for the descriptor fields whose *encoding* the vendor firmware
 * never shows a consumer for: D0 (cur/last frame type - the driver guesses
 * which value means "key frame"), D1 (motion compensation mode - the driver's
 * value comes from a formula copied from another protocol) and D4 (segmentation
 * and loop-filter adjustment flags). Negative keeps the derived value; 0..255
 * writes the field verbatim, so the whole value domain is sweepable instead of
 * argued about.
 */
static int histb_vdec_vp8_d0 = -1;
module_param(histb_vdec_vp8_d0, int, 0644);
MODULE_PARM_DESC(histb_vdec_vp8_d0, "raw D0 frame-type word, negative = derive (sweep only)");

static int histb_vdec_vp8_d1 = -1;
module_param(histb_vdec_vp8_d1, int, 0644);
MODULE_PARM_DESC(histb_vdec_vp8_d1, "raw D1 motion-compensation word, negative = derive (sweep only)");

static int histb_vdec_vp8_d4 = -1;
module_param(histb_vdec_vp8_d4, int, 0644);
MODULE_PARM_DESC(histb_vdec_vp8_d4, "raw D4 segmentation/lf-flag word, negative = derive (sweep only)");

/*
 * Raw seeds for the bool hand-off itself. D8/D9 are the only VP8 descriptor
 * words whose correct content can be computed off-target (replaying the vendor
 * firmware's own boolean-decoder arithmetic over the header's symbol trace), so
 * these two overrides let the board be fed the vendor's exact live state
 * instead of any closed-form guess.
 */
static int histb_vdec_vp8_d8 = -1;
module_param(histb_vdec_vp8_d8, int, 0644);
MODULE_PARM_DESC(histb_vdec_vp8_d8, "raw D8 bool value word, negative = derive (sweep only)");

static int histb_vdec_vp8_d9 = -1;
module_param(histb_vdec_vp8_d9, int, 0644);
MODULE_PARM_DESC(histb_vdec_vp8_d9, "raw D9 range/count word, negative = derive (sweep only)");

static const struct v4l2_ctrl_config histb_vdec_vp8_ctrls[] = {
	{ .id = V4L2_CID_STATELESS_VP8_FRAME },
};

/*
 * Cross-frame state the hardware does not track: the previous frame's filter
 * type, sharpness and frame type (D5 last_* and D0 last_frame_type). Reset by
 * stop(). On the first frame of a stream the previous values equal the current
 * ones, which holds naturally because a VP8 stream always begins with a key
 * frame (all-zero state).
 */
struct histb_vdec_vp8_state {
	u8	last_frame_type;
	u8	last_filter_type;
	u8	last_sharpness;
};

static const struct v4l2_ctrl_vp8_frame *
histb_vdec_vp8_hdr(struct histb_vdec_ctx *ctx)
{
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_VP8_FRAME);
	return ctrl->p_cur.p;
}

static dma_addr_t histb_vdec_ref_addr(struct histb_vdec_ctx *ctx, u64 ts,
				      dma_addr_t cur)
{
	struct vb2_buffer *vb;

	vb = vb2_find_buffer(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx), ts);
	if (!vb)
		return cur;

	return vb2_dma_contig_plane_dma_addr(vb, 0);
}

/*
 * Pack the probability image consumed through the table-address slot (D39).
 * Layout and its firmware evidence are documented in histb_vdec_tables_vp8.h;
 * the regions whose vendor source could not be identified are left zeroed.
 */
static void histb_vdec_vp8_pack_table(u8 *t,
				      const struct v4l2_ctrl_vp8_frame *hdr,
				      const struct v4l2_vp8_segment *seg,
				      unsigned int ab)
{
	const struct v4l2_vp8_entropy *e = &hdr->entropy;
	unsigned int plane, band, c, j, g, shift;
	const u8 *probs;

	memset(t, 0, HISTB_VDEC_VP8_TAB_SIZE);

	/* t[0]: prob_skip_false, t[1..3]: the segment tree probabilities,
	 * t[4..6]: intra/last/golden; the vendor firmware writes these as one
	 * store run against its parameter block, whose start sits 8 bytes ahead
	 * of the uploaded image. bit5 puts each region back at that +8, which
	 * is where this driver had them.
	 */
	shift = ab & BIT(5) ? 8 : 0;
	t[HISTB_VP8_TAB_MODE2_OFF + shift] = hdr->prob_skip_false;
	for (j = 0; j < 3; j++)
		t[shift + 1 + j] = seg->segment_probs[j];
	t[shift + 4] = hdr->prob_intra;
	t[shift + 5] = hdr->prob_last;
	t[shift + 6] = hdr->prob_gf;

	/* t[16..19]: y_mode_probs[4], t[20..22]: uv_mode_probs[3], again one
	 * store run in the vendor firmware's table build.
	 */
	for (j = 0; j < 4; j++)
		t[HISTB_VP8_TAB_MODE_OFF + shift + j] = e->y_mode_probs[j];
	for (j = 0; j < 3; j++)
		t[HISTB_VP8_TAB_MODE_OFF + shift + 4 + j] = e->uv_mode_probs[j];

	/* mv_probs[2][19]: two 16-byte chunks per array, chunk 0 holding
	 * {prob[1], prob[0], prob[2..8]} and chunk 1 {prob[1],
	 * prob[9..18]}. The vendor firmware writes these chunks at table
	 * offsets 704/720 and 736/752.
	 */
	for (plane = 0; plane < 2; plane++) {
		u8 *ch = t + HISTB_VP8_TAB_MV_OFF + shift + plane * 32;

		probs = e->mv_probs[plane];

		ch[0] = probs[1];
		ch[1] = probs[0];
		for (j = 2; j <= 8; j++)
			ch[j] = probs[j];
		ch[16] = probs[1];
		for (j = 9; j <= 18; j++)
			ch[17 + j - 9] = probs[j];
	}

	/* Coefficient probabilities: one 16-byte group per [plane][band]
	 * [context], 11 probabilities then zero padding, plane-major order. The
	 * vendor firmware's build loop covers 1536 bytes from offset 768, which
	 * is exactly the four plane types times 24 groups.
	 */
	for (plane = 0; plane < HISTB_VP8_TAB_COEF_PLANES; plane++) {
		for (band = 0; band < 8; band++) {
			for (c = 0; c < 3; c++) {
				g = plane * HISTB_VP8_TAB_COEF_GROUPS +
				    band * 3 + c;
				probs = e->coeff_probs[plane][band][c];
				for (j = 0; j < HISTB_VP8_TAB_COEF_PROBS; j++) {
					t[HISTB_VP8_TAB_COEF_OFF + shift +
					  g * HISTB_VP8_TAB_COEF_STRIDE + j] =
						probs[j];
				}
			}
		}
	}
}

/*
 * Initial state of the hardware boolean decoder (D8 value, D9 range and count).
 * The control's v4l2_vp8_entropy_coder_state.value is only 8 bits wide, so the
 * 24-bit initial value is re-read from the bitstream at the first
 * macroblock-data bit position: uncompressed data chunk (10 bytes for key
 * frames, 3 otherwise) plus first_part_header_bits, the same position the G1
 * driver derives in drivers/staging/media/hantro/hantro_g1_vp8_dec.c. The value
 * is taken big-endian with the sub-byte header residue shifted out; a fresh
 * (range 255, count 0) init is the RFC 6386 bool decoder start. Whether the
 * hardware restarts at that position with those semantics or wants the
 * carried-over decoder state cannot be told from the control, so both are
 * offered as arms of histb_vdec_vp8_ab and bounded by the seed and window
 * sweeps declared above.
 */
static u32 histb_vdec_vp8_bool_value(struct vb2_v4l2_buffer *src,
				     const struct v4l2_ctrl_vp8_frame *hdr,
				     unsigned int pos, bool wide, bool le)
{
	unsigned int byte, shift, need;
	const u8 *va;
	u32 window;

	byte = pos / 8;
	shift = pos % 8;

	va = vb2_plane_vaddr(&src->vb2_buf, 0);
	need = byte + (wide ? 4 : 3) + (shift ? 1 : 0);
	if (va && vb2_get_plane_payload(&src->vb2_buf, 0) >= need) {
		if (le) {
			unsigned int n = wide ? 4 : 3;

			window = 0;
			for (unsigned int i = 0; i < n; i++)
				window |= va[byte + i] << (8 * i);
			return window;
		}
		if (wide) {
			window = va[byte] << 24 | va[byte + 1] << 16 |
				 va[byte + 2] << 8 | va[byte + 3];
			return window >> shift;
		}
		window = va[byte] << 24 | va[byte + 1] << 16 |
			 va[byte + 2] << 8;
		if (shift)
			window |= va[byte + 3];
		return (window >> shift) & 0xffffff;
	}

	/* Kernel mapping unavailable (no dma-contig read access): fall back to
	 * the truncated 8-bit control value.
	 */
	return hdr->coder_state.value;
}

static int histb_vdec_vp8_run(struct histb_vdec_ctx *ctx,
			      struct vb2_v4l2_buffer *src,
			      struct vb2_v4l2_buffer *dst)
{
	const struct v4l2_ctrl_vp8_frame *hdr;
	const struct v4l2_vp8_segment *seg;
	const struct v4l2_vp8_loop_filter *lf;
	const struct v4l2_vp8_quantization *quant;
	struct histb_vdec_vp8_state *st = ctx->priv;
	struct histb_vdec_dev *dev = video_get_drvdata(ctx->fh.vdev);
	struct histb_vdec_msgpool *pool = &dev->pool;
	dma_addr_t stream_base, src_addr, dst_addr, refs[3];
	unsigned int stride, mb_width, mb_height, mb_total;
	unsigned int height, num_parts, i, part_off, part_bits;
	unsigned int use_bilinear, full_pixel, cap_w, cap_h;
	unsigned int seg_bit, seg_end, head_inf, uv_off, band_level, fps;
	int value_bit;
	unsigned int ddr_stride;
	int ret;
	unsigned int mb_bit;
	const unsigned int ab = histb_vdec_vp8_ab;
	const s8 *deltas;
	u32 *w = histb_vdec_pic_slot(pool);
	u32 seg3[3];
	s8 d;

	hdr = histb_vdec_vp8_hdr(ctx);
	seg = &hdr->segment;
	lf = &hdr->lf;
	quant = &hdr->quant;

	/*
	 * The header's own dimensions set the macroblock grid (D2), the tile
	 * geometry and every buffer size, so a zero size or a picture past the
	 * engine ceiling (HISTB_VDEC_MAX_WIDTH/MAX_HEIGHT, 1920 by 1920 here)
	 * has no register layout that fits it; refusing before the arithmetic
	 * wraps such a grid into a wrong count in the message.
	 */
	if (!hdr->width || !hdr->height ||
	    hdr->width > HISTB_VDEC_MAX_WIDTH ||
	    hdr->height > HISTB_VDEC_MAX_HEIGHT)
		return -EINVAL;

	ret = histb_vdec_cap_dims(ctx, &cap_w, &cap_h);
	if (ret)
		return ret;
	/*
	 * The VDM writes a whole frame into the CAPTURE buffer, so a coded
	 * picture larger than the negotiated rectangle would run past it. The
	 * write geometry comes from the negotiated format, not the coded one
	 * (histb_vdec_buf_geom()), so a picture that overruns it is refused
	 * rather than clipped, as the stateless decoder contract requires.
	 */
	if (hdr->width > cap_w || hdr->height > cap_h)
		return -EINVAL;

	mb_width = DIV_ROUND_UP(hdr->width, 16);
	mb_height = DIV_ROUND_UP(hdr->height, 16);
	mb_total = mb_width * mb_height;
	height = hdr->height;
	stride = ALIGN(hdr->width, 256);
	num_parts = hdr->num_dct_parts;

	/*
	 * The partition count reaches the engine only as a power of two up to
	 * eight: its log2 goes in a 2-bit field (D3 bits [10:9] below) and each
	 * partition needs one descriptor group in the D64+ area, which holds
	 * exactly eight, the width of hdr->dct_part_sizes. RFC 6386 codes the
	 * token-partition count as a 2-bit log2 too, so 1, 2, 4 or 8 are the
	 * only counts a conforming header asks for and no other is describable.
	 */
	if (num_parts != 1 && num_parts != 2 && num_parts != 4 &&
	    num_parts != 8)
		return -EINVAL;

	src_addr = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	dst_addr = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	stream_base = src_addr & ~255U;

	refs[0] = histb_vdec_ref_addr(ctx, hdr->last_frame_ts, dst_addr);
	refs[1] = histb_vdec_ref_addr(ctx, hdr->golden_frame_ts, dst_addr);
	refs[2] = histb_vdec_ref_addr(ctx, hdr->alt_frame_ts, dst_addr);

	memset(w, 0, HISTB_VDEC_SLOT_BYTES);

	/* D0: current and previous frame type (0 = key, 1 = inter). */
	w[0] = FIELD_PREP(GENMASK(2, 1), st->last_frame_type);
	if (!V4L2_VP8_FRAME_IS_KEY_FRAME(hdr))
		w[0] |= BIT(0);
	if (histb_vdec_vp8_d0 >= 0)
		w[0] = histb_vdec_vp8_d0;

	/* D1: motion compensation mode, derived from the frame version
	 * bits (RFC 6386: bit 0 bilinear, bit 1 full-pel) as
	 * 2 * full_pixel - use_bilinear + 1; 1 for the common 6-tap case.
	 */
	use_bilinear = hdr->version & 1;
	full_pixel = (hdr->version >> 1) & 1;
	w[1] = 2 * full_pixel + 1 - use_bilinear;
	if (histb_vdec_vp8_d1 >= 0)
		w[1] = histb_vdec_vp8_d1;

	/* D2: macroblock frame size minus one. */
	w[2] = FIELD_PREP(GENMASK(8, 0), mb_width - 1) |
	       FIELD_PREP(GENMASK(24, 16), mb_height - 1);

	/* D3: skip probability and flags, token partition count as
	 * log2(num_dct_parts).
	 */
	w[3] |= FIELD_PREP(GENMASK(7, 0), hdr->prob_skip_false);
	if (hdr->flags & V4L2_VP8_FRAME_FLAG_MB_NO_SKIP_COEFF)
		w[3] |= BIT(8);
	if (num_parts)
		w[3] |= FIELD_PREP(GENMASK(10, 9), ilog2(num_parts));

	/* D4: segmentation and loop filter adjustment flags. */
	if (seg->flags & V4L2_VP8_SEGMENT_FLAG_ENABLED)
		w[4] |= BIT(0);
	if (seg->flags & V4L2_VP8_SEGMENT_FLAG_UPDATE_MAP)
		w[4] |= BIT(1);
	if (seg->flags & V4L2_VP8_SEGMENT_FLAG_DELTA_VALUE_MODE)
		w[4] |= BIT(2);
	if (lf->flags & V4L2_VP8_LF_ADJ_ENABLE)
		w[4] |= BIT(3);
	if (histb_vdec_vp8_d4 >= 0)
		w[4] = histb_vdec_vp8_d4;

	/* D5: loop filter type, level and sharpness, current plus the previous
	 * frame's type and sharpness from the driver state.
	 */
	if (lf->flags & V4L2_VP8_LF_FILTER_TYPE_SIMPLE)
		w[5] |= BIT(0);
	w[5] |= FIELD_PREP(GENMASK(2, 1), st->last_filter_type);
	w[5] |= FIELD_PREP(GENMASK(8, 3), lf->level);
	w[5] |= FIELD_PREP(GENMASK(11, 9), lf->sharpness_level);
	w[5] |= FIELD_PREP(GENMASK(14, 12), st->last_sharpness);

	/* D6: quantisation deltas as 5-bit fields, sign bit first then a 4-bit
	 * magnitude, order y1dc, y2dc, y2ac, uvdc, uvac.
	 */
	deltas = &quant->y_dc_delta;
	for (i = 0; i < 5; i++) {
		d = deltas[i];

		if (d < 0)
			w[6] |= BIT(5 * i);
		w[6] |= (abs(d) & 0xf) << (5 * i + 1);
	}

	/* D7: base quantiser index. */
	w[7] |= FIELD_PREP(GENMASK(6, 0), quant->y_ac_qi);

	/*
	 * D8/D9: boolean decoder initial state. D16-D19: control partition
	 * segment starting at the first macroblock-data bit; bytes before it
	 * (frame tag, uncompressed chunk, bool-coded header) stay outside the
	 * segment because the hardware decodes from the handoff position with
	 * the D8 seed. The second segment pair is left unused; nothing here
	 * shows what the engine does with it.
	 */
	/*
	 * The uAPI leaves it open whether first_part_size covers the
	 * uncompressed chunk (3 or 10 bytes counted from the payload start) or
	 * only the bool-coded data after it; the frame header's own field
	 * counts the chunk in, and the in-tree consumer hantro_g1_vp8_dec.c
	 * works with the prefix subtracted through first_part_offset. bit11 =
	 * "chunk included".
	 */
	fps = hdr->first_part_size -
		(ab & BIT(11) ? (V4L2_VP8_FRAME_IS_KEY_FRAME(hdr) ? 10 : 3) : 0);

	mb_bit = (V4L2_VP8_FRAME_IS_KEY_FRAME(hdr) ? 10 : 3) * 8 +
		 hdr->first_part_header_bits;
	/*
	 * mb_bit is the first bit past the bool-coded partition header (the
	 * 10/3-byte uncompressed chunk plus first_part_header_bits); the
	 * control segment and the D8 value preload are both addressed from it.
	 * A payload ending at or before mb_bit carries no macroblock data and
	 * no byte behind the header to seed from, so its segment would start
	 * past the buffer.
	 */
	if (vb2_get_plane_payload(&src->vb2_buf, 0) * 8 <= mb_bit)
		return -EINVAL;

	/*
	 * The vendor firmware seeds the bool decoder 16 bits before the
	 * position where its own software stopped parsing the bool-coded first
	 * partition header, and lets the control segment run to the end of that
	 * partition, so that start + length is exactly that window: its message
	 * packer stores the partition length as 8 * first_part_size and shifts
	 * the segment start back by the seed count. The payload starts at the
	 * 3-byte frame tag and the 10/3 constants here are already tag +
	 * uncompressed chunk (the G1 driver's first_part_offset), so no further
	 * tag adjustment belongs in these offsets. The engine parses the
	 * bool-coded header itself, so the header bit count does not enter the
	 * partition table.
	 */
	/*
	 * The in-tree consumer adds eight bits on top of first_part_offset * 8
	 * + first_part_header_bits before locating the macroblock data
	 * (drivers/staging/media/hantro/hantro_g1_vp8_dec.c), while this
	 * driver's mb_bit does not; whether the VDM wants the header's trailing
	 * bits counted that way is sweepable with histb_vdec_vp8_win_off, which
	 * shifts this window's start (and the seed that rides on it) in bits.
	 */
	if (ab & BIT(0)) {
		/*
		 * The back-off starts from mb_bit, the position where the
		 * vendor firmware's software stopped parsing the bool-coded
		 * first partition header, not from the end of the uncompressed
		 * chunk; the window still ends at the first partition's last
		 * bit.
		 */
		seg_bit = mb_bit - 16;
		seg_end = ((V4L2_VP8_FRAME_IS_KEY_FRAME(hdr) ? 10 : 3) +
			   fps) * 8;
	} else {
		seg_bit = mb_bit;
		seg_end = vb2_get_plane_payload(&src->vb2_buf, 0) * 8;
	}
	/*
	 * histb_vdec_vp8_win_off is a sweep-only shift added to seg_bit on the
	 * next line. Tested before the add: an over-negative value would wrap
	 * the unsigned seg_bit past the seg_bit < 8 re-check below, and even a
	 * mild one must not pull the control-segment start under the first byte
	 * (bit 8), the lowest position the D16/D17 segment and the value
	 * preload read from seg_bit - 8 can address.
	 */
	if (histb_vdec_vp8_win_off < 0 &&
	    (int)seg_bit + histb_vdec_vp8_win_off < 8)
		return -EINVAL;
	seg_bit += histb_vdec_vp8_win_off;
	value_bit = seg_bit - 8 + histb_vdec_vp8_seed_off;
	/*
	 * Window validity before the message is programmed: seg_bit stays at or
	 * above the first byte and strictly below seg_end so the control
	 * partition has a positive length, value_bit (where the D8 value
	 * preloads from) stays non-negative, and seg_end stays inside the
	 * payload. The VDM is handed raw physical addresses and this SoC has no
	 * IOMMU, so a control segment past the buffer DMA-reads whatever
	 * follows it.
	 */
	if (seg_bit < 8 || seg_bit >= seg_end || value_bit < 0 ||
	    /* The seed window has to sit inside the payload, otherwise
	     * histb_vdec_vp8_bool_value() would silently fall back to the 8-bit
	     * control value and the arm would measure nothing.
	     */
	    value_bit / 8 + (ab & BIT(12) ? 5 : 4) >
		vb2_get_plane_payload(&src->vb2_buf, 0) ||
	    seg_end > vb2_get_plane_payload(&src->vb2_buf, 0) * 8)
		return -EINVAL;

	w[8] = histb_vdec_vp8_bool_value(src, hdr, value_bit, ab & BIT(12),
					 ab & BIT(14));
	/*
	 * The seed written here is the *fresh decoder* one (range 255, count 0
	 * or the vendor's 16). A key frame's macroblock data does not start at
	 * a fresh decoder though: the control's coder_state carries the live
	 * (range, value, count) triple left after the bool-coded header, so
	 * bit15/bit16 feed those two fields from the stream instead.
	 */
	w[9] = FIELD_PREP(GENMASK(7, 0),
			  ab & BIT(15) ? hdr->coder_state.range : 255) |
	       FIELD_PREP(GENMASK(21, 16),
			  ab & BIT(16) ? hdr->coder_state.bit_count :
			  (ab & BIT(0) && !(ab & BIT(13)) ? 16 : 0));

	part_bits = seg_end - seg_bit;
	/*
	 * `histb_vdec_seg_pack()` adds the 16-byte granule's own bits to the
	 * bit offset, so the caller passes only the bit position inside the
	 * byte: the frame's bit_offset field is seven bits wide and a
	 * double-added offset silently truncates to zero, which was confirmed
	 * by dumping the message slot through the debugfs pool window.
	 */
	histb_vdec_seg_pack(seg3, stream_base, src_addr + seg_bit / 8,
			    seg_bit % 8, part_bits);
	w[16] = FIELD_PREP(GENMASK(24, 0), seg3[2]) |
		FIELD_PREP(GENMASK(31, 25), seg3[1]);
	w[17] = seg3[0];

	if (ab & BIT(17)) {
		/*
		 * The vendor firmware's own seed, as traced through its syntax
		 * decoder: the bool state handed to the MB layer is live, not
		 * fresh. With k = the bit length of the parsed header, count =
		 * 8 + ((8 - k % 8) % 8) (so 9..16, never 0), value = the 24
		 * bits at the position its parse stopped at with the low
		 * 16-count bits cleared, and range = the live range the control
		 * carries. The segment then starts count bits before that
		 * position with the length covering them, and the granule
		 * borrow (+128 bit / -16 byte) applies when count exceeds the
		 * in-granule bit offset; all as the firmware's decoder and
		 * message packer compute them.
		 */
		unsigned int k = hdr->first_part_header_bits;
		unsigned int cnt = 8 + ((8 - k % 8) % 8);
		unsigned int anchor = ab & BIT(18) ?
			mb_bit + cnt + 8 : mb_bit;
		unsigned int granule = (unsigned int)(src_addr -
						     (stream_base & ~15u)) +
				       anchor / 8;
		unsigned int bit_off = (anchor % 8) + 8 * (granule & 15u);
		unsigned int byte_off = granule & ~15u;
		unsigned int len = ((V4L2_VP8_FRAME_IS_KEY_FRAME(hdr) ? 10 : 3) +
				    fps) * 8 - anchor + cnt;

		if (cnt > bit_off) {
			bit_off += 128;
			byte_off -= 16;
		}
		/*
		 * The granule borrow had nothing to borrow here: byte_off
		 * counts 16-byte granules from the stream base, so under 16 it
		 * wraps and the control segment would sit before the plane,
		 * which the VDM would read - raw addresses, no IOMMU. Serving
		 * it needs 16 bytes of this buffer before the plane.
		 */
		if (byte_off > 0xffffffu)
			return -EINVAL;
		bit_off -= cnt;

		w[8] = (histb_vdec_vp8_bool_value(src, hdr, mb_bit, true,
						  false) & 0xffffffu) &
		       ~((1u << (16 - cnt)) - 1);
		w[9] = FIELD_PREP(GENMASK(7, 0), hdr->coder_state.range) |
		       FIELD_PREP(GENMASK(21, 16), cnt);
		w[16] = FIELD_PREP(GENMASK(24, 0), len) |
			FIELD_PREP(GENMASK(31, 25), bit_off);
		w[17] = byte_off;
	}
	if (histb_vdec_vp8_d8 >= 0)
		w[8] = histb_vdec_vp8_d8;
	if (histb_vdec_vp8_d9 >= 0)
		w[9] = histb_vdec_vp8_d9;

	/* D20/D21: per-segment quantiser and loop filter deltas. */
	for (i = 0; i < 4; i++) {
		w[20] |= (u8)seg->quant_update[i] << (8 * i);
		w[21] |= (u8)seg->lf_update[i] << (8 * i);
	}

	/* D22/D23: reference frame and macroblock mode loop filter deltas.
	 * D24: reference frame sign biases, intra and last are always 0.
	 */
	for (i = 0; i < 4; i++) {
		w[22] |= (u8)lf->ref_frm_delta[i] << (8 * i);
		w[23] |= (u8)lf->mb_mode_delta[i] << (8 * i);
	}
	if (hdr->flags & V4L2_VP8_FRAME_FLAG_SIGN_BIAS_GOLDEN)
		w[24] |= BIT(16);
	if (hdr->flags & V4L2_VP8_FRAME_FLAG_SIGN_BIAS_ALT)
		w[24] |= BIT(24);

	/* D32-D35: current (reconstruction) and reference frames.
	 * D36-D40: SED/PMV/RCN band-line scratches, probability table and
	 * DBLK scratch.
	 */
	w[32] = dst_addr & ~15U;
	w[33] = refs[2] & ~15U;
	w[34] = refs[1] & ~15U;
	w[35] = refs[0] & ~15U;
	w[36] = (histb_vdec_tables_dma(pool) +
		 HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_SED_TOP)) & ~15U;
	w[37] = (histb_vdec_tables_dma(pool) +
		 HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_PMV_TOP)) & ~15U;
	w[38] = (histb_vdec_tables_dma(pool) +
		 HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_RCNTOP)) & ~15U;
	w[39] = (histb_vdec_tables_dma(pool) + HISTB_TABLE_OFF_VP8_TAB) & ~15U;
	w[40] = (histb_vdec_tables_dma(pool) +
		 HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_DBLK_TOP)) & ~15U;
	/* D25 is the segment-id map address, the map the vendor firmware builds
	 * while it decodes; the driver left it zero, which points the engine at
	 * stream base 0.
	 */
	if (ab & BIT(6))
		w[25] = (histb_vdec_tables_dma(pool) +
			 HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_SAO_TOP)) & ~15U;
	histb_vdec_vp8_pack_table(histb_vdec_tables_base(pool) +
				  HISTB_TABLE_OFF_VP8_TAB, hdr, seg, ab);

	/*
	 * D63 chain head and D64+ DCT partition descriptors: four words
	 * per partition, (bit_len[24:0] | bit_offset[31:25]) and a stream
	 * base relative byte offset, second segment pair unused. The
	 * partition data starts after the control partition and the
	 * 3-byte per-partition size list (hantro_g1_vp8_dec.c). Behaviour
	 * with more than two token partitions has not been verified on the
	 * hardware.
	 */
	w[63] = (histb_vdec_pic_slot_dma(pool) + 256) & ~15U;
	part_off = (V4L2_VP8_FRAME_IS_KEY_FRAME(hdr) ? 10 : 3) +
		   fps + 3 * (num_parts - 1);
	/*
	 * The descriptors hand raw physical addresses to the VDM; every
	 * partition has to stay inside the queued payload, or the hardware
	 * would read outside the plane (no IOMMU on this SoC).
	 */
	if (part_off + num_parts * 3 >
	    vb2_get_plane_payload(&src->vb2_buf, 0))
		return -EINVAL;
	/*
	 * The uncompressed chunk is the 10/3 bytes *after* the 3-byte frame
	 * tag, so the tag has to be counted once by hand wherever an offset is
	 * taken from the start of the payload (bit9, and bit0 already carries
	 * it in the control window).
	 */
	/*
	 * Offsetting the partition data by the 3-byte frame tag (bit9) makes
	 * every job run past the payload: the frame's own sizes add up to the
	 * queued length without it, so this driver's payload starts at the
	 * uncompressed chunk and the control window above must not add the tag
	 * either.
	 */
	if (ab & BIT(9))
		part_off += 3;
	for (i = 0; i < num_parts && i < 8; i++) {
		u32 *d = w + 64 + 4 * i;
		unsigned int bytes;

		/*
		 * The last partition has no size field in the frame header (the
		 * 3-byte list covers the first num_parts-1 ones), so the vendor
		 * takes the rest of the buffer; the uAPI's slot for it is
		 * usually zero, which used to ship a zero-length last
		 * partition.
		 */
		bytes = (ab & BIT(4)) && i == num_parts - 1 ?
			vb2_get_plane_payload(&src->vb2_buf, 0) - part_off :
			hdr->dct_part_sizes[i];
		/*
		 * RFC 6386 carries each token partition's size, except the
		 * last, as a 3-byte field, so a mis-stated one asks the engine
		 * for bytes outside the plane; the descriptors hold raw
		 * physical addresses and this SoC has no IOMMU. Clipping the
		 * descriptor would decode a truncated partition.
		 */
		if (part_off + bytes > vb2_get_plane_payload(&src->vb2_buf, 0))
			return -EINVAL;

		histb_vdec_seg_pack(seg3, stream_base, src_addr + part_off, 0,
				    bytes * 8);
		d[0] = FIELD_PREP(GENMASK(24, 0), seg3[2]) |
		       FIELD_PREP(GENMASK(31, 25), seg3[1]);
		d[1] = seg3[0];
		part_off += bytes;
	}

	/* Per-frame register set, as the vendor firmware programs it for VP8;
	 * video standard 12 in CFG1, see HISTB_VP8_HW_STD. Head information
	 * offset is the tile row height of this protocol.
	 */
	ddr_stride = ab & BIT(7) ? stride * 16 : stride;
	band_level = mb_width <= 128 ? 16 : mb_width <= 256 ? 32 :
		     mb_width <= 384 ? 48 : 64;
	head_inf = ab & BIT(3) ?
		   band_level * ALIGN(mb_height * 16, 32) :
		   16 * ALIGN(ALIGN(height, 16), 32) / 32;
	uv_off = ab & BIT(3) ?
		 stride * ALIGN(ALIGN(height, 16), 32) + head_inf :
		 stride * ALIGN(height, 16);
	vdm_write(dev, VDM_CFG0, FIELD_PREP(VDM_CFG0_MBAMT, mb_total - 1));
	vdm_write(dev, VDM_CFG1,
		  FIELD_PREP(VDM_CFG1_VID_STD, HISTB_VP8_HW_STD) |
		  FIELD_PREP(VDM_CFG1_DDR_STRIDE, ddr_stride >> 6) |
		  (ab & BIT(2) ? VDM_CFG1_FST_SLC_GRP | VDM_CFG1_MV_OUT_EN |
				VDM_CFG1_DBLK_1D_EN : 0));
	vdm_write(dev, VDM_AVM_ADDR, pool->dma & ~15U);
	vdm_write(dev, VDM_VAM_ADDR, histb_vdec_up_msgs_dma(pool) & ~15U);
	vdm_write(dev, VDM_STREAM_BASE, stream_base);
	vdm_write(dev, VDM_YSTADDR_1D, dst_addr & ~15U);
	vdm_write(dev, VDM_YSTRIDE_1D, ddr_stride);
	vdm_write(dev, VDM_UVOFFSET_1D, uv_off);
	/* Head information offset: one 16-byte row per 32-frame-row block. The
	 * vendor also points PPFD at the message-pool block.
	 */
	vdm_write(dev, VDM_HEAD_INF_OFFSET, head_inf);
	vdm_write(dev, VDM_PPFD_BUF_ADDR,
		  (histb_vdec_tables_dma(pool) + HISTB_TABLE_OFF_PPFD) & ~15U);
	vdm_write(dev, VDM_PPFD_BUF_LEN, HISTB_VDEC_PPFD_SIZE / 64 - 1);

	if (!(ab & BIT(8))) {
		for (i = 1; i <= 7; i++)
			vdm_write(dev, VDM_TO(i), VDM_TIMEOUT_MOD);
	}
	vdm_write(dev, VDM_INT_STATE, 0xffffffff);
	vdm_write(dev, VDM_INT_MASK, 0xfffffffe);

	histb_vdec_hw_kick(dev, ctx->desc->vidstd);

	/* Carry this frame's filter and frame type into the next one. */
	st->last_frame_type = V4L2_VP8_FRAME_IS_KEY_FRAME(hdr) ? 0 : 1;
	st->last_filter_type = !!(lf->flags & V4L2_VP8_LF_FILTER_TYPE_SIMPLE);
	st->last_sharpness = lf->sharpness_level;

	return 0;
}

static int histb_vdec_vp8_start(struct histb_vdec_ctx *ctx)
{
	struct histb_vdec_vp8_state *st;

	st = kzalloc_obj(*st, GFP_KERNEL);
	if (!st)
		return -ENOMEM;

	ctx->priv = st;

	return 0;
}

static void histb_vdec_vp8_stop(struct histb_vdec_ctx *ctx)
{
	kfree(ctx->priv);
	ctx->priv = NULL;
}

static const struct histb_vdec_codec_ops histb_vdec_vp8_ops = {
	.start		= histb_vdec_vp8_start,
	.stop		= histb_vdec_vp8_stop,
	.run		= histb_vdec_vp8_run,
};

/*
 * Kept as the implementation direction but not in histb_vdec_fmt_list[]: the
 * macroblock-layer seed this hardware needs (a live 24-bit bool-decoder residue
 * and a symbol-history-dependent count) is not transportable through the
 * current stateless VP8 control, whose coder_state value field is only 8 bits
 * wide, so registering it would only ever produce zero-residual pictures. Doing
 * it properly needs the control to carry the live coder value the vendor keeps
 * refilling as the decoder consumes, together with the count its header parse
 * stops at.
 */
const struct histb_vdec_fmt_desc histb_vdec_vp8_fmt = {
	.fourcc		= V4L2_PIX_FMT_VP8_FRAME,
	.stride		= histb_vdec_tile_stride,
	.grid_align	= 16,
	.num_ctrls	= ARRAY_SIZE(histb_vdec_vp8_ctrls),
	.ctrls		= histb_vdec_vp8_ctrls,
	.ops		= &histb_vdec_vp8_ops,
	.vidstd		= HISTB_VIDSTD_VP8,
};
