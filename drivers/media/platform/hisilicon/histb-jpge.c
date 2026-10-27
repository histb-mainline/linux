// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * JPEG encoder (JPGE) for HiSilicon Hi3798MV100.
 *
 * One raw picture per mem2mem job. The block entropy-codes a planar 4:2:0 frame
 * into the stream buffer the driver gives it, starting exactly at the
 * programmed address and ending with its own end-of-image marker padded out to
 * 64 bytes, and the driver writes the JFIF header the block does not produce;
 * the two pieces are copied out of the driver's scratch into the client's
 * CAPTURE buffer so that the result is one contiguous JPEG.
 *
 * The block also keeps a pointer block 32 bytes below the stream address; its
 * first word is the byte count the picture used. The vendor driver instead
 * expects a 64-byte frame header at the stream address, which this version of
 * the block does not write - measured on the board, and the reason the sizes
 * here come from the write pointer and the terminating marker rather than from
 * that header.
 *
 * The picture is copied into the scratch rather than coded in place: a client's
 * buffer is cached memory and this block is not cache-coherent, so a job against
 * it can code bytes the CPU has not written out yet. The decoder side keeps its
 * bitstream in coherent memory for the same reason.
 *
 * The block has no Huffman or frame-description registers: its coder is built
 * around the standard Annex K tables, so the header has to advertise exactly
 * those tables and the quantiser file uploaded to JPGE_QPT has to be the scaled
 * form of the same constants.
 *
 * The client feeds semi-planar NV12 or NV21 and the driver de-interleaves it into
 * two half-width planes, because that is the shape the reference's own copy
 * routine builds for this block. Measured on the board, the block does not
 * distinguish the layouts: feeding the interleaved frame as-is and setting
 * strFmt to semi-planar gives a byte-identical stream.
 *
 * The register layout and the programming order come from the vendor driver's
 * live path (msp/drv/jpge/src/jpge_reg.h and jpge_hal.c:364), which is built for
 * this chip but never shipped as a module, so it has never been the product's
 * own encode path.
 *
 * What the board shows, and where it stops. The block probes (version "V2R4"),
 * resets, accepts a format and a quality, codes every job, raises the
 * end-of-picture interrupt, reports its length through the write pointer below
 * the stream address, and its luma path is sound: a per-macroblock-coded picture
 * decodes through a reference decoder at its own codes with no level shift (flat
 * luma 128 comes back as 128.0). The JFIF header is byte-for-byte the
 * reference's own template and the quantiser upload matches its walk, table slots
 * included.
 *
 * Colour does not come out. Chroma is being consumed - a picture whose chroma
 * varies within each 8x8 block costs 5782 bytes where the same luma with
 * block-constant chroma costs 2346, and exchanging the two components changes
 * the bytes - but the entropy data does not fit the structure the header
 * declares. Three exhaustive searches on the captured streams came back empty:
 * every assignment of the Huffman table ids the header carries, every order of
 * the three components in the scan, and every sampling triple for
 * (luma, chroma) over {(1,1),(2,1),(2,2),(1,2)}; the data runs out at 1190 to
 * 1590 blocks where 4:2:0 over this picture requires 2376, stopping on a genuine
 * end-of-image marker each time. Neither strFmt, packageSel nor SrcCStride moves
 * the output by a single byte, so the difference is not in how the planes are
 * described to the block. What it codes for chroma, and with what structure, is
 * still unknown, and until it is known the deliverable is a luma coder with
 * unreadable chroma - not a usable JPEG encoder.
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
#include <linux/unaligned.h>
#include <linux/videodev2.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-jpeg.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define HISTB_JPGE_NAME		"histb-jpge"

/*
 * Semi-planar in from the client, planar to the block: the driver splits the
 * interleaved pairs into Cb and Cr planes in its own scratch, because that is the
 * one input shape the reference's copy routine produces for this block (Y at full
 * height, then two half-height planes) and the only one whose chroma the coder
 * can be shown to read.
 */
static const u32 histb_jpge_out_fmts[] = {
	V4L2_PIX_FMT_NV12,
	V4L2_PIX_FMT_NV21,
};

/* ------------------------------------------------------------------ */
/* Registers (msp/drv/jpge/src/jpge_reg.h, accumulated by member)      */
/* ------------------------------------------------------------------ */

#define JPGE_INTSTAT		0x000
#define  JPGE_INT_EOP		BIT(0)
#define  JPGE_INT_BUF_FULL	BIT(1)
#define  JPGE_INT_TIMEOUT	BIT(2)
#define  JPGE_INT_CFG_ERR	BIT(3)
#define  JPGE_INT_JOB		(JPGE_INT_EOP | JPGE_INT_BUF_FULL)

#define JPGE_INTMASK		0x004	/* a set bit enables that source */
#define JPGE_RAWINT		0x008
#define JPGE_INTCLR		0x00c	/* write 1 to clear */

#define JPGE_PICFG		0x010
#define  JPGE_PICFG_STR_FMT	GENMASK(1, 0)
#define  JPGE_PICFG_STR_SEMIPLANAR 0	/* hi_jpge_type.h: 0 semi, 1 planar, 2 pack */
#define  JPGE_PICFG_STR_PLANAR	1	/* hi_jpge_type.h: 0 semi, 1 planar, 2 package */
#define  JPGE_PICFG_YUV_FMT	GENMASK(3, 2)
#define  JPGE_PICFG_YUV_420	0	/* hi_jpge_type.h: 0 420, 1 422, 2 444 */
#define  JPGE_PICFG_ROTATION	GENMASK(13, 12)
#define  JPGE_PICFG_CLK_GATE	GENMASK(15, 14)
#define  JPGE_PICFG_MEM_GATE	BIT(16)
#define  JPGE_PICFG_PKG_SEL	GENMASK(27, 20)
#define  JPGE_PICFG_CLIP_EN	BIT(28)
/*
 * packageSel is the block's input pixel-layout selector, and the reference's own
 * table names the values (msp/drv/venc/venc_v1.0/drv_omxvenc.c:126-142 used by
 * msp/api/omx/omx_venc/omx_venc.c:1870): 0 is semi-planar with V before U, 1 is
 * semi-planar with U before V, and the 141/120/216 values are the packed 4:2:2
 * layouts. The reference's JPEG path hardcodes 216 (Y0 U Y1 V) because it is fed
 * packed frames from the encoder side; feeding it semi-planar bytes with that
 * value makes the block run a colour conversion over them, which is what the
 * first board measurement of this driver produced: a flat chroma plane and a
 * shifted luma level.
 */
#define  JPGE_PKG_V_U		0	/* V before U */
#define  JPGE_PKG_U_V		1	/* U before V */

#define JPGE_ECSCFG		0x014	/* restart-interval/slice config; unused */

#define JPGE_START		0x01c
#define  JPGE_START_RUN		BIT(0)

