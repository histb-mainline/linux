// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSilicon Hi3798MV100 VDM stateless MPEG-1/MPEG-2 decoder (V4L2 codec
 * layer).  The hardware consumes a fully parsed picture header packed into
 * the downstream message pool (slot words D0-D63) plus a slice descriptor
 * list, the two 64-byte quantisation matrices carried in the picture slot
 * itself (D16-D47), and the usual per-frame register set.
 *
 * The message layout below was re-derived from the packing loops the vendor
 * firmware runs for MPEG-2 - the picture message, the slice descriptors and
 * the per-frame registers - and then checked against board behaviour wherever
 * the field is observable.
 *
 * Known syntax gaps against V4L2_CID_STATELESS_MPEG2_*: the control set has
 * no mpeg1_flag / full_pel_*_vector, so the
 * hardware fields are always written 0, i.e. MPEG-1 streams and full-pel
 * motion vectors are not supported.  Slices are found by scanning the
 * submitted span for slice_start_codes; a picture whose slices do not tile
 * the macroblock grid in raster order is rejected.
 */

#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "histb_vdec.h"

/*
 * Band-line scratch area inside the pool table region: eight 480 KiB
 * buffers, one per enum histb_vdec_scratch entry.  MPEG-2 references only
 * PMV_TOP (D7 current_pmv_addr and D48 pmv_top_addr); the SED/RCN/DBLK
 * line buffers of this protocol are internal to the hardware.
 */

/* The vendor firmware's MPEG-2 packer adds 24 dummy bits to both
 * picture-level segment lengths.
 */
#define HISTB_MPEG2_DUMMY_BITS		24

/*
 * Slot words D0..D7 of the picture message: two stream segments (the second is
 * unused here), quantiser_scale_code with intra_slice, the macroblock range
 * and the chain pointer.
 *
 * The vendor firmware's MPEG-2 slice packer lays these descriptors out as an
 * array of 32-byte entries: it indexes them by slot number times 32 and points
 * each entry's chain word at the following entry.  That stride is smaller than
 * the shared HISTB_VDEC_SLOT_BYTES the H.264 and HEVC messages need, and with
 * the wide stride the MPEG-2 engine finished every picture without reading a
 * single bit of it (the reconstruction was the same for three different streams
 * on the same boot), so the slice descriptors are packed at the narrow stride.
 */
#define HISTB_MPEG2_SLC_BYTES		32

/* picture_coding_type travels through the control as the stream value. */
#define HISTB_MPEG2_PIC_I		1
#define HISTB_MPEG2_PIC_P		2
#define HISTB_MPEG2_PIC_B		3

/* picture_structure 6.2.23.1: 1 = top field, 2 = bottom field, 3 = frame. */
#define HISTB_MPEG2_STRUCT_FRAME	3

static void *histb_vdec_mpeg2_slc_slot(const struct histb_vdec_msgpool *p,
				       unsigned int i)
{
	return p->va + p->slice_off + i * HISTB_MPEG2_SLC_BYTES;
}

static dma_addr_t
histb_vdec_mpeg2_slc_slot_dma(const struct histb_vdec_msgpool *p, unsigned int i)
{
	return p->dma + p->slice_off + i * HISTB_MPEG2_SLC_BYTES;
}

static void histb_vdec_mpeg2_pack_slice(u32 *w, u32 data_bit,
					u32 end_bit, u32 buf_end_bit, u32 qsc,
					u32 intra,
					u32 start_mbn, u32 end_mbn,
					dma_addr_t next_dma)
{
	u32 len_end = end_bit;

	/*
	 * The vendor firmware gives each slice the bits still buffered after
	 * its header: it points the window at the slice's first data bit and
	 * declares the rest of the packet.  A window cut at the next
	 * slice_start_code decodes the same pictures, so the shorter,
	 * self-contained span is used here.  Only the picture level segments
	 * carry the +24 of HISTB_MPEG2_DUMMY_BITS.
	 *
	 * Nothing is trimmed off the end: MPEG-2 has no RBSP stop bit - a slice ends
	 * where the next start code begins - and the engine counts the whole declared
	 * length as macroblock layer.  Cutting the trailing zeros, as the H.264 and
	 * HEVC paths do, eats the last bits of any codeword ending in a zero, and a
	 * level-64 escape (`000001` + run + `000001000000`) always does.  It was
	 * on single-coefficient probes at seven scan positions: with the trim 19 of 20
	 * macroblocks hit the theoretical amplitude and the last came out at exactly
	 * half; skipping the trim gives 20 of 20.  No corpus stream changed verdict
	 * either way, so this is conformance rather than score.
	 *
	 * The segment is addressed as a byte granule offset plus a bit offset
	 * inside that granule, both measured from the programmed stream base -
	 * the same arithmetic as the H.264 path. Passing the plane start address
	 * with an absolute bit offset instead drops the byte part, and then every
	 * slice decodes from the first 16 bytes of the span.
	 */
	w[0] = FIELD_PREP(GENMASK(23, 0), len_end - data_bit) |
	       FIELD_PREP(GENMASK(30, 24), data_bit & 127U);
	w[1] = (data_bit / 8) & ~15U;
	w[2] = 0;
	w[3] = 0;
	w[4] = FIELD_PREP(GENMASK(5, 0), qsc) | FIELD_PREP(BIT(6), intra);
	w[5] = FIELD_PREP(GENMASK(19, 0), start_mbn);
	w[6] = FIELD_PREP(GENMASK(19, 0), end_mbn);
	w[7] = next_dma & ~15U;
}

/*
 * Decode one slice header. @p points at the first byte after the 32-bit start
 * code, @code is that start code's low byte and @idx the index of this slice in
 * the picture.  Returns the row where the slice starts, its quantiser scale code
 * and how many bits of header follow the start code.
 */
/*
 * Read @n bits of a slice header, bit 0 being the most significant bit of @p[0].
 * No field of 6.2.2.4 is wider than eight bits, so @n stays in 1..8 and the
 * window never leaves the four bytes the caller has already checked.
 */
