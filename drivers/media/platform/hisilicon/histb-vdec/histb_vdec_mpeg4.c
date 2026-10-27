// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSilicon Hi3798MV100 VDM stateless MPEG-4 Part 2 / H.263 short-header
 * decoder (V4L2 codec layer).
 *
 * Neither protocol has a V4L2 stateless control set, so the OUTPUT plane
 * carries the raw elementary stream and the driver reads the VOL/VOP, H.263
 * picture and slice headers itself - the shape coda uses for the same class of
 * hardware (coda_buffer_parse_headers() in
 * drivers/media/platform/chips-media/coda/coda-bit.c). The VDM still consumes
 * fully parsed syntax: one picture message (slot words D0-D63) plus a chain of
 * 32-byte slice descriptors. It never looks at a start code.
 *
 * Message and register layout: re-derived field by field from the three packing
 * paths of the vendor firmware's MPEG-4 decoder layer - the register setup, the
 * picture message and the slice descriptors. Header field order and widths are
 * cross-checked against the vendor's own software MPEG-4/H.263 decoder, which
 * is ffmpeg-derived and agrees with the firmware's syntax layer.
 *
 * Capability gates, all refused in .run() rather than decoded at a guessed bit
 * position: data partitioning, new prediction, reduced-resolution VOPs,
 * scalability, non-rectangular shapes, studio and core profiles, interlaced
 * VOLs, GMC and sprite usage, complexity-estimation signalling, a
 * quant_precision other than 5, MPEG-4 short VOP headers, and the H.263+
 * extended PTYPE branch (source_format 6/7, where Annex I/D/J signalling lives)
 * along with the group geometry that picture message D1 cannot state (a 16CIF
 * picture's 352-macroblock group exceeds its seven-bit field). The firmware's
 * VOL checks refuse the same set and report each of those shapes as
 * unsupported.
 *
 * Verification status: the header parse is exercised on 19 streams by a
 * user-space harness that builds against these functions; the message and
 * register values below come from re-deriving the vendor firmware's packing,
 * not from a board run, so some of them are still unmeasured on hardware.
 */

#include <linux/bitfield.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#include "histb_vdec.h"

/*
 * Slice descriptors are 32 bytes (8 words), one per slice, chained through word
 * D7: the vendor firmware's MPEG-4 slice packer writes entry i at (slice pool
 * base) + i * 32 and puts (slice pool base) + (i + 1) * 32 in D7, ending the
 * chain with 0. The picture message D63 points at the pool base, so the first
 * descriptor has to sit there.
 */
#define HISTB_MPEG4_SLC_BYTES		32
#define HISTB_MPEG4_MAX_SLICES		511

/*
 * The CFG1 VID_STD field for this engine is 2, which is what the vendor
 * firmware's MPEG-4 register setup writes (the literal 0x20000002: VID_STD 2
 * plus DBLK_1D_EN); the register value is not inherited from any other
 * protocol. The picture message word D0 carries 4 (the H.263 protocol code)
 * when the picture uses short headers, which is how the macroblock layer is
 * told to read GOB headers instead of MPEG-4 packet headers.
 */
#define HISTB_MPEG4_CFG1_VIDSTD		2
#define HISTB_MPEG4_D0_H263_SHORT	4

/* Motion-vector storage per macroblock, the size the vendor firmware allocates
 * the motion-vector buffer by (the same 32 bytes per macroblock the H.264 layer
 * uses).
 */
#define HISTB_MPEG4_PMV_BYTES_PER_MB	32

/* MPEG-4 vop_coding_type (14496-2 table 6-2). */
#define HISTB_MPEG4_I_VOP		0
#define HISTB_MPEG4_P_VOP		1
#define HISTB_MPEG4_B_VOP		2

/*
 * MPEG-4 start codes, held as the low byte of the 32-bit 0x000001xx code (the
 * three leading bytes are what the scan matches). The VOL is coded 0x120 +
 * vol_number and the video object 0x100 + vo_number: that is what ffmpeg's
 * MPEG-4 encoder writes and what the vendor firmware's own picture-header
 * dispatcher matches - start code 0x120..0x12F is the VOL there too, and it
 * refuses an access unit that carried none. Xvid and Lavc streams were checked
 * to carry the VOL at 0x120 and the visual object at 0x1B5, so 0x1B5 is not a
 * VOL and must not be parsed as one. Low bytes 0x20..0x2F are also slice codes
 * (mbk_number 32..47), which is why only the codes *ahead of the VOP* are read
 * as a VOL - the same disambiguation the vendor firmware applies. Everything
 * else the scan meets (VOS 0xB0, video object 0x00..0x1F, GOP 0xB3, user data
 * 0xB2, visual object 0xB5) contributes no descriptor input and is stepped
 * over.
 */
#define HISTB_MP4_CODE_VOL_FIRST	0x20
#define HISTB_MP4_CODE_VOL_LAST		0x2f
#define HISTB_MP4_CODE_VOP		0xb6

/*
 * Default quant matrices in macroblock (raster) order, entry 0 = DC, taken from
 * the two constant tables the vendor firmware's MPEG-4 syntax layer carries, 64
 * entries each. They are also what ffmpeg's MPEG-4 decoder installs as its
 * defaults, so a stream that sets vol_quant_type and loads no table of its own
 * has both sides reading the same weights. The first transcription here was not
 * that table (row 1 ended 32 not 28, rows 2..7 were a diagonal progression),
 * and that is what left mpegquant.raw 16277 bytes wrong by as much as 31: the
 * wrong weight is read per frequency, so the error grows with the coefficient's
 * position.
 */
static const u8 histb_mp4_intra_matrix[64] = {
	 8, 17, 18, 19, 21, 23, 25, 27,
	17, 18, 19, 21, 23, 25, 27, 28,
	20, 21, 22, 23, 24, 26, 28, 30,
	21, 22, 23, 24, 26, 28, 30, 32,
	22, 23, 24, 26, 28, 30, 32, 35,
	23, 24, 26, 28, 30, 32, 35, 38,
	25, 26, 28, 30, 32, 35, 38, 41,
	27, 28, 30, 32, 35, 38, 41, 45,
};

static const u8 histb_mp4_inter_matrix[64] = {
	16, 17, 18, 19, 20, 21, 22, 23,
	17, 18, 19, 20, 21, 22, 23, 24,
	18, 19, 20, 21, 22, 23, 24, 25,
	19, 20, 21, 22, 23, 24, 26, 27,
	20, 21, 22, 23, 25, 26, 27, 28,
	21, 22, 23, 24, 26, 27, 28, 30,
	22, 23, 24, 26, 27, 28, 30, 31,
	23, 24, 25, 27, 28, 30, 31, 33,
};

/* A VOL lists a custom matrix in scan order while the descriptor table is
 * indexed by block position; this is the mapping the vendor firmware applies
 * when it loads a VOL matrix (scan entry i lands at block position
 * histb_mp4_zigzag[i]).
 */
static const u8 histb_mp4_zigzag[64] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 50, 43, 36, 29, 22,
	15, 23, 30, 37, 44, 51, 52, 45,
	38, 31, 39, 46, 53, 54, 47, 55,
	56, 57, 62, 63, 58, 59, 60, 61,
};

/*
 * VOL parameters, kept per context: a raw-fourcc submission carries no
 * sequence-level control, and a stream writes the VOL once while every later
 * buffer carries only its VOP. Dropped with the context (and by a format change
 * through .stop()/.start()).
 */
struct histb_vdec_mpeg4_vol {
	unsigned int width;
	unsigned int height;
	unsigned int time_incr_res;
	unsigned int time_incr_bits;
	u8 quant_type;
	u8 quarter_sample;
	u8 resync_marker_disable;
	bool parsed;
	/* Set when a configuration boundary was met but not read (a VOL this
	 * parser could not parse in a stream whose packet codes make its
	 * position ambiguous): the grid of every later picture is then unknown
	 * and they are refused until an access unit opens with a readable VOL.
	 */
	bool config_pending;
	u8 intra_matrix[64];
	u8 inter_matrix[64];
};

/* An anchor picture, the reference of the pictures that follow it. */
struct histb_vdec_mpeg4_ref {
	u64 timestamp;
	unsigned int pmv_slot;
	bool valid;
};

struct histb_vdec_mpeg4_ctx {
	void *pmv_va[HISTB_VDEC_MAX_PMV_SLOTS];
	dma_addr_t pmv_dma[HISTB_VDEC_MAX_PMV_SLOTS];
	unsigned int pmv_slot_size;
	struct histb_vdec_mpeg4_vol vol;
	/* [0] is the most recently decoded anchor, [1] the one before it */
	struct histb_vdec_mpeg4_ref anchor[2];
	/*
	 * The vop time-code accumulation the picture message's B-VOP distances
	 * (word D3) are derived from, same units as the vendor's decoder: a
	 * second count in modulo_time_base steps and a picture time in
	 * vop_time_increment_resolution ticks.
	 */
	unsigned int time_base;
	unsigned int last_time_base;
	unsigned int last_non_b_time;
	unsigned int pp_time;
};

/* Everything the descriptors need about one picture. */
struct histb_vdec_mpeg4_pic {
	unsigned int width;
	unsigned int height;
	unsigned int mb_width;
	unsigned int mb_height;
	unsigned int mb_total;
	unsigned int mb_bits;		/* width of a slice macroblock number */
	unsigned int msg_mb_bits;	/* the width MP4HAL states in D2 */
	unsigned int coding_type;
	unsigned int quant;
	unsigned int rounding;
	unsigned int fcode_fwd;
	unsigned int fcode_bwd;
	unsigned int intra_dc_vlc_thr;
	unsigned int quarter_sample;
	unsigned int resync_marker_disable;
	unsigned int time_incr_bits;
	unsigned int time_modulo;	/* modulo_time_base run length */
	unsigned int time_increment;	/* vop_time_increment */
	unsigned int quant_type;
	unsigned int mb_per_gob;	/* H.263: macroblocks in one GOB */
	unsigned int num_gobs;
	bool short_header;
};

/* One parsed slice header. */
struct histb_vdec_mpeg4_slice {
	unsigned int data_bit;
	unsigned int start_mbn;
	unsigned int quant;
	unsigned int coding_type;
	unsigned int intra_dc_vlc_thr;
};

/* ------------------------------------------------------------------ bits */

static unsigned int histb_vdec_mpeg4_bits(const u8 *p, unsigned int pos,
					  unsigned int n)
{
	unsigned int val = 0;
	unsigned int i;

	for (i = 0; i < n; i++)
		val = val << 1 |
		      ((p[(pos + i) / 8] >> (7 - (pos + i) % 8)) & 1U);
	return val;
}

static unsigned int histb_vdec_mpeg4_bit(const u8 *p, unsigned int pos)
{
	return (p[pos / 8] >> (7 - pos % 8)) & 1U;
}

/* Number of zero bits at @pos, stopping at @max or at the first one. */
static unsigned int histb_vdec_mpeg4_zero_run(const u8 *p, unsigned int pos,
					      unsigned int max)
{
	unsigned int n = 0;

	while (n < max && !histb_vdec_mpeg4_bit(p, pos + n))
		n++;
	return n;
}

/*
 * Bounds-checked bit cursor. Every read advances @p, and a read that would pass
 * the end of the span latches an error instead of running past the buffer
 * mapping - a VOL near the end of a full OUTPUT buffer is the case that would
 * otherwise read unowned memory.
 */
struct histb_vdec_mpeg4_bs {
	const u8 *buf;
	unsigned int limit;
	unsigned int p;
	int err;
};

static unsigned int histb_vdec_mpeg4_bs_get(struct histb_vdec_mpeg4_bs *bs,
					    unsigned int n)
{
	unsigned int v;

	if (bs->err || bs->p + n > bs->limit) {
		bs->err = -EINVAL;
		return 0;
	}
	v = histb_vdec_mpeg4_bits(bs->buf, bs->p, n);
	bs->p += n;
	return v;
}

static void histb_vdec_mpeg4_bs_skip(struct histb_vdec_mpeg4_bs *bs,
				     unsigned int n)
{
	histb_vdec_mpeg4_bs_get(bs, n);
}

/* Find a byte-aligned MPEG-4 32-bit start code 0x000001xx at or after @from. */
static int histb_vdec_mpeg4_find_code(const u8 *buf, unsigned int size,
				      unsigned int from, unsigned int *pos,
				      unsigned int *code)
{
	unsigned int i;

	for (i = from; i + 4 <= size; i++) {
		if (buf[i] || buf[i + 1] || buf[i + 2] != 1)
			continue;
		*pos = i;
		*code = buf[i + 3];
		return 0;
	}

	/*
	 * No byte-aligned 0x000001xx code is left, and every VOL and VOP sits
	 * behind one (14496-2 6.2.1, 6.2.5), so the rest of the span holds no
	 * header to read. A picture scan that meets this has no grid, quantiser
	 * or packet geometry to program, so the job is refused; serving it
	 * needs the VOP inside the same submission.
	 */
	return -EINVAL;
}