#define JPGE_VEDIMGSIZE		0x020
#define  JPGE_IMG_WIDTH		GENMASK(12, 0)
#define  JPGE_IMG_HEIGHT	GENMASK(28, 16)

#define JPGE_SRCYADDR		0x030
#define JPGE_SRCCADDR		0x034
#define JPGE_SRCVADDR		0x038

#define JPGE_SSTRIDE		0x03c
#define  JPGE_SSTRIDE_Y		GENMASK(15, 0)
#define  JPGE_SSTRIDE_C		GENMASK(31, 16)

#define JPGE_STRMADDR		0x040
#define JPGE_SRPTRADDR		0x044
#define JPGE_SWPTRADDR		0x048

/*
 * The room the block may write, in whole bytes: the low six bits of the register
 * are not implemented, which is why the reference writes its byte size into the
 * word raw (jpge_hal.c:441). Dividing that size by 64 instead makes the block
 * report its buffer full on the first picture - measured: 152064 written as
 * 0x940 refused a job that 152064 written as itself encoded.
 */
#define JPGE_STRMBUFLEN		0x04c
#define  JPGE_STRMBUFLEN_UNIT	GENMASK(31, 6)	/* whole bytes, low 6 unused */

#define JPGE_CLIP_THR		0x024	/* jcpi clip window: luma and chroma
					 * min/max, behind PICFG's clip enable
					 */

#define JPGE_PTS0		0x070
#define JPGE_OUTSTD		0x0a4
#define  JPGE_OUTSTD_VAL	2	/* the reference's fixed value */

#define JPGE_VERSION		0x0fc	/* "V2R4" on this silicon */
#define JPGE_QPT		0x1000	/* 48 words: three tables of 16 */


#define JPGE_QPT_WORDS		16	/* words per quantiser table */
#define JPGE_REG_WINDOW		0x10c0	/* sizeof(S_JPGE_REGS_TYPE) */

/* ------------------------------------------------------------------ */
/* JFIF header, assembled from the tables the block's coder assumes     */
/* ------------------------------------------------------------------ */

#define JPGE_SOI_BYTES		2
#define JPGE_APP0_BYTES		18
#define JPGE_DQT_BYTES		(2 + 2 + 1 + 64)
#define JPGE_DHT_DC_BYTES	(2 + 2 + 1 + 16 + 12)
#define JPGE_DHT_AC_BYTES	(2 + 2 + 1 + 16 + 162)
#define JPGE_SOF0_BYTES		(2 + 2 + 1 + 2 + 2 + 1 + 3 * 3)
#define JPGE_SOS_BYTES		(2 + 2 + 1 + 3 * 2 + 3)

#define JPGE_HDR_BYTES		(JPGE_SOI_BYTES + JPGE_APP0_BYTES + \
				 3 * JPGE_DQT_BYTES + JPGE_SOF0_BYTES + \
				 2 * JPGE_DHT_DC_BYTES + 2 * JPGE_DHT_AC_BYTES + \
				 JPGE_SOS_BYTES)

/* ------------------------------------------------------------------ */
/* Block limits                                                        */
/* ------------------------------------------------------------------ */

#define JPGE_MIN_DIM		16	/* the reference gate; also one MCU */
#define JPGE_MAX_DIM		4096	/* the reference gate; the field is 13 bits */
#define JPGE_ALIGN_FRAME	16	/* plane addresses and strides, in bytes */
#define JPGE_ALIGN_MCU		16	/* a 4:2:0 MCU is 16x16 luma pixels */
#define JPGE_ALIGN_STREAM	64	/* STRMADDR granularity */
#define JPGE_INFO_BYTES		64	/* what the block itself writes at STRMADDR
					 * before the entropy data: the used window
					 * rounded to this granularity, the padding,
					 * and a constant
					 */
#define JPGE_PTR_BYTES		32	/* the block's own pointer block, below
					 * STRMADDR: the write pointer is the first
					 * word there and says how many bytes of the
					 * window the picture used, padding included
					 */
#define JPGE_ALIGN_BUFLEN	256	/* buffer sizes are kept whole multiples
					 * of this, so the length register never
					 * has to round a byte count off
					 */
#define JPGE_QUALITY_DEFAULT	50
/*
 * A bitstream buffer is bounded above by what a contiguous allocation can be
 * expected to give on this box, and the default is the raw picture size, which
 * is comfortably above what the coder produces at ordinary quality. A client
 * that asks for less gets the refusal it can act on.
 */
#define JPGE_MIN_STREAM_SIZE	SZ_1K
#define JPGE_MAX_STREAM_SIZE	SZ_8M
#define JPGE_JOB_TIMEOUT	msecs_to_jiffies(2000)
#define JPGE_SETTLE_US		10	/* the reference's own wait around reset */

/*
 * struct histb_jpge - one JPGE instance
 * @hw_lock: the block has no channels of its own, so a job holds it from the
 *	     first write to its completion; that also leaves @int_state and
 *	     @int_done to one writer and one reader
 */
/*
 * Bring-up overrides for the input-format register. -1 leaves a field as the
 * driver computes it; picfg_set and picfg_clr force individual bits of the
 * finished word, which is how chnId, roundEn, timeoutEn, configlockEn and the
 * clip enable get swept without a rebuild per arm. The clip window itself is
 * only written when clip_set is non-zero, because the block ignores it while the
 * enable is clear and its reset value is unknown here.
 */
static int yuvfmt = -1;
module_param(yuvfmt, int, 0644);
MODULE_PARM_DESC(yuvfmt, "override PICFG.yuvFmt: -1 as driver, 0 4:2:0, 1 4:2:2, 2 4:4:4");

static int strfmt = -1;
module_param(strfmt, int, 0644);
MODULE_PARM_DESC(strfmt, "override PICFG.strFmt: -1 as driver, 0 semi, 1 planar, 2 packed");

static int pkgsel = -1;
module_param(pkgsel, int, 0644);
MODULE_PARM_DESC(pkgsel, "override PICFG.packageSel: -1 as driver, else 0..255");

static uint picfg_set;
module_param(picfg_set, uint, 0644);
MODULE_PARM_DESC(picfg_set, "bits forced to 1 in PICFG");

static uint picfg_clr = 0xe0000000;
module_param(picfg_clr, uint, 0644);
MODULE_PARM_DESC(picfg_clr, "bits forced to 0 in PICFG (default: the reserved top three)");

static uint clip_thr = 0xf010eb10;
module_param(clip_thr, uint, 0644);
MODULE_PARM_DESC(clip_thr, "clip window written with clip_set: luma 16..235, chroma 16..240");

static uint semi;
module_param(semi, uint, 0644);
MODULE_PARM_DESC(semi, "1: feed the block the client's semi-planar frame as-is");

static int cstride = -1;
module_param(cstride, int, 0644);
MODULE_PARM_DESC(cstride, "chroma row stride: -1 = half the luma stride, else bytes");

