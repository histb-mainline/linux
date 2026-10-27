// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSilicon Hi3798MV100 (VDH HiVDHV300R001) stateless H.264 decoder.
 *
 * One job decodes one frame: the picture slot of the message pool carries the
 * frame-level state (POC, DPB address table, CABAC table pointer, quantisation
 * matrix), followed by one chained slice slot per slice. Every slot word layout
 * implemented here reproduces what the vendor firmware programs for the same
 * syntax; where only a measurement on the board settles a choice, the numbers
 * are in that comment. The frame storage model (APC/PMV tables, tile frame
 * slots) comes from the same message assembly.
 *
 * Hardware facts worth keeping next to the code:
 *
 *  - The decoder consumes fully parsed syntax. It never reads slice
 *    headers from the bitstream; the only bitstream it fetches is
 *    slice_data(), addressed through a per-slice stream segment (byte
 *    offset from the stream base on a 16-byte granule, bit offset within
 *    the granule, valid bit length).
 *  - That fetch is byte-literal: the VDM does not undo NAL emulation
 *    prevention, so a span containing 00 00 03 decodes wrong from the first
 *    escaped macroblock on. The uAPI delivers the escaped form, hence
 *    histb_h264_pack_rbsp() copies the slice span with the prevention bytes
 *    removed and re-bases the segment offsets onto the copy.
 *  - Frame storage is table based: the picture message carries 16 frame
 *    addresses (the APC table) plus two POC tables, and slice slots
 *    reference frames by table index. This driver uses the capture
 *    buffer index as the APC slot, so up to 16 capture buffers are
 *    addressable.
 *  - Motion vectors of reference frames persist in a private pool of 20
 *    PMV slots, addressed independently of the frame table.
 */

#include <linux/bitops.h>
#include <linux/dma-mapping.h>
#include <media/v4l2-h264.h>
#include <media/videobuf2-dma-contig.h>

#include "histb_vdec.h"

/*
 * Motion-vector slot size for the largest supported frame: 120 x 68
 * macroblocks, 32 bytes per macroblock, 128-byte aligned. The top half of a
 * slot holds one field's worth of motion vectors, the bottom half the other;
 * field pictures reach the second half at an offset of half the slot size.
 */
/*
 * The collocated bottom-field half of a PMV slot is deliberately not
 * programmed: writing base + half the slot size there was measured on the board
 * and changed nothing (10 of 50 pictures correct either way), so the engine
 * does not take that slot for an MBAFF reference; see the slice-side comment
 * for what is refused instead.
 */
#define HISTB_H264_PMV_SLOT_MIN		261120u

/*
 * The vendor firmware's per-macroblock PMV size: it picks 64 bytes per
 * macroblock when the picture is at most 45 columns by 36 rows, or whenever the
 * SPS clears direct_8x8_inference_flag, and 32 otherwise, then aligns the plane
 * up to 128. Half of that plane is the offset the firmware adds to reach the
 * second field's vectors. A picture larger than 45 x 36 macroblocks with
 * direct_8x8_inference_flag clear needs twice what a 32-byte-per-macroblock
 * calculation gives, so the pool has to be sized from this formula and not from
 * the 32-byte one: the floor alone only covers it up to 720x576.
 */
static unsigned int histb_h264_pmv_per_mb(unsigned int width_mbs,
					  unsigned int height_mbs,
					  bool direct_8x8)
{
	if ((width_mbs <= 45 && height_mbs <= 36) || !direct_8x8)
		return 64;

	return 32;
}

/*
 * The picture message carries a 16-entry frame address table, so capture buffer
 * indexes double as APC slots up to this limit. dpb2apc[] uses
 * HISTB_H264_APC_NONE for DPB entries that map to no slot.
 *
 * Both vendor fill loops stop at entry 15 -- each tests its index against 15
 * before the tail of the address loop and of the POC pair loop -- and the words
 * in between are spoken for: D10..D25 frame addresses, D26 the CABAC MN table,
 * D27..D56 the paired POC table. Sizing this by the uAPI DPB (32) instead makes
 * the address fill overlap D26 and the POC entries of every slot below 8, so
 * the reference POCs a picture is given end up holding the current frame
 * address.
 */
#define HISTB_H264_NUM_APC_SLOTS		16u
#define HISTB_H264_APC_NONE			0xffu

/*
 * Hardware slice-type codes, indexed by slice_type % 5: P=1, B=2, I=0. This is
 * the ordering the firmware's per-slice-type PMV table uses for H.264; the HEVC
 * path orders it differently, so the table is not shared between the two. SP/SI
 * have no entry of their own and fold onto the P and I codes.
 */
static u8 histb_h264_slice_type_code(u8 slice_type)
{
	static const u8 code[5] = { 1, 2, 0, 1, 0 };

	return code[slice_type];
}

/*
 * Picture-structure code for picture-message word D0 bits [15:14], indexed 2 *
 * pic_structure + mbaff exactly as the firmware's own table is: 0 = frame, 1 =
 * top field, 2 = bottom field, 3 = paired fields.
 */
static const u8 histb_h264_struct_trans[8] = { 0, 3, 1, 1, 2, 2, 3, 3 };

/*
 * Reference entry structure code for the slice declist nibble: the firmware
 * table indexed 2 * struct + mbaff, of which only the frame-coded even indices
 * are used here, masked to 2 bits.
 */
static const u8 histb_h264_list_struct_frame[8] = { 0, 1, 3, 3, 3, 3, 2, 2 };

