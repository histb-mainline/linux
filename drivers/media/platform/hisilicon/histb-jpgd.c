// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * JPEG decoder (JPGD) for HiSilicon Hi3798MV100.
 *
 * One baseline JPEG picture per mem2mem job. The bitstream header is parsed in
 * the kernel and expanded into the tables the block wants - quantiser values in
 * raster order, Huffman tables in the block's own min/base/symbol form - and the
 * block then writes 8-bit semi-planar YUV into the capture buffer.
 *
 * The programming order follows the boot reference implementation in the vendor
 * SDK under boot/product/driver/graphic/hard-dec, which is the only complete
 * description of the block: the shipping hi_jpeg.ko hands its registers to
 * userspace and compiles the sampling-factor and restart-interval writes out for
 * this chip (msp/drv/jpeg/include/hi_jpeg_config.h:127-147).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/videodev2.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-device.h>
#include <media/v4l2-jpeg.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>
#include <media/videobuf2-v4l2.h>

#include <linux/cleanup.h>

#define HISTB_JPGD_NAME		"histb-jpgd"

/* ------------------------------------------------------------------ */
/* Registers (msp/drv/jpeg/include/hi_jpeg_reg.h)                      */
/* ------------------------------------------------------------------ */

#define JPGD_REG_START		0x00
#define JPGD_START_RUN		BIT(0)
#define JPGD_START_BUSY		BIT(1)

#define JPGD_REG_RESUME		0x04
#define JPGD_RESUME_RESTART	0x02

#define JPGD_REG_STRIDE		0x0c
#define JPGD_STRIDE_Y		GENMASK(15, 0)
#define JPGD_STRIDE_CBCR	GENMASK(31, 16)

/* A picture is sized to the block in MCUs, not in pixels. */
#define JPGD_REG_PICSIZE	0x10
#define JPGD_PICSIZE_X		GENMASK(15, 0)
#define JPGD_PICSIZE_Y		GENMASK(31, 16)

#define JPGD_REG_PICTYPE	0x14
#define JPGD_PIC_YUV400		0
#define JPGD_PIC_YUV420		3
#define JPGD_PIC_YUV422_21	4
#define JPGD_PIC_YUV422_12	5
#define JPGD_PIC_YUV444		6

/* Stream buffer window and entropy-data window: the block reads past both. */
#define JPGD_REG_STADDR		0x20
#define JPGD_REG_ENDADDR	0x24
#define JPGD_REG_STADD		0x28
#define JPGD_REG_ENDADD		0x2c

#define JPGD_REG_YSTADDR	0x30
#define JPGD_REG_UVSTADDR	0x34

#define JPGD_REG_SCALE		0x40
#define JPGD_SCALE_1TO1		0x3c

#define JPGD_REG_OUTTYPE	0x44
#define JPGD_OUTTYPE_YUV420SP	0x04

#define JPGD_REG_INT		0x100
#define JPGD_INT_FINISH		BIT(0)
#define JPGD_INT_ERROR		BIT(1)
#define JPGD_INT_CONTINUE	BIT(2)
#define JPGD_INT_JOB		(JPGD_INT_FINISH | JPGD_INT_ERROR | JPGD_INT_CONTINUE)

#define JPGD_REG_INTMASK	0x104
#define JPGD_INTMASK_ALL	0x3f
#define JPGD_INTMASK_RUN	0x20	/* every source but the low-delay one */

#define JPGD_REG_SAMPLINGFACTOR	0x124
#define JPGD_REG_DRI		0x128
#define JPGD_REG_MMU_BYPASS	0x134
#define JPGD_MMU_BYPASS_ALL	0xff

/* Status side of the block, used by the bring-up dump. */
#define JPGD_REG_PICVLDNUM	0x08
#define JPGD_REG_TIME		0x18
#define JPGD_REG_DEBUG		0x108
#define JPGD_REG_AXI4KCNT	0x11c
#define JPGD_REG_AXI16MCNT	0x120

#define JPGD_REG_QUANT		0x200	/* 64 words, one per coefficient */
#define JPGD_REG_HDCTABLE	0x300	/* 12 words, indexed by DC symbol */
#define JPGD_REG_HACMINTABLE	0x340	/* 8 words, two code lengths each */
#define JPGD_REG_HACBASETABLE	0x360	/* 8 words */
#define JPGD_REG_HACSYMTABLE	0x400	/* 176 words */

#define JPGD_COEFFS		64
#define JPGD_DC_SYM_SLOTS	12
#define JPGD_SYM_TAB_WORDS	176
#define JPGD_MAX_CODE_LEN	16
#define JPGD_MAX_HTAB_SYMS	256

/* ------------------------------------------------------------------ */
/* Block limits                                                        */
/* ------------------------------------------------------------------ */

#define JPGD_MCU_UNIT		8	/* one sampling factor is 8 pixels */
#define JPGD_MAX_DIM		8095
#define JPGD_STRIDE_ALIGN	64	/* luma stride granularity */
#define JPGD_STREAM_MARGIN	128	/* window the block reads outside the ECS */
#define JPGD_DEF_STREAM_SIZE	SZ_4M
#define JPGD_MAX_STREAM_SIZE	SZ_32M
#define JPGD_JOB_TIMEOUT	msecs_to_jiffies(5000)
#define JPGD_RESET_BUDGET_US	1000

/*
 * struct histb_jpgd_chip - what this generation of the block exposes
 * @has_mmu_bypass: JPGD_REG_MMU_BYPASS (0x134) belongs to the SMMU-facing front
 *                  end. Hi3798MV100's register file names it and the vendor
 *                  driver programs it; Hi3716CV200's own file
 *                  (msp/drv/jpeg/include/hi_jpeg_reg.h) has no name at 0x134 at
 *                  all -- its map runs 0x130 then 0x200 -- so on that part the
 *                  word is reserved and is left alone.
 */
struct histb_jpgd_chip {
	bool has_mmu_bypass;
};

/*
 * struct histb_jpgd - one JPGD instance
 * @hw_lock: the block decodes one picture at a time, so a job holds it from its
 *	     first table write to its completion; that also leaves @int_state and
 *	     @int_done to a single writer and reader pair
 */
struct histb_jpgd {
	struct device		*dev;
	const struct histb_jpgd_chip *chip;
	void __iomem		*regs;
	struct clk		*clk;
	struct reset_control	*rst;
	struct v4l2_device	v4l2_dev;
	struct video_device	*vdev;
	struct v4l2_m2m_dev	*m2m_dev;
	struct mutex		hw_lock;
	struct mutex		vdev_lock;
	wait_queue_head_t	done;
	u32			int_state;
	bool			int_done;
};

/*
 * struct histb_jpgd_q - the plane layout the block is programmed with
 *
 * S_FMT installs it from a pixel size alone, so the row rounding assumes the
 * tallest MCU any accepted stream can have - four 8-pixel luma rows - which is
 * also where the chroma plane then starts.
 */