/* ------------------------------------------------------- VOL/VOP parsing */

/*
 * Parse one MPEG-4 VOL header (14496-2 6.2.1); @pos is the byte of the
 * 0x000001B5 start code. Markers are checked where the syntax requires them,
 * because a VOL the driver mis-locates puts every slice window at the wrong
 * bit.
 */
static int histb_vdec_mpeg4_vol(struct histb_vdec_mpeg4_vol *vol, const u8 *buf,
				unsigned int size, unsigned int pos)
{
	struct histb_vdec_mpeg4_bs bs = { buf, size * 8, pos * 8 + 32, 0 };
	unsigned int ver_id = 1, width, height, bits, i, v, last, load;
	u8 *table;

	/* random_access: legal either way, it says nothing the descriptors
	 * carry (the VOL of a short VOP header stream sets it as well, and that
	 * syntax is refused further down for want of a vop_time_code)
	 */
	histb_vdec_mpeg4_bs_skip(&bs, 1);

	/*
	 * video_object_type_indication. The layout walked below is the one
	 * these types share, and the gate keeps the ranges the encoders of this
	 * family actually write: 1..5 (ffmpeg names those values simple,
	 * simple-scalable, core, main and non-strict-timing video object
	 * layers) and 17..20 (advanced simple and its AQC, pixel-adaptive-RLD
	 * and interleaved variants, which is what lavc writes for advanced
	 * simple and what Xvid writes for B-frames or quarter-pel). 6..16
	 * (long-term, slice-structured, the scalability types, ARTS, ACE) and
	 * 21 on (including the studio profiles, whose VOL header is a different
	 * syntax) have no reference here for the field order, and 0 is
	 * reserved.
	 */
	bits = histb_vdec_mpeg4_bs_get(&bs, 8);
	/*
	 * Disposition of the ranges above: this is a refusal with no fallback,
	 * because video_object_type_indication (14496-2 6.2.1) is what tells
	 * the reader which field order follows. Inside the accepted set the
	 * layout walked below is the one those types share; outside it every
	 * later read in this VOL - and with it the grid, the quarter-pel flag
	 * and the position, so the VOL is not cached: a submission that opens
	 * with it is refused here, and one that meets it after a picture's own
	 * data marks the configuration unread until an access unit starts with
	 * a VOL this walk does parse (histb_vdec_mpeg4_tail_config()).
	 */
	if (!bits || (bits > 5 && bits < 17) || bits > 20)
		return -EINVAL;

	/*
	 * video_object_layer_id_present, verid, priority. The verid only
	 * selects the layout branches below (1 = the original version, which
	 * has no quarter-pel/newpred fields); every version above 1 takes the
	 * wider branch, so the value itself is not a capability and is not
	 * gated - with B-frames or quarter-pel the encoders write 5, which is
	 * neither of the two versions the vendor firmware warns about.
	 */
	if (histb_vdec_mpeg4_bs_get(&bs, 1)) {
		ver_id = histb_vdec_mpeg4_bs_get(&bs, 4);
		histb_vdec_mpeg4_bs_skip(&bs, 3);
	}

	/* aspect_ratio_info, the extended form (15) carries
	 * par_width/par_height and Xvid writes it for every 4:3 stream
	 * (ffmpeg's extended-par case)
	 */
	if (histb_vdec_mpeg4_bs_get(&bs, 4) == 15)
		histb_vdec_mpeg4_bs_skip(&bs, 16);

	/* vol_control_parameters: chroma_format, low_delay, vbv_parameters */
	if (histb_vdec_mpeg4_bs_get(&bs, 1)) {
		/*
		 * chroma_format (14496-2 6.2.1) other than the 4:2:0 value 1 is
		 * refused for the same reason as the MPEG-2 path's
		 * seq->chroma_format gate: the capture geometry this engine is
		 * programmed with is 4:2:0 (one luma plane plus the interleaved
		 * chroma plane at histb_vdec_buf_geom()'s offset), and nothing
		 * in the VOL read here reaches the message, so 4:2:2 or 4:4:0
		 * would be laid out as if it were 4:2:0. Serving it needs a
		 * chroma subsampled capture path, which is not a lightweight
		 * bypass.
		 */
		if (histb_vdec_mpeg4_bs_get(&bs, 2) != 1)	/* not 4:2:0 */
			return -EINVAL;
		histb_vdec_mpeg4_bs_skip(&bs, 1);		/* low_delay */
		if (histb_vdec_mpeg4_bs_get(&bs, 1))
			/* 15 m 15 m 15 m 3 11 m 15 m */
			histb_vdec_mpeg4_bs_skip(&bs, 15 + 1 + 15 + 1 + 15 + 1 +
						 3 + 11 + 1 + 15 + 1);
	}

	/*
	 * video_object_layer_shape: rectangular only. Both reference decoders
	 * number that shape 0 (the rectangular entry of their shape tables) and
	 * every VOL measured here reads 0.
	 */
	if (histb_vdec_mpeg4_bs_get(&bs, 2) != 0)
		return -EINVAL;

	/*
	 * marker, time_increment_resolution, marker. The markers are skipped
	 * rather than checked: the VOL writer emits '1' in each of these
	 * positions (ffmpeg's encoder writes a literal one there), so a check
	 * here would reject every real stream, which is why neither the vendor
	 * firmware nor ffmpeg acts on the mismatch.
	 */
	histb_vdec_mpeg4_bs_skip(&bs, 1);
	bits = histb_vdec_mpeg4_bs_get(&bs, 16);
	histb_vdec_mpeg4_bs_skip(&bs, 1);
	/*
	 * vop_time_increment_resolution (14496-2 6.2.1) is the tick the picture
	 * timing is counted in, so 0 has no use anywhere in this driver: it is
	 * the multiplier of every time value below (time_incr_bits is derived
	 * from it, and the B-VOP distances written to D3 are
	 * (modulo_time_base * this + vop_time_increment) differences). A VOL
	 * naming 0 makes both the width of vop_time_increment and the anchor
	 * distances meaningless, so the configuration is refused rather than
	 * cached with a bogus grid.
	 */
	if (!bits)
		return -EINVAL;
	vol->time_incr_res = bits;

	/* the width of a vop_time_increment: bits needed for resolution - 1 */
	vol->time_incr_bits = bits > 1 ? ilog2(bits - 1) + 1 : 1;

	/* fixed_vop_rate, then fixed_vop_time_increment when it is set */
	if (histb_vdec_mpeg4_bs_get(&bs, 1))
		histb_vdec_mpeg4_bs_skip(&bs, vol->time_incr_bits);

	/* marker, width(13), marker, height(13), marker (markers skipped as
	 * above; the vendor's own software decoder skips them outright too)
	 */
	histb_vdec_mpeg4_bs_skip(&bs, 1);
	width = histb_vdec_mpeg4_bs_get(&bs, 13);
	histb_vdec_mpeg4_bs_skip(&bs, 1);
	height = histb_vdec_mpeg4_bs_get(&bs, 13);
	histb_vdec_mpeg4_bs_skip(&bs, 1);
	/*
	 * video_object_layer_width/height (14496-2 6.2.1) are the grid every
	 * later picture is measured on, and they are 13-bit fields: 0 has no
	 * macroblock grid at all (histb_vdec_mpeg4_grid() would derive a total
	 * of 0 from them, which the packet parse width ilog2(total - 1) and the
	 * engine's macroblock count mb_total - 1 cannot express), and a size
	 * beyond HISTB_VDEC_MAX_WIDTH/MAX_HEIGHT is one the format layer
	 * neither advertises nor can clamp into a capture buffer, so no CAPTURE
	 * geometry this job could be programmed against exists. Refused per VOL
	 * rather than per picture: caching such a grid would mislabel every
	 * later picture's descriptor range.
	 */
	if (!width || !height || width > HISTB_VDEC_MAX_WIDTH ||
	    height > HISTB_VDEC_MAX_HEIGHT)
		return -EINVAL;

	/*
	 * interlaced: a field picture is not a submission this driver can make
	 * (one buffer per frame picture, no per-field descriptor), so an
	 * interlaced VOL is refused - the same boundary the H.264 layer keeps.
	 */
	if (histb_vdec_mpeg4_bs_get(&bs, 1))
		return -EINVAL;

	/*
	 * obmc_disable (14496-2 6.2.1): a VOL that clears it announces
	 * overlapped-block motion compensation for every VOP of this sequence,
	 * and the descriptor set has no input for it - the vendor firmware
	 * keeps that flag in one of its MPEG-4 parameter blocks, but nothing in
	 * its firmware or message program reads it, and the engine starts
	 * reading at the descriptor's data bit, so it never sees the header.
	 *
	 * The H.263 picture header's same bit is deliberately consumed and
	 * ignored instead (see histb_vdec_h263_pic()), on the measured argument
	 * that OBMC changes prediction but never the syntax the engine walks.
	 * This VOL gate has not been re-derived under that argument. TODO:
	 * settle it with the same one-variable pair on an MPEG-4 stream - one
	 * VOL with obmc_disable=1 and one identical VOL with it 0, coded
	 * content that has a motion-vector discontinuity between neighbouring
	 * macroblocks - and compare both against the software reference: if the
	 * 0 arm stays inside the rounding band, this gate becomes
	 * consume-and-ignore like its H.263 counterpart.
	 */
	if (!histb_vdec_mpeg4_bs_get(&bs, 1))
		return -EINVAL;

	/* vol_sprite_usage: GMC and sprite warping have no reachable descriptor
	 * path in this port, so any sprite usage is refused.
	 */
	if (histb_vdec_mpeg4_bs_get(&bs, ver_id == 1 ? 1 : 2))
		return -EINVAL;

	/*
	 * not_8_bit (14496-2 6.2.1): set, it opens the quant_precision,
	 * vertical FDC length and horizontal FDC length fields and moves the
	 * sample depth away from 8. Every consumer here is written for the
	 * 8-bit, precision-5 shape: the quantiser travels in the descriptor's
	 * 5-bit field, the matrix words carry one byte per coefficient, and the
	 * capture format is the 8-bit tiled NV21 the board read back
	 * byte-exactly. The firmware's own VOL walk refuses the same set.
	 * Accepting it would need both a depth-carrying capture format and
	 * measured matrix packing for that depth.
	 */
	if (histb_vdec_mpeg4_bs_get(&bs, 1))
		return -EINVAL;

	/*
	 * vol_quant_type, then the two custom matrices the VOL may load. The
	 * descriptor matrices are macroblock-order tables: the defaults, then
	 * any loaded table (scan order, terminated by a zero byte, the
	 * remaining entries replicating the last value). A quant_type=1 VOL
	 * that loads nothing keeps the MPEG-4 defaults - the flat MPEG-2 tables
	 * were seeded here once and scored worse on the board.
	 */
	vol->quant_type = histb_vdec_mpeg4_bs_get(&bs, 1);
	if (vol->quant_type) {
		for (load = 0; load < 2; load++) {
			table = load ? vol->inter_matrix : vol->intra_matrix;
			if (!histb_vdec_mpeg4_bs_get(&bs, 1))
				continue;
			last = 0;
			for (i = 0; i < 64; i++) {
				v = histb_vdec_mpeg4_bs_get(&bs, 8);
				if (bs.err)
					return bs.err;
				if (!v)
					break;
				last = v;
				table[histb_mp4_zigzag[i]] = v;
			}
			for (; i < 64; i++)
				table[histb_mp4_zigzag[i]] = last;
		}
	}

	/* quarter_sample, only from verid 2 on */
	vol->quarter_sample = ver_id != 1 ? histb_vdec_mpeg4_bs_get(&bs, 1) : 0;

	/*
	 * complexity_estimation_disable: while the VOL keeps estimation
	 * signalling, every VOP carries a data-dependent amount of estimation
	 * data. The firmware layer refuses these streams and so does this
	 * driver.
	 */
	if (!histb_vdec_mpeg4_bs_get(&bs, 1))
		return -EINVAL;

	vol->resync_marker_disable = histb_vdec_mpeg4_bs_get(&bs, 1);

	/*
	 * data_partitioned (14496-2 6.2.1), with the rvlc flag it guards behind
	 * it: a partitioned slice is three independently decodable segments
	 * (header, intra, inter) with their own start codes, while this
	 * protocol gives each slice one window, one quantiser and one coding
	 * type in the descriptor, and the engine consumes the window as one
	 * continuous macroblock layer from its data bit. There is no field to
	 * say "this slice continues in that other segment", so the shapes
	 * cannot be expressed; the firmware syntax layer refuses them too (its
	 * VOL checks report the same flags unsupported). Accepting one would
	 * need the segment addresses and resynchronisation markers to become
	 * per-slice inputs.
	 */
	if (histb_vdec_mpeg4_bs_get(&bs, 1))
		return -EINVAL;

	if (ver_id != 1) {
		/*
		 * newpred_enable (14496-2 6.2.1): the VOL then carries
		 * newpred_times, and every macroblock may scale its motion
		 * vectors by it instead of using the VOP's fcode pair. The
		 * descriptor carries the picture's fcode_forward/backward only,
		 * so the per-macroblock prediction would be programmed with the
		 * wrong scale; the same argument refuses reduced_res_vop below.
		 * The firmware walk rejects both.
		 */
		if (histb_vdec_mpeg4_bs_get(&bs, 1))	/* newpred_enable */
			return -EINVAL;
		/*
		 * reduced_res_vop_enable (14496-2 6.2.1): VOPs smaller than the
		 * VOL grid are legal from here on, and the reference then has
		 * to be upsampled per VOP. This message states one grid (D4)
		 * and one size (D5) for the picture and gives the engine the
		 * reference addresses to read at that grid, so a smaller VOP
		 * cannot be described at all; refusing is the only answer that
		 * keeps the descriptors from naming rows the buffer does not
		 * have.
		 */
		if (histb_vdec_mpeg4_bs_get(&bs, 1))	/* reduced_res_vop */
			return -EINVAL;
	}

	/*
	 * scalability (14496-2 6.2.1): a layered stream carries the enhancement
	 * layers' own VOL/VOP syntax behind this flag, which this parser does
	 * not walk, and the base-layer picture is not decodable without the
	 * layer parameters the message has no words for. Refused with the rest
	 * of the scalability family.
	 */
	if (histb_vdec_mpeg4_bs_get(&bs, 1))		/* scalability */
		return -EINVAL;

	if (bs.err)
		return bs.err;

	vol->width = width;
	vol->height = height;
	vol->parsed = true;

	return 0;
}