/*
 * SRC_V address arm. -1 leaves it as the driver computes, which is 0 for
 * semi-planar (the vendor precondition) and the Cr plane address for planar;
 * 0 forces zero, 1 forces the address, so the old behaviour stays reachable
 * in the same build.
 */
static int srcv = -1;
module_param(srcv, int, 0644);
MODULE_PARM_DESC(srcv, "SRC_V arm: -1 driver (0 semi, Cr planar), 0 force zero, 1 force address");

/* 0 keeps the SOS component list at 1,2,3; 1 reorders it to 1,3,2. */
static int sosorder;
module_param(sosorder, int, 0644);
MODULE_PARM_DESC(sosorder, "SOS component list: 0 = 1,2,3, 1 = 1,3,2 (Cr first)");

static uint clip_set;
module_param(clip_set, uint, 0644);
MODULE_PARM_DESC(clip_set, "1: write clip_thr and set the clip enable bit");

struct histb_jpge {
	struct device		*dev;
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
 * struct histb_jpge_q - one queue's layout as client and block see it
 *
 * For the OUTPUT queue it is the raw picture (and @chroma_offset is where the
 * interleaved chroma pair plane starts inside the single buffer); for CAPTURE
 * @size is the largest bitstream the client will accept.
 */
struct histb_jpge_q {
	u32	fourcc;
	unsigned int width;
	unsigned int height;
	unsigned int bytesperline;
	unsigned int chroma_offset;
	unsigned int size;
};

struct histb_jpge_ctx {
	struct histb_jpge	*jpge;
	struct v4l2_fh		fh;
	struct v4l2_ctrl_handler ctrl_hdl;
	struct histb_jpge_q	out_q;
	struct histb_jpge_q	cap_q;
	struct v4l2_ctrl	*quality;
	const char		*why;
	void			*stream;
	dma_addr_t		stream_dma;
	size_t			stream_alloc;
};

/* v4l2-ioctl passes no file handle to the format ops, only to the file. */
static struct histb_jpge_ctx *file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct histb_jpge_ctx, fh);
}

/* ------------------------------------------------------------------ */
/* Quantiser tables                                                    */
/* ------------------------------------------------------------------ */

/* The IJG scaling the reference applies to the Annex K tables. */
static void histb_jpge_scale_qt(u8 *dst, const u8 *base, unsigned int quality)
{
	unsigned int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
	unsigned int i;

	for (i = 0; i < V4L2_JPEG_PIXELS_IN_BLOCK; i++)
		dst[i] = clamp((base[i] * scale + 50) / 100, 1, 255);
}

/*
 * The quantiser file holds four values per word, and the reference walks the
 * raster-ordered table a quarter-row at a time: word 2c carries column c of
 * rows 0..3 and word 2c + 1 the same column of rows 4..7. Luma goes to table 0,
 * and the same chroma table is loaded twice more because the scan picks AC table
 * 1 for Cb and table 2 for Cr.
 */
static void histb_jpge_write_qpt(struct histb_jpge *jpge, const u8 *lum,
				 const u8 *chr)
{
	unsigned int w;

	for (w = 0; w < JPGE_QPT_WORDS; w++) {
		unsigned int i = (w & 1) * 32 + (w >> 1);

		writel(lum[i] | (lum[i + 8] << 8) |
		       (lum[i + 16] << 16) | (lum[i + 24] << 24),
		       jpge->regs + JPGE_QPT + 4 * w);
		writel(chr[i] | (chr[i + 8] << 8) |
		       (chr[i + 16] << 16) | (chr[i + 24] << 24),
		       jpge->regs + JPGE_QPT + 4 * (w + 16));
		writel(chr[i] | (chr[i + 8] << 8) |
		       (chr[i + 16] << 16) | (chr[i + 24] << 24),
		       jpge->regs + JPGE_QPT + 4 * (w + 32));
	}
}

/* ------------------------------------------------------------------ */
/* JFIF header writer                                                  */
/* ------------------------------------------------------------------ */

static void *jp_u8(void *p, u8 v)
{
	*(u8 *)p = v;
	return (u8 *)p + 1;
}

static void *jp_be16(void *p, u16 v)
{
	p = jp_u8(p, v >> 8);
	return jp_u8(p, v);
}

static void *jp_seg(void *p, u8 marker, const void *body, size_t len)
{
	p = jp_u8(p, 0xff);
	p = jp_u8(p, marker);
	p = jp_be16(p, len + 2);
	memcpy(p, body, len);

	return (u8 *)p + len;
}

static void *jp_dqt(void *p, u8 id, const u8 *tab)
{
	u8 body[1 + V4L2_JPEG_PIXELS_IN_BLOCK], zig[V4L2_JPEG_PIXELS_IN_BLOCK];
	unsigned int i;

	/* A DQT payload carries the table in zigzag scan order. */
	for (i = 0; i < V4L2_JPEG_PIXELS_IN_BLOCK; i++)
		zig[i] = tab[v4l2_jpeg_zigzag_scan_index[i]];
	body[0] = id;
	memcpy(body + 1, zig, sizeof(zig));

	return jp_seg(p, 0xdb, body, sizeof(body));
}

static void *jp_dht(void *p, u8 class, u8 id, const u8 *tab, size_t len)
{
	u8 body[1 + 16 + 162];

	BUILD_BUG_ON(1 + 16 + 162 < V4L2_JPEG_REF_HT_AC_LEN + 1);
	body[0] = (class << 4) | id;
	memcpy(body + 1, tab, len);

	return jp_seg(p, 0xc4, body, 1 + len);
}

/*
 * histb_jpge_write_hdr() - lay out the header the block does not produce
 *
 * Every field is the baseline, 8-bit, three-component 4:2:0 form the entropy
 * coder is hard-wired to; the table ids have to agree with what the scan names
 * and with the order the quantiser file was loaded in.
 */