struct histb_jpgd_q {
	unsigned int width;
	unsigned int height;
	unsigned int luma_stride;
	unsigned int chroma_stride;
	unsigned int coded_height;	/* luma rows the block writes */
	unsigned int chroma_height;
	unsigned int chroma_offset;
	unsigned int size;
};

struct histb_jpgd_ctx {
	struct histb_jpgd	*jpgd;
	struct v4l2_fh		fh;
	struct histb_jpgd_q	q;	/* CAPTURE layout installed by S_FMT */
	unsigned int		out_size;
	const char		*why;
	void			*stream;	/* bitstream copy with margins */
	dma_addr_t		stream_dma;
	size_t			stream_alloc;
};

/* v4l2-ioctl passes no file handle to the format ops, only to the file. */
static struct histb_jpgd_ctx *file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct histb_jpgd_ctx, fh);
}

/* ------------------------------------------------------------------ */
/* Capture layout                                                      */
/* ------------------------------------------------------------------ */

/*
 * The block stores every picture it accepts as 4:2:0 semi-planar with the red
 * difference in the first lane. Both halves of that were measured: a 4:2:2
 * stream left the second half of the chroma plane untouched, and the two
 * interleaved lanes matched Cr then Cb against a software decode. So one capture
 * format covers every accepted stream, and the source's own subsampling only
 * picks the PICTYPE the entropy decoder reads.
 */
#define HISTB_JPGD_CAP_FMT	V4L2_PIX_FMT_NV21

/*
 * 4:4:4 streams set the block's own error bit even when the picture is otherwise
 * described the way the reference does, and the shapes that have no measurement
 * behind them at all - 4:0:0, which writes luma alone, and 4:2:2 with two
 * vertical chroma samples - are refused rather than decoded into a layout nobody
 * read back.
 */
#define JPGD_PIC_UNSUPPORTED	0xff

/*
 * A picture is sized to the block in MCUs, not in pixels: the luma sampling
 * factor says how many 8-pixel blocks an MCU carries in each direction.
 */
struct histb_jpgd_pic {
	unsigned int mcu_x;
	unsigned int mcu_y;
	u8	pic_type;
	u8	factor[3];	/* SAMPLINGFACTOR bytes: (H << 4) | V */
};

static u8 histb_jpgd_pic_type(const struct v4l2_jpeg_frame_header *frame)
{
	const struct v4l2_jpeg_frame_component_spec *c = frame->component;

	if (frame->num_components != 3)
		return JPGD_PIC_UNSUPPORTED;
	if (c[1].horizontal_sampling_factor != c[2].horizontal_sampling_factor ||
	    c[1].vertical_sampling_factor != c[2].vertical_sampling_factor)
		return JPGD_PIC_UNSUPPORTED;

	if (c[0].horizontal_sampling_factor == 2 * c[1].horizontal_sampling_factor) {
		if (c[0].vertical_sampling_factor == 2 * c[1].vertical_sampling_factor)
			return JPGD_PIC_YUV420;
		if (c[0].vertical_sampling_factor == c[1].vertical_sampling_factor)
			return JPGD_PIC_YUV422_21;
	}

	return JPGD_PIC_UNSUPPORTED;
}

static void histb_jpgd_pic_of(const struct v4l2_jpeg_header *hdr,
			      struct histb_jpgd_pic *pic)
{
	const struct v4l2_jpeg_frame_component_spec *c = hdr->frame.component;
	unsigned int i;

	pic->pic_type = histb_jpgd_pic_type(&hdr->frame);
	pic->mcu_x = DIV_ROUND_UP(hdr->frame.width,
				  JPGD_MCU_UNIT * c[0].horizontal_sampling_factor);
	pic->mcu_y = DIV_ROUND_UP(hdr->frame.height,
				  JPGD_MCU_UNIT * c[0].vertical_sampling_factor);
	for (i = 0; i < 3; i++)
		pic->factor[i] = (c[i].horizontal_sampling_factor << 4) |
				 c[i].vertical_sampling_factor;
}

#define JPGD_MAX_LUMA_V		2	/* tallest luma factor of an accepted MCU */

static void histb_jpgd_geom(unsigned int width, unsigned int height,
			    unsigned int luma_v, struct histb_jpgd_q *q)
{
	unsigned int luma_h = JPGD_MCU_UNIT * luma_v;

	q->width = width;
	q->height = height;
	q->luma_stride = ALIGN(width, JPGD_STRIDE_ALIGN);
	q->coded_height = ALIGN(height, luma_h);
	q->chroma_stride = q->luma_stride;
	q->chroma_height = q->coded_height / 2;
	q->chroma_offset = q->luma_stride * q->coded_height;
	q->size = q->chroma_offset + q->chroma_stride * q->chroma_height;
}

/* Both PICSIZE fields are 16 bits wide, and the strides are 16 bits each. */
static bool histb_jpgd_fits(const struct histb_jpgd_q *q,
			     const struct histb_jpgd_pic *pic)
{
	return q->luma_stride <= U16_MAX && q->chroma_stride <= U16_MAX &&
	       pic->mcu_x <= U16_MAX && pic->mcu_y <= U16_MAX;
}

/* ------------------------------------------------------------------ */
/* Huffman and quantiser table expansion                               */
/* ------------------------------------------------------------------ */

/*
 * struct histb_jpgd_htab - one Huffman table in the block's own form
 * @bits: bits[n] codes of length n for n = 1..JPGD_MAX_CODE_LEN; bits[0] is 0
 * @code: canonical code per symbol position, in stream order
 * @vals: HUFFVAL bytes of the table
 * @max_len: longest code length the table uses
 * @total: symbol count
 */
struct histb_jpgd_htab {
	unsigned int bits[JPGD_MAX_CODE_LEN + 1];
	unsigned int code[JPGD_MAX_HTAB_SYMS];
	const u8	*vals;
	unsigned int max_len;
	unsigned int total;
};

static int histb_jpgd_htab_load(struct histb_jpgd_htab *t,
				const struct v4l2_jpeg_reference *ref)
{
	unsigned int len, n, code = 0;

	memset(t->bits, 0, sizeof(t->bits));
	t->max_len = 0;
	t->total = 0;
	t->vals = NULL;

	if (!ref || !ref->start || ref->length < JPGD_MAX_CODE_LEN)
		return -EINVAL;

	for (len = 1; len <= JPGD_MAX_CODE_LEN; len++)
		t->bits[len] = ref->start[len - 1];
	t->vals = ref->start + JPGD_MAX_CODE_LEN;
	t->total = ref->length - JPGD_MAX_CODE_LEN;
	if (t->total > ARRAY_SIZE(t->code))
		return -EINVAL;

	/* Canonical assignment: one length's codes start where twice the previous
	 * run left off.
	 */
	n = 0;
	for (len = 1; len <= JPGD_MAX_CODE_LEN; len++) {
		unsigned int count = t->bits[len];

		if (!count)
			continue;
		t->max_len = len;
		if (n + count > t->total)
			return -EINVAL;
		while (count--)
			t->code[n++] = code++;
		code <<= 1;
	}

	return 0;
}