/*
 * Parse one MPEG-4 VOP header (14496-2 6.2.5) at the 0x000001B6 start code and
 * return the first bit of the macroblock layer in @data_bit.
 */
static int histb_vdec_mpeg4_vop(const struct histb_vdec_mpeg4_vol *vol,
				const u8 *buf, unsigned int size,
				unsigned int pos,
				struct histb_vdec_mpeg4_pic *pic,
				unsigned int *data_bit)
{
	unsigned int limit = size * 8;
	unsigned int p = pos * 8 + 32;
	unsigned int type, run;

	/* vop_coding_type; N-VOPs carry S-VOPs, and sprite usage is refused by
	 * the VOL parser, so only I/P/B are acceptable.
	 */
	type = histb_vdec_mpeg4_bits(buf, p, 2);
	/*
	 * 14496-2 table 6-2 codes vop_coding_type, and 3 names the N-VOP: its
	 * sprite/GMC reference signalling is what vol_sprite_usage announces,
	 * and the VOL gate above refuses that configuration, so no stream this
	 * driver accepts can legitimately reach here with the value set.
	 * Reaching it means the parse is not at a VOP header, which is why the
	 * picture is refused rather than re-read at another position: the
	 * macroblock layer address every descriptor needs is derived from this
	 * header's length.
	 */
	if (type > HISTB_MPEG4_B_VOP)
		return -EINVAL;
	p += 2;

	/* modulo_time_base: a run of ones, a terminating zero, marker one */
	run = 0;
	while (run < 512 && p < limit && histb_vdec_mpeg4_bit(buf, p)) {
		p += 1;
		run++;
	}
	/*
	 * The run is open-ended in the syntax (one '1' per second), so 512 is
	 * this walk's bound rather than 14496-2's: a longer run cannot be a
	 * picture timing field, and letting the loop run on would scan most of
	 * the OUTPUT plane looking for a zero bit. The packet header's extended
	 * timing fields stop at the same count, so both walks refuse the same
	 * shape.
	 */
	if (run >= 512)
		return -EINVAL;
	/*
	 * The rest of the fixed header - terminating zero, marker,
	 * vop_time_increment, marker, vop_coded - has to sit inside the
	 * submitted span, because @p is published as the first bit of the
	 * macroblock layer and the descriptor window is measured from it. A VOP
	 * header cut by the buffer end is refused here rather than read past
	 * the mapping.
	 */
	if (p + 2 + vol->time_incr_bits + 2 > limit)
		return -EINVAL;
	p += 1 + 1;		/* terminating zero + marker */
	pic->time_modulo = run;
	pic->time_increment = histb_vdec_mpeg4_bits(buf, p, vol->time_incr_bits);
	p += vol->time_incr_bits;

	/* marker before vop_coded, then vop_coded */
	/*
	 * The marker here is checked, unlike the VOL's markers: encoders write
	 * '1' in this position (the VOP writer's one-bit put) and a 0 means
	 * this walk is not at the start of a VOP header. Every field after it -
	 * and therefore the macroblock layer address - is positioned by this
	 * bit, so a mismatch ends the picture; guessing it is the +112/-class
	 * failure of the MPEG-2 path, one wrong bit moving the whole macroblock
	 * layer.
	 */
	if (!histb_vdec_mpeg4_bit(buf, p))
		return -EINVAL;
	p += 1;
	/*
	 * vop_coded = 0 is the skipped picture of 14496-2 6.2.5: it carries no
	 * macroblock layer at all, the display is supposed to repeat the
	 * previous anchor, and a stateless job that names no slice data cannot
	 * be programmed (the engine would be handed whatever follows in the
	 * buffer and CFG0 would still announce mb_total macroblocks). The
	 * client's answer for a skipped picture is to submit nothing.
	 */
	if (!histb_vdec_mpeg4_bit(buf, p))	/* a skipped VOP has no data */
		return -EINVAL;
	p += 1;

	pic->coding_type = type;

	/* vop_rounding_type is carried for P-VOPs only */
	pic->rounding = 0;
	if (type == HISTB_MPEG4_P_VOP) {
		/*
		 * From here to the end of the header every read is guarded the
		 * same way: the field has to lie inside the submitted span, or
		 * the position published as the macroblock layer's first bit
		 * would be outside the buffer the descriptors address. These
		 * guards read like boilerplate, but each one stands in front of
		 * a histb_vdec_mpeg4_bits() call that does no bounds checking
		 * of its own, so dropping any of them turns a truncated VOP
		 * header into a read beyond the plane mapping rather than a
		 * refusal.
		 */
		if (p >= limit)
			return -EINVAL;
		pic->rounding = histb_vdec_mpeg4_bit(buf, p);
		p += 1;
	}

	/* No complexity-estimation data is carried (VOL gate). */

	/*
	 * intra_dc_vlc_threshold (14496-2 6.2.5) is the next read and the
	 * descriptor's bits 9:7 come straight from it, so a header the
	 * submitted span cuts inside this field cannot be walked: the value
	 * would be taken from bytes the buffer does not contain and the
	 * macroblock layer start published below would land outside the
	 * picture.
	 */
	if (p + 3 > limit)
		return -EINVAL;
	pic->intra_dc_vlc_thr = histb_vdec_mpeg4_bits(buf, p, 3);
	p += 3;

	/* top_field_first / alternate_vertical_scan are only carried by an
	 * interlaced VOL, which is refused, so they stay 0.
	 */

	/*
	 * Same guard for vop_quant, the 5-bit field of 14496-2 6.2.5 that
	 * picture message D1 and every descriptor carry: the five bits have to
	 * be inside the span before the read, because the uncapped
	 * histb_vdec_mpeg4_bits() below would otherwise walk past the plane
	 * mapping and the picture would be programmed from the bytes that
	 * happen to follow it.
	 */
	if (p + 5 > limit)
		return -EINVAL;
	pic->quant = histb_vdec_mpeg4_bits(buf, p, 5);
	/*
	 * vop_quant is the 5-bit field of 14496-2 6.2.5 and 0 is not a usable
	 * scale: it is the divisor of the dequantiser on the way in and the
	 * value the picture message carries in D1 bits 30:26, so a zero picture
	 * would be programmed with a step the engine cannot apply. The
	 * firmware's own walk shows the alternative it chose - rewriting a zero
	 * vop_quant to 1 and carrying on - which decodes against a scale the
	 * stream never declared, so this driver refuses instead of
	 * substituting. The upper bound only restates the width of the field.
	 */
	if (!pic->quant || pic->quant > 31)
		return -EINVAL;
	p += 5;

	pic->fcode_fwd = 1;
	pic->fcode_bwd = 1;
	if (type != HISTB_MPEG4_I_VOP) {
		/*
		 * The bound for the fcode read: an inter picture whose 3 bits
		 * are cut by the span end has no motion-vector range and, for
		 * MPEG-4 packets, no start-code prefix length either (both come
		 * from this field), so the picture cannot be windowed; refusing
		 * it keeps the read inside the mapping.
		 */
		if (p + 3 > limit)
			return -EINVAL;
		pic->fcode_fwd = histb_vdec_mpeg4_bits(buf, p, 3);
		/*
		 * 14496-2 6.2.5 gives fcode_forward the range 1..7 and 0 is not
		 * a code any consumer could use: the descriptor carries it
		 * verbatim as the vector range, and
		 * histb_vdec_mpeg4_packet_prefix() turns it into the length of
		 * this picture's video packet start code, so a zero here means
		 * the walk is not at a VOP header at all.
		 */
		if (!pic->fcode_fwd)
			return -EINVAL;
		p += 3;
	}
	if (type == HISTB_MPEG4_B_VOP) {
		/*
		 * The same bound as the forward side of 14496-2 6.2.5: an I-VOP
		 * carries no fcode at all, so this read only happens for a
		 * B-VOP and its three bits have to be inside the span like
		 * every other field whose value reaches the descriptors.
		 */
		if (p + 3 > limit)
			return -EINVAL;
		pic->fcode_bwd = histb_vdec_mpeg4_bits(buf, p, 3);
		/*
		 * fcode_backward of 14496-2 6.2.5, same 1..7 range and the same
		 * two consumers as the forward side: the descriptor's
		 * vector-range field and, through
		 * histb_vdec_mpeg4_packet_prefix(), the prefix length of this
		 * picture's video packet start code.
		 */
		if (!pic->fcode_bwd)
			return -EINVAL;
		p += 3;
	}

	/*
	 * The position published as the macroblock layer's first bit has to be
	 * inside the submitted span: every descriptor window is measured from it,
	 * and a start past the end would have the engine read the bytes after this
	 * buffer's mapping. Nothing in the walk above leaves @p able to pass
	 * @limit, so this is the invariant stated once more at the exit rather
	 * than a stream shape; a picture that really trips it is one whose header
	 * fields were misread, which is the class of defect that costs the whole
	 * picture rather than one macroblock.
	 */
	if (p > limit)
		return -EINVAL;

	pic->width = vol->width;
	pic->height = vol->height;
	pic->quarter_sample = vol->quarter_sample;
	pic->resync_marker_disable = vol->resync_marker_disable;
	pic->time_incr_bits = vol->time_incr_bits;
	pic->quant_type = vol->quant_type;
	*data_bit = p;

	return 0;
}

/* -------------------------------------------------- H.263 header parsing */

/*
 * H.263 source_format to picture size, the table the vendor's software decoder
 * carries: the two extended-format entries it leaves out are the ones whose
 * size is signalled in the header instead.
 */
static const u16 histb_h263_format[6][2] = {
	{ 0, 0 }, { 128, 96 }, { 176, 144 },
	{ 352, 288 }, { 704, 576 }, { 1408, 1152 },
};

/*
 * Locate and walk an H.263 picture header: the 22-bit picture start code 0x20
 * at a byte boundary, the 8-bit temporal reference, then the PTYPE. Only the
 * H.263v1 PTYPE is accepted - source_format 6 and 7 are the H.263+ extended
 * PTYPE, whose custom size, BILME and rounding-control fields have no
 * descriptor input, and 0 is reserved. The fields this returns are everything
 * the messages need from this syntax: the grid from source_format, and the type
 * and quantiser, which for a picture with no Annex-E group headers have no
 * other carrier.
 */
