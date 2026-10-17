/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * HiSilicon Hi3798MV100 (VDH HiVDHV300R001) stateless video decoder driver.
 *
 * The hardware decoder consumes fully parsed syntax: the userspace submits
 * per-frame parsing results through V4L2 stateless controls, and the driver
 * packs them into the hardware's downstream message pool, programs the VDM
 * registers and rings the doorbell. Frame storage uses a table+index model:
 * capture buffers are registered by index and referenced by id from the
 * message pool. Output frames are 16x16-tiled NV21, exposed here as
 * V4L2_PIX_FMT_NV21_16L16.
 *
 * The register layouts, message slot encodings and buffer geometries recorded
 * below were derived from the vendor decoder firmware and then checked against
 * the hardware.
 */
#ifndef HISTB_VDEC_H
#define HISTB_VDEC_H

#include <linux/delay.h>
#include <linux/platform_device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>

#include "histb_vdec_regs.h"

#define HISTB_VDEC_NAME			"histb-vdec"

/* One downstream message slot: 5 x 64-word bursts. */
#define HISTB_VDEC_SLOT_WORDS		320
#define HISTB_VDEC_SLOT_BYTES		(HISTB_VDEC_SLOT_WORDS * 4)
#define HISTB_VDEC_MAX_SLICE_SLOTS	140
#define HISTB_VDEC_UP_MSG_WORDS		4
#define HISTB_VDEC_MAX_UP_SLICES	200

#define HISTB_VDEC_MAX_DPB_SLOTS	32
#define HISTB_VDEC_MAX_APC_SLOTS	16
#define HISTB_VDEC_MAX_PMV_SLOTS	20
#define HISTB_VDEC_MAX_WIDTH		1920
#define HISTB_VDEC_MAX_HEIGHT		1920

/* Watchdog, matching the fixed timeout the vendor firmware arms here. */
#define HISTB_VDEC_TIMEOUT		msecs_to_jiffies(500)

/* Sizes of the fixed per-protocol lookup tables (extracted from the vendor
 * firmware's constant pools).
 */
#define HISTB_VDEC_CABAC_MN_H264_SIZE	5120
#define HISTB_VDEC_CABAC_MN_HEVC_SIZE	928
/* The tile/segment info image that picture message D55 points at, sized by the
 * reservation the vendor firmware makes for its own copy of the same area
 * (64 * 4 * 128 bytes); growing it from the 2 KB first used here changed no
 * board reading, so this is fidelity to the vendor layout rather than a fix.
 */
#define HISTB_VDEC_TILE_INFO_SIZE	(64 * 4 * 128)
#define HISTB_VDEC_PPFD_SIZE		(64 * 4 * 800)

/* Band-line scratch buffers, sized by the vendor layout: 64 * 4 * width
 * bytes each at 1920 columns. Which of these a protocol actually references
 * is fixed per protocol (see the codec sources).
 */
#define HISTB_VDEC_SCRATCH_SIZE		(64 * 4 * HISTB_VDEC_MAX_WIDTH)
enum histb_vdec_scratch {
	HISTB_SCRATCH_SED_TOP,
	HISTB_SCRATCH_PMV_TOP,
	HISTB_SCRATCH_RCNTOP,
	HISTB_SCRATCH_DBLK_TOP,
	HISTB_SCRATCH_SAO_TOP,
	HISTB_SCRATCH_PMV_LEFT,
	HISTB_SCRATCH_SAO_LEFT,
	HISTB_SCRATCH_DBLK_LEFT,
	HISTB_SCRATCH_ITRANS_TOP,
	HISTB_VDEC_NUM_SCRATCH,
};

/*
 * Single downstream/upstream message pool, allocated once per device: the
 * VDM decodes one frame at a time, so the pool is owned by the device and
 * borrowed by whichever job is running. Write-combining mapping: both the
 * CPU (message producer/consumer) and the VDM DMA master access it, and the
 * vendor firmware uses uncached mappings for the same purpose.
 *
 * Layout:
 *   [0]                          picture slot
 *   [slot 1 .. N]                slice slots
 *   [after slices]               upstream report messages
 *   [aligned]                    per-protocol tables & scratch
 */