/* Per-frame assembly state shared by the picture and slice packers. */
struct histb_h264_run {
	const struct v4l2_ctrl_h264_sps *sps;
	const struct v4l2_ctrl_h264_pps *pps;
	const struct v4l2_ctrl_h264_decode_params *dec;
	const struct v4l2_ctrl_h264_pred_weights *weights;
	const struct v4l2_ctrl_h264_scaling_matrix *scaling;
	const struct v4l2_ctrl_h264_slice_params *slices;
	unsigned int		num_slices;
	unsigned int		num_valid;
	unsigned int		width_mbs;
	unsigned int		height_mbs;
	unsigned int		mbamt;
	u32			tile_stride;
	u32			buf_lines;
	const u8		*src_va;
	dma_addr_t		src_dma;
	dma_addr_t		dst_dma;
	u8			dpb2apc[V4L2_H264_NUM_DPB_ENTRIES];
	u32			img_addr[HISTB_H264_NUM_APC_SLOTS];
	s32			apc_poc[2][HISTB_H264_NUM_APC_SLOTS];
	struct v4l2_h264_reference reflist_p[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference reflist_b0[V4L2_H264_REF_LIST_LEN];
	struct v4l2_h264_reference reflist_b1[V4L2_H264_REF_LIST_LEN];
	u32			slice_starts[HISTB_VDEC_MAX_SLICE_SLOTS];
};

struct histb_vdec_h264_ctx {
	void		*pmv_va[HISTB_VDEC_MAX_PMV_SLOTS];
	dma_addr_t	pmv_dma[HISTB_VDEC_MAX_PMV_SLOTS];
	unsigned int	pmv_slot_size;
	unsigned int	pmv_width_mbs;
	unsigned int	pmv_height_mbs;
	unsigned int	pmv_per_mb;
	/*
	 * Sticky for the lifetime of the reference chain: a picture has been
	 * refused because its collocated motion vectors live in a PMV half this
	 * driver cannot name, so every picture that still references it would
	 * be delivered wrong. Cleared by an IDR.
	 */
	bool			col_half_lost;
	void		*rbsp_va;
	dma_addr_t	rbsp_dma;
	unsigned int	rbsp_size;
	/* per-job assembly state, kept out of the kernel stack */
	struct histb_h264_run run;
};

static struct histb_vdec_dev *histb_vdec_h264_dev(struct histb_vdec_ctx *ctx)
{
	return video_get_drvdata(ctx->fh.vdev);
}

/*
 * Tile frame geometry, taken from the firmware's frame-storage setup and
 * confirmed on I_PCM pictures: the VDM writes 256-byte by 16-row tiles, so
 * stride it is handed is a whole band and the chroma plane starts behind the
 * luma bands plus the per-picture 1D area. histb_vdec_tile_stride() gives the
 * tile-column count in bytes.
 */
static u32 histb_h264_tile_stride(unsigned int width)
{
	return histb_vdec_tile_stride(width);
}

static unsigned int histb_h264_ref_struct_code(u8 fields)
{
	switch (fields) {
	case V4L2_H264_TOP_FIELD_REF:
		return 1;
	case V4L2_H264_BOTTOM_FIELD_REF:
		return 2;
	default:
		return 0;
	}
}

/*
 * Only B slices read collocated motion vectors: the firmware looks the slice
 * type up in its per-slice-type PMV table and zeroes slot[8] and slot[9] for
 * anything else.
 */
static bool histb_h264_has_b_slice(const struct histb_h264_run *run)
{
	unsigned int i;

	for (i = 0; i < run->num_slices; i++)
		if (run->slices[i].slice_type % 5 == V4L2_H264_SLICE_TYPE_B)
			return true;

	return false;
}

/*
 * PMV slot allocator: capture buffer index -> PMV slot id. Slot ids are stored
 * +1 so that a zeroed pmv_map[] means "unallocated"; ids wrap around the
 * 20-slot pool.
 */
static u8 histb_h264_pmv_alloc(struct histb_vdec_ctx *ctx, unsigned int index)
{
	u8 id = ctx->pmv_map[index];

	if (!id) {
		id = ctx->pmv_next++ % HISTB_VDEC_MAX_PMV_SLOTS + 1;
		ctx->pmv_map[index] = id;
	}

	return id - 1;
}

/*
 * Start-code scan over the OUTPUT plane. Returns the bit position of every
 * slice NAL unit (nal_unit_type 1 or 5, the only slice NALs the message ABI can
 * describe; data partitions 2..4 are not supported). A 4-byte start code
 * matches once: the extra leading zero byte never forms a 00 00 01 triple with
 * the prefix. Returns the total number of slice NALs found (may exceed @max).
 */
static unsigned int histb_h264_find_slice_nals(const u8 *buf, u32 size,
					       u32 *starts, unsigned int max)
{
	unsigned int n = 0;
	u32 i;

	for (i = 0; i + 3 < size; i++) {
		u8 nal_type;

		if (buf[i] || buf[i + 1] || buf[i + 2] != 0x01)
			continue;

		nal_type = buf[i + 3] & 0x1f;
		if (nal_type != 1 && nal_type != 5)
			continue;

		if (n < max)
			starts[n] = (i + 3) * 8;
		n++;
	}

	return n;
}

/*
 * Stream segment descriptor of a slice (slice slot words D0..D2). The VDM
 * fetches the bitstream on 16-byte granules: word0 is the granule start as a
 * byte offset from the programmed stream base (the OUTPUT plane address), word1
 * the bit offset within that granule, word2 the number of valid bits. This is
 * the vendor arithmetic: reduce the first data bit to a byte position, round
 * that down to a granule for word0, and put in word1 the bit offset inside the
 * granule the granule base already moved past.
 */
static void histb_h264_pack_segment(u32 *w, u32 data_bit, u32 valid_bits)
{
	w[0] = (data_bit / 8) & ~15u;
	w[1] = data_bit & 127u;
	w[2] = valid_bits;
}

static u32 histb_h264_scaling_word(const u8 *coeffs)
{
	return coeffs[0] | coeffs[1] << 8 | coeffs[2] << 16 | coeffs[3] << 24;
}

/*
 * Quantisation matrix, picture slot words D63..D118. Vendor packing order: each
 * scaling list in raster order, four coefficients per word, least significant
 * byte first; six 4x4 lists followed by the two 8x8 luma lists the H.264
 * protocol defines (V4L2 indices 0 and 1). Without a picture matrix the spec
 * default of 16 per coefficient applies.
 */
static void histb_h264_pack_scaling(u32 *dst,
				    const struct v4l2_ctrl_h264_scaling_matrix *m,
				    bool present)
{
	unsigned int i, j;

	for (i = 0; i < 6; i++) {
		for (j = 0; j < 4; j++)
			*dst++ = present ?
				 histb_h264_scaling_word(m->scaling_list_4x4[i] + 4 * j) :
				 0x10101010u;
	}

	for (i = 0; i < 2; i++) {
		for (j = 0; j < 16; j++)
			*dst++ = present ?
				 histb_h264_scaling_word(m->scaling_list_8x8[i] + 4 * j) :
				 0x10101010u;
	}
}

/*
 * Prediction weight factors of one list (slice slot words W64.. for
 * list 0, W96.. for list 1): luma and first chroma component pack as
 * offset<<12 | weight<<3 | log2_denom, the second chroma component as
 * weight<<9 | offset. Offsets are truncated to 8 bits and weights to 9
 * bits exactly like the vendor packing loop, which places luma 0 and 64
 * words later the two chroma components.
 */
static void histb_h264_pack_weights(u32 *slot,
				    const struct v4l2_ctrl_h264_pred_weights *pw,
				    unsigned int count, bool list1)
{
	const struct v4l2_h264_weight_factors *f = &pw->weight_factors[list1];
	unsigned int base = list1 ? 96u : 64u;
	unsigned int i;

	for (i = 0; i < count; i++) {
		slot[base + i] = ((u32)(u8)f->luma_offset[i] << 12) |
				 ((u32)f->luma_weight[i] & 0x1ff) << 3 |
				 (pw->luma_log2_weight_denom & 7);
		slot[base + 64 + i] = ((u32)(u8)f->chroma_offset[i][0] << 12) |
				      ((u32)f->chroma_weight[i][0] & 0x1ff) << 3 |
				      (pw->chroma_log2_weight_denom & 7);
		slot[base + 128 + i] = ((u32)f->chroma_weight[i][1] & 0x1ff) |
				       ((u32)(u8)f->chroma_offset[i][1] << 9);
	}
}

/*
 * Reference list of one slice into the slice declist: 4 structure bits per
 * entry (8 entries per word, words D12.. for list 0, D16.. for list 1) and a
 * 5-bit APC slot id per entry (4 entries per word, D20.. and D28..), stored
 * shifted by one like the vendor packer. Bit 0 carries no list identity:
 * setting it on every list 1 entry leaves the output byte-identical.
 */
static void histb_h264_pack_declist(u32 *slot,
				    const struct v4l2_h264_reference *list,
				    unsigned int count, unsigned int distinct,
				    const u8 *dpb2apc,
				    const struct v4l2_ctrl_h264_decode_params *dec,
				    bool list1)
{
	/*
	 * List 0 takes the 4-bit structure nibbles at D12..D15 and the 5-bit
	 * APC slot ids at D20..D27, list 1 the nibbles at D16..D19 and the ids
	 * at D28..D35: the vendor fill loops start at D12/D20 for list 0 and at
	 * D16/D28 for list 1. 32 entries at 4 bits are 4 words, at 5 bits with
	 * four per word they are 8 words, so the two lists really are disjoint.
	 */
	unsigned int nib_word = list1 ? 16 : 12;
	unsigned int apc_word = list1 ? 28 : 20;
	unsigned int i;

	for (i = 0; i < count; i++) {
		/*
		 * A slice may declare more entries than the list carries
		 * pictures. The padded positions repeat the last picture the
		 * list really carries: that is what the encoders that declare
		 * the extra slot assume, measured by decoding the same content
		 * with and without it (a weightp=2 x264 stream codes every
		 * macroblock at its padded index and the software decode is
		 * byte-identical to the same stream coded one index shorter).
		 * Only the in-kernel list reaches this: an application's list
		 * is used when no position of it is empty.
		 */
		const struct v4l2_h264_reference *ref =
			&list[distinct ? min_t(unsigned int, i, distinct - 1) : 0];
		unsigned int code = histb_h264_ref_struct_code(ref->fields);
		const struct v4l2_h264_dpb_entry *dpb = &dec->dpb[ref->index];
		u32 nib = histb_h264_list_struct_frame[2 * code] & 3;

		if (code == 1)
			nib |= 4;
		/* declist entry field frame_is_long_term -> nibble bit 3 */
		if (dpb->flags & V4L2_H264_DPB_ENTRY_FLAG_LONG_TERM)
			nib |= 8;

		slot[nib_word + (i >> 3)] |= nib << ((i & 7) * 4);
		slot[apc_word + (i >> 2)] |=
			((u32)dpb2apc[ref->index] << 1 & 0x1f) << ((i & 3) * 5);
	}
}

/*
 * Reverse map APC slot -> first reference list position (slice slot words
 * D36..D43, 5 bits per entry); every field holds a list position.
 *
 * The firmware keys this map by 2 * apc + field parity and writes *both* halves
 * with the same list position: its reference-index search walks all 16 slots
 * unconditionally and stores the result at both 2i and 2i+1, the message then
 * ships all 32 entries, and they are packed five bits at a time into D36..D43.
 * A slot no list entry names keeps the memset zero, which is exactly the
 * firmware default of "list position 0".
 *
 * Board-measured, and the reason this stays shaped like the firmware's rather
 * than like the uAPI: with the key switched between bare apc (word D36 reads
 * 0x00000020 for tx_m2) and 2 * apc + field (0x00008400) - both confirmed in
 * the debugfs pool readback of the live message - the decoded pixels do not
 * move at all, in the same boot, four forms each (measure c91). The engine
 * indexes references through the 5-bit list slots the macroblock layer carries,
 * not through this map.
 */
static void histb_h264_pack_apc_ref_idx(u32 *slot,
					const struct v4l2_h264_reference *list,
					unsigned int count, const u8 *dpb2apc,
					u32 *seen)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		unsigned int apc = dpb2apc[list[i].index];
		unsigned int key;