static int histb_vdec_h263_pic(struct histb_vdec_mpeg4_pic *pic, const u8 *buf,
			       unsigned int size, unsigned int *data_bit)
{
	struct histb_vdec_mpeg4_bs bs;
	unsigned int i, format;

	for (i = 0; i + 4 <= size; i++) {
		/* 16 zero bits, the '1' of the start code, and the two low bits
		 * of that byte belong to the temporal reference
		 */
		if (!buf[i] && !buf[i + 1] && (buf[i + 2] & 0xfc) == 0x80)
			break;
	}
	/*
	 * No byte-aligned picture start code in the span (the 22-bit pattern of
	 * ITU-T H.263 6.2.5 with its Annex G short header: sixteen zero bits,
	 * the '1', then the 8-bit temporal reference). This buffer then carries
	 * no H.263 picture header, and there is no VOL to fall back on for the
	 * grid the way MPEG-4 has - source_format here is the only carrier of
	 * the picture size, the type and the quantiser - so the job ends
	 * instead of being programmed from the grid a previous picture cached.
	 */
	if (i + 4 > size)
		return -EINVAL;

	bs.buf = buf;
	bs.limit = size * 8;
	bs.p = i * 8 + 30;	/* start code (22) + temporal reference (8) */
	bs.err = 0;

	/* marker, H.263 identification and the split-screen, camera and freeze
	 * signalling: the vendor firmware warns on the first two and skips the
	 * rest, and none of them reaches a descriptor, so they are consumed
	 * here without gating on them.
	 */
	histb_vdec_mpeg4_bs_skip(&bs, 5);

	format = histb_vdec_mpeg4_bs_get(&bs, 3);
	/*
	 * source_format (ITU-T H.263 Annex C table, which the sizes in
	 * histb_h263_format[] transcribe) is how this header states the picture
	 * geometry: 0 is reserved and 6/7 are the H.263+ extended PTYPE, which
	 * moves the size into its own custom-picture-format fields along with
	 * the BILME and rounding-control signalling - none of which has a
	 * descriptor input, and the firmware's short-header walk only has
	 * group-geometry entries through 4CIF before it rejects the larger
	 * formats. So the grid cannot be named for 0, 6 or 7 and the picture is
	 * refused; accepting one needs the custom size path plus the Annex
	 * I/D/J fields. 16CIF (5) is read here and refused one gate later,
	 * where picture message D2 cannot state its group geometry.
	 */
	if (format < 1 || format > 5)
		return -EINVAL;

	/* intra/inter, then unrestricted vector range and slice-based coding,
	 * both of which change what the macroblock layer means and neither of
	 * which the descriptor set can state, so they are refused. The OBMC bit
	 * between them and the PB-frame flag has the same no-input status but a
	 * different consequence: it only changes prediction, never the syntax
	 * the engine walks, and this engine generation has no field for it at
	 * all - the obmc_disable flag the vendor firmware keeps in one
	 * parameter structure has no consumer anywhere in its message program,
	 * and the engine starts reading at the descriptor's data bit, so it
	 * never sees this header. Decoding without the overlapped smoothing is
	 * what the vendor's own product delivers, so the bit is consumed and
	 * ignored rather than gating the picture.
	 */
	pic->coding_type = histb_vdec_mpeg4_bs_get(&bs, 1);
	/*
	 * The two bits are unrestricted_motion_vector and
	 * slice_based_coding_flag (ITU-T H.263 Annex G picture header). URR
	 * widens every motion-vector range in the picture and SBC changes what
	 * the macroblock layer means (slice groups signalled as groups rather
	 * than as whole rows), and the descriptor set can state neither: a
	 * short-header picture carries no fcode for the wider vector range to
	 * be scaled by, and there is no group-geometry word. The firmware's own
	 * walk skips these four bits as one group immediately before the
	 * quantiser and acts on none of them, so a product built on it decodes
	 * such streams with the plain interpretation; this driver refuses them
	 * because the delivered pixels then do not match the standard.
	 * Accepting them needs engine inputs for the vector range and for
	 * slice-group geometry. The OBMC bit between these and the PB flag is
	 * consumed and ignored instead - the paragraph above is the measured
	 * reason for that difference.
	 */
	if (histb_vdec_mpeg4_bs_get(&bs, 1) || histb_vdec_mpeg4_bs_get(&bs, 1))
		return -EINVAL;
	histb_vdec_mpeg4_bs_skip(&bs, 1);		/* OBMC */
	/*
	 * pb_frame_flag: an H.263+ PB picture is two coded frames behind one
	 * header. One OUTPUT submission is one picture written to one CAPTURE
	 * buffer, and neither the picture message nor the descriptors name a
	 * second target, so the shape has no representation here; the firmware
	 * skips this bit with the three above and would be handed one job for
	 * what the syntax says are two frames.
	 */
	if (histb_vdec_mpeg4_bs_get(&bs, 1))		/* PB frame */
		return -EINVAL;

	pic->quant = histb_vdec_mpeg4_bs_get(&bs, 5);
	histb_vdec_mpeg4_bs_skip(&bs, 1);		/* continuous presence */
	/*
	 * bs.err means the PTYPE ran out of the submitted span, so nothing read
	 * above this - the grid among it - was complete. QSCALE 0 is not a
	 * quantiser: for a picture with no Annex-E group headers this short
	 * header is the only carrier of the value, and it goes to picture
	 * message D1 bits 30:26, where a zero step has nothing for the
	 * dequantiser to divide by. The firmware's own walk shows the
	 * alternative it chose - rewriting a zero vop_quant to 1 and carrying
	 * on, at the store that follows this field - which decodes against a
	 * scale the stream never declared, so this driver refuses instead of
	 * substituting.
	 */
	if (bs.err || !pic->quant)
		return -EINVAL;

	/* The header's "1 stop bit, 8 data bits" trailer, which the vendor
	 * firmware loops over after the continuous-presence flag and bounds at
	 * sixteen data words. The macroblock layer starts after the terminating
	 * zero: a picture whose groups are addressed by number has no start
	 * code before the first one for the engine to resync on, so leaving the
	 * trailer unconsumed costs exactly the leading group - the board showed
	 * it as one wrong macroblock row of an Annex-E picture.
	 */
	for (i = 0; i < 16; i++) {
		if (histb_vdec_mpeg4_bs_get(&bs, 1))
			histb_vdec_mpeg4_bs_skip(&bs, 8);
		else
			break;
	}

	pic->width = histb_h263_format[format][0];
	pic->height = histb_h263_format[format][1];
	pic->short_header = true;
	*data_bit = bs.p;

	return 0;
}

/* ------------------------------------------------------- slice discovery */

/*
 * Length of the zero run in front of an MPEG-4 video packet start code (14496-2
 * 6.2.5.1.4; the firmware syntax layer and the vendor software decoder use the
 * same expression): 16 for an I-VOP, fcode_forward + 15 for P,
 * max(fcode_forward, fcode_backward, 2) + 15 for B.
 */
static unsigned int
histb_vdec_mpeg4_packet_prefix(const struct histb_vdec_mpeg4_pic *pic)
{
	if (pic->coding_type == HISTB_MPEG4_I_VOP)
		return 16;
	if (pic->coding_type == HISTB_MPEG4_P_VOP)
		return pic->fcode_fwd + 15;
	return max(max(pic->fcode_fwd, pic->fcode_bwd), 2U) + 15;
}

/*
 * Read one MPEG-4 video packet header whose zero run starts at bit @pos.
 * mbk_number is the first macroblock of the packet; its width is the bit length
 * of the macroblock total, which is what both the firmware's packet walk and
 * the slice descriptor carry.
 */
static int histb_vdec_mpeg4_packet(const u8 *buf, unsigned int size,
				   unsigned int pos, unsigned int prefix,
				   const struct histb_vdec_mpeg4_pic *pic,
				   struct histb_vdec_mpeg4_slice *sl)
{
	unsigned int limit = size * 8;
	unsigned int p = pos, run, bits;

	/* the zero run, its terminating '1', the macroblock number, the
	 * quantiser, the extension flag and, for an extended header, at least a
	 * two-bit type and a three-bit threshold: everything read before the
	 * next bounds check fits in this.
	 *
	 * -ENOENT here is the answer "this byte is not a header", not an error
	 * under 14496-2 6.2.5.1.4: histb_vdec_mpeg4_slice_headers() steps to
	 * the next byte-aligned position and keeps looking, and when the scan
	 * runs out of span it returns the chain found so far. That is the
	 * intended shape for an MPEG-4 packet picture, whose last window is
	 * declared to the end of the span precisely so that a stream with a
	 * missing tail still terminates; what must not happen is a header read
	 * past the mapping, and that is what this bound exists for: the 14496-2
	 * 6.2.5.1.4 packet header is a fixed-length prefix, so a span that
	 * cannot hold it holds no header.
	 */
	if (p + prefix + 1 + pic->mb_bits + 5 + 1 + 2 + 1 + 3 > limit)
		return -ENOENT;
	/*
	 * The run has to be exactly @prefix bits. 14496-2 6.2.5.1.4 gives this
	 * picture's video packet code that many leading zeros - 16 for an
	 * I-VOP, fcode_forward + 15 for a P-VOP, max(fcode_forward,
	 * fcode_backward, 2) + 15 for a B-VOP, as
	 * histb_vdec_mpeg4_packet_prefix() computes - and the two other shapes
	 * are both things this function must not read as a header: a shorter
	 * run is an ordinary macroblock codeword, and a longer one is a 32-bit
	 * start code (a byte-aligned 00 00 01 is a run of 23 zeros, which is
	 * why the picture's own data can never be mistaken for one of these
	 * codes either). The length is matched exactly rather than as a minimum
	 * because a false hit inside macroblock data has to be excluded by what
	 * it looks like, which is the rule the caller states.
	 */
	if (histb_vdec_mpeg4_zero_run(buf, p, prefix + 1) != prefix)
		return -ENOENT;
	/*
	 * The '1' that closes the video packet code and separates it from
	 * mbk_number (14496-2 6.2.5.1.4). The exact-length run above already
	 * implies it, since a run that stops at @prefix bits has a set bit at
	 * its end; it is retested so that the scan cannot be handed a code
	 * position whose fields start one bit early if the length test ever
	 * changes - an off-by-one here is the same class of defect that costs
	 * the whole macroblock layer its alignment.
	 */
	if (!histb_vdec_mpeg4_bit(buf, p + prefix))
		return -ENOENT;
	p += prefix + 1;

	bits = histb_vdec_mpeg4_bits(buf, p, pic->mb_bits);
	/*
	 * mbk_number names the packet's first macroblock, so a value at or
	 * above the grid total is not a header of this picture: either the
	 * field was read at the wrong position (the parse width comes from this
	 * VOL, so a picture whose grid disagrees with it lands here) or it is a
	 * false hit in macroblock data. Neither can be windowed: the value is
	 * published as the descriptor's start_mbn, and the previous packet's
	 * end_mbn is derived from it as start_mbn - 1, so an out-of-range
	 * number would address macroblocks the picture has no rows for. The
	 * vendor's video packet walk compares it with the same macroblock total
	 * and reports the mismatch instead of building a descriptor.
	 */
	if (bits >= pic->mb_total)
		return -ENOENT;
	p += pic->mb_bits;

	run = histb_vdec_mpeg4_bits(buf, p, 5);
	/*
	 * The packet quantiser is that 5-bit field and 0 is not a scale: the
	 * value is carried to the engine verbatim in descriptor word 4, so
	 * there is no arithmetic here that could give a zero a meaning. The
	 * firmware's own picture-level walk shows what the alternative is: it
	 * rewrites a zero vop_quant to 1 where it stores the picture quantiser
	 * and carries on, decoding against a scale the stream never declared,
	 * while the same routine stores this packet field without any range
	 * check at all. Refusing is the honest answer, and the scored corpus
	 * pays nothing for it: its refusals all belong to the other gates. The
	 * upper bound only restates the field's width.
	 */
	if (!run || run > 31)
		return -ENOENT;
	p += 5;

	sl->start_mbn = bits;
	sl->quant = run;
	sl->coding_type = pic->coding_type;
	sl->intra_dc_vlc_thr = pic->intra_dc_vlc_thr;

	/* header_extension: the packet carries its own timing and, for a
	 * non-intra packet, its own intra_dc_vlc_threshold. The fcode fields
	 * are consumed for position only - the vendor's video-packet slice walk
	 * skips them and the descriptor keeps the picture-level fcode.
	 */
	if (histb_vdec_mpeg4_bit(buf, p)) {
		p += 1;
		run = 0;
		while (run < 512 && p < limit && histb_vdec_mpeg4_bit(buf, p)) {
			p += 1;
			run++;
		}
		/*
		 * Once the extension flag reads 1 this is an extended video
		 * packet header (14496-2 6.2.5.1.4), so the answer flips from
		 * -ENOENT to -EINVAL: the header has been recognised, and its
		 * position cannot be recovered without walking the macroblock
		 * layer to find where the timing fields end. Returning -ENOENT
		 * instead would make the caller look past a real header, window
		 * the previous packet over this one's data, and deliver a
		 * picture whose every later codeword is misaligned. The 512
		 * bound is a guard against an open-ended field, not a syntax
		 * limit - modulo_time_base is one bit per second - and the
		 * picture-level VOP walk stops at the same count, so a stream
		 * that really carries 512 seconds of run is refused identically
		 * in both places.
		 */
		if (run >= 512 ||
		    p + 2 + pic->time_incr_bits + 9 > limit)
			return -EINVAL;
		p += 2;		/* terminating zero + marker */
		p += pic->time_incr_bits;
		/* marker, ignored as the vendor firmware ignores it */
		p += 1;
		bits = histb_vdec_mpeg4_bits(buf, p, 2);
		/*
		 * The packet's own vop_coding_type. 3 is the one value outside
		 * the I/P/B triple this engine is programmed for - it names the
		 * N-VOP, whose shape the VOL gate rejects on the way in
		 * (vol_sprite_usage), so a packet claiming it here cannot be
		 * described in descriptor word 4's two coding-type bits either.
		 * As with the timing fields, the header is already recognised,
		 * so this is the picture that is refused and not the byte
		 * position; the picture-level walk applies the identical bound
		 * to the same field.
		 */
		if (bits > HISTB_MPEG4_B_VOP)
			return -EINVAL;
		p += 2;
		sl->coding_type = bits;
		sl->intra_dc_vlc_thr = histb_vdec_mpeg4_bits(buf, p, 3);
		p += 3;
		if (sl->coding_type != HISTB_MPEG4_I_VOP)
			p += 3;		/* fcode_for */
		if (sl->coding_type == HISTB_MPEG4_B_VOP)
			p += 3;		/* fcode_back */
	} else {
		p += 1;
	}