static size_t histb_jpge_write_hdr(void *buf, const struct histb_jpge_q *pic,
				   const u8 *lum_qt, const u8 *chr_qt)
{
	static const u8 app0[14] = {
		'J', 'F', 'I', 'F', 0,
		1, 1,			/* JFIF 1.1 */
		0,			/* no density units claimed */
		0x00, 0x01, 0x00, 0x01,	/* 1x1 density */
		0, 0,			/* no thumbnail */
	};
	static const u8 sof_body[1 + 2 + 2 + 1 + 3 * 3] = {
		8, 0, 0, 0, 0, 3,
		1, 0x22, 0,
		2, 0x11, 1,
		3, 0x11, 2,
	};
	static const u8 sos_body[1 + 3 * 2 + 3] = {
		3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0,
	};
	static const u8 sos_cr_first[1 + 3 * 2 + 3] = {
		3, 1, 0x00, 3, 0x11, 2, 0x11, 0, 63, 0,
	};
	u8 *p = buf;

	p = jp_u8(p, 0xff);
	p = jp_u8(p, 0xd8);
	p = jp_seg(p, 0xe0, app0, sizeof(app0));

	p = jp_dqt(p, 0, lum_qt);
	p = jp_dqt(p, 1, chr_qt);
	p = jp_dqt(p, 2, chr_qt);

	{
		u8 body[sizeof(sof_body)];

		memcpy(body, sof_body, sizeof(body));
		body[1] = pic->height >> 8;
		body[2] = pic->height;
		body[3] = pic->width >> 8;
		body[4] = pic->width;
		p = jp_seg(p, 0xc0, body, sizeof(body));
	}

	p = jp_dht(p, 0, 0, v4l2_jpeg_ref_table_luma_dc_ht,
		   V4L2_JPEG_REF_HT_DC_LEN);
	p = jp_dht(p, 0, 1, v4l2_jpeg_ref_table_chroma_dc_ht,
		   V4L2_JPEG_REF_HT_DC_LEN);
	p = jp_dht(p, 1, 0, v4l2_jpeg_ref_table_luma_ac_ht,
		   V4L2_JPEG_REF_HT_AC_LEN);
	p = jp_dht(p, 1, 1, v4l2_jpeg_ref_table_chroma_ac_ht,
		   V4L2_JPEG_REF_HT_AC_LEN);
	p = jp_seg(p, 0xda, sosorder ? sos_cr_first : sos_body,
		 sizeof(sos_body));

	if (WARN_ON((u8 *)p - (u8 *)buf != JPGE_HDR_BYTES))
		return 0;

	return JPGE_HDR_BYTES;
}

/* ------------------------------------------------------------------ */
/* Hardware control                                                    */
/* ------------------------------------------------------------------ */

/*
 * The reference only ever releases this block from reset; the assert is the one
 * addition, because the block is found held in reset by the bootloader and a job
 * must not start against an unknown state. The version register is polled
 * afterwards only as a "the window answers" check: it reads back "V2R4" once the
 * gate is on and zero while the clock is off.
 */
static int histb_jpge_reset(struct histb_jpge *jpge)
{
	u32 val;
	int ret;

	writel(0, jpge->regs + JPGE_INTMASK);

	ret = reset_control_assert(jpge->rst);
	if (ret)
		return ret;

	udelay(JPGE_SETTLE_US);
	writel(JPGE_INT_JOB, jpge->regs + JPGE_INTCLR);

	ret = reset_control_deassert(jpge->rst);
	if (ret)
		return ret;

	udelay(JPGE_SETTLE_US);

	writel(JPGE_INT_JOB, jpge->regs + JPGE_INTMASK);

	/*
	 * The version register is only read as a "does the window answer" check: a
	 * zero here means the gate or the reset is not what the driver believes.
	 */
	if (readl_relaxed_poll_timeout(jpge->regs + JPGE_VERSION, val, val, 10,
				       1000))
		dev_warn(jpge->dev, "version register reads 0\n");

	return 0;
}

static u32 histb_jpge_pkg_sel(u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_NV21 ? JPGE_PKG_V_U : JPGE_PKG_U_V;
}

/*
 * DIAGNOSTIC: the reference's register struct leaves 0x100 to 0x4fc as one
 * reserved block. Whether the block keeps state - or a chroma address - in there
 * is only answerable by looking.
 */
static void histb_jpge_dump_window(struct histb_jpge *jpge, const char *when)
{
	unsigned int off;

	for (off = 0x100; off < 0x500; off += 0x20)
		dev_dbg(jpge->dev, "%s: %#04x %08x %08x %08x %08x\n", when, off,
			 readl(jpge->regs + off), readl(jpge->regs + off + 4),
			 readl(jpge->regs + off + 8), readl(jpge->regs + off + 12));
	for (off = 0x500; off < 0x600; off += 0x20)
		dev_dbg(jpge->dev, "%s: %#04x %08x %08x %08x %08x\n", when, off,
			 readl(jpge->regs + off), readl(jpge->regs + off + 4),
			 readl(jpge->regs + off + 8), readl(jpge->regs + off + 12));
}

static void histb_jpge_program(struct histb_jpge *jpge,
			       const struct histb_jpge_q *pic, dma_addr_t y,
			       dma_addr_t c, dma_addr_t v, dma_addr_t strm, u32 buf_len)
{
	u32 picfg;

	picfg = FIELD_PREP(JPGE_PICFG_STR_FMT,
			   strfmt >= 0 ? strfmt :
			   semi ? JPGE_PICFG_STR_SEMIPLANAR :
			   JPGE_PICFG_STR_PLANAR) |
		FIELD_PREP(JPGE_PICFG_YUV_FMT,
			   yuvfmt < 0 ? JPGE_PICFG_YUV_420 : yuvfmt) |
		FIELD_PREP(JPGE_PICFG_CLK_GATE, 2) |
		JPGE_PICFG_MEM_GATE |
		FIELD_PREP(JPGE_PICFG_PKG_SEL,
			   pkgsel < 0 ? histb_jpge_pkg_sel(pic->fourcc) : pkgsel);
	picfg = (picfg & ~picfg_clr) | (picfg_set & picfg_clr);

	if (clip_set) {
		writel(clip_thr, jpge->regs + JPGE_CLIP_THR);
		picfg |= JPGE_PICFG_CLIP_EN;
	}

	writel(picfg, jpge->regs + JPGE_PICFG);
	writel(0, jpge->regs + JPGE_ECSCFG);
	writel(FIELD_PREP(JPGE_IMG_WIDTH, pic->width - 1) |
	       FIELD_PREP(JPGE_IMG_HEIGHT, pic->height - 1),
	       jpge->regs + JPGE_VEDIMGSIZE);

	writel(y, jpge->regs + JPGE_SRCYADDR);
	writel(c, jpge->regs + JPGE_SRCCADDR);
	writel(v, jpge->regs + JPGE_SRCVADDR);
	writel(FIELD_PREP(JPGE_SSTRIDE_Y, pic->bytesperline) |
	       FIELD_PREP(JPGE_SSTRIDE_C,
			  cstride >= 0 ? cstride :
			  semi ? pic->bytesperline : pic->bytesperline / 2),
	       jpge->regs + JPGE_SSTRIDE);

	writel(strm, jpge->regs + JPGE_STRMADDR);
	writel(strm - 16, jpge->regs + JPGE_SRPTRADDR);
	writel(strm - 32, jpge->regs + JPGE_SWPTRADDR);
	writel(buf_len & JPGE_STRMBUFLEN_UNIT, jpge->regs + JPGE_STRMBUFLEN);

	writel(0, jpge->regs + JPGE_PTS0);
	writel(JPGE_OUTSTD_VAL, jpge->regs + JPGE_OUTSTD);

	writel(0, jpge->regs + JPGE_START);
	writel(JPGE_START_RUN, jpge->regs + JPGE_START);
}