static int histb_jpgd_htab_ref(struct histb_jpgd_htab *t, const u8 *table,
			       size_t len)
{
	struct v4l2_jpeg_reference ref = { (u8 *)table, len };

	return histb_jpgd_htab_load(t, &ref);
}

/*
 * The block's DC table has one slot per symbol value and packs into each slot
 * the luma length and code in its low half and the chroma pair above, so a table
 * with more symbols than there are slots - or a symbol value beyond them -
 * cannot be programmed at all.
 */
static int histb_jpgd_write_dc(struct histb_jpgd *jpgd,
			       const struct histb_jpgd_htab *lum,
			       const struct histb_jpgd_htab *chroma)
{
	const struct histb_jpgd_htab *tab[2] = { lum, chroma };
	u32 words[JPGD_DC_SYM_SLOTS] = { 0 };
	unsigned int class, i;

	for (class = 0; class < 2; class++) {
		const struct histb_jpgd_htab *t = tab[class];
		unsigned int sym[JPGD_DC_SYM_SLOTS], len[JPGD_DC_SYM_SLOTS];
		unsigned int code[JPGD_DC_SYM_SLOTS];
		unsigned int n = 0, l, k;

		if (t->total > ARRAY_SIZE(sym))
			return -EINVAL;

		for (l = 1; l <= t->max_len; l++) {
			for (k = 0; k < t->bits[l]; k++) {
				sym[n] = t->vals[n];
				len[n] = l;
				code[n] = t->code[n];
				n++;
			}
		}

		/* The hardware is addressed by symbol value, not stream order. */
		for (i = 0; i < n; i++) {
			for (k = i + 1; k < n; k++) {
				unsigned int v;

				if (sym[i] <= sym[k])
					continue;
				v = sym[i]; sym[i] = sym[k]; sym[k] = v;
				v = len[i]; len[i] = len[k]; len[k] = v;
				v = code[i]; code[i] = code[k]; code[k] = v;
			}
		}

		for (i = 0; i < n; i++) {
			if (sym[i] >= JPGD_DC_SYM_SLOTS)
				return -EINVAL;
			if (class == 0)
				words[sym[i]] = (words[sym[i]] & 0xfffff000) |
						FIELD_PREP(GENMASK(11, 8),
							   len[i] & 0xf) |
						FIELD_PREP(GENMASK(7, 0),
							   code[i] & 0xff);
			else
				words[sym[i]] = (words[sym[i]] & 0xff000fff) |
						FIELD_PREP(GENMASK(23, 20),
							   len[i] & 0xf) |
						FIELD_PREP(GENMASK(19, 12),
							   code[i] & 0xff);
		}
	}

	for (i = 0; i < ARRAY_SIZE(words); i++)
		writel(words[i], jpgd->regs + JPGD_REG_HDCTABLE + 4 * i);

	return 0;
}

static int histb_jpgd_write_ac(struct histb_jpgd *jpgd,
			       const struct histb_jpgd_htab *lum,
			       const struct histb_jpgd_htab *chroma, u32 *sym,
			       u32 (*min)[JPGD_MAX_CODE_LEN],
			       u32 (*base)[JPGD_MAX_CODE_LEN])
{
	const struct histb_jpgd_htab *tab[2] = { lum, chroma };
	unsigned int class, i, l;

	memset(sym, 0, sizeof(sym[0]) * JPGD_MAX_HTAB_SYMS);

	for (class = 0; class < 2; class++) {
		const struct histb_jpgd_htab *t = tab[class];
		unsigned int total = 0;

		memset(min[class], 0, sizeof(min[class][0]) *
		       JPGD_MAX_CODE_LEN);
		memset(base[class], 0, sizeof(base[class][0]) *
		       JPGD_MAX_CODE_LEN);

		for (l = 0; l < JPGD_MAX_CODE_LEN; l++) {
			unsigned int prev = l ? l - 1 : 0;

			if (l >= t->max_len) {
				min[class][l] = ~0;
				continue;
			}
			min[class][l] = (min[class][prev] + t->bits[l]) << 1;
			if (t->bits[l + 1])
				base[class][l] = total - min[class][l];
			total += t->bits[l + 1];
		}
		if (total > JPGD_MAX_HTAB_SYMS)
			return -EINVAL;

		/* One word per symbol position: luma value below, chroma above. */
		for (i = 0; i < total; i++)
			sym[i] |= class ? (u32)t->vals[i] << 8 : t->vals[i];
	}

	for (i = 0; i < JPGD_MAX_CODE_LEN / 2; i++) {
		writel(FIELD_PREP(GENMASK(7, 0), min[0][2 * i] & 0xff) |
		       FIELD_PREP(GENMASK(15, 8), min[0][2 * i + 1] & 0xff) |
		       FIELD_PREP(GENMASK(23, 16), min[1][2 * i] & 0xff) |
		       FIELD_PREP(GENMASK(31, 24), min[1][2 * i + 1] & 0xff),
		       jpgd->regs + JPGD_REG_HACMINTABLE + 4 * i);
		writel(FIELD_PREP(GENMASK(7, 0), base[0][2 * i] & 0xff) |
		       FIELD_PREP(GENMASK(15, 8), base[0][2 * i + 1] & 0xff) |
		       FIELD_PREP(GENMASK(23, 16), base[1][2 * i] & 0xff) |
		       FIELD_PREP(GENMASK(31, 24), base[1][2 * i + 1] & 0xff),
		       jpgd->regs + JPGD_REG_HACBASETABLE + 4 * i);
	}

	for (i = 0; i < JPGD_SYM_TAB_WORDS; i++)
		writel(sym[i], jpgd->regs + JPGD_REG_HACSYMTABLE + 4 * i);

	return 0;
}

/*
 * The block wants its tables in raster order while a DQT segment carries them in
 * zigzag scan order. A selector that names no table falls back the way the
 * reference does: a stream with no quantiser tables at all decodes against the
 * Annex K pair, otherwise chroma follows the component above it.
 */
static const u8 *histb_jpgd_quant_for(const struct v4l2_jpeg_header *hdr,
				      unsigned int c, const u8 *prev)
{
	unsigned int sel = hdr->frame.component[c].quantization_table_selector;

	if (sel < V4L2_JPEG_MAX_TABLES && hdr->quantization_tables[sel].start)
		return hdr->quantization_tables[sel].start;
	if (c == 0)
		return v4l2_jpeg_ref_table_luma_qt;
	if (prev == v4l2_jpeg_ref_table_luma_qt)
		return v4l2_jpeg_ref_table_chroma_qt;

	return prev;
}