	/*
	 * The last invariant before this position is published: sl->data_bit
	 * becomes the descriptor's window start, and the window length is
	 * computed from it as end_bit - data_bit
	 * (histb_vdec_mpeg4_pack_slice()), so a start beyond the submitted span
	 * would declare a nonsense length over bytes this buffer does not
	 * describe. Every read above is bounded, so this arm answers a position
	 * that drifted rather than a stream shape; it is -EINVAL and not
	 * -ENOENT because the header itself was recognised, and a picture with
	 * an unreadable packet position has no window left to declare.
	 */
	if (p > limit)
		return -EINVAL;
	sl->data_bit = p;

	return 0;
}

/*
 * Read one H.263 Annex-E group-of-block header at bit @pos: sixteen zero bits,
 * a '1', GN = u(5), GFID = u(2), GQUANT = u(5). The encoder writes exactly
 * that, and the vendor firmware reads exactly that: its short-header slice walk
 * skips 17 bits from the start-code position, then 5 + 2 + 5.
 *
 * GN counts groups from zero here, the way both vendor paths turn it into a
 * macroblock number - the slice descriptor's start macroblock is the group
 * number times the group height, and the vendor's software decoder scales its
 * row index the same way - so the standard's 1-based group number would push
 * the last group past the grid, which is the check right below.
 *
 * GFID is not used: the group's macroblock layer carries the intra flag per
 * macroblock, so the firmware skips those two bits and gives every group the
 * picture's coding type.
 */
static int histb_vdec_h263_gob(const u8 *buf, unsigned int size,
			       unsigned int pos,
			       const struct histb_vdec_mpeg4_pic *pic,
			       struct histb_vdec_mpeg4_slice *sl)
{
	unsigned int limit = size * 8;
	unsigned int p = pos, gbid, gfid, quant;

	/*
	 * Same shape rule as the MPEG-4 packet scan: the group start code of
	 * ITU-T H.263 Annex I is sixteen zero bits closed by a '1', so a
	 * shorter or longer run is macroblock data or a real start code and not
	 * a header here. -ENOENT keeps the caller's byte scan moving rather
	 * than refusing the picture, because a group chain is only required to
	 * be found where the groups actually are; the coverage rule at the end
	 * of histb_vdec_mpeg4_slice_headers() is what turns a chain that stops
	 * short of the grid into a refusal.
	 */
	if (histb_vdec_mpeg4_zero_run(buf, p, 17) != 16)
		return -ENOENT;
	/* The closing '1' of the group start code; implied by the run test
	 * above and kept so a future change there cannot hand this function a
	 * group whose GN field starts one bit early.
	 */
	if (!histb_vdec_mpeg4_bit(buf, p + 16))
		return -ENOENT;
	p += 17;
	/*
	 * GN(5) + GFID(2) + GQUANT(5), the twelve bits of ITU-T H.263 Annex I's
	 * group header, have to be inside the span: all three are published,
	 * the group number as the descriptor's macroblock range and GQUANT as
	 * its quantiser, so a header the buffer only holds half of cannot be
	 * turned into a window. -EINVAL rather than the -ENOENT above, because
	 * at this point the group start code *was* recognised.
	 */
	if (p + 12 > limit)
		return -EINVAL;

	gbid = histb_vdec_mpeg4_bits(buf, p, 5);
	gfid = histb_vdec_mpeg4_bits(buf, p + 5, 2);
	quant = histb_vdec_mpeg4_bits(buf, p + 7, 5);
	p += 12;

	/*
	 * Three reasons to answer "not a group header" (-ENOENT) and let the
	 * caller's scan keep moving: GFID 2 and 3 are the reserved pair of
	 * ITU-T H.263 Annex I that selects a group allocation table rather than
	 * saying intra or inter, GQUANT 0 is not a quantiser step (the
	 * firmware's walk substitutes 1 and carries on where it stores the
	 * picture quantiser, which decodes at a scale the stream never
	 * declared), and a value above the 5-bit field cannot come from this
	 * syntax at all. The picture is not lost by this answer: the chain
	 * coverage rule in histb_vdec_mpeg4_slice_headers() refuses the group
	 * stream that stops short of the grid, which is what a mid-chain
	 * unsupported header produces. Measured on the board with the eighth
	 * group's GFID set to an allocation-table value: with only this arm
	 * answering, the picture was accepted and every macroblock row from 8
	 * down came out wrong - 84480 bytes of a CIF frame, max 250, rows 0..7
	 * byte-exact.
	 */
	if (gfid > 1 || !quant || quant > 31)
		return -ENOENT;

	sl->start_mbn = gbid * pic->mb_per_gob;
	/*
	 * A group number whose product leaves the grid cannot be windowed: the
	 * value goes into this descriptor as start_mbn and into the previous
	 * group's record as end_mbn - 1, so the chain would address macroblocks
	 * the capture buffer has no rows for and the engine would decode past
	 * the picture. This is a refusal rather than a scan answer because the
	 * group start code *was* recognised - only its number is impossible.
	 * The vendor's own walk bounds it the same way: both its short-header
	 * slice routine and the software decoder compare the derived macroblock
	 * against the grid total, with the group number counted from zero as
	 * here.
	 */
	if (sl->start_mbn > pic->mb_total - 1)
		return -EINVAL;
	sl->quant = quant;
	sl->coding_type = pic->coding_type;
	sl->intra_dc_vlc_thr = 0;
	sl->data_bit = p;

	return 0;
}

/*
 * Collect the slice headers of one picture into @sls and return their count.
 *
 * A picture without resync markers is one slice spanning the whole grid, the
 * shape the vendor's descriptors take when the stream carries no packet header.
 * Otherwise headers are found by their bit pattern; a false hit inside
 * macroblock data would have to look like a valid header, so the exact
 * zero-prefix length for this picture, an in-range macroblock number, a
 * non-zero quantiser and a strictly increasing macroblock number are all
 * required. Encoders byte-align before every slice header (the MPEG-4 packet
 * and H.263 GOB writers both assert it), so only byte positions are tried - the
 * same scan the firmware's resync search makes.
 *
 * A picture whose first header does not open the grid gets a leading slice
 * covering macroblock 0 up to that header, built here rather than by the
 * vendor's synthesis branch (see below); the returned chain always starts at
 * macroblock 0 and is contiguous.
 */
static int histb_vdec_mpeg4_slice_headers(const u8 *buf, unsigned int size,
					  const struct histb_vdec_mpeg4_pic *pic,
					  unsigned int data_bit,
					  struct histb_vdec_mpeg4_slice *sls,
					  unsigned int max)
{
	struct histb_vdec_mpeg4_slice sl;
	unsigned int n = 0, pos = data_bit, prefix = 0, last = 0;
	unsigned int step;
	int ret;

	if (!pic->short_header && pic->resync_marker_disable) {
		sls[0].data_bit = data_bit;
		sls[0].start_mbn = 0;
		sls[0].quant = pic->quant;
		sls[0].coding_type = pic->coding_type;
		sls[0].intra_dc_vlc_thr = pic->intra_dc_vlc_thr;
		return 1;
	}

	/* A GOB chain has a fixed stride and ends with the last macroblock row;
	 * MPEG-4 packets state their own start, so the only stop condition
	 * there is that no further header is found.
	 */
	step = pic->short_header ? pic->mb_per_gob : 0;
	if (!pic->short_header)
		prefix = histb_vdec_mpeg4_packet_prefix(pic);

	while (n < max) {
		pos = n ? sls[n - 1].data_bit : data_bit;

		for (;;) {
			if (pos + prefix + 17 >= size * 8) {
				/* A short-header picture whose stream carries
				 * no group header at all cannot be given to
				 * this engine: the firmware's short-header walk
				 * reads a group header before every group,
				 * skipping the 17-bit start code then reading
				 * GN, GFID and GQUANT, so Annex E is not a
				 * layout the driver may fake. The board showed
				 * the consequence of trying - a header-less
				 * QCIF picture decoded as a flat black frame
				 * with either group count stated in D1, one
				 * group for the whole grid or one group per
				 * macroblock row. Accepting one would need the
				 * engine to walk its macroblock layer without
				 * consuming a group header per group, which
				 * short-header mode does not do.
				 */
				if (!n)
					return -EINVAL;
				/* The coverage rule below applies here too: a
				 * chain that runs out of stream before the grid
				 * ends is a picture whose later headers are not
				 * readable here, and an unsupported GFID is the
				 * case measured on the board.
				 */
				if (pic->short_header &&
				    last + step != pic->mb_total)
					return -EINVAL;
				return n;
			}

			if (pic->short_header)
				ret = histb_vdec_h263_gob(buf, size, pos,
							  pic, &sl);
			else
				ret = histb_vdec_mpeg4_packet(buf, size, pos,
							      prefix, pic,
							      &sl);
			if (ret != -ENOENT)
				break;
			pos = (pos + 8) & ~7U;
		}
		if (ret)
			return ret;
		/*
		 * The picture's first macroblock need not open with a header:
		 * the data before the first packet header is a packet of its
		 * own starting at grid position 0 (the corpus's Lavc-encoded
		 * I-VOP has its first header at macroblock 88). The vendor
		 * firmware's MPEG-4 slice packer synthesises a leading slot for
		 * that case, when the first descriptor's start macroblock is
		 * not 0, but gives it the *next* packet's address with a zero
		 * length and then indexes the real slots 128 bytes per skipped
		 * macroblock instead of 32, which breaks the chain it just
		 * wrote; the leading slot is therefore built here, from this
		 * picture's own data start, and the chain stays contiguous.
		 */
		if (!n && sl.start_mbn) {
			sls[0].data_bit = data_bit;
			sls[0].start_mbn = 0;
			sls[0].quant = pic->quant;
			sls[0].coding_type = pic->coding_type;
			sls[0].intra_dc_vlc_thr = pic->intra_dc_vlc_thr;
			n = 1;
		} else if (n && sl.start_mbn <= last) {
			return -EINVAL;
		}

		sls[n++] = sl;
		last = sl.start_mbn;
		if (step && last + step >= pic->mb_total)
			break;
	}

	/*
	 * A group chain has to reach the end of the grid. Groups are addressed
	 * by number with a fixed stride, so a chain that stops early can only
	 * mean the headers after that point are unreadable here, and the
	 * picture would be programmed with one window running to the end of the
	 * buffer over data that is not macroblock layer. The board case is
	 * Annex F: with the eighth group's GFID set to an allocation table the
	 * driver does not carry, the picture was accepted and decoded wrong
	 * from macroblock row 8 down - 84480 bytes of a CIF frame, max 250,
	 * rows 0..7 byte exact.
	 */
	if (pic->short_header && last + step != pic->mb_total)
		return -EINVAL;

	return n;
}

/* ------------------------------------------------------- message packing */

/*
 * Vendor matrix word layout: word 28 + 2 * column + row parity holds the
 * entries of that column whose row is congruent to the parity modulo two, least
 * significant byte first (this is how the vendor firmware's MPEG-4
 * picture-message packer fills bytes 112-175 and 176-239). The same
 * interleaving the MPEG-2 layer uses, twelve words later in the slot.
 */
static void histb_vdec_mpeg4_pack_qmatrix(u32 *w, const u8 *intra,
					  const u8 *nonintra)
{
	unsigned int c, r, i;

	for (i = 0; i < 16; i++) {
		w[28 + i] = 0;
		w[44 + i] = 0;
	}

	for (c = 0; c < 8; c++) {
		for (r = 0; r < 4; r++) {
			w[28 + 2 * c] |= (u32)intra[2 * r * 8 + c] << (8 * r);
			w[29 + 2 * c] |= (u32)intra[(2 * r + 1) * 8 + c] << (8 * r);
			w[44 + 2 * c] |= (u32)nonintra[2 * r * 8 + c] << (8 * r);
			w[45 + 2 * c] |= (u32)nonintra[(2 * r + 1) * 8 + c] << (8 * r);
		}
	}
}