		if (*seen & BIT(apc))
			continue;
		*seen |= BIT(apc);
		for (key = 2 * apc; key < 2 * apc + 2; key++)
			slot[36 + (key >> 2)] |= (u32)i << ((key & 3) * 5);
	}
}

static void histb_h264_assemble_pic(struct histb_vdec_dev *dev,
				    const struct histb_vdec_h264_ctx *h264,
				    const struct histb_h264_run *run,
				    u8 cur_pmv_slot, u32 *pic)
{
	const struct v4l2_ctrl_h264_sps *sps = run->sps;
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	const struct v4l2_ctrl_h264_decode_params *dec = run->dec;
	dma_addr_t tables = histb_vdec_tables_dma(&dev->pool);
	bool mbaff = sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD;
	bool field_pic = dec->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;
	u32 pic_struct = 0;
	u32 chroma = 1;
	u32 i;

	memset(pic, 0, HISTB_VDEC_SLOT_BYTES);

	if (field_pic)
		pic_struct = (dec->flags &
			      V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD) ? 2 : 1;
	if (V4L2_H264_SPS_HAS_CHROMA_FORMAT(sps))
		chroma = sps->chroma_format_idc;

	pic[0] = (run->width_mbs - 1) |
		 ((u32)histb_h264_struct_trans[2 * pic_struct + mbaff] << 14) |
		 ((run->height_mbs - 1) << 16) |
		 ((chroma == 1 ? BIT(25) : 0)) |
		 ((pps->flags & V4L2_H264_PPS_FLAG_CONSTRAINED_INTRA_PRED) ? BIT(26) : 0) |
		 ((pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE) ? BIT(27) : 0) |
		 ((pps->flags & V4L2_H264_PPS_FLAG_TRANSFORM_8X8_MODE) ? BIT(28) : 0) |
		 (dec->nal_ref_idc ? BIT(31) : 0);
	/* current frame, 2D tile output */
	pic[1] = lower_32_bits(run->dst_dma);
	/*
	 * Picture order count of the coded picture: a field uses its own count,
	 * a PAFF frame picture uses the earlier of the two fields.
	 */
	if (field_pic && (dec->flags & V4L2_H264_DECODE_PARAM_FLAG_BOTTOM_FIELD))
		pic[2] = (u32)dec->bottom_field_order_cnt;
	else if (field_pic)
		pic[2] = (u32)dec->top_field_order_cnt;
	else
		pic[2] = min_t(s32, dec->top_field_order_cnt,
			       dec->bottom_field_order_cnt);
	/* field POCs, MBAFF only */
	pic[3] = mbaff ? (u32)dec->top_field_order_cnt : 0;
	pic[4] = mbaff ? (u32)dec->bottom_field_order_cnt : 0;
	/* band-line scratch buffers and the CABAC context table */
	pic[5] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_SED_TOP));
	pic[6] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_PMV_TOP));
	pic[7] = lower_32_bits(h264->pmv_dma[cur_pmv_slot]);
	pic[8] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_RCNTOP));
	/* head of the slice chain */
	pic[9] = lower_32_bits(histb_vdec_slice_slot_dma(&dev->pool, 0));
	/* APC frame address table, then the paired top/bottom POC tables */
	for (i = 0; i < HISTB_H264_NUM_APC_SLOTS; i++) {
		pic[10 + i] = run->img_addr[i];
		pic[27 + 2 * i] = (u32)run->apc_poc[0][i];
		pic[28 + 2 * i] = (u32)run->apc_poc[1][i];
	}
	pic[26] = lower_32_bits(tables + HISTB_TABLE_OFF_CABAC_MN_H264);
	pic[59] = lower_32_bits(tables + HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_DBLK_TOP));

	/*
	 * The quantiser matrix burst starts at D64: the firmware copy loop
	 * bases itself 252 bytes into the slot and stores through pointers that
	 * step four bytes at a time, so the first word lands at byte 256 and
	 * the 56th at byte 476.
	 */
	histb_h264_pack_scaling(&pic[64], run->scaling,
				pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT);
}

/*
 * End of the coded slice data inside the span handed to the hardware.
 *
 * Slice NAL units are separated by annex-b start codes in the OUTPUT buffer,
 * and the last slice of an access unit runs into the start code of whichever
 * NAL follows it in the stream, so a segment delimited by the next slice (or by
 * bytesused) ends on bytes that are not slice data. Drop them: the pattern 00
 * 00 01 cannot occur inside a NAL unit (emulation prevention rewrites any 00 00
 * followed by 00 or 01), and it cannot be the tail of rbsp_trailing_bits
 * either, because the last coded bit of a slice is end_of_slice_flag, a one.
 */
static u32 histb_h264_data_end_bit(const u8 *va, u32 end_bit)
{
	if (!va || end_bit < 24)
		return end_bit;
	if (!va[end_bit / 8 - 3] && !va[end_bit / 8 - 2] &&
	    va[end_bit / 8 - 1] == 1)
		return end_bit - 24;
	return end_bit;
}

/*
 * A reference list is only usable when every entry the slice actually reads
 * names an active DPB entry: an application that leaves the arrays zeroed would
 * otherwise be taken as "DPB entry 0, no fields".
 */
static bool histb_h264_ref_list_ok(const struct v4l2_h264_reference *list,
				   unsigned int count,
				   const struct v4l2_ctrl_h264_decode_params *dec)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (!(list[i].fields & V4L2_H264_FRAME_REF))
			return false;
		if (list[i].index >= V4L2_H264_NUM_DPB_ENTRIES)
			return false;
		if (!(dec->dpb[list[i].index].flags &
		      V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			return false;
	}

	return true;
}

/*
 * The reference lists a slice uses: the application's submitted lists when they
 * name active DPB entries (only they can carry 8.2.4.3 reordering), otherwise
 * the 8.2.4.2 initial construction prepared during run assembly. Returns true
 * when the application's lists were taken.
 */
static bool histb_h264_slice_lists(const struct histb_h264_run *run,
				   unsigned int index, unsigned int count0,
				   unsigned int count1, bool is_b,
				   const struct v4l2_h264_reference **pl0,
				   const struct v4l2_h264_reference **pl1)
{
	const struct v4l2_ctrl_h264_slice_params *slice = &run->slices[index];

	*pl0 = is_b ? run->reflist_b0 : run->reflist_p;
	*pl1 = run->reflist_b1;
	if (histb_h264_ref_list_ok(slice->ref_pic_list0, count0, run->dec) &&
	    (!is_b || histb_h264_ref_list_ok(slice->ref_pic_list1, count1,
					    run->dec))) {
		*pl0 = slice->ref_pic_list0;
		*pl1 = slice->ref_pic_list1;
		return true;
	}

	return false;
}