static void histb_jpgd_write_quant(struct histb_jpgd *jpgd,
				   const struct v4l2_jpeg_header *hdr)
{
	const u8 *src[3];
	unsigned int i, c;

	for (c = 0; c < 3; c++)
		src[c] = histb_jpgd_quant_for(hdr, c, c ? src[c - 1] : NULL);

	for (i = 0; i < JPGD_COEFFS; i++) {
		u32 word = 0;

		for (c = 0; c < 3; c++)
			word |= src[c][v4l2_jpeg_zigzag_scan_index[i]] << (8 * c);
		writel(word, jpgd->regs + JPGD_REG_QUANT + 4 * i);
	}
}

/*
 * The block holds one Huffman slot per class, so both chroma components have to
 * name the same tables. Where a stream carries no DHT at all the Annex K tables
 * stand in, as they do in the reference path.
 */
/* The expanded tables and their derivations are more than a stack frame holds. */
struct histb_jpgd_htabs {
	struct histb_jpgd_htab dc[2];
	struct histb_jpgd_htab ac[2];
	u32 sym[JPGD_MAX_HTAB_SYMS];
	u32 min[2][JPGD_MAX_CODE_LEN];
	u32 base[2][JPGD_MAX_CODE_LEN];
};

static int histb_jpgd_write_huff(struct histb_jpgd *jpgd,
				 const struct v4l2_jpeg_header *hdr)
{
	const struct v4l2_jpeg_reference *h = hdr->huffman_tables;
	const struct v4l2_jpeg_scan_header *scan = hdr->scan;
	struct histb_jpgd_htabs *tabs __free(kfree) = kzalloc_obj(*tabs);
	unsigned int i;
	int ret;

	if (!tabs)
		return -ENOMEM;

	if (scan->num_components < 3)
		return -EINVAL;
	if (scan->component[1].dc_entropy_coding_table_selector !=
	    scan->component[2].dc_entropy_coding_table_selector ||
	    scan->component[1].ac_entropy_coding_table_selector !=
	    scan->component[2].ac_entropy_coding_table_selector)
		return -EINVAL;

	for (i = 0; i < 2; i++) {
		unsigned int dc_sel = scan->component[i].dc_entropy_coding_table_selector;
		unsigned int ac_sel = scan->component[i].ac_entropy_coding_table_selector;

		ret = histb_jpgd_htab_load(&tabs->dc[i],
					   dc_sel < 2 ? &h[dc_sel] : NULL);
		if (ret)
			ret = histb_jpgd_htab_ref(&tabs->dc[i],
					i ? v4l2_jpeg_ref_table_chroma_dc_ht :
					      v4l2_jpeg_ref_table_luma_dc_ht,
					V4L2_JPEG_REF_HT_DC_LEN);
		if (ret)
			return ret;

		ret = histb_jpgd_htab_load(&tabs->ac[i],
					   ac_sel < 2 ? &h[2 + ac_sel] : NULL);
		if (ret)
			ret = histb_jpgd_htab_ref(&tabs->ac[i],
					i ? v4l2_jpeg_ref_table_chroma_ac_ht :
					      v4l2_jpeg_ref_table_luma_ac_ht,
					V4L2_JPEG_REF_HT_AC_LEN);
		if (ret)
			return ret;
	}

	ret = histb_jpgd_write_dc(jpgd, &tabs->dc[0], &tabs->dc[1]);
	if (ret)
		return ret;

	return histb_jpgd_write_ac(jpgd, &tabs->ac[0], &tabs->ac[1], tabs->sym,
				   tabs->min, tabs->base);
}

/* ------------------------------------------------------------------ */
/* Hardware control                                                    */
/* ------------------------------------------------------------------ */

/*
 * Pulse the block's own reset line, as the reference does before a session. The
 * wait on the busy flag keeps the reference's 1 ms bound and is not a reset
 * proof: this block was seen to leave START bit 1 set even after a decode that
 * finished and wrote its picture, so the flag says nothing about the reset.
 */
static int histb_jpgd_reset(struct histb_jpgd *jpgd)
{
	u32 val;
	int ret;

	writel(JPGD_INTMASK_ALL, jpgd->regs + JPGD_REG_INTMASK);

	ret = reset_control_assert(jpgd->rst);
	if (ret)
		return ret;

	(void)readl_relaxed_poll_timeout(jpgd->regs + JPGD_REG_START, val,
					 !(val & JPGD_START_BUSY), 10,
					 JPGD_RESET_BUDGET_US);

	ret = reset_control_deassert(jpgd->rst);
	if (ret)
		return ret;

	/* The reference settles a full millisecond after releasing the block. */
	udelay(1000);
	writel(JPGD_INTMASK_RUN, jpgd->regs + JPGD_REG_INTMASK);

	return 0;
}

static void histb_jpgd_program(struct histb_jpgd *jpgd,
			       const struct histb_jpgd_q *q,
			       const struct histb_jpgd_pic *pic, u32 dri,
			       dma_addr_t stream_dma, size_t payload,
			       size_t ecs_offset, dma_addr_t y_dma,
			       dma_addr_t uv_dma)
{
	dma_addr_t data = stream_dma + JPGD_STREAM_MARGIN;
	dma_addr_t end = data + payload;

	if (jpgd->chip->has_mmu_bypass)
		writel(JPGD_MMU_BYPASS_ALL, jpgd->regs + JPGD_REG_MMU_BYPASS);
	writel(dri, jpgd->regs + JPGD_REG_DRI);

	writel(FIELD_PREP(JPGD_PICSIZE_X, pic->mcu_x) |
	       FIELD_PREP(JPGD_PICSIZE_Y, pic->mcu_y),
	       jpgd->regs + JPGD_REG_PICSIZE);
	writel(pic->pic_type, jpgd->regs + JPGD_REG_PICTYPE);
	writel(JPGD_SCALE_1TO1, jpgd->regs + JPGD_REG_SCALE);
	writel(FIELD_PREP(JPGD_STRIDE_Y, q->luma_stride) |
	       FIELD_PREP(JPGD_STRIDE_CBCR, q->chroma_stride),
	       jpgd->regs + JPGD_REG_STRIDE);
	writel((pic->factor[0] << 16) | (pic->factor[1] << 8) | pic->factor[2],
	       jpgd->regs + JPGD_REG_SAMPLINGFACTOR);
	writel(JPGD_OUTTYPE_YUV420SP, jpgd->regs + JPGD_REG_OUTTYPE);

	/*
	 * The window may be wider than the data: the block reads backwards from
	 * the entropy start and forwards past the picture end, so both have to
	 * stay inside the mapping this driver owns.
	 */
	writel(stream_dma, jpgd->regs + JPGD_REG_STADDR);
	writel(end + JPGD_STREAM_MARGIN, jpgd->regs + JPGD_REG_ENDADDR);
	writel(data + ecs_offset, jpgd->regs + JPGD_REG_STADD);
	writel(end, jpgd->regs + JPGD_REG_ENDADD);

	writel(y_dma, jpgd->regs + JPGD_REG_YSTADDR);
	writel(uv_dma, jpgd->regs + JPGD_REG_UVSTADDR);

	writel(JPGD_RESUME_RESTART, jpgd->regs + JPGD_REG_RESUME);
	writel(JPGD_START_RUN, jpgd->regs + JPGD_REG_START);
}