static void *histb_vdec_mpeg4_slc_slot(const struct histb_vdec_msgpool *p,
				       unsigned int i)
{
	return p->va + p->slice_off + i * HISTB_MPEG4_SLC_BYTES;
}

static dma_addr_t
histb_vdec_mpeg4_slc_slot_dma(const struct histb_vdec_msgpool *p,
			      unsigned int i)
{
	return p->dma + p->slice_off + i * HISTB_MPEG4_SLC_BYTES;
}

/*
 * Write one slice descriptor. The segment is measured from the programmed
 * stream base: D1 carries the 16-byte granule holding the first data bit and
 * D0's bit offset counts the bits inside that granule - the vendor firmware's
 * MPEG-4 slice packer forms it as the bit offset inside the packet plus the low
 * nibble of the byte address times eight - the same arithmetic the MPEG-2 layer
 * uses. The descriptor starts at the first bit of the macroblock layer, after
 * the slice header, because the engine reads macroblock data and nothing else.
 *
 * The length runs to the end of the submitted span, as the vendor firmware's
 * remaining-bits count does: the engine stops a slice at its own end
 * macroblock, so a window that also covers later slices costs nothing and it is
 * what lets a picture with a missing tail still terminate. MPEG-4 slice data
 * carries no stop bit (as in MPEG-2), so nothing is trimmed off the end here.
 */
static void histb_vdec_mpeg4_pack_slice(u32 *w, unsigned int end_bit,
					const struct histb_vdec_mpeg4_slice *sl,
					const struct histb_vdec_mpeg4_pic *pic,
				       unsigned int start_mbn,
				       unsigned int end_mbn,
				       dma_addr_t next_dma)
{
	w[0] = FIELD_PREP(GENMASK(23, 0), end_bit - sl->data_bit) |
	       FIELD_PREP(GENMASK(30, 24), sl->data_bit & 127U);
	w[1] = FIELD_PREP(GENMASK(23, 0), (sl->data_bit / 8) & ~15U);
	w[2] = 0;
	w[3] = 0;
	w[4] = FIELD_PREP(GENMASK(4, 0), sl->quant) |
	       FIELD_PREP(GENMASK(6, 5), sl->coding_type) |
	       FIELD_PREP(GENMASK(9, 7), sl->intra_dc_vlc_thr) |
	       FIELD_PREP(GENMASK(12, 10), pic->fcode_fwd) |
	       FIELD_PREP(GENMASK(15, 13), pic->fcode_bwd);
	w[5] = FIELD_PREP(GENMASK(19, 0), start_mbn);
	w[6] = FIELD_PREP(GENMASK(19, 0), end_mbn);
	w[7] = next_dma & ~15U;
}

/* ---------------------------------------------------- picture parameters */

/*
 * The H.263 GOB grid: the vendor's syntax layer takes macroblocks-per-GOB and
 * GOB count from the picture header's source_format through a table with one
 * GOB per macroblock row up to CIF and two rows per GOB at 4CIF. That is the
 * same split as the height rule its software decoder applies (one row up to 400
 * lines, two up to 800, four above), so the rule is used here - it also covers
 * 16CIF, which the vendor table has no entry for.
 */
static void histb_vdec_mpeg4_grid(struct histb_vdec_mpeg4_pic *pic)
{
	unsigned int rows;

	pic->mb_width = DIV_ROUND_UP(pic->width, 16);
	pic->mb_height = DIV_ROUND_UP(pic->height, 16);
	pic->mb_total = pic->mb_width * pic->mb_height;
	/* Width of a video packet's macroblock number: the bits of (total - 1),
	 * not of the total. ffmpeg's packet writer counts that way, and so does
	 * the vendor firmware's MPEG-4 parser over its own macroblock width and
	 * height.
	 */
	pic->mb_bits = ilog2(pic->mb_total - 1) + 1;
	/* Picture message D2 bits [25:22] carries the bit length of the
	 * macroblock *total* (the vendor firmware's MPEG-4 message packer
	 * shifts the macroblock width times the height right until it is zero
	 * and counts the steps), which is one wider than the field above
	 * whenever the total is a power of two. Both are kept because each has
	 * its own provenance: the parse width is what the encoder wrote
	 * (tiny.raw, a 4x4 grid, only decodes its packet headers at 4 bits),
	 * the message value is what the vendor programmed.
	 */
	pic->msg_mb_bits = ilog2(pic->mb_total) + 1;

	if (!pic->short_header)
		return;

	rows = pic->height <= 400 ? 1 : (pic->height <= 800 ? 2 : 4);
	pic->mb_per_gob = rows * pic->mb_width;
	pic->num_gobs = DIV_ROUND_UP(pic->mb_height, rows);
}

/*
 * A client that cuts access units at the VOP start codes hands the bytes that
 * open the *next* configuration - VOS, visual object, VOL, GOP - to this
 * picture's buffer, so a resolution change arrives here instead of with the
 * picture it configures. Walk what is left after this picture's data: cache
 * every VOL that is found (that is what puts the new grid in front of the next
 * picture) and end this picture's stream window at the first code, because from
 * there the buffer is not macroblock layer.
 *
 * A byte-aligned 00 00 01 cannot appear in the macroblock layer: it is a run of
 * 23 zero bits, which the macroblock escape exists to break, and a video packet
 * header is a shorter zero run (16 to 21 bits) closed by a '1', so its third
 * byte is 0x80 or more and never 0x01. The first code after the picture's data
 * is therefore its end, in sliced streams as well as in ones with the resync
 * marker disabled. Over the corpus this fires on 292 of 10616 pictures and
 * every one of them opens with VOS 0x000001B0 - Lavc and Xvid both re-issue the
 * sequence header before a GOP - and no VOL there failed to parse. The board
 * consequence of not doing this is a whole-frame error (168k bytes, max 246) on
 * every picture after a mid-stream VOL.
 */
static void histb_vdec_mpeg4_tail_config(struct histb_vdec_mpeg4_vol *vol,
					 const struct histb_vdec_mpeg4_pic *pic,
					 const u8 *buf, unsigned int size,
					 unsigned int data_bit,
					 unsigned int *au_end_bit)
{
	unsigned int pos = data_bit / 8, code, end = size;
	int ret;

	if (pic->short_header)
		return;

	while (!histb_vdec_mpeg4_find_code(buf, size, pos, &pos, &code)) {
		if (pos < end)
			end = pos;
		pos += 4;
		if (code == HISTB_MP4_CODE_VOP)
			break;
		if (code < HISTB_MP4_CODE_VOL_FIRST ||
		    code > HISTB_MP4_CODE_VOL_LAST)
			continue;
		ret = histb_vdec_mpeg4_vol(vol, buf, size, pos - 4);
		if (ret) {
			vol->config_pending = true;
			break;
		}
		vol->config_pending = false;
	}

	*au_end_bit = end * 8;
}

/*
 * Find the header structure of this buffer: an MPEG-4 VOL when one is present
 * (it updates the cached sequence parameters), then the VOP or H.263 picture
 * header. The start codes that carry nothing the descriptors need (VOS, GOP,
 * USER_DATA) are stepped over.
 */
static int histb_vdec_mpeg4_pic_start(struct histb_vdec_mpeg4_vol *vol,
				      u32 fourcc, const u8 *buf,
				      unsigned int size,
				      struct histb_vdec_mpeg4_pic *pic,
				      unsigned int *data_bit,
				      unsigned int *au_end_bit)
{
	unsigned int pos, code;
	int ret;

	*au_end_bit = size * 8;

	if (fourcc == V4L2_PIX_FMT_H263)
		return histb_vdec_h263_pic(pic, buf, size, data_bit);

	if (fourcc != V4L2_PIX_FMT_MPEG4)
		/*
		 * Only the two fourccs this codec's ops are registered for
		 * reach .run() (V4L2_PIX_FMT_MPEG4 and V4L2_PIX_FMT_H263, both
		 * bound to histb_vdec_mpeg4_ops), and the VOL/VOP walk below is
		 * MPEG-4 syntax: any other format arriving here would be parsed
		 * as something it is not, so the job is refused rather than
		 * walked.
		 */
		return -EINVAL;

	for (pos = 0; pos + 4 <= size;) {
		ret = histb_vdec_mpeg4_find_code(buf, size, pos, &pos, &code);
		if (ret)
			return ret;

		switch (code) {
		case HISTB_MP4_CODE_VOL_FIRST ... HISTB_MP4_CODE_VOL_LAST:
			ret = histb_vdec_mpeg4_vol(vol, buf, size, pos);
			if (ret)
				return ret;
			vol->config_pending = false;
			pos += 4;
			break;
		case HISTB_MP4_CODE_VOP:
			/* A VOL is required: the raw fourcc gives no other way
			 * to learn the grid, the quarter-pel flag, the packet
			 * mode and the quantiser tables the picture message
			 * carries. An unread configuration boundary makes every
			 * later grid a guess, so those pictures are refused
			 * too.
			 */
			if (!vol->parsed || vol->config_pending)
				return -EINVAL;
			ret = histb_vdec_mpeg4_vop(vol, buf, size, pos, pic,
						   data_bit);
			if (ret)
				return ret;
			histb_vdec_mpeg4_tail_config(vol, pic, buf, size,
						     *data_bit, au_end_bit);
			return 0;
		default:
			/* VOS, visual object, video object, GOP, user data and
			 * every other code carry nothing the descriptors need;
			 * step over them. The vendor's loop does the same (its
			 * VOS branch only warns about profile/level).
			 */
			pos += 4;
			break;
		}
	}

	/*
	 * The scan stepped over every start code in the buffer and never met a
	 * VOL followed by a VOP, so this submission carries no picture: a
	 * buffer of sequence-level syntax only (VOS, VO, GOP, user data), or
	 * one whose VOP lies beyond the bytes the client handed over. There is
	 * nothing to program - the picture message takes its grid, quantiser
	 * tables and packet geometry from the cached VOL and its timing, type
	 * and quantiser from the VOP - and inventing a window over the rest of
	 * the buffer would have the engine read sequence headers as macroblock
	 * layer. A client that splits access units at the VOP codes (the shape
	 * this driver is written for, see histb_vdec_mpeg4_tail_config()) never
	 * reaches this.
	 */
	return -EINVAL;
}

/* ----------------------------------------------- per-context frame state */

static void histb_vdec_mpeg4_pmv_free(struct histb_vdec_dev *dev,
				      struct histb_vdec_mpeg4_ctx *m4)
{
	unsigned int i;

	for (i = 0; i < HISTB_VDEC_MAX_PMV_SLOTS; i++) {
		if (!m4->pmv_va[i])
			continue;
		dma_free_wc(dev->dev, m4->pmv_slot_size, m4->pmv_va[i],
			    m4->pmv_dma[i]);
		m4->pmv_va[i] = NULL;
	}
	m4->pmv_slot_size = 0;
}

static int histb_vdec_mpeg4_pmv_grow(struct histb_vdec_ctx *ctx,
				     unsigned int mb_total)
{
	struct histb_vdec_dev *dev = video_get_drvdata(ctx->fh.vdev);
	struct histb_vdec_mpeg4_ctx *m4 = ctx->priv;
	unsigned int slot = ALIGN(mb_total * HISTB_MPEG4_PMV_BYTES_PER_MB,
				  128);
	unsigned int i;

	if (m4->pmv_slot_size >= slot)
		return 0;

	histb_vdec_mpeg4_pmv_free(dev, m4);

	for (i = 0; i < HISTB_VDEC_MAX_PMV_SLOTS; i++) {
		m4->pmv_va[i] = dma_alloc_wc(dev->dev, slot,
					     &m4->pmv_dma[i], GFP_KERNEL);
		if (!m4->pmv_va[i])
			break;
	}
	if (i < HISTB_VDEC_MAX_PMV_SLOTS) {
		histb_vdec_mpeg4_pmv_free(dev, m4);
		return -ENOMEM;
	}
	m4->pmv_slot_size = slot;
	memset(ctx->pmv_map, 0, sizeof(ctx->pmv_map));
	ctx->pmv_next = 0;

	return 0;
}

/* Slot id (+1, so a zeroed pmv_map[] reads as "unallocated") of one capture
 * buffer index, mirroring the H.264 allocator.
 */
static unsigned int histb_vdec_mpeg4_pmv_alloc(struct histb_vdec_ctx *ctx,
					       unsigned int index)
{
	u8 id = ctx->pmv_map[index];

	if (!id) {
		id = ctx->pmv_next++ % HISTB_VDEC_MAX_PMV_SLOTS + 1;
		ctx->pmv_map[index] = id;
	}

	return id;
}