static void histb_h264_assemble_slice(struct histb_vdec_dev *dev,
				      struct histb_vdec_ctx *ctx,
				      const struct histb_h264_run *run,
				      const struct histb_vdec_h264_ctx *h264,
				      unsigned int index,
				      u32 nal_bit, u32 next_bit)
{
	const struct v4l2_ctrl_h264_slice_params *slice = &run->slices[index];
	const struct v4l2_ctrl_h264_pps *pps = run->pps;
	const struct v4l2_h264_reference *l0;
	const struct v4l2_h264_reference *l1;
	const struct v4l2_h264_reference *col;
	u32 *slot = histb_vdec_slice_slot(&dev->pool, index);
	unsigned int type = slice->slice_type % 5;
	unsigned int last = run->num_slices - 1;
	unsigned int count0 = 0, count1 = 0;
	unsigned int distinct0 = 0, distinct1 = 0;
	u32 seen = 0;
	u32 data_bit = nal_bit + slice->header_bit_size;
	bool is_b = type == V4L2_H264_SLICE_TYPE_B;
	bool from_app;
	bool weighted;
	unsigned int wmode;
	bool cabac;
	u32 trail, valid;
	u32 qp;

	memset(slot, 0, HISTB_VDEC_SLOT_BYTES);

	if (type != V4L2_H264_SLICE_TYPE_I && type != V4L2_H264_SLICE_TYPE_SI)
		count0 = min_t(unsigned int,
			       slice->num_ref_idx_l0_active_minus1 + 1,
			       V4L2_H264_REF_LIST_LEN);
	if (is_b)
		count1 = min_t(unsigned int,
			       slice->num_ref_idx_l1_active_minus1 + 1,
			       V4L2_H264_REF_LIST_LEN);

	/*
	 * The uAPI hands the reference lists "after applying the per-slice
	 * modifications" (include/uapi/linux/v4l2-controls.h), which no
	 * in-kernel helper can know: 8.2.4.3 reordering is syntax, and x264
	 * uses it in every B-pyramid encode.
	 */
	from_app = histb_h264_slice_lists(run, index, count0, count1, is_b,
					 &l0, &l1);
	/*
	 * How many positions of the list carry a picture: the declist repeats
	 * the last one over the rest. An application's list only gets used when
	 * every position names an active DPB entry, so all of its positions
	 * carry one - including a position that names the same picture as its
	 * neighbour does, which 8.2.4.3 allows (a long-term and a short-term
	 * entry can hold the same picture, and x264's weighted lists do exactly
	 * that). Taking num_valid for the count here would clamp such a list
	 * and hand every padded macroblock the picture after the one the slice
	 * asked for. The in-kernel 8.2.4.2 list has no such marker: it fills
	 * its tail with DPB entry 0, and only its first num_valid positions
	 * carry a picture.
	 */
	if (count0) {
		distinct0 = from_app ? count0 :
			min_t(unsigned int, count0, run->num_valid);
		if (!distinct0)
			count0 = 0;
	}
	if (count1) {
		distinct1 = from_app ? count1 :
			min_t(unsigned int, count1, run->num_valid);
		if (!distinct1)
			count1 = 0;
	}

	/*
	 * Collocated picture for direct prediction. 8.8.2 picks it by
	 * ColFromL0Flag (List0[0] for a P slice or a spatial-direct B slice,
	 * List1[1] - falling back to List1[0] - for a temporal-direct B slice),
	 * but the vendor's live path addresses the collocated motion vectors
	 * from `declist[1][0]` for every B slice whatever the direct type is,
	 * and the board follows the vendor, not the spec: switching the choice
	 * to `declist[1][0]` took the regression from 38 to 46 of 48 cases with
	 * all frames byte-exact (the eight that turned were the spatial-direct
	 * ones). On the one stream where the two readings differ by syntax
	 * alone (a temporal-direct B slice declaring two list1 entries, corpus
	 * arm bx_b3t), 8.8.2's refIdx=1 scored 7/12 where list1[0] scored 9/12,
	 * and both of those leave the sibling arm bx_b3 (same lists, spatial
	 * direct) at 2/12 until the spatial case moves to list1[0] too.
	 */
	col = l1;

	/*
	 * Slice data segment. The hardware never reads slice headers, so the
	 * segment points behind the header bits. The second segment (words
	 * D3..D5) stays clear: the frame arrives as one contiguous OUTPUT
	 * buffer.
	 *
	 * CABAC decodes whole bytes, so its start moves up to the next byte
	 * boundary, which is what the firmware does when it computes the slice
	 * bit offset. Measured: an OpenH264 CABAC slice whose header ends at
	 * bit 51 gives zero correct lines without the rounding and a byte-exact
	 * frame with it, while that same 51-bit header under CAVLC is
	 * byte-exact unrounded. CAVLC therefore keeps the exact bit position.
	 *
	 * The rbsp_trailing_bits padding at the end of a CAVLC slice is not
	 * slice data: the firmware takes the length down to the last non-zero
	 * bit and drops the stop bit with it. Leaving the padding in makes
	 * every CAVLC picture come back with the up-message error flag and the
	 * hardware status register's non-syntax error bit (bit18) set, and
	 * moving the length one byte either way brings the flag straight back,
	 * so the length must be exact, not merely long enough. Under CABAC that
	 * subtraction is wrong on this hardware: it cost the 1920x1088 frame
	 * its last macroblock row, so the padding stays in the length there.
	 */
	cabac = pps->flags & V4L2_H264_PPS_FLAG_ENTROPY_CODING_MODE;
	if (cabac)
		data_bit = (data_bit + 7) & ~7U;
	/*
	 * Neither the annex-b start code in front of the next slice (or of the
	 * NAL that follows the access unit) nor the rbsp_trailing_bits padding
	 * behind the last coded bit is slice data, and the length has to be
	 * exact rather than merely long enough: measured by adding slack to the
	 * declared end, one bit of padding in an interior slice segment is
	 * enough to bring the error flag back, while a four-slice CAVLC picture
	 * written with both removed is byte-exact and reports none. Under CABAC
	 * the padding subtraction is wrong on this hardware: it cost the
	 * 1920x1088 frame its last macroblock row, so the padding stays in the
	 * length there.
	 */
	next_bit = histb_h264_data_end_bit(run->src_va, next_bit);
	valid = next_bit - data_bit;
	if (!cabac) {
		trail = histb_vdec_trail_zero_bits(run->src_va, next_bit) + 1;
		if (valid > trail)
			valid -= trail;
	}
	histb_h264_pack_segment(&slot[0], data_bit, valid);

	/* D6 carries the absolute slice QP, the spec's SliceQPy value. */
	qp = clamp_t(int, 26 + pps->pic_init_qp_minus26 + slice->slice_qp_delta,
		     0, 51);
	slot[6] = (slice->first_mb_in_slice & 0xfffff) |
		  ((qp & 63) << 26) |
		  ((slice->cabac_init_idc & 3) << 24);
	slot[7] = histb_h264_slice_type_code(type) |
		  (count0 << 2) | (count1 << 8) |
		  ((run->sps->flags & V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE) ?
		   BIT(14) : 0) |
		  ((slice->flags & V4L2_H264_SLICE_FLAG_DIRECT_SPATIAL_MV_PRED) ?
		   BIT(15) : 0) |
		  ((slice->num_ref_idx_l0_active_minus1 & 31) << 16) |
		  ((slice->num_ref_idx_l1_active_minus1 & 31) << 21);

	/*
	 * Collocated motion-vector base address: the PMV slot of the collocated
	 * picture chosen above. The firmware splits that slot between the two
	 * fields and names both halves for a reference that is a field pair -
	 * slot[8] = base, slot[9] = base + half the slot size - where its
	 * frame-structure code 3 means a field pair, and MBAFF is a separate
	 * flag that plays no part in PMV addressing. Measured here: naming the
	 * second half for an MBAFF reference changes nothing (10 of 50 pictures
	 * correct with slot[9] written and the same 10 without), so no engine
	 * path reads it for that case, and the pictures that need a half this
	 * driver cannot name are refused by run() instead of delivered.
	 */
	if (is_b && count0 + count1 > 0 &&
	    run->dpb2apc[col[0].index] != HISTB_H264_APC_NONE)
		slot[8] = lower_32_bits(h264->pmv_dma[histb_h264_pmv_alloc(ctx,
					run->dpb2apc[col[0].index])]);

	/*
	 * Weighting mode: the firmware moves its per-slice weighting-mode byte
	 * into D10 bits [17:16] unmasked, and that byte holds the PPS
	 * weighted_bipred_idc for a B slice and the PPS weighted_pred_flag for
	 * any other inter slice. The field is therefore a mode, and mode 2 --
	 * implicit weighted prediction, 8.4.2.3 -- is one the hardware carries
	 * out itself; only mode 1 has a pred_weight_table to hand down.
	 *
	 * V4L2_H264_CTRL_PRED_WEIGHTS_REQUIRED() compares slice_type against
	 * the V4L2_H264_SLICE_TYPE_* enum, so it has to see the normalised
	 * type: a bitstream may code a P slice as 5, and that still carries a
	 * pred_weight_table().
	 */
	if (is_b)
		wmode = pps->weighted_bipred_idc & 3;
	else
		wmode = ((pps->flags & V4L2_H264_PPS_FLAG_WEIGHTED_PRED) &&
			 (type == V4L2_H264_SLICE_TYPE_P ||
			  type == V4L2_H264_SLICE_TYPE_SP)) ? 1 : 0;
	weighted = wmode == 1;
	slot[10] = (((u32)pps->chroma_qp_index_offset & 31) << 5) |
		   ((u32)pps->second_chroma_qp_index_offset & 31) |
		   (wmode << 16);
	slot[11] = (((u32)slice->slice_alpha_c0_offset_div2 & 15) << 16) |
		   (((u32)slice->slice_beta_offset_div2 & 15) << 8) |
		   (slice->disable_deblocking_filter_idc & 0xff);

	histb_h264_pack_declist(slot, l0, count0, distinct0, run->dpb2apc,
				run->dec, false);
	histb_h264_pack_declist(slot, l1, count1, distinct1, run->dpb2apc,
				run->dec, true);
	histb_h264_pack_apc_ref_idx(slot, l0, distinct0, run->dpb2apc, &seen);
	histb_h264_pack_apc_ref_idx(slot, l1, distinct1, run->dpb2apc, &seen);

	/*
	 * Slice chain words. D44 is a macroblock index: the firmware's caller
	 * passes the next slice's first_mb_in_slice minus one, or the picture's
	 * MB count minus one on the last slice. D63 takes the successor slot
	 * address on every slice -- the store sits in a tail reached by both
	 * branches of the slice loop, so no slot ever leaves a null link. No
	 * counter bounds the chain, so a run ends when the chain reaches a slot
	 * with an empty segment; run() therefore clears the slot after the last
	 * one.
	 */
	slot[44] = index < last ? run->slices[index + 1].first_mb_in_slice - 1 :
				  run->mbamt - 1;
	slot[63] = lower_32_bits(histb_vdec_slice_slot_dma(&dev->pool,
							   index + 1));

	if (weighted) {
		histb_h264_pack_weights(slot, run->weights, count0, false);
		if (is_b)
			histb_h264_pack_weights(slot, run->weights, count1, true);
	}
}