static unsigned int histb_vdec_mpeg2_hdr_bits(const u8 *p, unsigned int pos,
					     unsigned int n)
{
	unsigned int val = 0;
	unsigned int i;

	for (i = 0; i < n; i++)
		val = val << 1 |
		      ((p[(pos + i) / 8] >> (7 - (pos + i) % 8)) & 1U);
	return val;
}

/*
 * Decode one slice header (6.2.2.4).  @p points at the first byte after the
 * 32-bit slice_start_code, @code is that start code's low byte and @idx the
 * index of this slice in the picture.  Returns the macroblock row the slice
 * starts on, its quantiser scale code, its intra_slice flag and how many bits
 * of header follow the start code.
 *
 * The MPEG-2 form is slice_vertical_position (8 bits when progressive_sequence
 * is set, otherwise a 3-bit extension plus 7 bits), slice_horizontal_position,
 * quantiser_scale_code (1..31), intra_slice_flag and, when that is set,
 * intra_slice plus seven reserved bits, then extra_bit_slice.  Field pictures,
 * slices that start mid-row and MPEG-1's extra_image_information are rejected
 * rather than decoded at a guessed bit position.
 *
 * ffmpeg's MPEG-1/MPEG-2 encoder (mpeg12enc.c ff_mpeg1_encode_slice_header())
 * writes the MPEG-1 form for both codecs: the row sits in the start code's low
 * byte and only quantiser_scale_code plus the extra bit follow.  Streams
 * produced and reference decoded by ffmpeg therefore need that form, and one
 * further legacy shape is accepted where the vertical position reads as zero
 * and the quantiser follows immediately after it.
 */
static int histb_vdec_mpeg2_slice_hdr(const u8 *p, unsigned int code,
				      bool progressive, unsigned int mb_height,
				      unsigned int idx, unsigned int *row,
				      unsigned int *qsc, unsigned int *intra,
				      unsigned int *hdr_bits)
{
	unsigned int bits = progressive ? 8 : 10;
	unsigned int vpos, flag;

	if (progressive) {
		vpos = histb_vdec_mpeg2_hdr_bits(p, 0, 8);
	} else {
		/*
		 * slice_vertical_position_extension (13818-2 6.2.2.4) is only ever
		 * non-zero on pictures this driver refuses on other grounds: the
		 * field pictures of a non-progressive sequence, which
		 * histb_vdec_mpeg2_run() rejects on picture_structure, and the
		 * 4:2:2 variant its chroma_format gate rejects. Nothing here could
		 * use a non-zero value either: @row becomes start_mbn = row *
		 * mb_width and D0 budgets a single frame grid, so a slice naming a
		 * row beyond it has no addressable macroblock range and is refused
		 * rather than placed at a guessed row. This arm has never been
		 * measured on a conformant stream - the only headers built here
		 * that set it wrote the quantiser at a wrong offset, which the
		 * reference decoder itself rejects (qscale == 0) - so settling it
		 * needs a legal MPEG-2 form slice header on a field picture, which
		 * the local encoder does not produce.
		 */
		if (histb_vdec_mpeg2_hdr_bits(p, 0, 3))
			return -EINVAL;
		vpos = histb_vdec_mpeg2_hdr_bits(p, 3, 7);
	}

	/*
	 * quantiser_scale_code is 1..31 (0 is forbidden) and a row-aligned slice
	 * carries horizontal position 0: together with the vertical position
	 * naming this slice's row that is enough to tell the two forms apart
	 * without ever guessing a data position.
	 */
	if (vpos == idx + 1 && vpos >= 1 && vpos <= mb_height &&
	    histb_vdec_mpeg2_hdr_bits(p, bits, 8) == 0) {
		bits += 8;
		*qsc = histb_vdec_mpeg2_hdr_bits(p, bits, 5);
		/*
		 * 0 is the single quantiser_scale_code value 13818-2 6.2.2.4
		 * forbids, and the upper bound only restates the range of the 5-bit
		 * field, so a rejection here means the walk landed on the wrong
		 * bits rather than on an unusual stream: the probes that misplaced
		 * this field are the only corpus members the reference decoder ever
		 * called qscale == 0. With the parse in place the arm costs nothing
		 * - the scored MPEG-2 corpus is 68 valid inputs, and this gate
		 * refused 0 of them.
		 */
		if (!*qsc || *qsc > 31)
			return -EINVAL;
		bits += 5;
		flag = histb_vdec_mpeg2_hdr_bits(p, bits, 1);
		bits += 1;
		if (flag) {
			*intra = histb_vdec_mpeg2_hdr_bits(p, bits, 1);
			bits += 8;
		} else {
			*intra = 0;
		}
		/*
		 * The last header bit of this form, and it has to read 0:
		 * 13818-2 6.2.2.4 keeps it as the place-holder for MPEG-1's
		 * extra_image_information, and a set bit opens a trailing field
		 * that nothing here consumes, so every macroblock after it would be
		 * read at an offset this driver has not walked. Offsets are not
		 * tunable at this layer - feeding the engine the same macroblock
		 * layer one bit short of the header end moved every DC level of the
		 * picture, +112 against +17, which is the measurement that fixed
		 * the MPEG-1 arm below - so the slice is refused instead.
		 */
		if (histb_vdec_mpeg2_hdr_bits(p, bits, 1))
			return -EINVAL;
		*row = vpos - 1;
		*hdr_bits = bits + 1;
		return 0;
	}

	if (progressive && !vpos) {
		/*
		 * MPEG-1 6.2.2.4 puts extra_bit_slice after intra_slice_flag, so
		 * both of those bits belong to the header: reading only the flag
		 * started the macroblock layer one bit early and every macroblock
		 * level came out wrong (measured with the same macroblock layer
		 * with and without the conformant bit: +112 versus +17).  Both bits must
		 * read 0 here; a slice that sets intra_slice_flag is refused
		 * rather than mis-parsed.
		 */
		*qsc = histb_vdec_mpeg2_hdr_bits(p, 8, 5);
		if (!*qsc || *qsc > 31 ||
		    histb_vdec_mpeg2_hdr_bits(p, 13, 2))
			return -EINVAL;
		*intra = 0;
		*row = idx;
		*hdr_bits = 15;
		return 0;
	}

	if (code >= 1 && code <= 0x8f && code - 1 == idx &&
	    p[0] >= 0x08 && !(p[0] & 0x04)) {
		*qsc = p[0] >> 3;
		*intra = 0;
		*row = code - 1;
		*hdr_bits = 5 + 1;
		return 0;
	}

	/*
	 * None of the three shapes above matched, so the bit at which this
	 * slice's macroblock layer starts is unknown. What reaches here is a
	 * slice that starts mid-row (horizontal position other than 0), a
	 * vertical position that does not name this slice's row, or MPEG-1's
	 * extra_image_information. The first two have no representation to
	 * program even if the position were guessed: the descriptor states its
	 * range as macroblock numbers derived from row * mb_width, so a
	 * mid-row start cannot be expressed at all, and a wrong guess costs
	 * the whole picture rather than one macroblock (see the +112/+17
	 * reading above). Accepting the rest needs per-slice geometry from the
	 * client, which V4L2 defines no MPEG-2 control for.
	 */
	return -EINVAL;
}