static irqreturn_t histb_jpge_irq(int irq, void *dev_id)
{
	struct histb_jpge *jpge = dev_id;
	u32 state = readl(jpge->regs + JPGE_INTSTAT);

	if (!(state & JPGE_INT_JOB))
		return IRQ_NONE;

	writel(state, jpge->regs + JPGE_INTCLR);
	jpge->int_state = state;
	WRITE_ONCE(jpge->int_done, true);
	wake_up(&jpge->done);

	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Encode job                                                          */
/* ------------------------------------------------------------------ */

/* A refusal travels with the job, so it is tellable by name from the log. */
static int histb_jpge_no(struct histb_jpge_ctx *ctx, const char *why)
{
	ctx->why = why;
	return -EINVAL;
}

static int histb_jpge_stream_get(struct histb_jpge_ctx *ctx, size_t want)
{
	size_t need = round_up(want, PAGE_SIZE);
	void *mem;
	dma_addr_t dma;

	if (ctx->stream_alloc >= need)
		return 0;

	if (ctx->stream)
		dma_free_coherent(ctx->jpge->dev, ctx->stream_alloc, ctx->stream,
				  ctx->stream_dma);
	ctx->stream = NULL;
	ctx->stream_alloc = 0;

	mem = dma_alloc_coherent(ctx->jpge->dev, need, &dma, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	ctx->stream = mem;
	ctx->stream_dma = dma;
	ctx->stream_alloc = need;

	return 0;
}

/*
 * The client's interleaved chroma pair plane becomes the two half-height planes
 * the block is programmed with. Which byte of the pair is Cb is the format's
 * meaning, so NV21 has V first.
 */
static void histb_jpge_split_chroma(u8 *luma, u8 *cb, u8 *cr, unsigned int bpl,
				  unsigned int height, const u8 *in, u32 fourcc)
{
	unsigned int x, y, rows = height / 2, pair = fourcc == V4L2_PIX_FMT_NV21;

	for (y = 0; y < height; y++)
		memcpy(luma + bpl * y, in + bpl * y, bpl);
	for (y = 0; y < rows; y++) {
		const u8 *p = in + bpl * height + bpl * y;

		for (x = 0; x < bpl; x += 2) {
			cb[bpl / 2 * y + x / 2] = p[x + pair];
			cr[bpl / 2 * y + x / 2] = p[x + !pair];
		}
	}
}

static int histb_jpge_run(struct histb_jpge_ctx *ctx, struct vb2_buffer *src,
			  struct vb2_buffer *dst)
{
	struct histb_jpge *jpge = ctx->jpge;
	u8 lum_qt[V4L2_JPEG_PIXELS_IN_BLOCK], chr_qt[V4L2_JPEG_PIXELS_IN_BLOCK];
	unsigned int pic_off = ALIGN(JPGE_HDR_BYTES, JPGE_ALIGN_FRAME);
	unsigned int luma = ctx->out_q.bytesperline * ctx->out_q.height;
	unsigned int cb_off = pic_off + luma, cr_off = cb_off + luma / 4;
	size_t prefix = cr_off + luma / 4 + JPGE_PTR_BYTES;
	size_t hdr, strm_off, buf_len, scan, total, end;
	dma_addr_t base;
	dma_addr_t src_pic;
	dma_addr_t vaddr;
	u8 *mem, *in;
	u32 wp;
	bool timed_out;
	int ret;

	if (vb2_get_plane_payload(src, 0) < ctx->out_q.size)
		return histb_jpge_no(ctx, "picture size");

	histb_jpge_scale_qt(lum_qt, v4l2_jpeg_ref_table_luma_qt,
			    ctx->quality->val);
	histb_jpge_scale_qt(chr_qt, v4l2_jpeg_ref_table_chroma_qt,
			    ctx->quality->val);

	/*
	 * STRMADDR has to be 64-byte aligned, and it sits after the header and the
	 * copied picture with a whole alignment quantum to spare, so that the block's
	 * pointer block - the 32 bytes below STRMADDR - cannot land on anything the
	 * driver or the picture needs. The length register is the room from STRMADDR,
	 * rounded down to the 256-byte granularity the reference uses.
	 */
	buf_len = round_down(ctx->cap_q.size, JPGE_ALIGN_BUFLEN);
	if (buf_len <= JPGE_ALIGN_STREAM)
		return histb_jpge_no(ctx, "capture size");

	ret = histb_jpge_stream_get(ctx, ALIGN(prefix, JPGE_ALIGN_STREAM) +
				   JPGE_ALIGN_STREAM + buf_len);
	if (ret) {
		ctx->why = "stream buffer";
		return ret;
	}

	mem = ctx->stream;
	base = ctx->stream_dma;
	if (base & (JPGE_ALIGN_FRAME - 1))
		return histb_jpge_no(ctx, "stream alignment");

	strm_off = ALIGN(base + prefix + JPGE_ALIGN_STREAM, JPGE_ALIGN_STREAM) - base;
	src_pic = base + pic_off;
	in = vb2_plane_vaddr(src, 0);

	if (semi)
		memcpy(mem + pic_off, in, luma + luma / 2);
	else
		histb_jpge_split_chroma(mem + pic_off, mem + cb_off, mem + cr_off,
					ctx->out_q.bytesperline, ctx->out_q.height,
					in, ctx->out_q.fourcc);
	hdr = histb_jpge_write_hdr(mem, &ctx->out_q, lum_qt, chr_qt);
	if (!hdr)
		return histb_jpge_no(ctx, "header build");
	histb_jpge_write_qpt(jpge, lum_qt, chr_qt);

	jpge->int_state = 0;
	WRITE_ONCE(jpge->int_done, false);
	/*
	 * Semi-planar has no third plane: the vendor HAL rejects the job when
	 * strFmt is semi and SRC_V is not 0 (jpge_hal.c:1058), so the address
	 * partway into the interleaved UV plane this driver used to hand over
	 * is not a chroma plane at all.
	 */
	if (srcv > 0)
		vaddr = base + cr_off;
	else if (srcv == 0)
		vaddr = 0;
	else
		vaddr = semi ? 0 : base + cr_off;
	histb_jpge_program(jpge, &ctx->out_q, src_pic, base + cb_off, vaddr,
			   base + strm_off, buf_len);
	dev_dbg(jpge->dev, "want: y %#x cb %#x v %#x strm %#x len %zu q %u\n",
		(u32)src_pic, (u32)(base + cb_off), (u32)vaddr,
		(u32)(base + strm_off), buf_len, ctx->quality->val);
	dev_dbg(jpge->dev, "act:  STRMADDR %#x STRMBUFLEN %#x PICFG %#x\n",
		readl(jpge->regs + JPGE_STRMADDR),
		readl(jpge->regs + JPGE_STRMBUFLEN),
		readl(jpge->regs + JPGE_PICFG));
	dev_dbg(jpge->dev, "act:  SRCY %#x SRCC %#x SRCV %#x STRIDE %#x SIZE %#x\n",
		readl(jpge->regs + JPGE_SRCYADDR),
		readl(jpge->regs + JPGE_SRCCADDR),
		readl(jpge->regs + JPGE_SRCVADDR),
		readl(jpge->regs + JPGE_SSTRIDE),
		readl(jpge->regs + JPGE_VEDIMGSIZE));

	timed_out = !wait_event_timeout(jpge->done, READ_ONCE(jpge->int_done),
					JPGE_JOB_TIMEOUT);
	if (timed_out) {
		dev_warn(jpge->dev, "encode timed out (RAWINT %#x START %#x)\n",
			 readl(jpge->regs + JPGE_RAWINT),
			 readl(jpge->regs + JPGE_START));
		histb_jpge_reset(jpge);
		return -ETIMEDOUT;
	}

	if (jpge->int_state & JPGE_INT_BUF_FULL) {
		dev_info(jpge->dev, "stream full: capture %u is too small\n",
			 ctx->cap_q.size);
		histb_jpge_reset(jpge);
		return -ENOSPC;
	}

	if (!(jpge->int_state & JPGE_INT_EOP)) {
		dev_warn(jpge->dev, "no end of picture (INT %#x)\n",
			 jpge->int_state);
		histb_jpge_reset(jpge);
		return -EIO;
	}

	/*
	 * The block's own report is the write pointer below the stream address: how
	 * many bytes of the window the picture used, rounded up to its 64-byte
	 * granularity and filled with zeros. The picture proper ends at its
	 * end-of-image marker, which is where the stream handed to the client ends.
	 */
	wp = get_unaligned_le32(mem + strm_off - JPGE_PTR_BYTES);
	if (!wp || wp > buf_len) {
		ctx->why = "write pointer";
		dev_warn(jpge->dev, "write pointer %#x of window %zu\n", wp,
			 buf_len);
		histb_jpge_reset(jpge);
		return -EIO;
	}

	end = wp;
	while (end && mem[strm_off + end - 1] == 0x00)
		end--;
	if (end < JPGE_INFO_BYTES + 2 || mem[strm_off + end - 2] != 0xff ||
	    mem[strm_off + end - 1] != 0xd9 || wp - end >= JPGE_ALIGN_STREAM) {
		ctx->why = "no end of image";
		dev_warn(jpge->dev, "no terminating marker in %u bytes\n", wp);
		histb_jpge_reset(jpge);
		return -EIO;
	}

	scan = end - JPGE_INFO_BYTES;
	total = hdr + scan;
	if (total > dst->planes[0].length)
		return histb_jpge_no(ctx, "capture buffer");

	memcpy(vb2_plane_vaddr(dst, 0), mem, hdr);
	memcpy(vb2_plane_vaddr(dst, 0) + hdr, mem + strm_off + JPGE_INFO_BYTES,
	       scan);
	vb2_set_plane_payload(dst, 0, total);
	dev_dbg(jpge->dev, "encoded %ux%u -> %zu bytes (scan %zu of %u used)\n",
		ctx->out_q.width, ctx->out_q.height, total, scan, wp);
	histb_jpge_dump_window(jpge, "post-job");

	return 0;
}

/* ------------------------------------------------------------------ */
/* mem2mem                                                             */
/* ------------------------------------------------------------------ */

static void histb_jpge_device_run(void *priv)
{
	struct histb_jpge_ctx *ctx = priv;
	struct histb_jpge *jpge = ctx->jpge;
	struct vb2_v4l2_buffer *src, *dst;
	enum vb2_buffer_state state;
	int ret;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!src || !dst) {
		v4l2_m2m_job_finish(jpge->m2m_dev, ctx->fh.m2m_ctx);
		return;
	}

	v4l2_m2m_buf_copy_metadata(src, dst);
	/* This driver never holds a CAPTURE buffer across jobs. */
	src->flags &= ~V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;

	mutex_lock(&jpge->hw_lock);
	ret = histb_jpge_run(ctx, &src->vb2_buf, &dst->vb2_buf);
	mutex_unlock(&jpge->hw_lock);

	if (ret)
		dev_dbg(jpge->dev, "job refused: %d (%s)\n", ret,
			ctx->why ? : "unspecified");
	ctx->why = NULL;

	state = ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE;
	v4l2_m2m_buf_done_and_job_finish(jpge->m2m_dev, ctx->fh.m2m_ctx, state);
}

/*
 * No job_abort(): device_run() is synchronous and leaves both buffers on the
 * ready lists until it completes, so an abort request from VIDIOC_STREAMOFF or
 * close only has to wait for the job already in flight to finish itself.
 */
static const struct v4l2_m2m_ops histb_jpge_m2m_ops = {
	.device_run = histb_jpge_device_run,
};

/* ------------------------------------------------------------------ */
/* vb2                                                                 */
/* ------------------------------------------------------------------ */

static int histb_jpge_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
				  unsigned int *num_planes, unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct histb_jpge_ctx *ctx = vq->drv_priv;
	const struct histb_jpge_q *q = V4L2_TYPE_IS_OUTPUT(vq->type) ?
				       &ctx->out_q : &ctx->cap_q;

	/* Both queues describe themselves, so a buffer cannot be sized before
	 * the format is set.
	 */
	if (!q->size)
		return -EINVAL;

	if (*num_planes)
		return sizes[0] < q->size ? -EINVAL : 0;

	*num_planes = 1;
	sizes[0] = q->size;

	return 0;
}