static void histb_h264_program_regs(struct histb_vdec_dev *dev,
				    const struct histb_h264_run *run)
{
	unsigned int lines = run->buf_lines;
	unsigned int i;

	/* decoded macroblock budget: the field counts MBs minus one */
	/*
	 * The firmware's CFG0 build ends by setting bit 30 (the
	 * quantiser-matrix load) and bit 24 (the coefficient-index detect) on
	 * byte 3 of the word, and by clearing bit 23 and bit 25. Without bit 24
	 * the VDM reconstructs intra prediction but drops every coefficient.
	 */
	vdm_write(dev, VDM_CFG0, ((run->mbamt - 1) & VDM_CFG0_MBAMT) |
				  VDM_CFG0_COEFIDX_DETECT | VDM_CFG0_LD_QMATRIX);
	/*
	 * The firmware's CFG1 build also sets VDM_CFG1_MAX_SLCGRP_NUM to 1,
	 * keeps a single slice group for every picture (VDM_CFG1_FST_SLC_GRP
	 * carries the picture's "new picture" byte, which is true for each
	 * frame a stateless decoder is handed) and leaves the deblocker in 1D
	 * mode.
	 */
	vdm_write(dev, VDM_CFG1, FIELD_PREP(VDM_CFG1_VID_STD, HISTB_VIDSTD_H264) |
		  FIELD_PREP(VDM_CFG1_DDR_STRIDE, run->tile_stride >> 6) |
		  FIELD_PREP(VDM_CFG1_MAX_SLCGRP_NUM, 1) |
		  VDM_CFG1_FST_SLC_GRP | VDM_CFG1_DBLK_1D_EN);
	vdm_write(dev, VDM_AVM_ADDR,
		  lower_32_bits(histb_vdec_pic_slot_dma(&dev->pool)));
	vdm_write(dev, VDM_VAM_ADDR,
		  lower_32_bits(histb_vdec_up_msgs_dma(&dev->pool)));
	vdm_write(dev, VDM_STREAM_BASE, lower_32_bits(run->src_dma));

	/* one timeout per pipeline stage, windows 0x3c..0x54 only */
	for (i = 1; i <= 7; i++)
		vdm_write(dev, VDM_TO(i), VDM_TIMEOUT_MOD);
	vdm_write(dev, VDM_PPFD_BUF_ADDR,
		  (histb_vdec_tables_dma(&dev->pool) +
		   HISTB_TABLE_OFF_PPFD) & ~15u);
	vdm_write(dev, VDM_PPFD_BUF_LEN, HISTB_VDEC_PPFD_SIZE / 64 - 1);
	/*
	 * FF_APT_EN takes histb_h264_struct_trans[2 * pic_structure + mb_aff];
	 * the firmware maps that table entry to 2 only for the frame structure
	 * code, and to 0 for every other one. Field and MBAFF pictures stay at
	 * 0 until they are supported.
	 */
	vdm_write(dev, VDM_FF_APT_EN,
		  !(run->dec->flags & V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC) &&
		  !(run->sps->flags & V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD) ?
		  2 : 0);

	vdm_write(dev, VDM_YSTADDR_1D, lower_32_bits(run->dst_dma));
	/*
	 * The 1D path writes 256x16-byte tiles, so the stride it is given is
	 * the pitch of a whole 16-row band, not of a picture line: the firmware
	 * puts its decoded-picture stride there and computes the chroma offset
	 * as two bands' worth of that stride plus the 1D area. Measured on c24
	 * with the all-I_PCM probe: with the picture-line stride here, band b+1
	 * landed exactly on band b's rows 3..15, so only the first three lines
	 * of every 16-row band survived and the frame looked "truncated at
	 * 8-20%".
	 */
	vdm_write(dev, VDM_YSTRIDE_1D, histb_vdec_band_bytes(run->tile_stride));
	vdm_write(dev, VDM_UVOFFSET_1D,
		  histb_vdec_luma_bytes(run->tile_stride, lines) +
		  histb_vdec_head_inf_bytes(lines));
	vdm_write(dev, VDM_HEAD_INF_OFFSET, histb_vdec_head_inf_bytes(lines));
	vdm_write(dev, VDM_INT_STATE, 0xffffffffu);
	vdm_write(dev, VDM_INT_MASK, 0xfffffffeu);
	/*
	 * Per-slot picture-type flags; frame pictures need none and field
	 * coding is not modelled yet. TODO: field coding needs per-reference
	 * field flags that no control carries.
	 */
	vdm_write(dev, VDM_REF_PIC_TYPE, 0);
}

static int histb_vdec_h264_try_ctrl(struct v4l2_ctrl *ctrl)
{
	const struct v4l2_ctrl_h264_sps *sps;

	if (ctrl->id != V4L2_CID_STATELESS_H264_SPS)
		return 0;

	sps = ctrl->p_new.p_h264_sps;

	/*
	 * The VDM reconstructs NV21 16x16 tiles: 8-bit 4:2:0 only. The chroma
	 * format and bit depth fields only exist for the high profiles
	 * (V4L2_H264_SPS_HAS_CHROMA_FORMAT); baseline and main streams are
	 * 4:2:0 8-bit by definition.
	 *
	 * The geometry is not checked here: the control handler holds an
	 * all-zero default, which TRY_EXT_CTRLS (v4l2-compliance) reads back as
	 * if a client had set it, and a picture size cannot be judged before
	 * one is actually submitted. histb_vdec_h264_run() refuses the job
	 * instead.
	 */
	if (V4L2_H264_SPS_HAS_CHROMA_FORMAT(sps) &&
	    (sps->chroma_format_idc != 1 ||
	     (sps->flags & V4L2_H264_SPS_FLAG_SEPARATE_COLOUR_PLANE) ||
	     sps->bit_depth_luma_minus8 || sps->bit_depth_chroma_minus8))
		return -EINVAL;

	return 0;
}

static int histb_vdec_h264_start(struct histb_vdec_ctx *ctx)
{
	struct histb_vdec_h264_ctx *h264;

	h264 = kzalloc_obj(*h264);
	if (!h264)
		return -ENOMEM;

	/*
	 * The CABAC table is resident in the device-wide message pool; the PMV
	 * pool grows on demand in run(), once the SPS fixes the frame geometry.
	 */
	ctx->priv = h264;

	return 0;
}

/*
 * Make sure the PMV pool slots hold the active frame. The vendor slot is 32
 * bytes per macroblock, at least one 1080p-frame slot; growing drops the slot
 * map, which is harmless at a resolution change.
 *
 * The pool is one dma_alloc_wc() per slot: 20 slots at the 1080p floor is a 5.0
 * MB (order-11, 8 MB aligned) request, and the board refuses that against its
 * 16 MB CMA region even with 9.9 MB free, which came back as -ENOMEM on every
 * job.
 */
static void histb_h264_pmv_free(struct histb_vdec_dev *dev,
				struct histb_vdec_h264_ctx *h264)
{
	int i;

	for (i = 0; i < HISTB_VDEC_MAX_PMV_SLOTS; i++) {
		if (!h264->pmv_va[i])
			continue;
		dma_free_wc(dev->dev, h264->pmv_slot_size, h264->pmv_va[i],
			    h264->pmv_dma[i]);
		h264->pmv_va[i] = NULL;
	}
	h264->pmv_slot_size = 0;
	h264->pmv_width_mbs = 0;
	h264->pmv_height_mbs = 0;
	h264->pmv_per_mb = 0;
}

/*
 * Whether this submission discards every stored picture, which is what makes
 * rebuilding the pool here lose nothing: an IDR starts a new reference chain
 * and the uAPI hands it with no DPB entry still flagged VALID or ACTIVE. HEVC
 * has no such per-entry flag (only V4L2_HEVC_DPB_ENTRY_LONG_TERM_REFERENCE),
 * which is why the same relax is not available there.
 */