/*
 * Address and motion-vector slot of one recorded anchor picture. The capture
 * buffer is found again by the timestamp its job carried, the reference
 * mechanism the MPEG-2 and H.264 layers use; a timestamp that no longer maps to
 * a queued buffer makes the picture fall back to its own frame.
 */
static dma_addr_t histb_vdec_mpeg4_ref_addr(struct histb_vdec_ctx *ctx,
					    const struct histb_vdec_mpeg4_ref *ref,
					    dma_addr_t cur, dma_addr_t cur_pmv,
					    dma_addr_t *pmv)
{
	struct histb_vdec_mpeg4_ctx *m4 = ctx->priv;
	struct vb2_buffer *vb;

	*pmv = cur_pmv;
	if (!ref->valid || ref->pmv_slot > HISTB_VDEC_MAX_PMV_SLOTS)
		return cur;

	vb = vb2_find_buffer(v4l2_m2m_get_dst_vq(ctx->fh.m2m_ctx),
			     ref->timestamp);
	if (!vb)
		return cur;

	*pmv = m4->pmv_dma[ref->pmv_slot - 1];

	return vb2_dma_contig_plane_dma_addr(vb, 0);
}

/* ------------------------------------------------------------------ run */

static int histb_vdec_mpeg4_run(struct histb_vdec_ctx *ctx,
				struct vb2_v4l2_buffer *src,
				struct vb2_v4l2_buffer *dst)
{
	struct histb_vdec_dev *dev = video_get_drvdata(ctx->fh.vdev);
	struct histb_vdec_msgpool *pool = &dev->pool;
	struct histb_vdec_mpeg4_ctx *m4 = ctx->priv;
	struct histb_vdec_mpeg4_pic pic = {};
	struct histb_vdec_mpeg4_slice *sls;
	unsigned int cap_w, cap_h, bytesused, data_bit = 0, au_end_bit = 0;
	unsigned int lines, tile_stride, end_bit, n_sl, i, cur_slot, next;
	void *slot;
	dma_addr_t src_addr, dst_addr, stream_base;
	dma_addr_t fwd, bwd, fwd_pmv, bwd_pmv, cur_pmv;
	dma_addr_t scratch_pmv, scratch_sed, scratch_itrans;
	u32 *w = histb_vdec_pic_slot(pool);
	u32 val, trb = 0, trd = 0;
	int ret;

	ret = histb_vdec_cap_dims(ctx, &cap_w, &cap_h);
	if (ret)
		return ret;

	bytesused = vb2_get_plane_payload(&src->vb2_buf, 0);
	/*
	 * Empty and runt OUTPUT submissions are refused here, before the header
	 * scan. The floor is not a syntax number and no measurement pins it:
	 * the header alone is far shorter than 32 bytes - the Annex-E pictures
	 * that decoded byte-exactly on this engine start their macroblock layer
	 * at bit 50 of the picture header, under 7 bytes with their start code,
	 * and an MPEG-4 VOP header is of the same order - so a buffer this
	 * short holds no header at all, and anything longer is answered by the
	 * span checks the parsers carry themselves rather than by this
	 * constant. TODO: to settle the number, run the MPEG-4 and H.263
	 * fixtures at descending payload floors and record, per fourcc, the
	 * shortest access unit that still decodes inside the rounding band;
	 * that is what belongs here - or the gate goes if the parsers' own
	 * bounds checks already refuse every shorter submission.
	 */
	if (bytesused < 32)
		return -EINVAL;

	{
		const u8 *buf = vb2_plane_vaddr(&src->vb2_buf, 0);

		/*
		 * These fourccs carry no stateless control set, so every number
		 * the picture message needs - the grid, the quantiser tables,
		 * the packet geometry, the slice windows - comes out of the
		 * submitted bytes, and reading them needs a kernel address. The
		 * OUTPUT queue deliberately keeps one (histb_vdec_queue_init()
		 * in histb_vdec_drv.c declines DMA_ATTR_NO_KERNEL_MAPPING for
		 * exactly this parse), so this arm answers a missing mapping,
		 * not a stream shape: with no bytes there is no header to read
		 * and no descriptor to write, and the alternative would be
		 * programming the engine with whatever the previous picture
		 * left in the pool. A client that cannot map its OUTPUT plane
		 * would need the parsed syntax to arrive through a control,
		 * which is the interface these two codecs do not have yet.
		 */
		if (!buf) {
			dev_warn_once(dev->dev,
				      "OUTPUT queue has no kernel mapping, cannot parse headers\n");
			return -EINVAL;
		}

		/* H.263 takes its size from the picture header's source_format
		 * and MPEG-4 from the VOL; either way the coded size has to fit
		 * the capture buffer the client negotiated, checked below.
		 */
		ret = histb_vdec_mpeg4_pic_start(&m4->vol, ctx->desc->fourcc,
						 buf, bytesused, &pic, &data_bit,
						 &au_end_bit);
		if (ret)
			return ret;
	}

	histb_vdec_mpeg4_grid(&pic);

	/*
	 * A picture of one macroblock column is refused for the reason measured
	 * on the H.264 path: the engine reports an error on such a submission,
	 * and declaring a wider grid does not fix the delivery.
	 */
	if (pic.mb_width < 2 || pic.mb_bits > 20 ||
	    pic.width > cap_w || pic.height > cap_h)
		return -EINVAL;

	/*
	 * The group grid has to fit the fields picture message D2 states it in:
	 * seven bits for the macroblocks per group and five for the group count
	 * (the field masks of the vendor firmware's MPEG-4 picture message), so
	 * a 16CIF picture, whose group is 4 x 88 = 352 macroblocks wide, has no
	 * representable description and is refused rather than truncated.
	 */
	if (pic.short_header && (pic.mb_per_gob > 127 || pic.num_gobs > 32))
		return -EINVAL;

	/*
	 * Inter pictures on grids below the smallest one measured reliable are
	 * refused (176x144 = 99 macroblocks). The reason is a delivery defect
	 * in the engine's inter path at these sizes: eight streams (16, 49, 64,
	 * 65, 66, 70, 72 and 80 macroblocks) showed 146..240-count errors from
	 * their second picture on, while every stream of 99 macroblocks or more
	 * matched the reference within the rounding family across two
	 * independent sweeps. The trigger could not be characterised: the same
	 * stream with the same predecessor gave 229, 1 and 1 in three
	 * consecutive runs of one boot, and buffer addresses, RAM pressure and
	 * a long mixed-resolution history were each measured not to move it, so
	 * no narrower condition can be written. Intra pictures of the same
	 * grids are not affected and stay accepted, and the H.263 short-header
	 * path is not gated because its small-grid inter pictures were measured
	 * clean.
	 */
	if (!pic.short_header && pic.coding_type != HISTB_MPEG4_I_VOP &&
	    pic.mb_total < 99)
		return -EINVAL;

	src_addr = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	dst_addr = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	stream_base = src_addr & ~255U;
	/* The stream base is the minimum of the 16-byte-aligned slice window
	 * addresses (the vendor firmware's MPEG-4 register setup takes it that
	 * way); every window lies in this buffer and vb2 dma-contig addresses
	 * are page aligned, so it is the buffer start - and the descriptor
	 * offsets are relative to it.
	 *
	 * That identity is what the gate below guards, not a stream shape: the
	 * descriptors count their granule from byte 0 of this buffer
	 * (histb_vdec_mpeg4_pack_slice() writes (data_bit / 8) & ~15), while
	 * VDM_STREAM_BASE is programmed with the address rounded down to 256
	 * bytes. If the two ever differ, every window the engine reads lands
	 * (src_addr & 255) bytes early - the head of the slice data is missed
	 * and bytes belonging to whatever precedes this buffer are walked
	 * instead. Page-aligned dma-contig addresses make that unreachable with
	 * today's allocator, which is why no measurement is attached to it; it
	 * would become reachable through a different mem_ops or an
	 * exporter-provided plane, and refusing the job is the only answer that
	 * keeps the engine out of memory this job does not own.
	 */
	if (stream_base != src_addr)
		return -EINVAL;

	ret = histb_vdec_mpeg4_pmv_grow(ctx, pic.mb_total);
	if (ret)
		return ret;

	sls = kvmalloc_array(HISTB_MPEG4_MAX_SLICES, sizeof(*sls), GFP_KERNEL);
	if (!sls)
		return -ENOMEM;

	{
		const u8 *buf = vb2_plane_vaddr(&src->vb2_buf, 0);

		ret = histb_vdec_mpeg4_slice_headers(buf, bytesused, &pic,
						     data_bit, sls,
						     HISTB_MPEG4_MAX_SLICES);
		if (ret < 0)
			goto out;
		n_sl = ret;
		/* The end of this picture's stream window: the buffer payload,
		 * or the first start code after its data when the client left
		 * the next configuration in here with it.
		 */
		end_bit = au_end_bit;

		if (pic.short_header) {
			/* A short-header picture takes its quantiser and its
			 * type from the first group header, which is the only
			 * place that syntax carries them; it has no fcode and
			 * no MPEG-4 packet geometry.
			 */
			pic.quant = sls[0].quant;
			pic.coding_type = sls[0].coding_type;
			pic.fcode_fwd = 0;
			pic.fcode_bwd = 0;
		}

		if (n_sl + 1 >= HISTB_VDEC_MAX_SLICE_SLOTS) {
			ret = -EINVAL;
			goto out;
		}

		for (i = 0; i < n_sl; i++) {
			unsigned int end_mbn = i + 1 < n_sl ?
				sls[i + 1].start_mbn - 1 : pic.mb_total - 1;

			if (sls[i].start_mbn > end_mbn ||
			    end_mbn >= pic.mb_total) {
				ret = -EINVAL;
				goto out;
			}
			slot = histb_vdec_mpeg4_slc_slot(pool, i);
			next = i + 1 < n_sl ?
				histb_vdec_mpeg4_slc_slot_dma(pool, i + 1) : 0;
			histb_vdec_mpeg4_pack_slice(slot, end_bit, &sls[i],
						    &pic, sls[i].start_mbn,
						    end_mbn, next);
		}
		/* The chain ends on a null pointer, and the slot after the last
		 * descriptor is left cleared: that is where a follower that
		 * ignores the terminator lands.
		 */
		memset(histb_vdec_mpeg4_slc_slot(pool, n_sl), 0,
		       HISTB_MPEG4_SLC_BYTES);
	}

	histb_vdec_buf_geom(ctx, &tile_stride, &lines);

	cur_slot = histb_vdec_mpeg4_pmv_alloc(ctx, dst->vb2_buf.index);
	cur_pmv = m4->pmv_dma[cur_slot - 1];

	fwd = dst_addr;
	bwd = dst_addr;
	fwd_pmv = cur_pmv;
	bwd_pmv = cur_pmv;
	if (pic.coding_type == HISTB_MPEG4_P_VOP) {
		fwd = histb_vdec_mpeg4_ref_addr(ctx, &m4->anchor[0], dst_addr,
						cur_pmv, &fwd_pmv);
		bwd = fwd;
		bwd_pmv = fwd_pmv;
	} else if (pic.coding_type == HISTB_MPEG4_B_VOP) {
		/* Both anchors were coded before this picture: anchor[0] is the
		 * later one, which is the backward reference, anchor[1] the
		 * earlier one.
		 */
		fwd = histb_vdec_mpeg4_ref_addr(ctx, &m4->anchor[1], dst_addr,
						cur_pmv, &fwd_pmv);
		bwd = histb_vdec_mpeg4_ref_addr(ctx, &m4->anchor[0], dst_addr,
						cur_pmv, &bwd_pmv);
	}