static int histb_jpge_buf_prepare(struct vb2_buffer *vb)
{
	struct histb_jpge_ctx *ctx = vb->vb2_queue->drv_priv;

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		if (vb2_get_plane_payload(vb, 0) < ctx->out_q.size ||
		    vb2_get_plane_payload(vb, 0) > vb->planes[0].length)
			return -EINVAL;
		return 0;
	}

	if (vb->planes[0].length < ctx->cap_q.size)
		return -EINVAL;
	/* The payload says how short the bitstream really was, once it is out. */
	vb2_set_plane_payload(vb, 0, 0);

	return 0;
}

static void histb_jpge_buf_queue(struct vb2_buffer *vb)
{
	struct histb_jpge_ctx *ctx = vb->vb2_queue->drv_priv;

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int histb_jpge_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	return 0;
}

static void histb_jpge_stop_streaming(struct vb2_queue *vq)
{
	struct histb_jpge_ctx *ctx = vq->drv_priv;
	struct vb2_v4l2_buffer *buf;

	do {
		buf = V4L2_TYPE_IS_OUTPUT(vq->type) ?
		      v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
		      v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (buf)
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
	} while (buf);
}

static const struct vb2_ops histb_jpge_vb2_ops = {
	.queue_setup = histb_jpge_queue_setup,
	.buf_prepare = histb_jpge_buf_prepare,
	.buf_queue = histb_jpge_buf_queue,
	.start_streaming = histb_jpge_start_streaming,
	.stop_streaming = histb_jpge_stop_streaming,
};

/* ------------------------------------------------------------------ */
/* Format ioctls                                                       */
/* ------------------------------------------------------------------ */