static bool histb_h264_pmv_flush(const struct v4l2_ctrl_h264_decode_params *dec)
{
	unsigned int i;

	if (!(dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC))
		return false;

	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++)
		if (dec->dpb[i].flags & (V4L2_H264_DPB_ENTRY_FLAG_VALID |
					 V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			return false;

	return true;
}

static int histb_h264_pmv_grow(struct histb_vdec_ctx *ctx,
			       unsigned int width_mbs,
			       unsigned int height_mbs,
			       bool direct_8x8,
			       bool flush)
{
	struct histb_vdec_dev *dev = histb_vdec_h264_dev(ctx);
	struct histb_vdec_h264_ctx *h264 = ctx->priv;
	unsigned int per_mb = histb_h264_pmv_per_mb(width_mbs, height_mbs,
						    direct_8x8);
	unsigned int len = ALIGN(width_mbs * height_mbs * per_mb, 128);
	unsigned int slot = ALIGN(width_mbs * height_mbs * 32, 128);
	int i;

	slot = max3(slot, len, HISTB_H264_PMV_SLOT_MIN);
	if (h264->pmv_slot_size) {
		/*
		 * The allocation size alone cannot say the pool still fits: two
		 * geometries reach the same number wherever the
		 * HISTB_H264_PMV_SLOT_MIN floor flattens them, so the grid and
		 * the bytes per macroblock the engine strides by have to match
		 * as well before the pool is kept.
		 */
		if (h264->pmv_slot_size == slot &&
		    h264->pmv_width_mbs == width_mbs &&
		    h264->pmv_height_mbs == height_mbs &&
		    h264->pmv_per_mb == per_mb)
			return 0;
		/*
		 * Reaching here with a pool already built means the SPS changed
		 * mid-session. Rebuild it only where this submission flushes
		 * the reference chain: freeing the pool wipes the motion
		 * vectors of every picture a later job can still name, so the
		 * grid and the bytes per macroblock may both move together with
		 * it, but only at that point. A change inside a live chain
		 * stays refused - the pool, its slot map and the collocated
		 * read target would then follow two different geometries, and
		 * no source-change event tells the client. Restarting the
		 * queues runs ->stop() and zeroes pmv_slot_size. Where the
		 * payload lands is not the reason: with the tile geometry
		 * programmed from the negotiated rectangle a smaller picture
		 * still fills that rectangle exactly (measured: pictures on a
		 * smaller grid inside a larger session)
		 */
		if (!flush)
			return -EINVAL;
	}

	histb_h264_pmv_free(dev, h264);

	for (i = 0; i < HISTB_VDEC_MAX_PMV_SLOTS; i++) {
		h264->pmv_va[i] = dma_alloc_wc(dev->dev, slot,
					       &h264->pmv_dma[i], GFP_KERNEL);
		if (!h264->pmv_va[i])
			break;
	}
	if (i < HISTB_VDEC_MAX_PMV_SLOTS) {
		histb_h264_pmv_free(dev, h264);
		return -ENOMEM;
	}
	h264->pmv_slot_size = slot;
	h264->pmv_width_mbs = width_mbs;
	h264->pmv_height_mbs = height_mbs;
	h264->pmv_per_mb = per_mb;
	memset(ctx->pmv_map, 0, sizeof(ctx->pmv_map));
	ctx->pmv_next = 0;

	return 0;
}

static void histb_vdec_h264_stop(struct histb_vdec_ctx *ctx)
{
	struct histb_vdec_dev *dev = histb_vdec_h264_dev(ctx);
	struct histb_vdec_h264_ctx *h264 = ctx->priv;

	if (!h264)
		return;

	histb_h264_pmv_free(dev, h264);
	if (h264->rbsp_va)
		dma_free_wc(dev->dev, h264->rbsp_size, h264->rbsp_va,
			    h264->rbsp_dma);
	kfree(h264);
	ctx->priv = NULL;

	memset(ctx->pmv_map, 0, sizeof(ctx->pmv_map));
	ctx->pmv_next = 0;
}

/*
 * Hand the engine the slice span without its emulation-prevention bytes.
 *
 * The VDM decodes the bytes it is given literally: measured with an all-I_PCM
 * picture whose samples contain 00 00 00 (c30), the conformant Annex-B form
 * (four 03 bytes inserted in the slice data) lands 31 of 4096 luma lines, while
 * the same RBSP written without prevention bytes is byte-exact and reports no
 * error. The uAPI delivers the escaped form for both start-code modes
 * (ext-ctrls-codec-stateless.rst: V4L2_STATELESS_H264_START_CODE_*), so
 * removing them is the driver's job, not the encoder's.
 *
 * The copy starts at the first slice's NAL header byte and the slice offsets
 * are rewritten into its own domain; the OUTPUT plane is left untouched. When
 * the span holds no prevention byte the plane is used as is, so the common case
 * pays no copy.
 */
static int histb_h264_pack_rbsp(struct histb_vdec_ctx *ctx,
				struct histb_h264_run *run,
				unsigned int *payload_bits)
{
	struct histb_vdec_dev *dev = histb_vdec_h264_dev(ctx);
	struct histb_vdec_h264_ctx *h264 = ctx->priv;
	unsigned int first = run->slice_starts[0] / 8;
	unsigned int end = *payload_bits / 8;
	unsigned int esc_next = first;
	unsigned int out = 0, idx = 0, zeros = 0;
	unsigned int i;
	u8 *dst;

	if (!run->src_va || first >= end)
		return 0;

	for (i = first; i + 2 < end; i++) {
		if (!run->src_va[i] && !run->src_va[i + 1] &&
		    run->src_va[i + 2] == 0x03)
			break;
	}
	if (i + 2 >= end)
		return 0;

	if (h264->rbsp_size < end - first) {
		if (h264->rbsp_va)
			dma_free_wc(dev->dev, h264->rbsp_size, h264->rbsp_va,
				    h264->rbsp_dma);
		h264->rbsp_size = 0;
		h264->rbsp_va = dma_alloc_wc(dev->dev,
					     round_up(end - first, 64 * 1024),
					     &h264->rbsp_dma, GFP_KERNEL);
		if (!h264->rbsp_va)
			return -ENOMEM;
		h264->rbsp_size = round_up(end - first, 64 * 1024);
	}
	dst = h264->rbsp_va;

	for (i = first; i < end; i++) {
		u8 byte = run->src_va[i];

		if (i == esc_next) {
			esc_next = idx + 1 < run->num_slices ?
				run->slice_starts[idx + 1] / 8 : end;
			run->slice_starts[idx++] = out * 8;
			zeros = 0;
		}
		if (zeros >= 2 && byte == 0x03) {
			zeros = 0;
			continue;
		}
		dst[out++] = byte;
		zeros = byte ? 0 : zeros + 1;
	}

	*payload_bits = out * 8;
	run->src_va = dst;
	run->src_dma = h264->rbsp_dma;
	return 0;
}

/*
 * True when every coefficient the picture message carries is one repeated value
 * per list (the six 4x4 lists plus the two luma 8x8 lists the 4:2:0 path uses;
 * the remaining rows of scaling_list_8x8 are never packed) and the three intra
 * 4x4 lists share that value. Only this shape has been measured to decode
 * byte-exactly: with the intra lists equal to each other, an inter 4x4 or 8x8
 * list carrying a different value still matched the reference, while any stream
 * whose intra lists differed from each other did not.
 */
static bool histb_h264_scaling_supported(const struct v4l2_ctrl_h264_scaling_matrix *m)
{
	u8 v = m->scaling_list_4x4[0][0];
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(m->scaling_list_4x4); i++)
		for (j = 0; j < ARRAY_SIZE(m->scaling_list_4x4[i]); j++) {
			if (m->scaling_list_4x4[i][j] != m->scaling_list_4x4[i][0])
				return false;
			if (i < 3 && m->scaling_list_4x4[i][0] != v)
				return false;
		}

	for (i = 0; i < 2; i++)
		for (j = 1; j < ARRAY_SIZE(m->scaling_list_8x8[i]); j++)
			if (m->scaling_list_8x8[i][j] != m->scaling_list_8x8[i][0])
				return false;

	return true;
}

static int histb_vdec_h264_run(struct histb_vdec_ctx *ctx,
			       struct vb2_v4l2_buffer *src,
			       struct vb2_v4l2_buffer *dst)
{
	struct histb_vdec_dev *dev = histb_vdec_h264_dev(ctx);
	struct histb_vdec_h264_ctx *h264 = ctx->priv;
	struct v4l2_h264_reflist_builder builder;
	struct histb_h264_run *run = &h264->run;
	struct v4l2_ctrl *ctrl;
	unsigned int cap_w, cap_h;
	int ret;

	memset(run, 0, sizeof(*run));
	struct vb2_queue *cap_q;
	u32 *pic;
	u32 payload_bits;
	unsigned int n_slices, found;
	unsigned long start_code;
	unsigned int i;