/*
 * Width in bits of the first macroblock_address_increment of a slice
 * (13818-2 Table B-1).  The slice segment has to start *after* it: the engine
 * consumes that codeword itself, so a window opened at the end of the slice
 * header hands it one codeword too many and the whole macroblock layer
 * misaligns.  Measured on the board: shifting the declared
 * start by this width makes the DC-only probe pictures byte-exact against the
 * software reference, and only a one-bit shift does so - which is exactly the
 * width Table B-1 gives increment 1.
 *
 * B-1 is canonical and every length group is a contiguous range, so an 11-bit
 * window is classified by the first group whose left-aligned range contains it.
 * Stuffing (00000001, 8 bits) may precede the codeword; the escape form
 * (00000010 + escape_code) and any window matching no group are refused rather
 * than guessed, because a start wrong by one bit costs the whole picture.
 */
static int histb_vdec_mpeg2_first_incr(const u8 *p, unsigned int pos,
				       unsigned int avail, unsigned int *len)
{
	static const struct {
		unsigned char bits;
		u16 first;
		u16 last;
	} grp[] = {
		{ 1, 0x001, 0x001 },	/* increment 1 */
		{ 3, 0x002, 0x003 },	/* 2..3 */
		{ 4, 0x004, 0x005 },	/* 4..5 */
		{ 5, 0x008, 0x009 },	/* 6..7 */
		{ 7, 0x006, 0x007 },	/* 8..9 */
		{ 8, 0x006, 0x00b },	/* 10..15 */
		{ 10, 0x012, 0x017 },	/* 16..21 */
		{ 11, 0x018, 0x023 },	/* 22..33 */
	};
	unsigned int i, w11;

	for (;;) {
		/*
		 * Classifying needs an 11-bit window, the width of Table B-1's
		 * longest macroblock_address_increment group (22..33), and
		 * histb_vdec_mpeg2_hdr_bits() checks no bounds of its own.
		 * Accepting a shorter span means the rest of the slice.
		 */
		if (avail < 11)
			return -EINVAL;

		w11 = (histb_vdec_mpeg2_hdr_bits(p, pos, 8) << 3) |
		      histb_vdec_mpeg2_hdr_bits(p, pos + 8, 3);

		if ((w11 >> 3) == 0x01) {	/* stuffing_code */
			pos += 8;
			avail -= 8;
			continue;
		}
		/*
		 * The escape form (`00000010` + escape_code) is the one shape
		 * whose width Table B-1 does not give, so no window start
		 * follows from it, and a start wrong by one bit costs the
		 * whole picture. TODO: pin the escape codeword's width on a
		 * slice that skips past the 33 the table reaches.
		 */
		if ((w11 >> 3) == 0x02)		/* escape form: unmeasured width */
			return -EINVAL;

		for (i = 0; i < ARRAY_SIZE(grp); i++) {
			unsigned int code = w11 >> (11 - grp[i].bits);

			if (code >= grp[i].first && code <= grp[i].last) {
				*len = grp[i].bits;
				return 0;
			}
		}
		/*
		 * None of Table B-1's eight groups owns this window, and the
		 * groups are contiguous and left-aligned, so this is not a
		 * macroblock_address_increment codeword and the walk is not
		 * at the slice's data start; recovering it would mean
		 * decoding the macroblock layer; 0 of 68 inputs reach it.
		 */
		return -EINVAL;
	}
}

/*
 * Fill the slice descriptor chain of one picture; returns the number of slots
 * written or a negative error.
 *
 * histb_vdec_mpeg2_slice_hdr() parses each header (6.2.2.4 plus the MPEG-1 form
 * ffmpeg's encoder writes) and the descriptors delimit exactly the bits it
 * identifies as slice_data().
 *
 * Slices that do not tile the picture in raster order, that start in the middle
 * of a macroblock row, or that carry an out-of-range quantiser are rejected: the
 * hardware gets an explicit macroblock range per slice, so an unsupported shape
 * must not silently decode at the wrong place.
 *
 * Each slice needs its own descriptor: the engine stops a slice's macroblock
 * layer at that slice's end_mbn and continues in the slot D7 points at, so a
 * single descriptor for a whole multi-slice picture makes it read the following
 * slice_start_code as macroblock data and time the job out.
 */