static irqreturn_t histb_jpgd_irq(int irq, void *dev_id)
{
	struct histb_jpgd *jpgd = dev_id;
	u32 state = readl(jpgd->regs + JPGD_REG_INT);

	if (!(state & JPGD_INT_JOB))
		return IRQ_NONE;

	/* Writing the latched bits back clears them and drops the level line. */
	writel(state, jpgd->regs + JPGD_REG_INT);
	jpgd->int_state = state;
	WRITE_ONCE(jpgd->int_done, true);
	wake_up(&jpgd->done);

	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Decode job                                                          */
/* ------------------------------------------------------------------ */

static int histb_jpgd_stream_get(struct histb_jpgd_ctx *ctx, size_t payload)
{
	size_t want = round_up(payload + 2 * JPGD_STREAM_MARGIN, PAGE_SIZE);
	void *mem;
	dma_addr_t dma;

	if (ctx->stream_alloc >= want)
		return 0;

	if (ctx->stream)
		dma_free_coherent(ctx->jpgd->dev, ctx->stream_alloc, ctx->stream,
				  ctx->stream_dma);
	ctx->stream = NULL;
	ctx->stream_alloc = 0;

	mem = dma_alloc_coherent(ctx->jpgd->dev, want, &dma, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	ctx->stream = mem;
	ctx->stream_dma = dma;
	ctx->stream_alloc = want;

	return 0;
}

/* What the block holds after a run; reachable through dynamic debug. */
static void histb_jpgd_dump(struct histb_jpgd *jpgd, const struct histb_jpgd_q *q,
			    const struct histb_jpgd_pic *pic,
			    dma_addr_t stream_dma, size_t payload,
			    size_t ecs_offset, dma_addr_t y_dma)
{
	void __iomem *r = jpgd->regs;

	dev_dbg(jpgd->dev, "regs: type %#x scale %#x mcu %u/%u stride %u/%u out %#x samp %#x bypass %#x dri %#x\n",
		 readl(r + JPGD_REG_PICTYPE), readl(r + JPGD_REG_SCALE),
		 (unsigned int)FIELD_GET(JPGD_PICSIZE_X,
					readl(r + JPGD_REG_PICSIZE)),
		 (unsigned int)FIELD_GET(JPGD_PICSIZE_Y,
					readl(r + JPGD_REG_PICSIZE)),
		 (unsigned int)FIELD_GET(JPGD_STRIDE_Y,
					 readl(r + JPGD_REG_STRIDE)),
		 (unsigned int)FIELD_GET(JPGD_STRIDE_CBCR,
					 readl(r + JPGD_REG_STRIDE)),
		 readl(r + JPGD_REG_OUTTYPE), readl(r + JPGD_REG_SAMPLINGFACTOR),
		 readl(r + JPGD_REG_MMU_BYPASS), readl(r + JPGD_REG_DRI));
	dev_dbg(jpgd->dev, "addr: st %#x en %#x data %#x endd %#x y %#x uv %#x (want st %#x data %#x y %#x)\n",
		 readl(r + JPGD_REG_STADDR), readl(r + JPGD_REG_ENDADDR),
		 readl(r + JPGD_REG_STADD), readl(r + JPGD_REG_ENDADD),
		 readl(r + JPGD_REG_YSTADDR), readl(r + JPGD_REG_UVSTADDR),
		 (u32)stream_dma, (u32)(stream_dma + JPGD_STREAM_MARGIN +
					ecs_offset), (u32)y_dma);
	dev_dbg(jpgd->dev, "want: type %#x mcu %u/%u stride %u/%u samp %02x %02x %02x\n",
		 pic->pic_type, pic->mcu_x, pic->mcu_y, q->luma_stride,
		 q->chroma_stride, pic->factor[0], pic->factor[1],
		 pic->factor[2]);
	dev_dbg(jpgd->dev, "act: int %#x start %#x vld %#x time %#x axi4k %#x axi16m %#x dbg %#x size %zu payload %zu\n",
		 readl(r + JPGD_REG_INT), readl(r + JPGD_REG_START),
		 readl(r + JPGD_REG_PICVLDNUM), readl(r + JPGD_REG_TIME),
		 readl(r + JPGD_REG_AXI4KCNT), readl(r + JPGD_REG_AXI16MCNT),
		 readl(r + JPGD_REG_DEBUG), q->size, payload);
}

/* A refusal travels with the job, so it is tellable by name from the log. */
static int histb_jpgd_no(struct histb_jpgd_ctx *ctx, const char *why)
{
	ctx->why = why;
	return -EINVAL;
}

static int histb_jpgd_run(struct histb_jpgd_ctx *ctx, struct vb2_buffer *src,
			  struct vb2_buffer *dst)
{
	struct histb_jpgd *jpgd = ctx->jpgd;
	struct v4l2_jpeg_header hdr;
	struct v4l2_jpeg_scan_header scan;
	struct v4l2_jpeg_reference qtbls[V4L2_JPEG_MAX_TABLES];
	struct v4l2_jpeg_reference htbls[V4L2_JPEG_MAX_TABLES];
	struct histb_jpgd_q q;
	struct histb_jpgd_pic pic;
	unsigned int payload = vb2_get_plane_payload(src, 0);
	dma_addr_t dst_dma = vb2_dma_contig_plane_dma_addr(dst, 0);
	bool timed_out;
	u32 state;
	int ret;

	if (payload < 2 || payload > JPGD_MAX_STREAM_SIZE)
		return histb_jpgd_no(ctx, "stream length");

	ret = histb_jpgd_stream_get(ctx, payload);
	if (ret) {
		ctx->why = "stream buffer";
		return ret;
	}

	memcpy(ctx->stream + JPGD_STREAM_MARGIN, vb2_plane_vaddr(src, 0),
	       payload);

	memset(&hdr, 0, sizeof(hdr));
	memset(&scan, 0, sizeof(scan));
	memset(qtbls, 0, sizeof(qtbls));
	memset(htbls, 0, sizeof(htbls));
	hdr.scan = &scan;
	hdr.quantization_tables = qtbls;
	hdr.huffman_tables = htbls;

	/*
	 * Baseline DCT only: the parser rejects progressive and arithmetic scans,
	 * and 12-bit samples have no table slot in this block. A stream with more
	 * than one scan is not detectable here, so it decodes as its first one.
	 */
	ret = v4l2_jpeg_parse_header(ctx->stream + JPGD_STREAM_MARGIN, payload,
				     &hdr);
	if (ret) {
		ctx->why = "header parse";
		return ret;
	}

	if (hdr.frame.precision != 8 || hdr.frame.width > JPGD_MAX_DIM ||
	    hdr.frame.height > JPGD_MAX_DIM)
		return histb_jpgd_no(ctx, "precision or size");

	histb_jpgd_pic_of(&hdr, &pic);
	if (pic.pic_type == JPGD_PIC_UNSUPPORTED)
		return histb_jpgd_no(ctx, "subsampling");

	/*
	 * The client stated the picture size, so a header that disagrees with it
	 * would be a different picture than the buffer describes.
	 */
	if (hdr.frame.width != ctx->q.width || hdr.frame.height != ctx->q.height)
		return histb_jpgd_no(ctx, "picture size");

	/*
	 * The block's row rounding follows the stream's own luma factor, and the
	 * chroma plane it writes is half that rounded height. S_FMT reserved the
	 * worst case, so a shorter MCU only has to stay inside it.
	 */
	histb_jpgd_geom(hdr.frame.width, hdr.frame.height,
			hdr.frame.component[0].vertical_sampling_factor, &q);
	if (!histb_jpgd_fits(&q, &pic) || q.luma_stride != ctx->q.luma_stride ||
	    q.chroma_offset > ctx->q.chroma_offset ||
	    q.size > dst->planes[0].length) {
		dev_dbg(jpgd->dev, "picture %ux%u needs %u of %u bytes\n",
			 q.width, q.height, q.size,
			 dst->planes[0].length);
		return histb_jpgd_no(ctx, "geometry");
	}

	histb_jpgd_write_quant(jpgd, &hdr);
	ret = histb_jpgd_write_huff(jpgd, &hdr);
	if (ret) {
		ctx->why = "huffman tables";
		return ret;
	}

	jpgd->int_state = 0;
	WRITE_ONCE(jpgd->int_done, false);
	histb_jpgd_program(jpgd, &q, &pic, hdr.restart_interval,
			   ctx->stream_dma, payload, hdr.ecs_offset, dst_dma,
			   dst_dma + ctx->q.chroma_offset);

	timed_out = !wait_event_timeout(jpgd->done, READ_ONCE(jpgd->int_done),
					JPGD_JOB_TIMEOUT);
	histb_jpgd_dump(jpgd, &q, &pic, ctx->stream_dma, payload,
			hdr.ecs_offset, dst_dma);
	if (timed_out) {
		state = readl(jpgd->regs + JPGD_REG_START);
		dev_warn(jpgd->dev, "decode timed out, START %#x\n", state);
		histb_jpgd_reset(jpgd);
		return -ETIMEDOUT;
	}

	if (jpgd->int_state & JPGD_INT_ERROR) {
		dev_warn(jpgd->dev, "block reported an error (INT %#x)\n",
			 jpgd->int_state);
		histb_jpgd_reset(jpgd);
		return -EIO;
	}

	if (!(jpgd->int_state & JPGD_INT_FINISH)) {
		dev_warn(jpgd->dev, "unexpected interrupt (INT %#x)\n",
			 jpgd->int_state);
		return -EIO;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* mem2mem                                                             */
/* ------------------------------------------------------------------ */

static void histb_jpgd_device_run(void *priv)
{
	struct histb_jpgd_ctx *ctx = priv;
	struct histb_jpgd *jpgd = ctx->jpgd;
	struct vb2_v4l2_buffer *src, *dst;
	enum vb2_buffer_state state;
	int ret;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!src || !dst) {
		v4l2_m2m_job_finish(jpgd->m2m_dev, ctx->fh.m2m_ctx);
		return;
	}

	v4l2_m2m_buf_copy_metadata(src, dst);
	/* This driver never holds a CAPTURE buffer across jobs. */
	src->flags &= ~V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;

	mutex_lock(&jpgd->hw_lock);
	ret = histb_jpgd_run(ctx, &src->vb2_buf, &dst->vb2_buf);
	mutex_unlock(&jpgd->hw_lock);

	if (ret)
		dev_dbg(jpgd->dev, "job refused: %d (%s)\n", ret,
			ctx->why ? : "unspecified");
	ctx->why = NULL;

	state = ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE;
	v4l2_m2m_buf_done_and_job_finish(jpgd->m2m_dev, ctx->fh.m2m_ctx, state);
}

/*
 * No job_abort(): device_run() is synchronous and leaves both buffers on the
 * ready lists until it completes, so an abort request from VIDIOC_STREAMOFF or
 * close only has to wait for the job already in flight to finish itself.
 */
static const struct v4l2_m2m_ops histb_jpgd_m2m_ops = {
	.device_run = histb_jpgd_device_run,
};

/* ------------------------------------------------------------------ */
/* vb2                                                                 */
/* ------------------------------------------------------------------ */

static int histb_jpgd_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
				  unsigned int *num_planes, unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct histb_jpgd_ctx *ctx = vq->drv_priv;
	unsigned int want;

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		want = ctx->out_size;
	} else if (ctx->q.size) {
		want = ctx->q.size;
	} else {
		/* The client has to say what picture it wants before buffers can
		 * be sized for it.
		 */
		return -EINVAL;
	}

	if (*num_planes)
		return sizes[0] < want ? -EINVAL : 0;

	*num_planes = 1;
	sizes[0] = want;

	return 0;
}

static int histb_jpgd_buf_prepare(struct vb2_buffer *vb)
{
	struct histb_jpgd_ctx *ctx = vb->vb2_queue->drv_priv;

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		if (!vb2_get_plane_payload(vb, 0) ||
		    vb2_get_plane_payload(vb, 0) > vb->planes[0].length)
			return -EINVAL;
		return 0;
	}

	/* A CAPTURE buffer is always filled completely by the block. */
	if (vb->planes[0].length < ctx->q.size)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, ctx->q.size);

	return 0;
}