static int histb_jpge_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	struct histb_jpge *jpge = video_get_drvdata(video_devdata(file));

	strscpy(cap->driver, HISTB_JPGE_NAME, sizeof(cap->driver));
	strscpy(cap->card, HISTB_JPGE_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(jpge->dev));

	return 0;
}

static int histb_jpge_enum_fmt_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_JPEG;

	return 0;
}

static int histb_jpge_enum_fmt_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(histb_jpge_out_fmts))
		return -EINVAL;

	f->pixelformat = histb_jpge_out_fmts[f->index];

	return 0;
}

static bool histb_jpge_is_out_fmt(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(histb_jpge_out_fmts); i++)
		if (histb_jpge_out_fmts[i] == fourcc)
			return true;

	return false;
}

/*
 * The coder works in whole 16x16 MCUs, so a size that is not a multiple of one
 * is rounded down to the largest one it can code; the strides and plane
 * addresses the reference API asks for are 16-byte items, and both stride
 * fields are 16 bits wide.
 */
static int histb_jpge_out_geom(struct v4l2_pix_format *pix,
			       struct histb_jpge_q *q)
{
	unsigned int width = round_down(pix->width, JPGE_ALIGN_MCU);
	unsigned int height = round_down(pix->height, JPGE_ALIGN_MCU);
	unsigned int bpl;

	if (!histb_jpge_is_out_fmt(pix->pixelformat))
		return -EINVAL;
	if (width < JPGE_MIN_DIM || width > JPGE_MAX_DIM ||
	    height < JPGE_MIN_DIM || height > JPGE_MAX_DIM)
		return -EINVAL;

	bpl = max(width, round_up(pix->bytesperline ? : width, JPGE_ALIGN_FRAME));
	if (bpl > U16_MAX)
		return -EINVAL;

	q->fourcc = pix->pixelformat;
	q->width = width;
	q->height = height;
	q->bytesperline = bpl;
	q->chroma_offset = bpl * height;
	q->size = q->chroma_offset + bpl * height / 2;

	pix->width = width;
	pix->height = height;
	pix->bytesperline = bpl;
	pix->sizeimage = q->size;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;

	return 0;
}

/*
 * The bitstream length is what the client is willing to accept, and the picture
 * it belongs to is the one the OUTPUT queue describes: nothing here parses a
 * size out of a raw frame, because there is nothing to parse.
 */
static int histb_jpge_cap_geom(const struct histb_jpge_q *out,
			       struct v4l2_pix_format *pix,
			       struct histb_jpge_q *q)
{
	unsigned int size;

	if (pix->pixelformat != V4L2_PIX_FMT_JPEG)
		return -EINVAL;
	if (!out->size)
		return -EINVAL;

	size = clamp(pix->sizeimage ? : out->size,
		     (unsigned int)JPGE_MIN_STREAM_SIZE,
		     (unsigned int)JPGE_MAX_STREAM_SIZE);

	q->fourcc = pix->pixelformat;
	q->width = out->width;
	q->height = out->height;
	q->bytesperline = 0;
	q->chroma_offset = 0;
	q->size = round_up(size, JPGE_ALIGN_BUFLEN);

	pix->width = out->width;
	pix->height = out->height;
	pix->bytesperline = 0;
	pix->sizeimage = q->size;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;

	return 0;
}

static int histb_jpge_g_fmt(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct histb_jpge_ctx *ctx = file_to_ctx(file);
	const struct histb_jpge_q *q = V4L2_TYPE_IS_OUTPUT(f->type) ?
				       &ctx->out_q : &ctx->cap_q;
	struct v4l2_pix_format *pix = &f->fmt.pix;

	if (f->type != V4L2_BUF_TYPE_VIDEO_OUTPUT &&
	    f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	pix->pixelformat = q->fourcc;
	pix->width = q->width;
	pix->height = q->height;
	pix->bytesperline = q->bytesperline;
	pix->sizeimage = q->size;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;

	return 0;
}

static int histb_jpge_s_fmt(struct file *file, void *priv,
			    struct v4l2_format *f)
{
	struct histb_jpge_ctx *ctx = file_to_ctx(file);
	struct histb_jpge_q q;
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;
	if (vb2_is_busy(vq))
		return -EBUSY;

	/*
	 * The two queues are set independently, as on the decoder side: the
	 * picture geometry comes from OUTPUT and the bitstream bound from CAPTURE,
	 * and a CAPTURE format before an OUTPUT one is refused rather than guessed
	 * at.
	 */
	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		struct v4l2_pix_format cap = { 0 };

		ret = histb_jpge_out_geom(&f->fmt.pix, &q);
		if (ret)
			return ret;
		ctx->out_q = q;
		/*
		 * The bitstream queue describes the same picture, so a new geometry
		 * re-derives it; what the client asked for as the bound is carried
		 * over, since that is the one thing not dictated by the picture.
		 */
		cap.pixelformat = V4L2_PIX_FMT_JPEG;
		cap.sizeimage = ctx->cap_q.size;
		return histb_jpge_cap_geom(&ctx->out_q, &cap, &ctx->cap_q);
	}

	ret = histb_jpge_cap_geom(&ctx->out_q, &f->fmt.pix, &q);
	if (!ret)
		ctx->cap_q = q;

	return ret;
}

static int histb_jpge_try_fmt(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	struct histb_jpge_ctx *ctx = file_to_ctx(file);
	struct histb_jpge_q q;

	if (V4L2_TYPE_IS_OUTPUT(f->type))
		return histb_jpge_out_geom(&f->fmt.pix, &q);
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return histb_jpge_cap_geom(&ctx->out_q, &f->fmt.pix, &q);
}

static const struct v4l2_ioctl_ops histb_jpge_ioctl_ops = {
	.vidioc_querycap = histb_jpge_querycap,
	.vidioc_enum_fmt_vid_cap = histb_jpge_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out = histb_jpge_enum_fmt_out,
	.vidioc_g_fmt_vid_cap = histb_jpge_g_fmt,
	.vidioc_g_fmt_vid_out = histb_jpge_g_fmt,
	.vidioc_s_fmt_vid_cap = histb_jpge_s_fmt,
	.vidioc_s_fmt_vid_out = histb_jpge_s_fmt,
	.vidioc_try_fmt_vid_cap = histb_jpge_try_fmt,
	.vidioc_try_fmt_vid_out = histb_jpge_try_fmt,
	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ------------------------------------------------------------------ */
/* File handles and device                                             */
/* ------------------------------------------------------------------ */

static int histb_jpge_queue_init(void *priv, struct vb2_queue *src_vq,
				 struct vb2_queue *dst_vq);

static int histb_jpge_open(struct file *file)
{
	struct histb_jpge *jpge = video_drvdata(file);
	struct histb_jpge_ctx *ctx;
	struct v4l2_ctrl *quality;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	ctx->jpge = jpge;
	/* Both queues have one format each; the geometry is filled in by S_FMT. */
	ctx->out_q.fourcc = histb_jpge_out_fmts[0];
	ctx->cap_q.fourcc = V4L2_PIX_FMT_JPEG;

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, 1);
	/* The block takes the same quality scale as the reference's Qlevel. */
	quality = v4l2_ctrl_new_std(&ctx->ctrl_hdl, NULL,
				    V4L2_CID_JPEG_COMPRESSION_QUALITY, 1, 99, 1,
				    JPGE_QUALITY_DEFAULT);
	if (ctx->ctrl_hdl.error) {
		ret = ctx->ctrl_hdl.error;
		v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);
		return ret;
	}
	ctx->quality = quality;
	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(jpge->m2m_dev, ctx,
					    histb_jpge_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);
		return ret;
	}

	v4l2_fh_add(&ctx->fh, file);

	return 0;
}