	scratch_pmv = histb_vdec_tables_dma(pool) +
		HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_PMV_TOP);
	scratch_sed = histb_vdec_tables_dma(pool) +
		HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_SED_TOP);
	scratch_itrans = histb_vdec_tables_dma(pool) +
		HISTB_TABLE_OFF_SCRATCH(HISTB_SCRATCH_ITRANS_TOP);

	/*
	 * The B-VOP distances the picture message states in D3 come from the
	 * vop time code: the forward distance shifted by one into bits [15:0]
	 * and the anchor-to-anchor distance into [31:17], which is how the
	 * vendor firmware's MPEG-4 picture-message packer forms them. The two
	 * variables are the vendor's own accumulation - time in ticks of
	 * vop_time_increment_resolution, pp_time the anchor-to-anchor distance
	 * updated by every I/P, pb_time this B's distance from its forward
	 * anchor, as its software decoder computes them. Writing zero here
	 * leaves the engine no way to split a bidirectional motion vector in
	 * time, which is what the board showed on B pictures (max deviation 163
	 * against the reference, against 1 elsewhere).
	 */
	if (!pic.short_header) {
		unsigned int tir = m4->vol.time_incr_res;
		unsigned int time, pb;

		if (pic.coding_type == HISTB_MPEG4_B_VOP)
			time = (m4->last_time_base + pic.time_modulo) * tir +
			       pic.time_increment;
		else
			time = (m4->time_base + pic.time_modulo) * tir +
			       pic.time_increment;

		if (pic.coding_type != HISTB_MPEG4_B_VOP) {
			m4->last_time_base = m4->time_base;
			m4->time_base += pic.time_modulo;
			if (time < m4->last_non_b_time) {
				/* the bare time_base rollover DivX/Xvid streams
				 * produce; the vendor's software decoder
				 * compensates it the same way under its umpeg4
				 * work-around
				 */
				m4->time_base++;
				time += tir;
			}
			m4->pp_time = time - m4->last_non_b_time;
			m4->last_non_b_time = time;
			trd = m4->pp_time;
		} else {
			pb = m4->pp_time - (m4->last_non_b_time - time);
			/*
			 * The picture must sit strictly between its two
			 * anchors: 0 < pb_time < pp_time, which is what the
			 * three tests here say. These two numbers are operands,
			 * not labels - D3 is the only place the engine learns
			 * the temporal split of a bidirectional vector, and the
			 * vendor's own path treats a zero pp_time as a division
			 * it must not attempt, while its header walk skips the
			 * picture outright for these same three predicates and
			 * reports the time code as out of order. A pair outside
			 * that range cannot be stated in the message either:
			 * both fields are unsigned and pb_time is the one the
			 * backward distance is derived from, so writing it
			 * would leave the engine interpolating between two
			 * anchors it cannot order. Only this B picture is
			 * dropped, the accumulation above stays as it is, and
			 * the vendor drops the same frame for the same reason.
			 * Accepting it would need the distances from somewhere
			 * other than the vop time code, which this raw-fourcc
			 * interface has no control for.
			 */
			if (!m4->pp_time || m4->pp_time <= pb ||
			    m4->pp_time <= m4->pp_time - pb)
				return -EINVAL;	/* the vendor drops this frame */
			trb = pb;
			trd = m4->pp_time;
		}
	}

	memset(w, 0, HISTB_VDEC_SLOT_BYTES);
	w[3] = ((trb << 1) & GENMASK(14, 0)) | FIELD_PREP(GENMASK(31, 17), trd);

	if (pic.short_header) {
		/* D0 selects the H.263 slice syntax. D1 carries the GOB grid
		 * the engine needs to turn a GOB number into a macroblock
		 * range, D2 the picture type and the slice number width.
		 */
		w[0] = HISTB_MPEG4_D0_H263_SHORT;
		w[1] = FIELD_PREP(GENMASK(4, 0), pic.num_gobs - 1) |
		       FIELD_PREP(GENMASK(11, 5), pic.mb_per_gob) |
		       FIELD_PREP(GENMASK(30, 26), pic.quant);
		w[2] = FIELD_PREP(GENMASK(6, 5), pic.coding_type) |
		       FIELD_PREP(GENMASK(25, 22), pic.msg_mb_bits);
	} else {
		w[1] = FIELD_PREP(GENMASK(30, 26), pic.quant) |
		       FIELD_PREP(BIT(31), pic.quant_type);
		/*
		 * The rounding bit is forced to 0 for B-VOPs, which is what the
		 * vendor firmware's MPEG-4 picture-message packer does for
		 * picture type 2.
		 */
		val = FIELD_PREP(GENMASK(6, 5), pic.coding_type) |
		      FIELD_PREP(GENMASK(10, 8), pic.intra_dc_vlc_thr) |
		      FIELD_PREP(GENMASK(13, 11), pic.fcode_fwd) |
		      FIELD_PREP(GENMASK(16, 14), pic.fcode_bwd) |
		      FIELD_PREP(GENMASK(21, 17), pic.time_incr_bits) |
		      FIELD_PREP(GENMASK(25, 22), pic.msg_mb_bits);
		if (pic.coding_type != HISTB_MPEG4_B_VOP && pic.rounding)
			val |= BIT(3);
		if (pic.quarter_sample)
			val |= BIT(4);
		if (pic.resync_marker_disable)
			val |= BIT(7);
		/* sprite_enable (bits 27:26), sprite_warping_accuracy (29:28)
		 * and sprite_warping_points (31:30) stay 0: those pictures are
		 * refused.
		 */
		w[2] = val;
		/*
		 * D3 carries the B-VOP time increments (trb, trd), which come
		 * from sequence-level timing state the raw-fourcc interface has
		 * no field for, so they stay 0.
		 */
	}

	w[4] = FIELD_PREP(GENMASK(15, 0), pic.mb_width - 1) |
	       FIELD_PREP(GENMASK(31, 16), pic.mb_height - 1);
	w[5] = FIELD_PREP(GENMASK(15, 0), pic.width) |
	       FIELD_PREP(GENMASK(31, 16), pic.height);
	/* D6/D7 carry a one-point warp offset for sprite pictures (refused),
	 * D8-D11 the picture-level stream segments, which this protocol leaves
	 * to the slice descriptors, and D19 the encoder-compatibility flags the
	 * vendor fills from DivX/Xvid build numbers found in USER_DATA -
	 * nothing here emulates an encoder bug. All stay 0.
	 */
	w[12] = dst_addr & ~15U;		/* display frame */
	w[13] = fwd & ~15U;
	w[14] = bwd & ~15U;
	w[15] = cur_pmv & ~15U;
	w[16] = bwd_pmv & ~15U;
	w[17] = scratch_itrans & ~15U;
	w[18] = scratch_pmv & ~15U;
	/* The matrices go in D28-D59 only when the VOL uses MPEG-2 style
	 * quantisation (the vendor's load gate); they are written either way,
	 * CFG0's LD_QMATRIX being set on every picture, because the engine
	 * ignores them for the linear quantiser.
	 */
	histb_vdec_mpeg4_pack_qmatrix(w, m4->vol.intra_matrix,
				      m4->vol.inter_matrix);
	w[60] = scratch_sed & ~15U;
	w[63] = histb_vdec_mpeg4_slc_slot_dma(pool, 0) & ~15U;

	/* Register set. CFG0 reproduces the vendor firmware's MPEG-4 register
	 * setup bit for bit: the macroblock count with LD_QMATRIX, VOP-type
	 * detect and marker detect. CFG1 does not: the value the firmware
	 * writes there is the band pitch shifted right by six into the stride
	 * field, plus VID_STD 2, DBLK_1D_EN, FST_SLC_GRP, MV_OUT_EN, a maximum
	 * slice group number of 1 and a compress flag taken from its decode
	 * parameters. Those extras belong to the vendor's macroblock
	 * information export: MV_OUT_EN is paired with VDM_DNR_MBINFO_STADDR,
	 * which the firmware fills from the macroblock-information buffer it
	 * allocated in its global hardware memory pool and which this driver
	 * never writes because no such buffer is registered here. Keeping the
	 * bit clear is therefore the arm that cannot write to an address the
	 * driver does not own. The compress flag is the vendor's
	 * compressed-frame output switch, outside the stateless uAPI. The slice
	 * walk is bounded by the descriptor chain and the capacity gate above,
	 * not by the maximum slice group number, so the 3 written here and the
	 * vendor's 1 cannot be told apart by any stream.
	 */
	vdm_write(dev, VDM_CFG0,
		  FIELD_PREP(VDM_CFG0_MBAMT, pic.mb_total - 1) |
		  VDM_CFG0_MARKER_DETECT | VDM_CFG0_VOPTYPE_DETECT |
		  VDM_CFG0_LD_QMATRIX);
	/*
	 * The vendor firmware feeds this field and VDM_YSTRIDE_1D from one and
	 * the same parameter (the band pitch shifted right by six into
	 * CFG1[13:4], the band pitch itself into the stride register), so the
	 * band pitch goes in both. The H.264 and MPEG-2 paths write the line
	 * stride here and deliver byte-exact pictures, and an on-board arm that
	 * cleared DDR_STRIDE moved nothing, so the field is inert either way;
	 * this file keeps vendor parity.
	 */
	vdm_write(dev, VDM_CFG1,
		  FIELD_PREP(VDM_CFG1_VID_STD, HISTB_MPEG4_CFG1_VIDSTD) |
		  FIELD_PREP(VDM_CFG1_DDR_STRIDE,
			     histb_vdec_band_bytes(tile_stride) >> 6) |
		  FIELD_PREP(VDM_CFG1_MAX_SLCGRP_NUM, 3) |
		  VDM_CFG1_DBLK_1D_EN);
	vdm_write(dev, VDM_AVM_ADDR, pool->dma & ~15U);
	vdm_write(dev, VDM_VAM_ADDR, histb_vdec_up_msgs_dma(pool) & ~15U);
	vdm_write(dev, VDM_STREAM_BASE, stream_base);
	vdm_write(dev, VDM_YSTADDR_1D, dst_addr & ~15U);
	vdm_write(dev, VDM_YSTRIDE_1D, histb_vdec_band_bytes(tile_stride));
	vdm_write(dev, VDM_UVOFFSET_1D,
		  histb_vdec_luma_bytes(tile_stride, lines) +
		  histb_vdec_head_inf_bytes(lines));
	vdm_write(dev, VDM_HEAD_INF_OFFSET, histb_vdec_head_inf_bytes(lines));
	vdm_write(dev, VDM_PRC_CACHE_TYPE, 0);

	for (i = 1; i <= 7; i++)
		vdm_write(dev, VDM_TO(i), VDM_TIMEOUT_MOD);
	vdm_write(dev, VDM_INT_STATE, 0xffffffff);
	vdm_write(dev, VDM_INT_MASK, 0xfffffffe);

	/* The vendor firmware's MPEG-4 register setup also drives bit16 of the
	 * SCD global register: set for a picture of at most 120 macroblock
	 * columns, clear above it.
	 */
	val = readl(dev->scd + SCD_EMAR_ID);
	if (pic.mb_width <= 120)
		val |= BIT(16);
	else
		val &= ~BIT(16);
	writel(val, dev->scd + SCD_EMAR_ID);

	/* An anchor picture enters the history; B-VOPs are never referenced. */
	if (pic.coding_type != HISTB_MPEG4_B_VOP) {
		m4->anchor[1] = m4->anchor[0];
		m4->anchor[0] = (struct histb_vdec_mpeg4_ref){
			src->vb2_buf.timestamp, cur_slot, true,
		};
	}

	ret = 0;

out:
	kvfree(sls);
	if (ret)
		return ret;

	histb_vdec_hw_kick(dev, ctx->desc->vidstd);

	return 0;
}

/* --------------------------------------------------------------- wiring */

static int histb_vdec_mpeg4_start(struct histb_vdec_ctx *ctx)
{
	struct histb_vdec_mpeg4_ctx *m4;

	m4 = kzalloc_obj(*m4);
	if (!m4)
		return -ENOMEM;

	memcpy(m4->vol.intra_matrix, histb_mp4_intra_matrix, 64);
	memcpy(m4->vol.inter_matrix, histb_mp4_inter_matrix, 64);
	ctx->priv = m4;

	return 0;
}

static void histb_vdec_mpeg4_stop(struct histb_vdec_ctx *ctx)
{
	struct histb_vdec_dev *dev = video_get_drvdata(ctx->fh.vdev);
	struct histb_vdec_mpeg4_ctx *m4 = ctx->priv;

	if (!m4)
		return;

	histb_vdec_mpeg4_pmv_free(dev, m4);
	kfree(m4);
	ctx->priv = NULL;

	memset(ctx->pmv_map, 0, sizeof(ctx->pmv_map));
	ctx->pmv_next = 0;
}

static const struct histb_vdec_codec_ops histb_vdec_mpeg4_ops = {
	.start = histb_vdec_mpeg4_start,
	.stop = histb_vdec_mpeg4_stop,
	.run = histb_vdec_mpeg4_run,
};

const struct histb_vdec_fmt_desc histb_vdec_mpeg4_fmt = {
	.fourcc = V4L2_PIX_FMT_MPEG4,
	.stride = histb_vdec_tile_stride,
	.grid_align = 16,
	.num_ctrls = 0,
	.ctrls = NULL,
	.ops = &histb_vdec_mpeg4_ops,
	.vidstd = HISTB_VIDSTD_MPEG4,
};

/* H.263 short-header pictures go through the same MPEG-4 engine, with the
 * picture message word D0 and the slice syntax switched over.
 */
const struct histb_vdec_fmt_desc histb_vdec_h263_fmt = {
	.fourcc = V4L2_PIX_FMT_H263,
	.stride = histb_vdec_tile_stride,
	.grid_align = 16,
	.num_ctrls = 0,
	.ctrls = NULL,
	.ops = &histb_vdec_mpeg4_ops,
	.vidstd = HISTB_VIDSTD_MPEG4,
};