struct histb_vdec_msgpool {
	dma_addr_t	dma;
	void		*va;
	size_t		size;
	/* Fixed offsets inside va, initialised at allocation time. */
	unsigned int	pic_off;
	unsigned int	slice_off;
	unsigned int	up_off;
	unsigned int	table_off;
};

/*
 * Mirror of the luma-stride rule the vendor firmware applies to the tiled
 * write path: a stride that is already a multiple of 256 is kept, otherwise
 * round up to the next 256-byte tile column count and skip to an odd one.
 * 320 -> 768, 640 -> 768, 192 -> 256, 512 -> 512, 1920 -> 2304.
 */
static inline unsigned int histb_vdec_tile_stride(unsigned int width)
{
	unsigned int cols = width / 256;

	if (!(width % 256))
		return width;
	cols += (cols % 2) ? 2 : 1;

	return cols * 256;
}

/*
 * HEVC's band pitch is a plain ALIGN(width, 256), not the odd/even column rule
 * above: 320 -> 512 and 1920 -> 2048. Board-measured line by line - the HEVC
 * corpus only untiles byte-exactly at this pitch.
 */
static inline unsigned int histb_vdec_hevc_stride(unsigned int width)
{
	return ALIGN(width, 256);
}

/*
 * Frame-buffer geometry of the 1D (tiled) write path, measured on the board
 * with the all-I_PCM probe and matched to the geometry the vendor firmware
 * programs for H.264. The VDM writes tiles of 256 bytes by 16 picture
 * rows: inside a band the line pitch is the tile width, VDM_YSTRIDE_1D carries
 * the pitch *between* bands, and the chroma tiles start behind the whole luma
 * band array plus a per-picture 1D area. The band count rounds up to an even
 * number because field coding always reserves a pair.
 */
static inline unsigned int histb_vdec_band_bytes(unsigned int tile_stride)
{
	return tile_stride * 16;
}

static inline unsigned int histb_vdec_head_inf_bytes(unsigned int lines)
{
	return 16 * ALIGN(lines, 32);
}

static inline unsigned int histb_vdec_luma_bytes(unsigned int tile_stride,
						 unsigned int lines)
{
	return tile_stride * ALIGN(lines, 32);
}

/* Luma bands, the 1D area behind them, then the chroma bands at half size. */
static inline unsigned int histb_vdec_frame_bytes(unsigned int tile_stride,
						  unsigned int lines)
{
	return histb_vdec_luma_bytes(tile_stride, lines) / 2 * 3 +
	       histb_vdec_head_inf_bytes(lines);
}

static inline void *histb_vdec_pic_slot(const struct histb_vdec_msgpool *p)
{
	return p->va + p->pic_off;
}

static inline dma_addr_t
histb_vdec_pic_slot_dma(const struct histb_vdec_msgpool *p)
{
	return p->dma + p->pic_off;
}

static inline void *histb_vdec_slice_slot(const struct histb_vdec_msgpool *p,
					  unsigned int i)
{
	return p->va + p->slice_off + i * HISTB_VDEC_SLOT_BYTES;
}

static inline dma_addr_t
histb_vdec_slice_slot_dma(const struct histb_vdec_msgpool *p, unsigned int i)
{
	return p->dma + p->slice_off + i * HISTB_VDEC_SLOT_BYTES;
}

static inline void *histb_vdec_up_msgs(const struct histb_vdec_msgpool *p)
{
	return p->va + p->up_off;
}

static inline dma_addr_t
histb_vdec_up_msgs_dma(const struct histb_vdec_msgpool *p)
{
	return p->dma + p->up_off;
}

static inline void *histb_vdec_tables_base(const struct histb_vdec_msgpool *p)
{
	return p->va + p->table_off;
}

static inline dma_addr_t
histb_vdec_tables_dma(const struct histb_vdec_msgpool *p)
{
	return p->dma + p->table_off;
}