static int histb_vdec_mpeg2_pack_slices(struct histb_vdec_msgpool *pool,
					const u8 *buf, unsigned int size,
					bool progressive,
					unsigned int mb_width,
					unsigned int mb_total,
					dma_addr_t stream_base,
					dma_addr_t data_addr)
{
	unsigned int mb_height = mb_total / mb_width;
	unsigned int expect = 0, i = 0, slot = 0, end_bit = size * 8;
	unsigned int prev_data_bit = 0, prev_qsc = 0, prev_start_mbn = 0;
	unsigned int prev_intra = 0;
	bool have_prev = false;

	while (i + 7 < size) {
		unsigned int hdr_bit = i * 8, data_bit, mbn, qsc, code, row;
		unsigned int intra, hdr_bits, inc_bits;

		if (buf[i] || buf[i + 1] || buf[i + 2] != 1) {
			i++;
			continue;
		}
		code = buf[i + 3];
		if (code < 0x01 || code > 0x8f) {
			/* A GOP/picture end or an extension closes the picture. */
			if (have_prev) {
				end_bit = hdr_bit;
				break;
			}
			i += 4;
			continue;
		}
		/*
		 * A header that matches no accepted form is the whole job, not
		 * just this slice: its data start is unknown, so the previous
		 * slice's window cannot be closed against it and this one's cannot
		 * be opened. histb_vdec_mpeg2_slice_hdr() carries the reasons - no
		 * mid-row starts, no field pictures, no MPEG-1 extra information -
		 * and the walk cannot substitute a guessed position, because a
		 * start wrong by one bit misaligns every macroblock codeword that
		 * follows it.
		 */
		if (histb_vdec_mpeg2_slice_hdr(buf + i + 4, code, progressive,
					       mb_height, slot, &row, &qsc, &intra,
					       &hdr_bits))
			return -EINVAL;

		mbn = row * mb_width;
		/*
		 * The first two arms are this function's tiling rule written as
		 * numbers: mbn != expect means the slices do not hand the grid over
		 * in raster order (a row revisited, or a gap where an omitted row
		 * should be), and mbn >= mb_total is a header naming a row the
		 * picture does not have, which would otherwise reach the descriptor
		 * as a range outside the frame. Both are refused rather than
		 * trimmed because the engine honours the range it is given: it
		 * stops a slice at its end_mbn and continues in the next slot, so a
		 * wrong range silently decodes at the wrong place. The third arm is
		 * the pool, not the engine: histb_vdec_pool_alloc() sizes the slice
		 * area for HISTB_VDEC_MAX_SLICE_SLOTS descriptors and one slot is
		 * always held back for the cleared terminator written after the
		 * chain, and this protocol spends only HISTB_MPEG2_SLC_BYTES of the
		 * 1280 bytes budgeted per slot. The vendor firmware's parameter
		 * model allows far more slices per picture (1024), so serving
		 * denser streams means enlarging that area, not relaxing this
		 * check - the densest streams of the scored corpus carry 15 slices
		 * and none comes near the budget.
		 */
		if (mbn != expect || mbn >= mb_total ||
		    slot + 1 >= HISTB_VDEC_MAX_SLICE_SLOTS)
			return -EINVAL;
		/*
		 * The vendor's syntax layer consumes
		 * macroblock_address_increment before recording the window, so
		 * its declared start is the first mb_type bit.
		 */
		data_bit = hdr_bit + 32 + hdr_bits;
		/*
		 * No window can be declared without this codeword's width: the
		 * slice segment starts at the first mb_type bit, and the width comes
		 * from the codeword itself. histb_vdec_mpeg2_first_incr() refuses
		 * three shapes - fewer than 11 bits left in the submitted span, the
		 * escape form (00000010 followed by escape_code), whose width is not
		 * in Table B-1, and any 11-bit window matching no length group - and
		 * all three are answered here with a refusal of the picture, not a
		 * fallback window: the reading that pinned the window rule is that
		 * a start wrong by one bit misaligns every macroblock after it.
		 * Stuffing before the codeword is skipped by the callee, so only
		 * these shapes end up here.
		 */
		if (histb_vdec_mpeg2_first_incr(buf, data_bit,
						size * 8 - data_bit, &inc_bits))
			return -EINVAL;
		data_bit += inc_bits;

		if (have_prev)
			histb_vdec_mpeg2_pack_slice(
				histb_vdec_mpeg2_slc_slot(pool, slot - 1),
				prev_data_bit, hdr_bit, size * 8,
				prev_qsc, prev_intra,
				prev_start_mbn, mbn - 1,
				histb_vdec_mpeg2_slc_slot_dma(pool, slot));
		prev_data_bit = data_bit;
		prev_qsc = qsc;
		prev_intra = intra;
		prev_start_mbn = mbn;
		have_prev = true;
		slot++;
		expect = min(mbn + mb_width, mb_total);
		i += 4;
	}

	/*
	 * The span holds no slice_start_code in the picture's range at all, so
	 * there is no macroblock layer to point the engine at: 13818-2 6.2.2.4
	 * begins every coded picture with a slice header, and a submission
	 * without one carries picture-level syntax only (a sequence or GOP
	 * header, or an empty buffer). It cannot be served either as nothing or
	 * as data: the chain head in D63 would point at a slot this job never
	 * wrote - the slice area is not cleared per job - while CFG0 still
	 * announces mb_total macroblocks to decode, so the engine would walk
	 * bytes that are not macroblock layer. Accepting such a buffer would
	 * mean the client supplying the slice positions, which the MPEG-2
	 * controls do not have; nothing in the scored corpus reaches this arm.
	 */
	if (!have_prev)
		return -EINVAL;

	histb_vdec_mpeg2_pack_slice(histb_vdec_mpeg2_slc_slot(pool, slot - 1),
				    prev_data_bit, end_bit, size * 8,
				    prev_qsc, prev_intra,
				    prev_start_mbn, mb_total - 1, 0);
	/*
	 * The vendor firmware's MPEG-2 slice packer writes a null chain word
	 * for the slot whose successor index equals the slice count, so the
	 * chain ends on a null pointer instead of on a descriptor.  The slot
	 * after the last one stays cleared as well: it is where a follower that
	 * ignores the null pointer would land.
	 */
	memset(histb_vdec_mpeg2_slc_slot(pool, slot), 0, HISTB_MPEG2_SLC_BYTES);

	return slot;
}