static int histb_jpge_release(struct file *file)
{
	struct histb_jpge *jpge = video_drvdata(file);
	struct v4l2_fh *fh = file->private_data;
	struct histb_jpge_ctx *ctx = container_of(fh, struct histb_jpge_ctx, fh);

	v4l2_fh_del(fh, file);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_exit(fh);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	if (ctx->stream)
		dma_free_coherent(jpge->dev, ctx->stream_alloc, ctx->stream,
				  ctx->stream_dma);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations histb_jpge_fops = {
	.owner = THIS_MODULE,
	.open = histb_jpge_open,
	.release = histb_jpge_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int histb_jpge_queue_init(void *priv, struct vb2_queue *src_vq,
				 struct vb2_queue *dst_vq)
{
	struct histb_jpge_ctx *ctx = priv;
	unsigned int i;
	struct vb2_queue *vq[2];
	int ret;

	/* Both queues hold a full picture per buffer, so nothing is shared. */
	vq[0] = src_vq;
	vq[1] = dst_vq;
	for (i = 0; i < ARRAY_SIZE(vq); i++) {
		vq[i]->io_modes = VB2_MMAP | VB2_DMABUF;
		vq[i]->drv_priv = ctx;
		vq[i]->ops = &histb_jpge_vb2_ops;
		vq[i]->mem_ops = &vb2_dma_contig_memops;
		vq[i]->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		vq[i]->min_queued_buffers = 1;
		vq[i]->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		vq[i]->lock = &ctx->jpge->vdev_lock;
		vq[i]->dev = ctx->jpge->dev;
		/*
		 * The header is assembled in the kernel and the finished bitstream
		 * is copied out of the scratch into the CAPTURE plane, so both queues
		 * need a kernel mapping.
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

/* ------------------------------------------------------------------ */
/* Platform device                                                     */
/* ------------------------------------------------------------------ */

static const struct of_device_id histb_jpge_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-jpge" },
	{ .compatible = "hisilicon,hi3716cv200-jpge" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, histb_jpge_of_match);

static int histb_jpge_probe(struct platform_device *pdev)
{
	struct histb_jpge *jpge;
	struct video_device *vdev;
	int ret, irq;

	jpge = devm_kzalloc(&pdev->dev, sizeof(*jpge), GFP_KERNEL);
	if (!jpge)
		return -ENOMEM;

	jpge->dev = &pdev->dev;
	platform_set_drvdata(pdev, jpge);
	mutex_init(&jpge->hw_lock);
	mutex_init(&jpge->vdev_lock);
	init_waitqueue_head(&jpge->done);

	jpge->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(jpge->regs))
		return PTR_ERR(jpge->regs);

	jpge->clk = devm_clk_get(&pdev->dev, "jpge");
	if (IS_ERR(jpge->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(jpge->clk),
				     "no clock\n");

	ret = clk_prepare_enable(jpge->clk);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "cannot enable clock\n");

	jpge->rst = devm_reset_control_get_exclusive(&pdev->dev, "jpge");
	if (IS_ERR(jpge->rst)) {
		ret = PTR_ERR(jpge->rst);
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

	ret = v4l2_device_register(&pdev->dev, &jpge->v4l2_dev);
	if (ret)
		goto err_clk;

	jpge->m2m_dev = v4l2_m2m_init(&histb_jpge_m2m_ops);
	if (IS_ERR(jpge->m2m_dev)) {
		ret = PTR_ERR(jpge->m2m_dev);
		v4l2_err(&jpge->v4l2_dev, "failed to init mem2mem device\n");
		goto err_v4l2;
	}

	vdev = video_device_alloc();
	if (!vdev) {
		ret = -ENOMEM;
		goto err_m2m;
	}
	jpge->vdev = vdev;
	vdev->v4l2_dev = &jpge->v4l2_dev;
	vdev->fops = &histb_jpge_fops;
	vdev->ioctl_ops = &histb_jpge_ioctl_ops;
	vdev->lock = &jpge->vdev_lock;
	vdev->release = video_device_release;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	strscpy(vdev->name, HISTB_JPGE_NAME, sizeof(vdev->name));
	video_set_drvdata(vdev, jpge);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&jpge->v4l2_dev, "failed to register video device\n");
		goto err_vdev;
	}

	ret = histb_jpge_reset(jpge);
	if (ret)
		goto err_unregister;
	histb_jpge_dump_window(jpge, "idle");

	ret = devm_request_irq(&pdev->dev, irq, histb_jpge_irq, 0,
			       HISTB_JPGE_NAME, jpge);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq\n");
		goto err_assert;
	}

	dev_info(&pdev->dev, "JPEG encoder, version %#x\n",
		 readl(jpge->regs + JPGE_VERSION));

	return 0;

err_assert:
	reset_control_assert(jpge->rst);
err_unregister:
	video_unregister_device(jpge->vdev);
	jpge->vdev = NULL;
	/*
	 * video_unregister_device() already drops the last reference, so the
	 * release callback frees the node here; video_device_release() below
	 * is only for a registration that never got that far.
	 */
	goto err_m2m;
err_vdev:
	video_device_release(vdev);
err_m2m:
	v4l2_m2m_release(jpge->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&jpge->v4l2_dev);
err_clk:
	clk_disable_unprepare(jpge->clk);
	return ret;
}

static void histb_jpge_remove(struct platform_device *pdev)
{
	struct histb_jpge *jpge = platform_get_drvdata(pdev);

	video_unregister_device(jpge->vdev);
	v4l2_m2m_release(jpge->m2m_dev);
	v4l2_device_unregister(&jpge->v4l2_dev);
	reset_control_assert(jpge->rst);
	clk_disable_unprepare(jpge->clk);
}

static struct platform_driver histb_jpge_driver = {
	.probe = histb_jpge_probe,
	.remove = histb_jpge_remove,
	.driver = {
		.name = HISTB_JPGE_NAME,
		.of_match_table = histb_jpge_of_match,
	},
};
module_platform_driver(histb_jpge_driver);

MODULE_DESCRIPTION("HiSilicon Hi3798MV100 JPEG encoder driver");
MODULE_LICENSE("GPL");