struct histb_vdec_dev {
	struct device		*dev;
	void __iomem		*vdm;
	void __iomem		*scd;
	struct clk_bulk_data	*clks;
	int			num_clks;
	struct reset_control	*rst;
	int			irq;

	struct v4l2_device	v4l2_dev;
	struct media_device	mdev;
	struct video_device	*vdev;
	struct v4l2_m2m_dev	*m2m_dev;

	struct delayed_work	watchdog;

	/* VDM state captured by the primary handler for the IRQ thread */
	u32			irq_state;

	/* protects the message pool and the VDM against concurrent jobs */
	struct mutex		hw_lock;
	struct histb_vdec_msgpool pool;
	bool			job_running;

};

struct histb_vdec_fmt_desc;

struct histb_vdec_ctx {
	struct v4l2_fh		fh;
	struct v4l2_ctrl_handler ctrl_hdl;
	const struct histb_vdec_fmt_desc *desc;

	/* dpb (capture buffer) index -> pmv slot id, filled on demand by the
	 * codec layers; pmv slots are a private 20-entry pool.
	 */
	u8			pmv_map[HISTB_VDEC_MAX_DPB_SLOTS];
	u8			pmv_next;

	void			*priv;
};

struct histb_vdec_codec_ops {
	int (*start)(struct histb_vdec_ctx *ctx);
	void (*stop)(struct histb_vdec_ctx *ctx);
	/*
	 * Assemble the message pool and program the VDM for one frame, then
	 * ring the doorbell via histb_vdec_hw_kick(). Runs with dev->hw_lock
	 * held and the runtime PM reference taken.
	 */
	int (*run)(struct histb_vdec_ctx *ctx, struct vb2_v4l2_buffer *src,
		   struct vb2_v4l2_buffer *dst);
	int (*try_ctrl)(struct v4l2_ctrl *ctrl);
};

struct histb_vdec_fmt_desc {
	u32					fourcc;
	unsigned int				num_ctrls;
	const struct v4l2_ctrl_config		*ctrls;
	const struct histb_vdec_codec_ops	*ops;
	enum histb_vdec_vidstd			vidstd;
	/*
	 * Band pitch this protocol's engine writes, and therefore the
	 * bytesperline/sizeimage the capture queue has to advertise: the two
	 * must be one number, or a client copying the tile array reads a sheared
	 * frame. H.264/MPEG-2/MPEG-4 use the firmware's odd/even column rule,
	 * HEVC a plain ALIGN(width, 256).
	 */
	unsigned int				(*stride)(unsigned int width);
	/*
	 * Height granularity of that pitch: the engine lays out whole coding
	 * blocks, so the frame footprint is the coding grid, not the coded
	 * height. MB protocols are 16; HEVC's largest CTB is 64 and CTB size is
	 * only known once the SPS arrives, so its capture size is rounded for 64.
	 */
	unsigned int				grid_align;
};

/* Per-protocol descriptors, defined by the codec sources. */
extern const struct histb_vdec_fmt_desc histb_vdec_h264_fmt;
extern const struct histb_vdec_fmt_desc histb_vdec_hevc_fmt;
extern const struct histb_vdec_fmt_desc histb_vdec_mpeg2_fmt;
extern const struct histb_vdec_fmt_desc histb_vdec_mpeg4_fmt;
extern const struct histb_vdec_fmt_desc histb_vdec_h263_fmt;

/* drv.c */
irqreturn_t histb_vdec_irq_handler(int irq, void *priv);
int histb_vdec_cap_dims(struct histb_vdec_ctx *ctx, unsigned int *w,
			unsigned int *h);
void histb_vdec_buf_geom(struct histb_vdec_ctx *ctx, unsigned int *stride,
			 unsigned int *lines);

/* hw.c */
int histb_vdec_hw_init(struct histb_vdec_dev *dev);
void histb_vdec_hw_reset(struct histb_vdec_dev *dev);
void histb_vdec_hw_kick(struct histb_vdec_dev *dev,
			enum histb_vdec_vidstd std);