static void histb_jpgd_buf_queue(struct vb2_buffer *vb)
{
	struct histb_jpgd_ctx *ctx = vb->vb2_queue->drv_priv;

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int histb_jpgd_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	return 0;
}

static void histb_jpgd_stop_streaming(struct vb2_queue *vq)
{
	struct histb_jpgd_ctx *ctx = vq->drv_priv;
	struct vb2_v4l2_buffer *buf;

	do {
		buf = V4L2_TYPE_IS_OUTPUT(vq->type) ?
		      v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
		      v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (buf)
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
	} while (buf);
}

static const struct vb2_ops histb_jpgd_vb2_ops = {
	.queue_setup = histb_jpgd_queue_setup,
	.buf_prepare = histb_jpgd_buf_prepare,
	.buf_queue = histb_jpgd_buf_queue,
	.start_streaming = histb_jpgd_start_streaming,
	.stop_streaming = histb_jpgd_stop_streaming,
};

/* ------------------------------------------------------------------ */
/* Format ioctls                                                       */
/* ------------------------------------------------------------------ */

static int histb_jpgd_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	struct histb_jpgd *jpgd = video_get_drvdata(video_devdata(file));

	strscpy(cap->driver, HISTB_JPGD_NAME, sizeof(cap->driver));
	strscpy(cap->card, HISTB_JPGD_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(jpgd->dev));

	return 0;
}