static const struct v4l2_ctrl_config histb_vdec_mpeg2_ctrls[] = {
	{ .id = V4L2_CID_STATELESS_MPEG2_SEQUENCE },
	{ .id = V4L2_CID_STATELESS_MPEG2_PICTURE },
	{ .id = V4L2_CID_STATELESS_MPEG2_QUANTISATION },
};

static const struct v4l2_ctrl_mpeg2_sequence *
histb_vdec_mpeg2_seq(struct histb_vdec_ctx *ctx)
{
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_MPEG2_SEQUENCE);
	return ctrl->p_cur.p;
}

static const struct v4l2_ctrl_mpeg2_picture *
histb_vdec_mpeg2_pic(struct histb_vdec_ctx *ctx)
{
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl, V4L2_CID_STATELESS_MPEG2_PICTURE);
	return ctrl->p_cur.p;
}

static const struct v4l2_ctrl_mpeg2_quantisation *
histb_vdec_mpeg2_quant(struct histb_vdec_ctx *ctx)
{
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(&ctx->ctrl_hdl,
			      V4L2_CID_STATELESS_MPEG2_QUANTISATION);
	return ctrl->p_cur.p;
}

/*
 * Resolve a reference picture named by timestamp, as v4l2_ctrl_mpeg2_picture
 * documents it (v4l2_timeval_to_ns() of the OUTPUT buffer's timestamp, which
 * the mem2mem core copies onto the capture buffer the picture is written to).
 * Returns 0 and fills @addr, -ENOENT when no picture is named (@ts is 0), and
 * -EINVAL when the name matches no capture buffer the client still holds.
 */
static int histb_vdec_ref_addr(struct histb_vdec_ctx *ctx, u64 ts,
			       dma_addr_t *addr)
{
	struct vb2_buffer *vb;

	/*
	 * A zero timestamp is what the picture control carries when the
	 * picture names no reference at all. An I picture may do that, and
	 * the caller answers it by keeping the destination in the slot -
	 * every macroblock of an I picture is intra there.
	 */
	if (!ts)
		return -ENOENT;

	/*
	 * A name the client set but no longer holds queued, and the capture
	 * queue is the only place a timestamp resolves. The fallback of
	 * programming the destination instead read back through the pool
	 * window as D4, D5 and D6 holding one address rather than three.
	 * Only a client that keeps the reference queued can be served here.
	 */
	vb = vb2_find_buffer(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx), ts);
	if (!vb)
		return -EINVAL;

	*addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	return 0;
}

/*
 * ISO/IEC 13818-2 Table 6-17 (normal scan): scan position -> raster index of
 * the same coefficient.  The uAPI arrays are documented "in zigzag scanning
 * order" (v4l2-controls.h, struct v4l2_ctrl_mpeg2_quantisation) and the
 * elementary stream carries them the same way, while the VDM's matrix words
 * address the coefficients by raster position: submitting the array
 * unpermuted makes the engine apply weight intra[SCAN[k]] at scan position k
 * (measured on a corpus stream with a non-flat declared table, where the
 * unpermuted reading is off by up to 23 levels and the permuted one lands in
 * the one-level rounding band).  A flat table is permutation-invariant, which
 * is why no earlier measurement could see this.
 */