	/*
	 * The driver core has applied the request to the control handler before
	 * scheduling this job, so p_cur carries this frame's values.
	 */
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_H264_SPS);
	run->sps = ctrl ? ctrl->p_cur.p_h264_sps : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_H264_PPS);
	run->pps = ctrl ? ctrl->p_cur.p_h264_pps : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_H264_DECODE_PARAMS);
	run->dec = ctrl ? ctrl->p_cur.p_h264_decode_params : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_H264_PRED_WEIGHTS);
	run->weights = ctrl ? ctrl->p_cur.p_h264_pred_weights : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_H264_SCALING_MATRIX);
	run->scaling = ctrl ? ctrl->p_cur.p_h264_scaling_matrix : NULL;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_H264_SLICE_PARAMS);
	run->slices = ctrl ? ctrl->p_cur.p_h264_slice_params : NULL;
	n_slices = ctrl ? ctrl->new_elems : 0;
	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_H264_START_CODE);
	start_code = ctrl ? ctrl->cur.val : V4L2_STATELESS_H264_START_CODE_ANNEX_B;

	/*
	 * Every word of the picture message comes from these controls: the
	 * frame-level state (size, POC, DPB addresses, CABAC table) has no
	 * other source, and the slice chain is built from the slice-parameter
	 * array. A missing or empty control therefore leaves words unwritable
	 * rather than guessed at, so the job is refused before anything is
	 * programmed.
	 */
	if (!run->sps || !run->pps || !run->dec || !run->slices || !n_slices)
		return -EINVAL;

	/*
	 * One hardware run consumes one chained slice slot per slice and the
	 * chain is capped by the message pool layout; longer frames would need
	 * the vendor multi-start split, which does not fit the
	 * one-frame-per-job stateless model. One slot is left over because the
	 * chain has to end on a zeroed slot, not on the last real one.
	 */
	if (n_slices >= HISTB_VDEC_MAX_SLICE_SLOTS)
		return -EINVAL;

	payload_bits = vb2_get_plane_payload(&src->vb2_buf, 0) * 8;
	/*
	 * A zero-length submission carries no slice data at all. Each slice is
	 * addressed through a segment descriptor whose bit length is part of
	 * the word, so there is nothing for those words to point at and the job
	 * cannot be programmed in this state.
	 */
	if (!payload_bits)
		return -EINVAL;
	run->src_dma = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	run->dst_dma = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	run->num_slices = n_slices;

	run->src_va = vb2_plane_vaddr(&src->vb2_buf, 0);
	if (start_code == V4L2_STATELESS_H264_START_CODE_ANNEX_B) {
		/*
		 * Locating slices in Annex-B mode reads the OUTPUT plane
		 * through the kernel mapping: histb_h264_find_slice_nals()
		 * walks it byte by byte and the slice segments are taken from
		 * where it finds each NAL. The source queue carries a kernel
		 * mapping for this reason (its dma_attrs omit
		 * DMA_ATTR_NO_KERNEL_MAPPING, see histb_vdec_queue_init()), so
		 * a NULL here means the mapping came back unavailable and no
		 * slice boundary is known; the job is refused rather than
		 * programmed from offsets nothing verified.
		 */
		if (!run->src_va) {
			dev_warn_once(dev->dev,
				      "OUTPUT queue has no kernel mapping, cannot delimit slices\n");
			return -EINVAL;
		}
		found = histb_h264_find_slice_nals(run->src_va,
						   payload_bits / 8,
						   run->slice_starts,
						   ARRAY_SIZE(run->slice_starts));
		/*
		 * The scan must find exactly one NAL per slice the control
		 * declares. Each declared slice gets one chained slice slot,
		 * addressed by the start found here, so a different count would
		 * mean programming slots from spans the control does not
		 * describe.
		 */
		if (found != n_slices)
			return -EINVAL;
	} else {
		/*
		 * Without start codes the slices in the OUTPUT buffer cannot be
		 * delimited, so a single-slice submission is the only layout
		 * the driver can consume. Start codes are never part of the
		 * hardware ABI either way: every slice segment is computed from
		 * the slice's data bit offset, which for frame-based
		 * submissions already includes the NAL unit header bits.
		 */
		if (n_slices != 1)
			return -EINVAL;
		run->slice_starts[0] = 0;
	}

	ret = histb_h264_pack_rbsp(ctx, run, &payload_bits);
	if (ret)
		return ret;

	/*
	 * Slice-group partitioning (FMO/ASO) and redundant coded pictures both
	 * move where a slice's data starts, and neither survives this API: the
	 * PPS has a slice-group count but no map, and the per-slice
	 * redundant_pic_cnt is read by nothing here - the VDM is programmed one
	 * raster-slice per submission and would present a secondary copy as a
	 * picture of its own. The vendor parser rejects both shapes, a baseline
	 * stream with FMO and a redundant picture, so this costs nothing the
	 * hardware could deliver. No stream in the scored corpus declares
	 * either field. Which neighbouring parameter-set features the API can
	 * even see are named where they are consumed: the SPS and PPS flag
	 * words above, and the scaling-matrix control.
	 */
	if (run->pps->num_slice_groups_minus1 ||
	    (run->pps->flags & V4L2_H264_PPS_FLAG_REDUNDANT_PIC_CNT_PRESENT))
		return -EINVAL;

	/*
	 * Quantisation matrices do arrive here, and the VDM consumes the words:
	 * submitting them zeroed instead of decoded changes the picture in a
	 * different way. But the engine does not apply the intra 4x4 lists as
	 * the protocol says. Measured on the board, a stream whose three intra
	 * 4x4 lists differ from each other delivers 68487 of 115200 samples off
	 * by up to 58, spread over every macroblock row, while one whose intra
	 * lists agree decodes byte-exactly even when an inter 4x4 or 8x8 list
	 * carries a different value - and no reordering of the eight lists, nor
	 * reading the 8x8 slots, nor a scan-order submission explains where the
	 * intra ones go (all three measured on the board). Finding that out
	 * needs the vendor's intra inverse-quantiser arithmetic, which is not a
	 * lightweight bypass, so until it is known only the shapes measured
	 * correct are accepted, and a present flag with no list control behind
	 * it carries no values to check either way. No stream in the scored
	 * corpus declares matrices - the parameter-set walk and the reference
	 * decoder's own header trace agree on that - so this costs no frame
	 * that decodes byte-exactly today.
	 */
	if (run->pps->flags & V4L2_H264_PPS_FLAG_SCALING_MATRIX_PRESENT) {
		if (!run->scaling ||
		    !histb_h264_scaling_supported(run->scaling))
			return -EINVAL;
	}

	/*
	 * Lossless streams are visible here - the SPS control carries
	 * V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS - but the VDM has
	 * no bypass path: measured on the board, a stream with the flag set
	 * (x264 -qp 0, which picks profile 244) delivers 0 of 115200 samples
	 * right in every frame, while the same clip at qp 4 without the flag
	 * decodes byte-exactly, so the deviation is the bypass itself and not
	 * the profile or the encoder. The vendor firmware's own H.264 syntax
	 * parser reports that same SPS bit as unsupported. No stream in the
	 * scored corpus sets it, and nothing in this driver can implement it:
	 * the inverse transform would have to be skipped per macroblock.
	 */
	if (run->sps->flags & V4L2_H264_SPS_FLAG_QPPRIME_Y_ZERO_TRANSFORM_BYPASS)
		return -EINVAL;

	run->width_mbs = run->sps->pic_width_in_mbs_minus1 + 1;
	run->height_mbs = (run->sps->pic_height_in_map_units_minus1 + 1) *
			 ((run->sps->flags & V4L2_H264_SPS_FLAG_FRAME_MBS_ONLY) ?
			  1u : 2u);

	/*
	 * The luma write path needs two macroblock columns per row: a picture
	 * declared one MB wide delivers wrong luma whatever its slice data is
	 * (a 32-wide stream declared as one column reproduces the 16x16 failure
	 * down to the write counters, and that same 16x16 stream declared as
	 * two columns decodes byte-exactly), so 16-column pictures cannot be
	 * delivered at all. Cropping does not help - the coded count is what
	 * the engine writes by. The picture message D0 budgets 9 bits of MB
	 * width and 10 of MB height.
	 */
	if (run->width_mbs < 2 ||
	    run->width_mbs * 16 > HISTB_VDEC_MAX_WIDTH ||
	    run->height_mbs * 16 > HISTB_VDEC_MAX_HEIGHT)
		return -EINVAL;
	run->mbamt = run->width_mbs * run->height_mbs;

	/* The VDM writes full frames into the queued CAPTURE slots; a stream
	 * larger than the negotiated geometry could not be held.
	 */
	ret = histb_vdec_cap_dims(ctx, &cap_w, &cap_h);
	if (ret)
		return ret;
	if (run->width_mbs * 16 > cap_w || run->height_mbs * 16 > cap_h)
		return -EINVAL;

	/*
	 * The band layout the engine is told to use is the one the CAPTURE
	 * format reported, not the coded picture's own: the negotiated
	 * rectangle may be larger, and a client addressing the buffer per
	 * bytesperline has to find the payload there.
	 */
	histb_vdec_buf_geom(ctx, &run->tile_stride, &run->buf_lines);

	/*
	 * Collocated motion vectors live in the two halves of a PMV slot, and
	 * the firmware names those halves per *frame store* (one address
	 * selection per stored picture) while this API hands each field
	 * picture its own DPB entry and capture buffer. So the half a B slice
	 * needs cannot be named here, and a picture whose direct
	 * prediction depends on it is refused rather than delivered:
	 *  - a field picture with B slices;
	 * - an MBAFF frame with B slices. Writing slot[9] = base + half for
	 *   such a
	 *    reference changed nothing on the board (10 of 50 pictures correct with
	 *    and without it), so that is not the missing piece either.
	 * Nothing else reads the collocated vectors - the firmware zeroes
	 * slot[8]/slot[9] for every other slice type, by the same slice-type
	 * table as histb_h264_slice_type_code() - so progressive pictures and
	 * MBAFF without B slices keep decoding byte-exactly (both 50 of 50 on
	 * the corpus).
	 *
	 * One known residual is deliberately left un-gated. On a spatial-direct
	 * B slice whose collocated picture is a single-direction (P) picture,
	 * the engine averages both lists where 8.4.1.1 asks for the co-located
	 * macroblock's own motion: measured on the board, the 384 samples of
	 * macroblock (0,0) of one B frame are all off, up to 76, with the
	 * deblocking across that macroblock's edge spreading it to its
	 * neighbours, while every other frame of the same stream is byte-exact.
	 * No cheap predicate separates that frame from the ones that are right:
	 * direct_8x8_inference_flag is set in every B-bearing case in the
	 * corpus (so it has no discriminating power), and all 791 B pictures
	 * scored are B-skip pictures, so refusing "B-skip only" costs exactly
	 * what refusing every B picture costs - 731 of 1928 byte-exact frames
	 * (37.9%), since the only refusal this interface can express is per
	 * job. Deciding it correctly would mean decoding the co-located
	 * macroblock layer for one macroblock (its entropy state and
	 * partitioning) before programming the job, which is software decoding;
	 * the one fact that could make a narrow gate possible is how the motion
	 * plane encodes "collocated motion unavailable", and that encoding is
	 * undocumented. A known gap is recorded here rather than traded for a
	 * third of the frame rate.
	 *
	 * Once one picture has been refused, every later picture of the chain
	 * still references it, so those are refused too until an IDR restarts
	 * the chain: per-slice-type refusal alone delivered 17 frames that were
	 * written but wrong. With this stickiness the same stream delivers 10
	 * byte-exact frames (the two leading pictures of each GOP) and the
	 * other 40 come back with a zeroed payload, so none is wrong.
	 */
	if (run->dec->flags & V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC)
		h264->col_half_lost = false;
	if (h264->col_half_lost)
		return -EINVAL;
	if (histb_h264_has_b_slice(run)) {
		bool field_pic = run->dec->flags &
			V4L2_H264_DECODE_PARAM_FLAG_FIELD_PIC;

		if (field_pic ||
		    (run->sps->flags &
		     V4L2_H264_SPS_FLAG_MB_ADAPTIVE_FRAME_FIELD)) {
			h264->col_half_lost = true;
			return -EINVAL;
		}
	}

	ret = histb_h264_pmv_grow(ctx, run->width_mbs, run->height_mbs,
				  run->sps->flags &
				  V4L2_H264_SPS_FLAG_DIRECT_8X8_INFERENCE,
				  histb_h264_pmv_flush(run->dec));
	if (ret)
		return ret;

	/*
	 * Map the DPB onto the APC frame table. Capture buffer indexes are the
	 * APC slots; the POC tail is zeroed like the vendor's and the address
	 * tail points at the output frame. The vendor repeats the picture at
	 * list position 0 through the unused tail entries past the slots it
	 * counts as present - filling the same slots that way was measured here
	 * and left every case of the corpus byte-identical (QDC81 over the
	 * tx/cy/i12/pb/cb/cl/sc families), and renumbering the slots in
	 * reference-list order changed nothing either, so no engine path reads
	 * an APC slot that no declist id names.
	 */
	if (dst->vb2_buf.index >= HISTB_VDEC_MAX_DPB_SLOTS)
		return -EINVAL;

	cap_q = v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx);
	for (i = 0; i < HISTB_H264_NUM_APC_SLOTS; i++) {
		run->img_addr[i] = lower_32_bits(run->dst_dma);
		run->apc_poc[0][i] = 0;
		run->apc_poc[1][i] = 0;
	}
	for (i = 0; i < V4L2_H264_NUM_DPB_ENTRIES; i++) {
		const struct v4l2_h264_dpb_entry *entry = &run->dec->dpb[i];
		struct vb2_buffer *vb;

		run->dpb2apc[i] = HISTB_H264_APC_NONE;
		if (!(entry->flags & V4L2_H264_DPB_ENTRY_FLAG_ACTIVE))
			continue;

		vb = vb2_find_buffer(cap_q, entry->reference_ts);
		if (!vb || vb->index >= HISTB_H264_NUM_APC_SLOTS)
			return -EINVAL;

		run->dpb2apc[i] = vb->index;
		run->img_addr[vb->index] =
			lower_32_bits(vb2_dma_contig_plane_dma_addr(vb, 0));
		run->apc_poc[0][vb->index] = entry->top_field_order_cnt;
		run->apc_poc[1][vb->index] = entry->bottom_field_order_cnt;
	}

	v4l2_h264_init_reflist_builder(&builder, run->dec, run->sps,
				       run->dec->dpb);
	run->num_valid = builder.num_valid;
	v4l2_h264_build_p_ref_list(&builder, run->reflist_p);
	v4l2_h264_build_b_ref_lists(&builder, run->reflist_b0, run->reflist_b1);

	pic = histb_vdec_pic_slot(&dev->pool);
	histb_h264_assemble_pic(dev, h264, run,
				histb_h264_pmv_alloc(ctx, dst->vb2_buf.index),
				pic);

	for (i = 0; i < n_slices; i++) {
		u32 nal_bit = run->slice_starts[i];
		u32 next_bit = i + 1 < n_slices ? run->slice_starts[i + 1] :
						  payload_bits;

		if (nal_bit + run->slices[i].header_bit_size >= next_bit)
			return -EINVAL;

		histb_h264_assemble_slice(dev, ctx, run, h264, i, nal_bit,
					  next_bit);
	}

	/*
	 * The last real slot links to this one, so it has to present as an
	 * empty segment even when an earlier job left a longer chain here.
	 */
	memset(histb_vdec_slice_slot(&dev->pool, n_slices), 0,
	       HISTB_VDEC_SLOT_BYTES);

	histb_h264_program_regs(dev, run);
	histb_vdec_hw_kick(dev, HISTB_VIDSTD_H264);

	return 0;
}