static int histb_jpgd_enum_fmt_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = HISTB_JPGD_CAP_FMT;

	return 0;
}

static int histb_jpgd_enum_fmt_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_JPEG;

	return 0;
}

static int histb_jpgd_try_out(struct v4l2_pix_format *pix)
{
	if (pix->pixelformat != V4L2_PIX_FMT_JPEG)
		return -EINVAL;

	pix->width = 0;
	pix->height = 0;
	pix->bytesperline = 0;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;
	/* A client that knows its picture size asks for it; 4 MB is the default. */
	pix->sizeimage = clamp(pix->sizeimage ? : JPGD_DEF_STREAM_SIZE, 2U,
			       (unsigned int)JPGD_MAX_STREAM_SIZE);

	return 0;
}

static int histb_jpgd_cap_geom(struct v4l2_pix_format *pix,
			       struct histb_jpgd_q *q)
{
	if (pix->pixelformat != HISTB_JPGD_CAP_FMT)
		return -EINVAL;

	/*
	 * The header is not available at S_FMT time, so the client states the
	 * picture size it parsed for itself; 0 is refused rather than guessed at.
	 */
	if (!pix->width || !pix->height)
		return -EINVAL;
	if (pix->width > JPGD_MAX_DIM || pix->height > JPGD_MAX_DIM)
		return -EINVAL;

	histb_jpgd_geom(pix->width, pix->height, JPGD_MAX_LUMA_V, q);
	if (q->luma_stride > U16_MAX || q->chroma_stride > U16_MAX)
		return -EINVAL;

	/*
	 * The plane is reported by its coded height, which also puts the chroma
	 * where the single-planar NV21 rule says it is: one bytesperline and a
	 * height in.
	 */
	pix->height = q->coded_height;
	pix->bytesperline = q->luma_stride;
	pix->sizeimage = q->size;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;

	return 0;
}

static int histb_jpgd_g_fmt(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct histb_jpgd_ctx *ctx = file_to_ctx(file);
	struct v4l2_pix_format *pix = &f->fmt.pix;

	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		pix->pixelformat = V4L2_PIX_FMT_JPEG;
		pix->width = 0;
		pix->height = 0;
		pix->bytesperline = 0;
		pix->field = V4L2_FIELD_NONE;
		pix->colorspace = V4L2_COLORSPACE_JPEG;
		pix->sizeimage = ctx->out_size;
		return 0;
	}

	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	pix->pixelformat = HISTB_JPGD_CAP_FMT;
	pix->width = ctx->q.width;
	pix->height = ctx->q.height;
	pix->height = ctx->q.coded_height;
	pix->bytesperline = ctx->q.luma_stride;
	pix->sizeimage = ctx->q.size;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;

	return 0;
}

static int histb_jpgd_s_fmt(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	struct histb_jpgd_ctx *ctx = file_to_ctx(file);
	struct histb_jpgd_q q;
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;
	if (vb2_is_busy(vq))
		return -EBUSY;

	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		ret = histb_jpgd_try_out(&f->fmt.pix);
		if (ret)
			return ret;
		ctx->out_size = f->fmt.pix.sizeimage;
		return 0;
	}

	ret = histb_jpgd_cap_geom(&f->fmt.pix, &q);
	if (!ret)
		ctx->q = q;

	return ret;
}

static int histb_jpgd_try_fmt(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct histb_jpgd_q q;

	if (V4L2_TYPE_IS_OUTPUT(f->type))
		return histb_jpgd_try_out(&f->fmt.pix);
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return histb_jpgd_cap_geom(&f->fmt.pix, &q);
}

static const struct v4l2_ioctl_ops histb_jpgd_ioctl_ops = {
	.vidioc_querycap = histb_jpgd_querycap,
	.vidioc_enum_fmt_vid_cap = histb_jpgd_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out = histb_jpgd_enum_fmt_out,
	.vidioc_g_fmt_vid_cap = histb_jpgd_g_fmt,
	.vidioc_g_fmt_vid_out = histb_jpgd_g_fmt,
	.vidioc_s_fmt_vid_cap = histb_jpgd_s_fmt,
	.vidioc_s_fmt_vid_out = histb_jpgd_s_fmt,
	.vidioc_try_fmt_vid_cap = histb_jpgd_try_fmt,
	.vidioc_try_fmt_vid_out = histb_jpgd_try_fmt,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};

/* ------------------------------------------------------------------ */
/* File handles and device                                             */
/* ------------------------------------------------------------------ */

static int histb_jpgd_queue_init(void *priv, struct vb2_queue *src_vq,
				 struct vb2_queue *dst_vq);

static int histb_jpgd_open(struct file *file)
{
	struct histb_jpgd *jpgd = video_drvdata(file);
	struct histb_jpgd_ctx *ctx;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	ctx->jpgd = jpgd;
	ctx->out_size = JPGD_DEF_STREAM_SIZE;

	v4l2_fh_init(&ctx->fh, video_devdata(file));

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(jpgd->m2m_dev, ctx,
					    histb_jpgd_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);
		return ret;
	}

	v4l2_fh_add(&ctx->fh, file);

	return 0;
}