/* Program the constant tables once at probe. */
void histb_vdec_hw_init_tables(struct histb_vdec_dev *dev);

static inline void vdm_write(struct histb_vdec_dev *dev, unsigned int reg,
			     u32 val)
{
	writel(val, dev->vdm + reg);
}

static inline u32 vdm_read(struct histb_vdec_dev *dev, unsigned int reg)
{
	return readl(dev->vdm + reg);
}

/* Convert an OUTPUT plane into the vendor's bitstream segment descriptor:
 * a (byte offset from the aligned stream base, bit offset, bit length)
 * triple. The hardware fetches on 16-byte granularity, so the bits inside the
 * granule join the bit offset, which is what the vendor firmware programs for
 * every segment it hands over. data_addr must be the address of the
 * segment's first bit, not of the plane start.
 */
static inline void histb_vdec_seg_pack(u32 *words, dma_addr_t stream_base,
				       dma_addr_t data_addr,
				       unsigned int bit_offset,
				       unsigned int bit_len)
{
	unsigned int bytes = (unsigned int)(data_addr - (stream_base & ~15u));

	words[0] = bytes & ~15u;
	words[1] = bit_offset + ((bytes & 15u) * 8u);
	words[2] = bit_len;
}

/*
 * Zero bits at the tail of a segment, counted from the end of the byte range
 * backwards: whole zero bytes, then the zeros below the lowest set bit of the
 * last non-zero byte (the stream is MSB-first, so the tail of the last byte is
 * the last thing written). This is the same scan the vendor firmware runs over
 * a segment before handing it to the engine; the '1' stop bit above those
 * zeros belongs to the rbsp trailing too, so callers add one.
 * The VDM counts every bit of a segment as macroblock layer, so a length that
 * still covers the padding makes it report surplus bits at the end of the
 * segment (measured on the board: one bit of padding in an interior slice is
 * enough to set the error flag).
 */
static inline u32 histb_vdec_trail_zero_bits(const u8 *va, u32 end_bit)
{
	u32 bits = 0;
	u32 byte = end_bit / 8;

	if (!va)
		return 0;
	while (byte--) {
		u8 value = va[byte];

		if (value)
			return bits + __ffs(value);
		bits += 8;
	}
	return bits;
}

/* Single CAPTURE format: 16x16-tiled NV21, as the VDM writes it. */
#define V4L2_PIX_FMT_NV21_16L16		v4l2_fourcc('S', 'N', '2', '1')

/* Nominal OUTPUT (bitstream) plane size. */
#define HISTB_VDEC_OUTPUT_SIZE_DEFAULT	(3 * 1024 * 1024)

/* Message pool allocation size. */
#define HISTB_VDEC_POOL_SIZE		(6 * 1024 * 1024)

/*
 * Layout of the per-protocol table area, as offsets from
 * histb_vdec_msgpool::table_off. The CABAC M/N tables are constant and are
 * programmed once at probe, the tile info image is zeroed there because no
 * accepted picture uses it, and the remaining areas are filled per frame by
 * the message assembly code. The scratch blocks follow in
 * enum histb_vdec_scratch order, HISTB_VDEC_SCRATCH_SIZE bytes each.
 */
#define HISTB_TABLE_OFF_CABAC_MN_H264	0
#define HISTB_TABLE_OFF_CABAC_MN_HEVC	5120
#define HISTB_TABLE_OFF_TILE_INFO	6048
#define HISTB_TABLE_OFF_PPFD		(HISTB_TABLE_OFF_TILE_INFO + \
					 HISTB_VDEC_TILE_INFO_SIZE)
#define HISTB_TABLE_OFF_SCRATCH_BASE	286720
#define HISTB_TABLE_OFF_SCRATCH(i)	(HISTB_TABLE_OFF_SCRATCH_BASE + \
					 (i) * HISTB_VDEC_SCRATCH_SIZE)

#endif /* HISTB_VDEC_H */