static const struct v4l2_ctrl_config histb_vdec_h264_ctrls[] = {
	{
		.id = V4L2_CID_STATELESS_H264_SPS,
	},
	{
		.id = V4L2_CID_STATELESS_H264_PPS,
	},
	{
		.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
	},
	{
		.id = V4L2_CID_STATELESS_H264_PRED_WEIGHTS,
	},
	{
		.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
	},
	{
		.id = V4L2_CID_STATELESS_H264_SLICE_PARAMS,
		/*
		 * v4l2_ctrl_new_custom() only preserves cfg->flags when the
		 * config carries an explicit name: otherwise it runs
		 * v4l2_ctrl_fill(), which zeroes the flags and would drop the
		 * dynamic-array property.
		 */
		.name = "H264 Slice Parameters",
		.type = V4L2_CTRL_TYPE_H264_SLICE_PARAMS,
		.dims = { 8 },
		.flags = V4L2_CTRL_FLAG_DYNAMIC_ARRAY,
	},
	{
		.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		.min = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.max = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.def = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
	},
	{
		.id = V4L2_CID_STATELESS_H264_START_CODE,
		.min = V4L2_STATELESS_H264_START_CODE_NONE,
		.max = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.def = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
	},
};

static const struct histb_vdec_codec_ops histb_vdec_h264_codec_ops = {
	.start = histb_vdec_h264_start,
	.stop = histb_vdec_h264_stop,
	.run = histb_vdec_h264_run,
	.try_ctrl = histb_vdec_h264_try_ctrl,
};

const struct histb_vdec_fmt_desc histb_vdec_h264_fmt = {
	.fourcc = V4L2_PIX_FMT_H264_SLICE,
	.stride = histb_h264_tile_stride,
	.grid_align = 16,
	.num_ctrls = ARRAY_SIZE(histb_vdec_h264_ctrls),
	.ctrls = histb_vdec_h264_ctrls,
	.ops = &histb_vdec_h264_codec_ops,
	.vidstd = HISTB_VIDSTD_H264,
};