static int histb_jpgd_release(struct file *file)
{
	struct v4l2_fh *fh = file->private_data;
	struct histb_jpgd_ctx *ctx = container_of(fh, struct histb_jpgd_ctx, fh);

	v4l2_fh_del(fh, file);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_exit(fh);
	if (ctx->stream)
		dma_free_coherent(ctx->jpgd->dev, ctx->stream_alloc, ctx->stream,
				  ctx->stream_dma);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations histb_jpgd_fops = {
	.owner = THIS_MODULE,
	.open = histb_jpgd_open,
	.release = histb_jpgd_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int histb_jpgd_queue_init(void *priv, struct vb2_queue *src_vq,
				 struct vb2_queue *dst_vq)
{
	struct histb_jpgd_ctx *ctx = priv;
	unsigned int i;
	struct vb2_queue *vq[2];
	int ret;

	/* Both queues hold a full picture per buffer, so nothing is shared. */
	vq[0] = src_vq;
	vq[1] = dst_vq;
	for (i = 0; i < ARRAY_SIZE(vq); i++) {
		vq[i]->io_modes = VB2_MMAP | VB2_DMABUF;
		vq[i]->drv_priv = ctx;
		vq[i]->ops = &histb_jpgd_vb2_ops;
		vq[i]->mem_ops = &vb2_dma_contig_memops;
		vq[i]->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		vq[i]->min_queued_buffers = 1;
		vq[i]->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		vq[i]->lock = &ctx->jpgd->vdev_lock;
		vq[i]->dev = ctx->jpgd->dev;
		/*
		 * The bitstream is parsed in the kernel, so the OUTPUT plane needs
		 * a kernel mapping; the decoded picture is only ever read back by
		 * the client, so the CAPTURE plane keeps one too for VIDIOC_EXPORTBUF
		 * and mmap bookkeeping.
		 */
		vq[i]->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES;
	}
	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	return vb2_queue_init(dst_vq);
}

static const struct histb_jpgd_chip histb_jpgd_chip_mv100 = {
	.has_mmu_bypass = true,
};

static const struct histb_jpgd_chip histb_jpgd_chip_cv200 = {
	.has_mmu_bypass = false,
};

static const struct of_device_id histb_jpgd_of_match[] = {
	{
		.compatible = "hisilicon,hi3798mv100-jpgd",
		.data = &histb_jpgd_chip_mv100,
	}, {
		.compatible = "hisilicon,hi3716cv200-jpgd",
		.data = &histb_jpgd_chip_cv200,
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, histb_jpgd_of_match);

static int histb_jpgd_probe(struct platform_device *pdev)
{
	struct histb_jpgd *jpgd;
	struct video_device *vdev;
	int ret, irq;

	jpgd = devm_kzalloc(&pdev->dev, sizeof(*jpgd), GFP_KERNEL);
	if (!jpgd)
		return -ENOMEM;

	jpgd->dev = &pdev->dev;
	jpgd->chip = of_device_get_match_data(&pdev->dev);
	if (!jpgd->chip)
		return dev_err_probe(&pdev->dev, -ENODEV, "unknown device\n");
	platform_set_drvdata(pdev, jpgd);
	mutex_init(&jpgd->hw_lock);
	mutex_init(&jpgd->vdev_lock);
	init_waitqueue_head(&jpgd->done);

	jpgd->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(jpgd->regs))
		return PTR_ERR(jpgd->regs);

	jpgd->clk = devm_clk_get(&pdev->dev, "jpgd");
	if (IS_ERR(jpgd->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(jpgd->clk),
				     "no clock\n");

	ret = clk_prepare_enable(jpgd->clk);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "cannot enable clock\n");

	jpgd->rst = devm_reset_control_get_exclusive(&pdev->dev, "jpgd");
	if (IS_ERR(jpgd->rst)) {
		ret = PTR_ERR(jpgd->rst);
		goto err_clk;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_clk;
	}

	/*
	 * Every buffer this block uses has to be physically contiguous, so bind
	 * the region named by "memory-region" before the first allocation. A DTB
	 * that draws no such pool is not an error (-ENODEV): the device keeps
	 * using the default area.
	 */
	ret = devm_of_reserved_mem_device_init(&pdev->dev);
	if (ret && ret != -ENODEV) {
		dev_err(&pdev->dev, "failed to bind reserved memory\n");
		goto err_clk;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "failed to set DMA mask\n");
		goto err_clk;
	}
	vb2_dma_contig_set_max_seg_size(&pdev->dev, DMA_BIT_MASK(32));

	ret = v4l2_device_register(&pdev->dev, &jpgd->v4l2_dev);
	if (ret)
		goto err_clk;

	jpgd->m2m_dev = v4l2_m2m_init(&histb_jpgd_m2m_ops);
	if (IS_ERR(jpgd->m2m_dev)) {
		ret = PTR_ERR(jpgd->m2m_dev);
		v4l2_err(&jpgd->v4l2_dev, "failed to init mem2mem device\n");
		goto err_v4l2;
	}

	vdev = video_device_alloc();
	if (!vdev) {
		ret = -ENOMEM;
		goto err_m2m;
	}
	jpgd->vdev = vdev;
	vdev->v4l2_dev = &jpgd->v4l2_dev;
	vdev->fops = &histb_jpgd_fops;
	vdev->ioctl_ops = &histb_jpgd_ioctl_ops;
	vdev->lock = &jpgd->vdev_lock;
	vdev->release = video_device_release;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	strscpy(vdev->name, HISTB_JPGD_NAME, sizeof(vdev->name));
	video_set_drvdata(vdev, jpgd);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&jpgd->v4l2_dev, "failed to register video device\n");
		goto err_vdev;
	}

	/*
	 * The block may come up mid-job from the bootloader: reset it, wait for it
	 * to idle, mask the low-delay source and only then take the line over.
	 */
	ret = histb_jpgd_reset(jpgd);
	if (ret)
		goto err_unregister;

	ret = devm_request_irq(&pdev->dev, irq, histb_jpgd_irq, 0,
			       HISTB_JPGD_NAME, jpgd);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq\n");
		goto err_assert;
	}

	return 0;

err_assert:
	reset_control_assert(jpgd->rst);
err_unregister:
	video_unregister_device(jpgd->vdev);
	jpgd->vdev = NULL;
	/*
	 * video_unregister_device() already drops the last reference, so the
	 * release callback frees the node here; video_device_release() below
	 * is only for a registration that never got that far.
	 */
	goto err_m2m;
err_vdev:
	video_device_release(vdev);
err_m2m:
	v4l2_m2m_release(jpgd->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&jpgd->v4l2_dev);
err_clk:
	clk_disable_unprepare(jpgd->clk);
	return ret;
}

static void histb_jpgd_remove(struct platform_device *pdev)
{
	struct histb_jpgd *jpgd = platform_get_drvdata(pdev);

	video_unregister_device(jpgd->vdev);
	v4l2_m2m_release(jpgd->m2m_dev);
	v4l2_device_unregister(&jpgd->v4l2_dev);
	reset_control_assert(jpgd->rst);
	clk_disable_unprepare(jpgd->clk);
}

static struct platform_driver histb_jpgd_driver = {
	.probe = histb_jpgd_probe,
	.remove = histb_jpgd_remove,
	.driver = {
		.name = HISTB_JPGD_NAME,
		.of_match_table = histb_jpgd_of_match,
	},
};
module_platform_driver(histb_jpgd_driver);

MODULE_DESCRIPTION("HiSilicon Hi3798MV100 JPEG decoder driver");
MODULE_LICENSE("GPL");