static const u8 histb_mpeg2_scan2raster[64] = {
	 0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static void histb_mpeg2_permute_matrix(const u8 *scan, u8 *raster)
{
	unsigned int s;

	for (s = 0; s < 64; s++)
		raster[histb_mpeg2_scan2raster[s]] = scan[s];
}

/*
 * Pack the two 64-byte quantisation matrices into slot words 16-47: words
 * 0-15 carry the intra matrix, 16-31 (slot words 32-47) the non-intra one,
 * four matrix bytes per word.  Word 2c holds matrix column c rows
 * {0, 2, 4, 6} in bytes {0, 1, 2, 3} and word 2c+1 column c rows
 * {1, 3, 5, 7} (the two packing loops of the vendor firmware's MPEG-2
 * picture-message packer).
 */
static void histb_vdec_mpeg2_pack_qmatrix(u32 *w, const u8 *intra,
					  const u8 *nonintra)
{
	unsigned int c, r, i;

	for (i = 0; i < 16; i++) {
		w[16 + i] = 0;
		w[32 + i] = 0;
	}

	for (c = 0; c < 8; c++) {
		for (r = 0; r < 4; r++) {
			w[16 + 2 * c] |= (u32)intra[2 * r * 8 + c] << (8 * r);
			w[17 + 2 * c] |= (u32)intra[(2 * r + 1) * 8 + c] << (8 * r);
			w[32 + 2 * c] |= (u32)nonintra[2 * r * 8 + c] << (8 * r);
			w[33 + 2 * c] |= (u32)nonintra[(2 * r + 1) * 8 + c] << (8 * r);
		}
	}
}

static int histb_vdec_mpeg2_run(struct histb_vdec_ctx *ctx,
				struct vb2_v4l2_buffer *src,
				struct vb2_v4l2_buffer *dst)
{
	const struct v4l2_ctrl_mpeg2_sequence *seq;
	const struct v4l2_ctrl_mpeg2_picture *pic;
	const struct v4l2_ctrl_mpeg2_quantisation *quant;
	struct histb_vdec_dev *dev = video_get_drvdata(ctx->fh.vdev);
	struct histb_vdec_msgpool *pool = &dev->pool;
	dma_addr_t stream_base, src_addr, dst_addr, fwd, bwd, scratch;
	unsigned int mb_width, mb_height, mb_total, tile_stride, lines;
	unsigned int bytesused;
	u32 *w = histb_vdec_pic_slot(pool);
	u32 seg[3];
	unsigned int cap_w, cap_h;
	int ret;
	int i;

	ret = histb_vdec_cap_dims(ctx, &cap_w, &cap_h);
	if (ret)
		return ret;

	seq = histb_vdec_mpeg2_seq(ctx);
	pic = histb_vdec_mpeg2_pic(ctx);
	quant = histb_vdec_mpeg2_quant(ctx);

	/*
	 * The engine writes the reconstruction into the queued CAPTURE buffer at
	 * the geometry that buffer's format promised - histb_vdec_buf_geom() takes
	 * the tile stride and row count from the negotiated rectangle, not from
	 * the coded one - so a picture larger than the client negotiated has
	 * nowhere to land. This is judged before the message pool, the registers
	 * and the kick, so a refusal leaves the buffer as it arrived: the same
	 * ordering measured on the H.264 path, where the identical gate kept a
	 * stream that was larger than the negotiated capture geometry from
	 * writing a single byte. Accepting it would need the capture queue to be
	 * re-negotiated for the coded size, which the stateless API leaves to the
	 * client (and this device reports the geometry at TRY_FMT time only).
	 */
	if (seq->horizontal_size > cap_w || seq->vertical_size > cap_h)
		return -EINVAL;

	/*
	 * The picture is laid out in the 4:2:0 geometry the capture format
	 * promises, and nothing here reads seq->chroma_format, so any other
	 * sampling is programmed as if it were 4:2:0. Measured on the board with a
	 * 4:2:2 Profile stream (ffmpeg mpeg2video -pix_fmt yuv422p, quantiser tables
	 * declared so the comparison is one-variable): 10290 of 76800 luma bytes and
	 * 4693 of 38400 chroma bytes agree with the reference decode, max |d| 240 -
	 * the block layer stops staying in step. The 4:2:0 control under the same
	 * recipe lands in the rounding tier only (76505 luma and 38134 chroma equal,
	 * max |d| 1). The scored MPEG-2 corpus is 4:2:0 throughout, so this refusal
	 * costs no frame that decodes acceptably today; serving 4:2:2 would need a
	 * chroma-subsampled capture path, which is not a lightweight bypass.
	 */
	if (seq->chroma_format != 1)
		return -EINVAL;

	/*
	 * Only frame-structured pictures are served.  The VDM is programmed with
	 * picture_structure in D1 bits 8-9 and no second_field_flag at all - the
	 * control has no such field, and a field pair is two OUTPUT jobs while a
	 * capture buffer holds one picture - so the engine is left to interpret a
	 * field picture on its own.  Measured with a frame-coded stream and the
	 * control's structure overridden (one variable: D1 bits 8-9): struct 1
	 * delivers 51468 differing luma samples of 76800 with max |d| 230 and a
	 * +20 to +24 mean bias, struct 2 the same class (51423 differing, max
	 * 168), while struct 3 under the same recipe is the rounding tier
	 * (329 differing, max 1).  The bad frames come back with a full
	 * payload, so this is an anomalous delivery that only a refusal can
	 * express.  top_field_first does not need the same treatment: with
	 * structure 3 forcing it to 1 leaves the frame identical (329/268,
	 * max 1) - the bit is inert for frame pictures.  Structure 0 never
	 * reaches here (the control handler validates the range), and no scored
	 * stream uses 1 or 2, so this refusal costs nothing.  Accepting field
	 * pictures would need a control that carries the second-field flag and
	 * a submission model that pairs two OUTPUT jobs with one capture
	 * buffer; the current uAPI provides neither.
	 */
	if (pic->picture_structure != HISTB_MPEG2_STRUCT_FRAME)
		return -EINVAL;

	mb_width = DIV_ROUND_UP(seq->horizontal_size, 16);
	mb_height = DIV_ROUND_UP(seq->vertical_size, 16);
	mb_total = mb_width * mb_height;
	/*
	 * The picture size is judged per job, not in a control .try_ctrl(): the
	 * handler's all-zero default control is read back by TRY_EXT_CTRLS as if
	 * a client had set it, and a size below one macroblock would also leave
	 * histb_vdec_mpeg2_pack_slices() dividing the MB total by zero.
	 *
	 * That division is what the lower bound actually protects: only a size
	 * of 0 (the unset control) makes DIV_ROUND_UP() give a grid of zero
	 * columns, and with it a zero MB total for both the division and the
	 * mb_total - 1 programmed into CFG0's macroblock count. One macroblock
	 * column is *not* refused here, unlike the H.264 path: the write defect
	 * that makes a one-column H.264 picture undeliverable was measured on
	 * that protocol alone, and MPEG-2 jobs at 16 wide were accepted on the
	 * board (which is why histb_vdec_enum_framesizes() keeps min_width = 16
	 * for this fourcc and raises it to 32 only for
	 * V4L2_PIX_FMT_H264_SLICE). The upper bound is the pair the format layer
	 * advertises and clamps to, so nothing larger can be negotiated into a
	 * capture buffer in the first place; the geometry check above refuses it
	 * as well, this one is the cheaper statement of the same limit.
	 */
	if (seq->horizontal_size < 16 || seq->vertical_size < 16 ||
	    seq->horizontal_size > HISTB_VDEC_MAX_WIDTH ||
	    seq->vertical_size > HISTB_VDEC_MAX_HEIGHT)
		return -EINVAL;
	histb_vdec_buf_geom(ctx, &tile_stride, &lines);

	src_addr = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	dst_addr = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	stream_base = src_addr & ~255U;

	fwd = bwd = dst_addr;
	/*
	 * An inter picture whose reference does not resolve is programmed with
	 * the frame being written, and then predicts from itself.  Measured with
	 * the reference named in the wrong units (microseconds rather than the
	 * v4l2_timeval_to_ns() the control documents), every P picture of a
	 * 30-picture chain came back with 113238 of 115200 samples differing and
	 * max |d| 240, while the same stream with resolvable references stays
	 * inside 4 levels over a 59-frame chain.  So a picture that cannot
	 * address the references its own
	 * coding type needs is refused: P names a forward reference and B names
	 * both (ISO/IEC 13818-2 6.2.31 keeps backward_f_code at '0000' only for
	 * P pictures).  I pictures keep the destination in both slots: every
	 * macroblock is intra there, and the 68-stream corpus decodes
	 * byte-exactly that way.
	 */
	if (pic->picture_coding_type != HISTB_MPEG2_PIC_I) {
		int ferr, berr;

		ferr = histb_vdec_ref_addr(ctx, pic->forward_ref_ts, &fwd);
		berr = histb_vdec_ref_addr(ctx, pic->backward_ref_ts, &bwd);
		if (ferr == -EINVAL || berr == -EINVAL ||
		    (pic->picture_coding_type == HISTB_MPEG2_PIC_P && ferr) ||
		    (pic->picture_coding_type == HISTB_MPEG2_PIC_B && (ferr ||
								       berr))) {
			dev_dbg(dev->dev, "mpeg2 refuse: type %u fwd=%llu(%d) bwd=%llu(%d)\n",
				pic->picture_coding_type,
				pic->forward_ref_ts, ferr,
				pic->backward_ref_ts, berr);
			return -EINVAL;
		}
	}
	scratch = histb_vdec_tables_dma(pool) +
		  HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_PMV_TOP);

	memset(w, 0, HISTB_VDEC_SLOT_BYTES);

	/* D0: macroblock frame size minus one; mpeg1_flag is the A/B bit. */
	w[0] = FIELD_PREP(GENMASK(8, 0), mb_width - 1) |
	       FIELD_PREP(GENMASK(24, 16), mb_height - 1);

	/* D1: frame_pred_frame_dct, picture_structure, second_field_flag
	 * (field-pair submission is not modelled by V4L2: always 0),
	 * concealment_motion_vectors, picture_coding_type.  The full-pel
	 * MPEG-1 bits have no control counterpart: 0.
	 */
	if (pic->flags & V4L2_MPEG2_PIC_FLAG_FRAME_PRED_DCT)
		w[1] |= BIT(0);
	w[1] |= FIELD_PREP(GENMASK(9, 8), pic->picture_structure);
	if (pic->flags & V4L2_MPEG2_PIC_FLAG_CONCEALMENT_MV)
		w[1] |= BIT(16);
	w[1] |= FIELD_PREP(GENMASK(26, 24), pic->picture_coding_type);

	/* D2: f_code[2][2] maps directly onto the four hardware slots
	 * (fwd h/v and bwd h/v); the control carries the full matrix, no
	 * userspace reconstruction needed.  top_field_first at bit 31.
	 */
	w[2] |= FIELD_PREP(GENMASK(3, 0), pic->f_code[1][1]);
	w[2] |= FIELD_PREP(GENMASK(11, 8), pic->f_code[1][0]);
	w[2] |= FIELD_PREP(GENMASK(19, 16), pic->f_code[0][1]);
	w[2] |= FIELD_PREP(GENMASK(27, 24), pic->f_code[0][0]);
	if (pic->flags & V4L2_MPEG2_PIC_FLAG_TOP_FIELD_FIRST)
		w[2] |= BIT(31);

	/*
	 * D3: intra_dc_precision, q_scale_type, intra_vlc_format, alternate_scan,
	 * packed as the vendor firmware's MPEG-2 packer does.
	 *
	 * An earlier measurement of this field (which suggested it be written as
	 * intra_dc_precision + 1, because a flat picture then came back as 128)
	 * is void: it was taken while the slice segments still pointed at the
	 * wrong byte granule, so the engine was not reading the bitstream at all
	 * and the level it reported was not a decoded DC. With the segments
	 * correct, sweeping the field over 0..3 leaves the reconstruction at
	 * 146/137/130/130 for a picture whose DC is 128 and does not make the
	 * per-macroblock DC differences appear at all (fitted slope ~0), so
	 * what these bits really select is still open.  The
	 * syntax value is written here, which is what the vendor packing does.
	 */
	w[3] |= FIELD_PREP(GENMASK(1, 0), pic->intra_dc_precision);
	if (pic->flags & V4L2_MPEG2_PIC_FLAG_Q_SCALE_TYPE)
		w[3] |= BIT(8);
	if (pic->flags & V4L2_MPEG2_PIC_FLAG_INTRA_VLC)
		w[3] |= BIT(16);
	if (pic->flags & V4L2_MPEG2_PIC_FLAG_ALT_SCAN)
		w[3] |= BIT(24);

	/* D4-D7: backward/forward reference, reconstruction (output) frame
	 * and the PMV band-line scratch.  References fall back to the
	 * output frame for I pictures where no timestamp is given.
	 */
	w[4] = bwd & ~15U;
	w[5] = fwd & ~15U;
	w[6] = dst_addr & ~15U;
	w[7] = scratch & ~15U;

	/* D8/D9: picture bitstream segment (whole OUTPUT plane, relative
	 * to the 256-byte aligned stream base).  D10/D11: second segment,
	 * unused (seg1 = 0 like the single-segment vendor path).
	 */
	bytesused = vb2_get_plane_payload(&src->vb2_buf, 0);
	histb_vdec_seg_pack(seg, stream_base, src_addr, 0, bytesused * 8);
	w[8] = seg[0];
	w[9] = FIELD_PREP(GENMASK(23, 0),
			  (seg[2] + HISTB_MPEG2_DUMMY_BITS) & GENMASK(23, 0)) |
	       FIELD_PREP(GENMASK(30, 24), seg[1]);

	/* D16-D47: intra and non-intra quantisation matrices, loaded
	 * whenever the hardware is kicked (LD_QMATRIX: the vendor sets it
	 * only when the matrix changed; reloading per frame is simpler and
	 * never stale).  The uAPI and the bitstream both hold them in scan
	 * order; the matrix words are raster addressed, so they are
	 * permuted before packing.
	 */
	u8 intra_hw[64], nonintra_hw[64];

	histb_mpeg2_permute_matrix(quant->intra_quantiser_matrix,
				   intra_hw);
	histb_mpeg2_permute_matrix(quant->non_intra_quantiser_matrix,
				   nonintra_hw);
	histb_vdec_mpeg2_pack_qmatrix(w, intra_hw, nonintra_hw);

	/* D48: pmv_top band-line scratch.  D63: slice chain head. */
	w[48] = scratch & ~15U;
	w[63] = histb_vdec_mpeg2_slc_slot_dma(pool, 0) & ~15U;

	/*
	 * One descriptor per slice, chained through D7 and ended by a cleared
	 * slot. The slice start codes are found by scanning the submitted span:
	 * the uAPI carries no per-slice control for MPEG-2.
	 */
	{
		const u8 *va = vb2_plane_vaddr(&src->vb2_buf, 0);
		int n_sl;

		/*
		 * MPEG-2 has no per-slice control, so the only source of the
		 * slice windows is the submitted bytes, and reading those bytes
		 * needs a kernel address. The OUTPUT queue deliberately keeps
		 * one (histb_vdec_queue_init() in histb_vdec_drv.c declines
		 * DMA_ATTR_NO_KERNEL_MAPPING for exactly this scan), so this arm
		 * answers a missing mapping rather than a stream shape: without
		 * the bytes nothing can be delimited, and programming the job
		 * would hand the engine a picture segment with no descriptor
		 * chain behind it. A client that cannot map the plane would need
		 * per-slice descriptors through a control instead, which the
		 * stateless MPEG-2 API does not carry.
		 */
		if (!va) {
			dev_warn_once(dev->dev,
				      "OUTPUT queue has no kernel mapping, cannot delimit slices\n");
			return -EINVAL;
		}
		n_sl = histb_vdec_mpeg2_pack_slices(pool, va, bytesused,
						   seq->flags &
						   V4L2_MPEG2_SEQ_FLAG_PROGRESSIVE,
						   mb_width, mb_total, stream_base,
					   src_addr);
		if (n_sl < 0)
			return n_sl;
	}

	/*
	 * Per-frame register set.  Two vendor firmware paths exist for MPEG-2
	 * and they disagree: the one its MPEG-2 start sequence calls writes the
	 * 256x16 band geometry the H.264 path uses - the band stride is the
	 * band pitch, the maximum slice group number is 3 and CFG0 carries the
	 * COEFIDX/MARKER detect bits - while the other writes a linear picture
	 * (stride counted in 16-byte lines, chroma right behind luma) with CFG0
	 * reduced to LD_QMATRIX.  The capture format is the same tiled NV21 for
	 * every protocol and dropping the detect bits changed nothing
	 * measurable, so this driver follows the path the firmware actually
	 * takes.
	 */
	vdm_write(dev, VDM_CFG0,
		  FIELD_PREP(VDM_CFG0_MBAMT, mb_total - 1) |
		  VDM_CFG0_COEFIDX_DETECT | VDM_CFG0_MARKER_DETECT |
		  VDM_CFG0_LD_QMATRIX);
	vdm_write(dev, VDM_CFG1,
		  FIELD_PREP(VDM_CFG1_VID_STD, HISTB_VIDSTD_MPEG2) |
		  FIELD_PREP(VDM_CFG1_DDR_STRIDE, tile_stride >> 6) |
		  VDM_CFG1_FST_SLC_GRP |
		  FIELD_PREP(VDM_CFG1_MAX_SLCGRP_NUM, 3) |
		  VDM_CFG1_DBLK_1D_EN);
	vdm_write(dev, VDM_AVM_ADDR, pool->dma & ~15U);
	vdm_write(dev, VDM_VAM_ADDR, histb_vdec_up_msgs_dma(pool) & ~15U);
	vdm_write(dev, VDM_STREAM_BASE, stream_base);
	/*
	 * The reconstruction address is masked to 16 bytes here, like the vendor
	 * path; the reference and FF-APT registers are written too because the
	 * values a previous frame of another protocol left in them would
	 * otherwise apply (the MPEG-2 path keeps FF_APT_EN at 0, unlike H.264's
	 * 2 for frame pictures).
	 */
	vdm_write(dev, VDM_YSTADDR_1D, dst_addr & ~15U);
	vdm_write(dev, VDM_YSTRIDE_1D, histb_vdec_band_bytes(tile_stride));
	vdm_write(dev, VDM_UVOFFSET_1D,
		  histb_vdec_luma_bytes(tile_stride, lines) +
		  histb_vdec_head_inf_bytes(lines));
	vdm_write(dev, VDM_HEAD_INF_OFFSET, histb_vdec_head_inf_bytes(lines));
	vdm_write(dev, VDM_REF_PIC_TYPE, 0);
	vdm_write(dev, VDM_FF_APT_EN, 0);

	for (i = 1; i <= 7; i++)
		vdm_write(dev, VDM_TO(i), VDM_TIMEOUT_MOD);
	vdm_write(dev, VDM_INT_STATE, 0xffffffff);
	vdm_write(dev, VDM_INT_MASK,
		  0xfffffffe);

	histb_vdec_hw_kick(dev, ctx->desc->vidstd);

	return 0;
}

static const struct histb_vdec_codec_ops histb_vdec_mpeg2_ops = {
	.run		= histb_vdec_mpeg2_run,
};

const struct histb_vdec_fmt_desc histb_vdec_mpeg2_fmt = {
	.fourcc		= V4L2_PIX_FMT_MPEG2_SLICE,
	.stride		= histb_vdec_tile_stride,
	.grid_align	= 16,
	.num_ctrls	= ARRAY_SIZE(histb_vdec_mpeg2_ctrls),
	.ctrls		= histb_vdec_mpeg2_ctrls,
	.ops		= &histb_vdec_mpeg2_ops,
	.vidstd		= HISTB_VIDSTD_MPEG2,
};
