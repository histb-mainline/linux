// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Video encoder (VEDU, "v1.0") for HiSilicon Hi3798MV100.
 *
 * One raw picture per mem2mem job, coded as an IDR picture: the block is
 * programmed with a whole frame and answers with an end-of-picture interrupt
 * once it has appended its packets to the ring the driver gave it. Every output
 * carries its own parameter sets, so every frame is independently decodable and
 * the channel needs no picture state of its own.
 *
 * The block does not build the parameter sets or the slice header. Both are the
 * driver's job, and the block's convention for receiving them is unusual: the
 * SPS and PPS go into the stream ring ahead of the picture as two 128-byte
 * packets the driver wrote itself, and the slice header goes into registers as
 * three separate right-justified bit groups - the picture header, the reference
 * list reordering, and dec_ref_pic_marking - each with its own width field.
 *
 * The ring is a circular buffer of 64-byte packets. Each packet begins with a
 * 64-byte header whose first word is the length of the whole packet, whose
 * second word says how many bytes at the end of the payload are padding, whose
 * ninth byte names the NAL unit and whose twelfth marks the picture's last
 * packet. The read and write pointers are not registers either: they are two
 * words at the addresses given by SRPTRADDR and SWPTRADDR, each one an offset
 * into the ring rather than an address, and the block and the driver both see
 * them.
 *
 * The picture is copied into the driver's scratch rather than coded in place: a
 * client's buffer is cached memory and this block is not cache-coherent, so a
 * job against it can code bytes the CPU has not written out yet. The block also
 * writes two reconstruction frames next to the ring, which is why the scratch
 * is picture-sized twice over even though an all-intra channel never reads them
 * back.
 *
 * Rate control has no off switch: the register file has no enable bit for it. A
 * fixed quantiser is programmed by putting the same QP in the minimum, the
 * maximum and the start field, because the reference clamps its picture QP into
 * that window before it uses it; the target-bits register still wants a number
 * a picture can plausibly spend.
 *
 * The register layout comes from accumulating msp/drv/venc/venc_v1.0/
 * drv_venc_reg_s5v100.h by member - 0xff14 bytes, with the version pair at 0xf8
 * and 0xfc as the self-check that no reserved hole was miscounted - and the
 * values from the live path of hal_venc.c:VENC_HAL_CfgReg() and drv_venc_efl.c
 * for the s5v100/98M build this chip uses. Two blocks are deliberately not
 * mirrored: the colour-space coefficient file and the temporal-filter
 * thresholds, both consumed only by paths this driver leaves disabled, where
 * the reset value is what the reference's own zeroed descriptor would have left
 * there.
 *
 * What the board measured, on an EC6108V9C with this block at 0xf8c80000 and
 * version 0x56325238: a 352x288 and a 1920x1080 NV12 picture, each coded at a
 * fixed QP of 32, come back as one SPS, one PPS and one IDR slice that ffmpeg
 * decodes. The 1920x1080 case reports 1920x1080 out of a 120x68 macroblock grid,
 * so the crop in the parameter set is being read. Every macroblock of a
 * per-block-coded picture decodes at its own code (0 of 4480 off by more than
 * five levels), luma shows no level shift at all (mean error 0.002), and a
 * chroma pair sent as U 60 / V 190 comes back as 59 / 189 - quantisation, not
 * the black-level offset the JPEG encoder of this family has. Feeding the same
 * picture as NV21 instead of NV12 decodes to the same 59 / 189, so both
 * packageSel values point at the chroma order they name. Four pictures coded
 * through one file handle all decode, with the identifier in the slice header
 * stepping 0, 1, 2, 3.
 *
 * One thing the block does not do is re-time the slice header it is handed. The
 * reference writes frame_num in four bits while its own parameter set declares
 * eight (log2_max_frame_num_minus4 = 0), and the coded slice reproduces the four
 * bits verbatim instead of widening them; decoders put up with it, and mirroring
 * the reference is the only option that is known to match shipped behaviour.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
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
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define HISTB_VENC_NAME		"histb-venc"

/*
 * Semi-planar only: the block's input-format register names the two chroma
 * orders of a semi-planar 4:2:0 frame, and in that layout the reference reads
 * one stride for both planes (drv_venc_efl.c:4644).
 */
static const u32 histb_venc_out_fmts[] = {
	V4L2_PIX_FMT_NV12,
	V4L2_PIX_FMT_NV21,
};

/* ------------------------------------------------------------------ */
/* Registers (drv_venc_reg_s5v100.h, accumulated by member)            */
/* ------------------------------------------------------------------ */

#define VENC_INTSTAT		0x000
#define  VENC_INT_EOP		BIT(0)
#define  VENC_INT_BUF_FULL	BIT(1)
#define  VENC_INT_PBIT		BIT(3)
#define  VENC_INT_TIMEOUT	BIT(27)
#define  VENC_INT_JOB		(VENC_INT_EOP | VENC_INT_BUF_FULL | \
				 VENC_INT_PBIT | VENC_INT_TIMEOUT)

#define VENC_INTMASK		0x004	/* a set bit enables that source */
#define VENC_RAWINT		0x008
#define VENC_INTCLR		0x00c	/* write 1 to clear */

#define VENC_START		0x010
#define  VENC_START_RUN		BIT(0)

#define VENC_MODE		0x020
#define  VENC_MODE_VENC_EN	BIT(1)
#define  VENC_MODE_STD_MOD	GENMASK(4, 3)	/* 0 is H.264 on this generation */
#define  VENC_MODE_CSC_EN	BIT(9)
#define  VENC_MODE_TIME_EN	GENMASK(11, 10)
#define  VENC_MODE_CLK_GATE	GENMASK(14, 13)
#define  VENC_MODE_MEM_GATE	BIT(15)
#define  VENC_MODE_PKG_SEL	GENMASK(24, 17)
#define  VENC_MODE_STR_FMT	GENMASK(27, 26)	/* 0 is semi-planar */
#define  VENC_MODE_VAL		(VENC_MODE_VENC_EN | \
				 FIELD_PREP(VENC_MODE_TIME_EN, 3) | \
				 FIELD_PREP(VENC_MODE_CLK_GATE, 2) | \
				 VENC_MODE_MEM_GATE)

/*
 * packageSel is the chroma order of a semi-planar frame: the reference's own
 * conversion table maps NV12 to 1 (U before V) and NV21 to 0 (V before U),
 * drv_venc_efl.c:2028-2037.
 */
#define  VENC_PKG_V_U		0
#define  VENC_PKG_U_V		1

#define VENC_PICFG0		0x024
#define  VENC_PICFG0_PTBITS_EN	BIT(8)
#define  VENC_PICFG0_CONST_I	BIT(12)
#define  VENC_PICFG0_I_PIC	BIT(13)
#define  VENC_PICFG0_TRANS	GENMASK(15, 14)	/* 0 is documented as unsupported */
#define  VENC_PICFG0_NAL_REF	GENMASK(17, 16)
#define  VENC_PICFG0_ENTROPY	BIT(19)	/* 0 is CAVLC, 1 is CABAC */
#define  VENC_PICFG0_I_VAL	(VENC_PICFG0_PTBITS_EN | VENC_PICFG0_I_PIC | \
				  FIELD_PREP(VENC_PICFG0_TRANS, 1) | \
				  FIELD_PREP(VENC_PICFG0_NAL_REF, 3))
/* A reference P picture: no I flag, and nal_ref_idc 2 rather than 3. */
#define  VENC_PICFG0_P_VAL	(VENC_PICFG0_PTBITS_EN | \
				 FIELD_PREP(VENC_PICFG0_TRANS, 1) | \
				 FIELD_PREP(VENC_PICFG0_NAL_REF, 2))

#define VENC_PICFG1		0x028	/* slice split, off: one slice per picture */

#define VENC_PICFG2		0x02c
#define  VENC_PICFG2_IPCM	BIT(0)
#define  VENC_PICFG2_I4		BIT(1)
#define  VENC_PICFG2_I16	BIT(3)
#define  VENC_PICFG2_ILOWPOW	BIT(31)
#define  VENC_PICFG2_VAL	(VENC_PICFG2_IPCM | VENC_PICFG2_I4 | \
				 VENC_PICFG2_I16 | VENC_PICFG2_ILOWPOW)

/*
 * The inter-prediction enables stay set exactly as the reference leaves them for
 * an H.264 channel (hal_venc.c:289 takes them from the same descriptor the intra
 * path fills). They cost nothing here, because no macroblock is coded inter in
 * an I picture.
 */
#define VENC_PICFG3		0x030
#define  VENC_PICFG3_VAL	0xe000001f

#define VENC_PICFG4		0x034	/* input clipping, off */

/* The window the block would clip to if PICFG4 enabled it: video range. */
#define VENC_PICFG5		0x038
#define  VENC_PICFG5_LUMA_MIN	GENMASK(7, 0)
#define  VENC_PICFG5_LUMA_MAX	GENMASK(15, 8)
#define  VENC_PICFG5_CHR_MIN	GENMASK(23, 16)
#define  VENC_PICFG5_CHR_MAX	GENMASK(31, 24)
#define  VENC_PICFG5_VAL	(FIELD_PREP(VENC_PICFG5_LUMA_MIN, 16) | \
				 FIELD_PREP(VENC_PICFG5_LUMA_MAX, 235) | \
				 FIELD_PREP(VENC_PICFG5_CHR_MIN, 16) | \
				 FIELD_PREP(VENC_PICFG5_CHR_MAX, 240))

#define VENC_ENCUPDATE		0x040	/* MPEG-4 intra refresh only */

#define VENC_VEDIMGSIZE		0x050
#define  VENC_IMG_WIDTH		GENMASK(12, 0)	/* pixels, minus one */
#define  VENC_IMG_HEIGHT	GENMASK(28, 16)

/* Per-picture bit budget; going over it raises the overflow source. */
#define VENC_PTBITS		0x058
#define  VENC_PTBITS_VAL	(500000 * 8)

/*
 * The three slice-header groups, right justified so that a group's last bit is
 * bit 0 of its word and its first bit sits at the width the parameter register
 * names. The reference passes all three even for a slice type that, read from
 * the standard alone, has no reordering field, so this driver mirrors the
 * reference and lets the block decide what to emit. The widths are bits minus
 * one: an IDR's marking group is the no-output and long-term pair, a P
 * picture's is the single adaptive-mode flag.
 */
#define VENC_SLCHDRSTRM		0x060	/* 4 header, 2 reorder, 2 marking words */
#define  VENC_SLCHDR_WORDS	8
#define VENC_SLCHDRPARA		0x080
#define  VENC_SLCHPARA_IDR	BIT(1)
#define  VENC_SLCHPARA_PARA_WD	GENMASK(14, 8)
#define  VENC_SLCHPARA_REORD_WD	GENMASK(21, 16)
#define  VENC_SLCHPARA_MARK_WD	GENMASK(27, 22)
/* The reorder group is the single '0' bit, the marking group the two '00' bits. */
#define  VENC_SLCHPARA_REORD	0
#define  VENC_SLCHPARA_MARK_I	1
#define  VENC_SLCHPARA_MARK_P	0

#define VENC_PTS0		0x090
#define VENC_TIMEOUT		0x0a0
#define  VENC_TIMEOUT_VAL	10000000
#define VENC_OUTSTD		0x0a4
#define  VENC_OUTSTD_NUM	GENMASK(3, 0)
#define  VENC_OUTSTD_VAL	FIELD_PREP(VENC_OUTSTD_NUM, 1)

#define VENC_VERSION		0x0fc	/* "V2R8" here; the pair at 0xf8 reads 0 */

#define VENC_SRCYADDR		0x100
#define VENC_SRCCADDR		0x104
#define VENC_SRCVADDR		0x108	/* unused by a semi-planar picture */
#define VENC_RCNYADDR		0x110
#define VENC_RCNCADDR		0x114
#define VENC_REFYADDR		0x120
#define VENC_REFCADDR		0x124

#define VENC_SSTRIDE		0x130
#define  VENC_SSTRIDE_Y		GENMASK(15, 0)
#define  VENC_SSTRIDE_C		GENMASK(31, 16)

#define VENC_RSTRIDE		0x134
#define  VENC_RSTRIDE_Y		GENMASK(15, 0)
#define  VENC_RSTRIDE_C		GENMASK(31, 16)

#define VENC_STRMADDR		0x140
#define VENC_SRPTRADDR		0x144	/* address of the word holding the offset */
#define VENC_SWPTRADDR		0x148
#define VENC_STRMBUFLEN		0x14c
#define  VENC_STRMBUFLEN_UNIT	GENMASK(31, 6)	/* whole bytes, low 6 unused:
						 * the reference writes its byte
						 * size into the word raw
						 * (drv_venc_efl.c:417), and the
						 * JPGE half of this block
						 * reported "stream full" when
						 * the size was divided by 64
						 */

/* Motion search: idle for an I picture, and programmed every frame anyway. */
#define VENC_MECFG0		0x200
#define  VENC_MECFG0_HSW	GENMASK(3, 0)
#define  VENC_MECFG0_VSW	GENMASK(6, 4)
#define  VENC_MECFG0_REAL_MV	GENMASK(19, 16)
#define VENC_MECFG1		0x204	/* four search ranges, all enabled */
#define  VENC_MECFG1_VAL	0xf
#define VENC_MERECTCFG		0x208	/* four words, 16 bytes apart */
#define VENC_METHR0		0x220
#define  VENC_METHR0_TH1	GENMASK(15, 0)
#define  VENC_METHR0_TH2	GENMASK(31, 16)
#define VENC_METHR1		0x224
#define  VENC_METHR1_INTRA	GENMASK(15, 0)
#define VENC_MERDOCFG		0x228	/* the lambda offsets, all zero */
#define VENC_CREFLDMODE		0x22c
#define  VENC_CREFLDBUR8	BIT(0)
#define VENC_MDCFG		0x230
#define  VENC_MDCFG_RDO		BIT(16)

/* The reference's search settings for the widest class, range 0 to range 3. */
static const u32 venc_me_rect[4] = { 0x00000202, 0x00008208, 0x010d0d0d,
				      0x00000404 };

/*
 * Pictures per group of pictures: 1 codes every picture as an IDR, which is what
 * the driver did before the inter path existed; N codes one IDR and N-1 P
 * pictures, the reference's IntraPic = (InterFrmCnt >= Gop - 1) rule
 * (drv_venc_efl.c:3412).
 */
static unsigned int gop = 1;
module_param(gop, uint, 0644);
MODULE_PARM_DESC(gop, "pictures per GOP: 1 = every picture is an IDR");

#define VENC_ROICFG		0x300
#define  VENC_ROICFG_VAL	0x0000ff00	/* no region enabled, all absolute QP */

#define VENC_QPTHR		0x350
#define  VENC_QPTHR_MIN		GENMASK(5, 0)
#define  VENC_QPTHR_MAX		GENMASK(13, 8)
#define  VENC_QPTHR_START	GENMASK(21, 16)

#define VENC_RC1		0x354
#define  VENC_RC1_QP_DELTA	GENMASK(5, 0)
#define  VENC_RC1_MADP_DELTA	GENMASK(15, 8)
#define  VENC_RC1_VAL		(FIELD_PREP(VENC_RC1_QP_DELTA, 2) | \
				 FIELD_PREP(VENC_RC1_MADP_DELTA, 0xf8))

/* The three QP-delta thresholds are constants in the reference, not derived. */
#define VENC_QPDELTATHR		0x358
static const u32 venc_qp_delta_thr[3] = { 0x09070707, 0x19120e0c, 0xffffffff };

#define VENC_TARGETBITS		0x364
#define VENC_PICBITS		0x374	/* bits the last picture spent */
#define VENC_MEANQP		0x388	/* mean QP it actually used */
#define VENC_MODLAMBDA		0x3a0	/* 20 words of two 16-bit lambdas */
#define VENC_VPP_OSD_CFG	0x500	/* eight overlay planes, all off */
#define VENC_SCALE		0x5e00	/* 32 words: intra list, then inter list */

#define VENC_REG_WINDOW		0x10000

/* ------------------------------------------------------------------ */
/* Stream ring                                                         */
/* ------------------------------------------------------------------ */

#define VENC_PKG_HDR_BYTES	64	/* the header block at a packet's start */
#define VENC_PKG_BLOCK		64	/* the payload block that follows it */
#define VENC_PKG_LEN		0	/* whole packet, 64-byte aligned */
#define VENC_PKG_INVLD		4	/* padding at the end of the payload */
#define VENC_PKG_TYPE		8	/* nal_unit_type of the payload */
#define VENC_PKG_LAST		11	/* set on the picture's last packet */
#define VENC_PKG_BYTES		(VENC_PKG_HDR_BYTES + VENC_PKG_BLOCK)
#define VENC_PTR_BYTES		32	/* the two pointer words, at 0 and 16 */
#define VENC_STRM_OFF		64	/* STRMADDR, above the pointer block */
#define VENC_ALIGN_STREAM	64	/* STRMADDR granularity */
#define VENC_ALIGN_FRAME	16	/* plane addresses and strides */
#define VENC_ALIGN_BUFLEN	256	/* sizes kept whole, so the 64-byte
					 * quanta the length register counts
					 * never round off
					 */

/* ------------------------------------------------------------------ */
/* H.264 parameter sets, written by the driver                         */
/* ------------------------------------------------------------------ */

#define VENC_NAL_IDR		5
#define VENC_NAL_SPS		7
#define VENC_NAL_PPS		8
#define VENC_PROFILE_IDC	66	/* baseline: the PPS carries no scaling lists */
#define VENC_START_CODE_BYTES	4
#define VENC_DEFAULT_FPS	30	/* what the bit budget is divided by */

struct venc_level {
	unsigned int total_mb;
	u8 level_idc;
};

/* The reference's own table (drv_venc_efl.c:663), in macroblock counts. */
static const struct venc_level venc_levels[] = {
	{ 99, 10 }, { 396, 20 }, { 792, 21 }, { 1620, 30 },
	{ 3600, 31 }, { 5120, 32 }, { 8192, 41 },
};

/* The two 8x8 quantiser lists, intra first, in the raster order the block wants. */
static const u8 venc_scale[128] = {
	6, 10, 13, 16, 18, 23, 25, 27, 10, 11, 16, 18, 23, 25, 27, 29,
	13, 16, 18, 23, 25, 27, 29, 31, 16, 18, 23, 25, 27, 29, 31, 33,
	18, 23, 25, 27, 29, 31, 33, 36, 23, 25, 27, 29, 31, 33, 36, 38,
	25, 27, 29, 31, 33, 36, 38, 40, 27, 29, 31, 33, 36, 38, 40, 42,
	9, 13, 15, 17, 19, 21, 22, 24, 13, 13, 17, 19, 21, 22, 24, 25,
	15, 17, 19, 21, 22, 24, 25, 27, 17, 19, 21, 22, 24, 25, 27, 28,
	19, 21, 22, 24, 25, 27, 28, 30, 21, 22, 24, 25, 27, 28, 30, 32,
	22, 24, 25, 27, 28, 30, 32, 33, 24, 25, 27, 28, 30, 32, 33, 35,
};

static const u16 venc_mod_lambda[40] = {
	1, 1, 1, 2, 2, 3, 3, 4, 5, 7, 9, 11, 14, 17, 22, 27, 34, 43, 54, 69,
	86, 109, 137, 173, 218, 274, 345, 435, 548, 691, 870, 1097, 1382,
	1741, 2193, 2763, 3482, 4095, 4095, 4095,
};

/* ------------------------------------------------------------------ */
/* Block limits                                                        */
/* ------------------------------------------------------------------ */

#define VENC_MIN_WIDTH		176
#define VENC_MIN_HEIGHT		144
#define VENC_MAX_WIDTH		1920
#define VENC_MAX_HEIGHT		1088
#define VENC_MB_ALIGN		16	/* the block codes whole macroblocks */
#define VENC_DIM_ALIGN		4	/* the reference's own size gate */
#define VENC_QP_DEFAULT		32
#define VENC_QP_MIN		0
#define VENC_QP_MAX		51
#define VENC_BITRATE_DEFAULT	2000000
#define VENC_MIN_STREAM_SIZE	SZ_256K	/* the reference's lower bound */
#define VENC_MAX_STREAM_SIZE	SZ_16M
#define VENC_JOB_TIMEOUT	msecs_to_jiffies(5000)
#define VENC_SETTLE_US		10

/*
 * struct histb_venc - one VEDU instance
 * @hw_lock: the block keeps one global descriptor rather than per-channel
 *	     state, so a job owns it from its first write to its completion;
 *	     that also leaves @int_state and @int_done to one writer
 */
struct histb_venc {
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
 * struct histb_venc_q - one queue's layout as client and block see it
 *
 * For the OUTPUT queue it is the raw picture; for CAPTURE @size is the largest
 * bitstream the client will accept, which is also the ring the block writes.
 */
struct histb_venc_q {
	u32	fourcc;
	unsigned int width;
	unsigned int height;
	unsigned int bytesperline;
	unsigned int size;
};

struct histb_venc_ctx {
	struct histb_venc	*venc;
	struct v4l2_fh		fh;
	struct v4l2_ctrl_handler ctrl_hdl;
	struct histb_venc_q	out_q;
	struct histb_venc_q	cap_q;
	struct v4l2_ctrl	*min_qp;
	struct v4l2_ctrl	*max_qp;
	struct v4l2_ctrl	*bitrate;
	unsigned int		idr_pic_id;
	unsigned int		frm_num;	/* the four-bit frame_num of the reference */
	unsigned int		frm_in_gop;
	unsigned int		rcn_sel;
	bool			coded;		/* the first picture of a context opens with an IDR */
	const char		*why;
	void			*stream;
	dma_addr_t		stream_dma;
	size_t			stream_alloc;
};

/* A job's addresses and the slice header that goes with them. */
struct histb_venc_job {
	dma_addr_t		src_y;
	dma_addr_t		rcn;
	size_t			strm_off;
	size_t			ring_len;
	unsigned int		rcn_luma;
	unsigned int		rcn_sel;	/* half of the pair being written */
	bool			intra;
	u32			slc_hdr;
	unsigned int		para_wd;
};

/* v4l2-ioctl passes no file handle to the format ops, only to the file. */
static struct histb_venc_ctx *file_to_ctx(struct file *file)
{
	return container_of(file_to_v4l2_fh(file), struct histb_venc_ctx, fh);
}

/* ------------------------------------------------------------------ */
/* Bit and slice-header writers                                        */
/* ------------------------------------------------------------------ */

struct venc_bs {
	u8	*buf;
	unsigned int bits;
};

/* Most significant bit first, into the buffer the client will be given. */
static void venc_bs_put(struct venc_bs *bs, u32 code, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		unsigned int sh = 7 - (bs->bits & 7);
		u8 bit = (code >> (n - 1 - i)) & 1;

		bs->buf[bs->bits >> 3] &= ~BIT(sh);
		bs->buf[bs->bits >> 3] |= bit << sh;
		bs->bits++;
	}
}

/* Exp-Golomb: the value plus one, in twice its bit length minus one bits. */
static void venc_bs_ue(struct venc_bs *bs, u32 v)
{
	u32 code = v + 1;

	venc_bs_put(bs, code, 2 * fls(code) - 1);
}

static void venc_bs_se(struct venc_bs *bs, int v)
{
	venc_bs_ue(bs, v <= 0 ? 2 * -v : 2 * v - 1);
}

/* The reference's trailing bits: one '1', then zeros up to the next byte. */
static void venc_bs_trailing(struct venc_bs *bs)
{
	venc_bs_put(bs, 1, 1);
	if (bs->bits & 7)
		venc_bs_put(bs, 0, 8 - (bs->bits & 7));
}

/*
 * The slice header of an IDR picture: ue(2) for the type, '1' for parameter set
 * 0, frame_num in the four bits the reference writes - always zero, because an
 * IDR picture's frame number is zero by the standard - and ue of the picture
 * identifier. Joined at the low end, so the finished group is right justified
 * the way the register wants it and its width is the total minus one.
 *
 * The reference increments its identifier per IDR picture rather than
 * alternating it between two values, and keeps only its low four bits; this
 * does the same, because what the block does with a group it is handed is only
 * knowable from the board.
 */
static void venc_slc_hdr(u32 *hdr, unsigned int *para_wd, bool intra,
			 unsigned int id, unsigned int fn)
{
	u32 code;
	unsigned int bits;

	if (!intra) {
		/*
		 * ue(0) for the P slice type, '1' for parameter set 0, the running
		 * frame_num, and the '0' override flag the reference emits because its
		 * NumRefIndex is 0 (drv_venc_efl.c:1281, :906-915).
		 */
		*hdr = (1u << 6) | (1u << 5) | ((fn & 0xf) << 1);
		*para_wd = 6;
		return;
	}

	code = (id & 0xf) + 1;
	bits = 2 * fls(code) - 1;
	*hdr = (0x3u << (5 + bits)) | (1u << (4 + bits)) | code;
	*para_wd = 3 + 1 + 4 + bits - 1;
}

/* ------------------------------------------------------------------ */
/* Parameter sets                                                      */
/* ------------------------------------------------------------------ */

static u8 venc_level_idc(unsigned int total_mb, bool *direct8x8)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(venc_levels); i++) {
		if (total_mb <= venc_levels[i].total_mb) {
			*direct8x8 = total_mb >= 1620;
			return venc_levels[i].level_idc;
		}
	}

	return 0;
}

/*
 * The SPS the block's coder needs: baseline with no constraint flags and no
 * VUI, one reference frame, and picture-order-count type 2, which is what lets
 * the slice header carry no picture order at all. The picture is cropped back
 * from whole macroblocks in half-sample units, the way the reference computes
 * it (drv_venc_efl.c:2554).
 */
static size_t histb_venc_write_sps(u8 *buf, unsigned int width,
				   unsigned int height)
{
	unsigned int mb_w = DIV_ROUND_UP(width, VENC_MB_ALIGN);
	unsigned int mb_h = DIV_ROUND_UP(height, VENC_MB_ALIGN);
	unsigned int crop_r = (mb_w * VENC_MB_ALIGN - width) / 2;
	unsigned int crop_b = (mb_h * VENC_MB_ALIGN - height) / 2;
	bool crop = crop_r || crop_b;
	struct venc_bs bs = { .buf = buf };
	bool direct8x8;
	u8 level;

	level = venc_level_idc(mb_w * mb_h, &direct8x8);
	if (!level)
		return 0;

	memset(buf, 0, VENC_PKG_BLOCK);
	venc_bs_put(&bs, 1, VENC_START_CODE_BYTES * 8);	/* the start code */
	venc_bs_put(&bs, 0, 1);		/* forbidden_zero_bit */
	venc_bs_put(&bs, 3, 2);		/* nal_ref_idc */
	venc_bs_put(&bs, VENC_NAL_SPS, 5);
	venc_bs_put(&bs, VENC_PROFILE_IDC, 8);
	venc_bs_put(&bs, 0x00, 8);	/* the six constraint flags, none set */
	venc_bs_put(&bs, level, 8);
	venc_bs_ue(&bs, 0);		/* seq_parameter_set_id */
	venc_bs_ue(&bs, 0);		/* log2_max_frame_num_minus4 */
	venc_bs_ue(&bs, 2);		/* pic_order_cnt_type */
	venc_bs_ue(&bs, 1);		/* num_ref_frames */
	venc_bs_put(&bs, 0, 1);		/* gaps_in_frame_num_value_allowed_flag */
	venc_bs_ue(&bs, mb_w - 1);	/* pic_width_in_mbs_minus1 */
	venc_bs_ue(&bs, mb_h - 1);	/* pic_height_in_mbs_minus1 */
	venc_bs_put(&bs, 1, 1);		/* frame_mbs_only_flag */
	venc_bs_put(&bs, direct8x8, 1);	/* direct_8x8_inference_flag */
	/* The offsets may not all be zero once the flag is set. */
	venc_bs_put(&bs, crop, 1);	/* frame_cropping_flag */
	if (crop) {
		venc_bs_ue(&bs, 0);		/* frame_crop_left_offset */
		venc_bs_ue(&bs, crop_r);	/* frame_crop_right_offset */
		venc_bs_ue(&bs, 0);		/* frame_crop_top_offset */
		venc_bs_ue(&bs, crop_b);	/* frame_crop_bottom_offset */
	}
	venc_bs_put(&bs, 0, 1);		/* vui_parameters_present_flag */
	venc_bs_trailing(&bs);

	return bs.bits / 8;
}

/*
 * CAVLC, one slice group, no weighting, no second chroma QP offset and no
 * scaling lists: baseline has no transform_8x8_mode_flag to hang them on, which
 * is why the reference skips the whole block for this profile even though it
 * uploads the lists to the scale registers anyway.
 */
static size_t histb_venc_write_pps(u8 *buf)
{
	struct venc_bs bs = { .buf = buf };

	memset(buf, 0, VENC_PKG_BLOCK);
	venc_bs_put(&bs, 1, VENC_START_CODE_BYTES * 8);
	venc_bs_put(&bs, 0, 1);
	venc_bs_put(&bs, 3, 2);
	venc_bs_put(&bs, VENC_NAL_PPS, 5);
	venc_bs_ue(&bs, 0);		/* pic_parameter_set_id */
	venc_bs_ue(&bs, 0);		/* seq_parameter_set_id */
	venc_bs_put(&bs, 0, 1);		/* entropy_coding_mode_flag */
	venc_bs_put(&bs, 0, 1);		/* bottom_field_pic_order_in_frame_present */
	venc_bs_ue(&bs, 0);		/* num_slice_groups_minus1 */
	venc_bs_ue(&bs, 0);		/* num_ref_idx_l0_active_minus1 */
	venc_bs_ue(&bs, 0);		/* num_ref_idx_l1_active_minus1 */
	venc_bs_put(&bs, 0, 1);		/* weighted_pred_flag */
	venc_bs_put(&bs, 0, 2);		/* weighted_bipred_idc */
	venc_bs_se(&bs, 0);		/* pic_init_qp_minus26 */
	venc_bs_se(&bs, 0);		/* pic_init_qs_minus26 */
	venc_bs_se(&bs, 0);		/* chroma_qp_index_offset */
	venc_bs_put(&bs, 1, 1);		/* deblocking_filter_control_present_flag */
	venc_bs_put(&bs, 0, 1);		/* constrained_intra_pred_flag */
	venc_bs_put(&bs, 0, 1);		/* redundant_pic_cnt_present_flag */
	venc_bs_trailing(&bs);

	return bs.bits / 8;
}

/* ------------------------------------------------------------------ */
/* Ring                                                                */
/* ------------------------------------------------------------------ */

/*
 * Every offset into the ring is taken modulo its length, because the block's
 * write pointer wraps with it.
 */
static void venc_ring_put(struct histb_venc_ctx *ctx, size_t off,
			  const void *src, size_t n)
{
	size_t len = ctx->cap_q.size;
	u8 *base = ctx->stream;
	size_t first;

	off %= len;
	first = min(n, len - off);
	memcpy(base + VENC_STRM_OFF + off, src, first);
	if (n > first)
		memcpy(base + VENC_STRM_OFF, (const u8 *)src + first,
		       n - first);
}

static void venc_ring_get(struct histb_venc_ctx *ctx, size_t off, void *dst,
			  size_t n)
{
	size_t len = ctx->cap_q.size;
	u8 *base = ctx->stream;
	size_t first;

	off %= len;
	first = min(n, len - off);
	memcpy(dst, base + VENC_STRM_OFF + off, first);
	if (n > first)
		memcpy((u8 *)dst + first, base + VENC_STRM_OFF, n - first);
}

/*
 * A parameter set is one header block and one payload block: the length covers
 * both, the padding word says how much of the payload block went unused, and the
 * picture is not finished, so the last-packet byte stays clear.
 */
static void histb_venc_pkg_param(struct histb_venc_ctx *ctx, size_t off,
				 const u8 *payload, size_t bytes, u32 type)
{
	u8 blk[VENC_PKG_BYTES] = { 0 };

	put_unaligned_le32(VENC_PKG_BYTES, blk + VENC_PKG_LEN);
	put_unaligned_le32(VENC_PKG_BLOCK - bytes, blk + VENC_PKG_INVLD);
	blk[VENC_PKG_TYPE] = type;
	memcpy(blk + VENC_PKG_HDR_BYTES, payload, bytes);

	venc_ring_put(ctx, off, blk, sizeof(blk));
}

/* ------------------------------------------------------------------ */
/* Hardware control                                                    */
/* ------------------------------------------------------------------ */

/*
 * The block is taken through a reset once, before anything is configured, from
 * the probe path. The reference does the same pair per frame around its own
 * register writes (BoardDeinit asserts at drv_venc_efl.c:5447, BoardInit
 * releases inside CfgRegVenc at :2305); a driver that owns the whole block and
 * never leaves it configured between jobs needs it only once, and again after a
 * job that ended without the block having finished.
 *
 * The version register is polled afterwards as a "does the window answer" check:
 * it reads "V2R8" with the gate on and zero with the clock off.
 */
static int histb_venc_reset(struct histb_venc *venc)
{
	u32 val;
	int ret;

	writel(0, venc->regs + VENC_INTMASK);

	ret = reset_control_assert(venc->rst);
	if (ret)
		return ret;

	udelay(VENC_SETTLE_US);
	writel(VENC_INT_JOB, venc->regs + VENC_INTCLR);

	ret = reset_control_deassert(venc->rst);
	if (ret)
		return ret;

	udelay(VENC_SETTLE_US);
	writel(VENC_INT_JOB, venc->regs + VENC_INTMASK);

	if (readl_relaxed_poll_timeout(venc->regs + VENC_VERSION, val, val, 10,
				       1000))
		dev_warn(venc->dev, "version register reads 0\n");

	return 0;
}

static u32 histb_venc_pkg_sel(u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_NV21 ? VENC_PKG_V_U : VENC_PKG_U_V;
}

/*
 * Everything the block needs to code one picture, in the order the reference's
 * own configuration routine writes it.
 *
 * The reconstruction slots are the second and the first 1.5-luma blocks of the
 * scratch, which is the pairing the reference programs for the first frame of a
 * channel: its frame index flips before the write, so the picture reads slot 0
 * as its reference and writes slot 1. An all-intra channel never reads either
 * back, but the block writes the reference it is given addresses for.
 */
static void histb_venc_program(struct histb_venc *venc,
			       struct histb_venc_ctx *ctx,
			       const struct histb_venc_job *job)
{
	const struct histb_venc_q *pic = &ctx->out_q;
	unsigned int min_qp = ctx->min_qp->val;
	unsigned int max_qp = ctx->max_qp->val;
	unsigned int stride = ALIGN(pic->width, VENC_MB_ALIGN);
	unsigned int target = max_t(unsigned int,
				    ctx->bitrate->val / VENC_DEFAULT_FPS, 1);
	dma_addr_t base = ctx->stream_dma;
	dma_addr_t strm = base + job->strm_off;
	dma_addr_t rcn_base, ref_base;
	unsigned int i;

	writel(VENC_INT_JOB, venc->regs + VENC_INTMASK);
	writel(VENC_MODE_VAL |
	       FIELD_PREP(VENC_MODE_PKG_SEL, histb_venc_pkg_sel(pic->fourcc)),
	       venc->regs + VENC_MODE);

	writel(job->intra ? VENC_PICFG0_I_VAL : VENC_PICFG0_P_VAL,
	       venc->regs + VENC_PICFG0);
	writel(0, venc->regs + VENC_PICFG1);
	writel(VENC_PICFG2_VAL, venc->regs + VENC_PICFG2);
	writel(VENC_PICFG3_VAL, venc->regs + VENC_PICFG3);
	writel(0, venc->regs + VENC_PICFG4);
	writel(VENC_PICFG5_VAL, venc->regs + VENC_PICFG5);
	writel(0, venc->regs + VENC_ENCUPDATE);

	writel(FIELD_PREP(VENC_IMG_WIDTH, pic->width - 1) |
	       FIELD_PREP(VENC_IMG_HEIGHT, pic->height - 1),
	       venc->regs + VENC_VEDIMGSIZE);
	writel(VENC_PTBITS_VAL, venc->regs + VENC_PTBITS);

	writel(job->slc_hdr, venc->regs + VENC_SLCHDRSTRM);
	for (i = 1; i < VENC_SLCHDR_WORDS; i++)
		writel(0, venc->regs + VENC_SLCHDRSTRM + 4 * i);
	writel((job->intra ? VENC_SLCHPARA_IDR : 0) |
	       FIELD_PREP(VENC_SLCHPARA_PARA_WD, job->para_wd) |
	       FIELD_PREP(VENC_SLCHPARA_REORD_WD, VENC_SLCHPARA_REORD) |
	       FIELD_PREP(VENC_SLCHPARA_MARK_WD,
			  job->intra ? VENC_SLCHPARA_MARK_I : VENC_SLCHPARA_MARK_P),
	       venc->regs + VENC_SLCHDRPARA);

	writel(0, venc->regs + VENC_PTS0);
	writel(VENC_TIMEOUT_VAL, venc->regs + VENC_TIMEOUT);
	writel(VENC_OUTSTD_VAL, venc->regs + VENC_OUTSTD);

	writel(job->src_y, venc->regs + VENC_SRCYADDR);
	writel(job->src_y + (dma_addr_t)pic->bytesperline * pic->height,
	       venc->regs + VENC_SRCCADDR);
	writel(0, venc->regs + VENC_SRCVADDR);
	/*
	 * Two pictures' worth of reconstruction, half the pair read as the reference
	 * and the other half written: the reference's RcnIdx flip per submitted frame
	 * (drv_venc_efl.c:2371, hal_venc.c:379-382).
	 */
	rcn_base = job->rcn +
		(dma_addr_t)job->rcn_sel * 3 * job->rcn_luma / 2;
	ref_base = job->rcn +
		(dma_addr_t)(job->rcn_sel ^ 1) * 3 * job->rcn_luma / 2;
	writel(rcn_base, venc->regs + VENC_RCNYADDR);
	writel(rcn_base + (dma_addr_t)job->rcn_luma,
	       venc->regs + VENC_RCNCADDR);
	writel(ref_base, venc->regs + VENC_REFYADDR);
	writel(ref_base + (dma_addr_t)job->rcn_luma,
	       venc->regs + VENC_REFCADDR);

	writel(FIELD_PREP(VENC_SSTRIDE_Y, pic->bytesperline) |
	       FIELD_PREP(VENC_SSTRIDE_C, pic->bytesperline),
	       venc->regs + VENC_SSTRIDE);
	writel(FIELD_PREP(VENC_RSTRIDE_Y, stride) |
	       FIELD_PREP(VENC_RSTRIDE_C, stride),
	       venc->regs + VENC_RSTRIDE);

	writel(strm, venc->regs + VENC_STRMADDR);
	writel(strm - VENC_STRM_OFF, venc->regs + VENC_SRPTRADDR);
	writel(strm - VENC_STRM_OFF + 16, venc->regs + VENC_SWPTRADDR);
	writel(job->ring_len & VENC_STRMBUFLEN_UNIT,
	       venc->regs + VENC_STRMBUFLEN);

	writel(FIELD_PREP(VENC_MECFG0_HSW, 5) |
	       FIELD_PREP(VENC_MECFG0_REAL_MV, 15), venc->regs + VENC_MECFG0);
	writel(VENC_MECFG1_VAL, venc->regs + VENC_MECFG1);
	for (i = 0; i < ARRAY_SIZE(venc_me_rect); i++)
		writel(venc_me_rect[i], venc->regs + VENC_MERECTCFG + 4 * i);
	writel(FIELD_PREP(VENC_METHR0_TH2, 1500), venc->regs + VENC_METHR0);
	writel(FIELD_PREP(VENC_METHR1_INTRA, 4096), venc->regs + VENC_METHR1);
	writel(0, venc->regs + VENC_MERDOCFG);
	writel(VENC_CREFLDBUR8, venc->regs + VENC_CREFLDMODE);
	writel(0x3c00 | VENC_MDCFG_RDO, venc->regs + VENC_MDCFG);	/* -1024, 1 */

	writel(FIELD_PREP(VENC_QPTHR_MIN, min_qp) |
	       FIELD_PREP(VENC_QPTHR_MAX, max_qp) |
	       FIELD_PREP(VENC_QPTHR_START, max_qp),
	       venc->regs + VENC_QPTHR);
	writel(VENC_RC1_VAL, venc->regs + VENC_RC1);
	for (i = 0; i < ARRAY_SIZE(venc_qp_delta_thr); i++)
		writel(venc_qp_delta_thr[i],
		       venc->regs + VENC_QPDELTATHR + 4 * i);
	writel(target, venc->regs + VENC_TARGETBITS);

	for (i = 0; i < 20; i++)
		writel(venc_mod_lambda[2 * i] |
		       (venc_mod_lambda[2 * i + 1] << 16),
		       venc->regs + VENC_MODLAMBDA + 4 * i);

	for (i = 0; i < VENC_PKG_BLOCK / 2; i++)
		writel(get_unaligned_le32(&venc_scale[4 * i]),
		       venc->regs + VENC_SCALE + 4 * i);

	writel(VENC_ROICFG_VAL, venc->regs + VENC_ROICFG);
	writel(0, venc->regs + VENC_VPP_OSD_CFG);

	writel(0, venc->regs + VENC_START);
	writel(VENC_START_RUN, venc->regs + VENC_START);
}

static void histb_venc_dump(struct histb_venc *venc, const char *when)
{
	dev_dbg(venc->dev, "%s: MODE %#x PICFG0 %#x SIZE %#x PTBITS %#x\n", when,
		readl(venc->regs + VENC_MODE),
		readl(venc->regs + VENC_PICFG0),
		readl(venc->regs + VENC_VEDIMGSIZE),
		readl(venc->regs + VENC_PTBITS));
	dev_dbg(venc->dev, "%s: STRM %#x RP %#x WP %#x LEN %#x QP %#x TGT %#x\n",
		when, readl(venc->regs + VENC_STRMADDR),
		readl(venc->regs + VENC_SRPTRADDR),
		readl(venc->regs + VENC_SWPTRADDR),
		readl(venc->regs + VENC_STRMBUFLEN),
		readl(venc->regs + VENC_QPTHR),
		readl(venc->regs + VENC_TARGETBITS));
	dev_dbg(venc->dev, "%s: SRCY %#x SRCC %#x RCNY %#x REFY %#x SST %#x RST %#x\n",
		when, readl(venc->regs + VENC_SRCYADDR),
		readl(venc->regs + VENC_SRCCADDR),
		readl(venc->regs + VENC_RCNYADDR),
		readl(venc->regs + VENC_REFYADDR),
		readl(venc->regs + VENC_SSTRIDE),
		readl(venc->regs + VENC_RSTRIDE));
	dev_dbg(venc->dev, "%s: SLCHDR %#x PARA %#x SLCHDR7 %#x\n", when,
		readl(venc->regs + VENC_SLCHDRSTRM),
		readl(venc->regs + VENC_SLCHDRPARA),
		readl(venc->regs + VENC_SLCHDRSTRM + 28));
}

static irqreturn_t histb_venc_irq(int irq, void *dev_id)
{
	struct histb_venc *venc = dev_id;
	u32 state = readl(venc->regs + VENC_INTSTAT);

	if (!(state & VENC_INT_JOB))
		return IRQ_NONE;

	writel(state, venc->regs + VENC_INTCLR);
	venc->int_state = state;
	WRITE_ONCE(venc->int_done, true);
	wake_up(&venc->done);

	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Encode job                                                          */
/* ------------------------------------------------------------------ */

/* A refusal travels with the job, so it is tellable by name from the log. */
static int histb_venc_no(struct histb_venc_ctx *ctx, const char *why)
{
	ctx->why = why;
	return -EINVAL;
}

static int histb_venc_stream_get(struct histb_venc_ctx *ctx, size_t want)
{
	size_t need = round_up(want, PAGE_SIZE);
	void *mem;
	dma_addr_t dma;

	if (ctx->stream_alloc >= need)
		return 0;

	if (ctx->stream)
		dma_free_coherent(ctx->venc->dev, ctx->stream_alloc,
				  ctx->stream, ctx->stream_dma);
	ctx->stream = NULL;
	ctx->stream_alloc = 0;

	mem = dma_alloc_coherent(ctx->venc->dev, need, &dma, GFP_KERNEL);
	if (!mem)
		return -ENOMEM;

	ctx->stream = mem;
	ctx->stream_dma = dma;
	ctx->stream_alloc = need;

	return 0;
}

/*
 * Pull the picture out of the ring: walk the packets from the start of the data
 * the driver queued to the offset the block reported, and copy each payload
 * without its header block or its tail padding. The parameter sets the driver
 * wrote itself are picked up by the same walk, so what the client is handed is
 * one Annex-B access unit in the order the block emitted it.
 */
static int histb_venc_collect(struct histb_venc_ctx *ctx, size_t wp, void *dst,
			      size_t avail, size_t *out)
{
	size_t off = 0, total = 0;
	bool last = false;

	while (off < wp) {
		u8 hdr[VENC_PKG_HDR_BYTES];
		size_t pkt, bytes;
		u32 invld, type;

		venc_ring_get(ctx, off, hdr, sizeof(hdr));
		pkt = get_unaligned_le32(hdr + VENC_PKG_LEN);
		invld = get_unaligned_le32(hdr + VENC_PKG_INVLD);
		type = hdr[VENC_PKG_TYPE];
		last = hdr[VENC_PKG_LAST];

		if (pkt < VENC_PKG_HDR_BYTES || pkt % VENC_PKG_HDR_BYTES ||
		    off + pkt > wp) {
			ctx->why = "packet length";
			dev_warn(ctx->venc->dev,
				 "packet at %zu says %zu of %zu used\n", off,
				 pkt, wp);
			return -EIO;
		}

		bytes = pkt - VENC_PKG_HDR_BYTES;
		if (bytes < invld) {
			ctx->why = "packet padding";
			return -EIO;
		}
		bytes -= invld;

		dev_dbg(ctx->venc->dev, "packet %zu: type %u len %zu take %zu%s\n",
			off, type, pkt, bytes, last ? " (last)" : "");

		if (total + bytes > avail) {
			ctx->why = "capture buffer";
			return -ENOSPC;
		}
		venc_ring_get(ctx, off + VENC_PKG_HDR_BYTES, (u8 *)dst + total,
			      bytes);
		total += bytes;
		off += pkt;
	}

	if (!total) {
		ctx->why = "empty picture";
		return -EIO;
	}
	if (!last) {
		ctx->why = "no last packet";
		dev_warn(ctx->venc->dev, "picture ended at %zu with no last flag\n",
			 wp);
		return -EIO;
	}

	*out = total;

	return 0;
}

static int histb_venc_run(struct histb_venc_ctx *ctx, struct vb2_buffer *src,
			  struct vb2_buffer *dst)
{
	struct histb_venc *venc = ctx->venc;
	unsigned int width = ctx->out_q.width, height = ctx->out_q.height;
	unsigned int bpl = ctx->out_q.bytesperline;
	size_t pic_bytes = (size_t)bpl * height * 3 / 2;
	size_t rcn_luma = (size_t)ALIGN(width, VENC_MB_ALIGN) *
			  ALIGN(height, VENC_MB_ALIGN);
	size_t ring_len, pic_off, rcn_off, total = 0;
	u8 sps[VENC_PKG_BLOCK], pps[VENC_PKG_BLOCK];
	struct histb_venc_job job = { 0 };
	size_t sps_bytes, pps_bytes;
	dma_addr_t base;
	u32 wp;
	bool timed_out;
	int ret;

	if (vb2_get_plane_payload(src, 0) < ctx->out_q.size)
		return histb_venc_no(ctx, "picture size");

	sps_bytes = histb_venc_write_sps(sps, width, height);
	pps_bytes = histb_venc_write_pps(pps);
	if (!sps_bytes || !pps_bytes)
		return histb_venc_no(ctx, "parameter sets");

	/*
	 * The ring has to hold the two parameter-set packets the driver queues
	 * before the block is started, and it is the client's CAPTURE size: the
	 * block wraps inside it and reports its own write offset, so a picture
	 * larger than the room left after the parameter sets overruns them.
	 */
	ring_len = round_down(ctx->cap_q.size, VENC_ALIGN_BUFLEN);
	if (ring_len <= 2 * VENC_PKG_BYTES)
		return histb_venc_no(ctx, "capture size");

	/*
	 * STRMADDR is 64-byte aligned and sits above the two pointer words the
	 * block and the driver share; the picture copy and the two reconstruction
	 * frames follow the ring.
	 */
	pic_off = ALIGN(VENC_STRM_OFF + ring_len, VENC_ALIGN_STREAM);
	rcn_off = ALIGN(pic_off + pic_bytes, VENC_ALIGN_STREAM);
	ret = histb_venc_stream_get(ctx, rcn_off + 3 * rcn_luma);
	if (ret) {
		ctx->why = "stream buffer";
		return ret;
	}

	base = ctx->stream_dma;
	if (base & (VENC_ALIGN_FRAME - 1))
		return histb_venc_no(ctx, "stream alignment");

	/* The client's cached picture, where the block can reach it. */
	memcpy((u8 *)ctx->stream + pic_off, vb2_plane_vaddr(src, 0), pic_bytes);

	histb_venc_pkg_param(ctx, 0, sps, sps_bytes, VENC_NAL_SPS);
	histb_venc_pkg_param(ctx, VENC_PKG_BYTES, pps, pps_bytes, VENC_NAL_PPS);

	/* Both pointers are offsets into the ring, and the data starts at 0. */
	put_unaligned_le32(0, ctx->stream);
	put_unaligned_le32(2 * VENC_PKG_BYTES, (u8 *)ctx->stream + 16);

	job.intra = gop <= 1 || !ctx->coded || ctx->frm_in_gop >= gop - 1;
	if (job.intra) {
		ctx->frm_num = 0;
		ctx->idr_pic_id++;
	} else {
		ctx->frm_num = (ctx->frm_num + 1) & 0xf;
	}
	venc_slc_hdr(&job.slc_hdr, &job.para_wd, job.intra, ctx->idr_pic_id,
		     ctx->frm_num);
	job.src_y = base + pic_off;
	job.rcn = base + rcn_off;
	job.rcn_sel = ctx->rcn_sel;
	job.strm_off = VENC_STRM_OFF;
	job.ring_len = ring_len;
	job.rcn_luma = rcn_luma;

	venc->int_state = 0;
	WRITE_ONCE(venc->int_done, false);
	histb_venc_program(venc, ctx, &job);
	histb_venc_dump(venc, "want");

	timed_out = !wait_event_timeout(venc->done, READ_ONCE(venc->int_done),
					VENC_JOB_TIMEOUT);
	/*
	 * A block that never reported is still coding, so it is stopped here rather than
	 * left writing into a ring the next job is about to re programme. The
	 * reference's timeout interrupt does the same pulse (Venc_ISR_TimeOut,
	 * drv_venc_efl.c:5178).
	 */
	if (timed_out) {
		dev_warn(venc->dev, "encode timed out (RAWINT %#x START %#x)\n",
			 readl(venc->regs + VENC_RAWINT),
			 readl(venc->regs + VENC_START));
		histb_venc_reset(venc);
		return -ETIMEDOUT;
	}

	if (venc->int_state & (VENC_INT_BUF_FULL | VENC_INT_PBIT)) {
		dev_info(venc->dev, "stream full or over budget: capture %u is too small (INT %#x)\n",
			 ctx->cap_q.size, venc->int_state);
		return -ENOSPC;
	}

	/* Any other report but the end of the picture is an unfinished block. */
	if (!(venc->int_state & VENC_INT_EOP)) {
		dev_warn(venc->dev, "no end of picture (INT %#x)\n",
			 venc->int_state);
		histb_venc_reset(venc);
		return -EIO;
	}

	/*
	 * The block's own report is the write word below STRMADDR: the offset in
	 * the ring it stopped at. Anything below what the driver queued there
	 * means the picture wrapped, and the parameter sets at the front of the
	 * ring are gone.
	 */
	wp = get_unaligned_le32((u8 *)ctx->stream + 16);
	if (!wp || wp > ring_len || wp < 2 * VENC_PKG_BYTES) {
		ctx->why = "write pointer";
		dev_warn(venc->dev, "write pointer %#x of ring %zu\n", wp,
			 ring_len);
		return -ENOSPC;
	}

	ret = histb_venc_collect(ctx, wp, vb2_plane_vaddr(dst, 0),
				 dst->planes[0].length, &total);
	if (ret)
		return ret;

	/*
	 * The group's position and the reference pair only advance once the picture is
	 * handed over; a job that failed is retried against the same reference, as the
	 * reference driver rolls its counters back when it drops a frame.
	 */
	ctx->frm_in_gop = job.intra ? 0 : ctx->frm_in_gop + 1;
	ctx->rcn_sel ^= 1;
	ctx->coded = true;

	vb2_set_plane_payload(dst, 0, total);
	dev_dbg(venc->dev, "encoded %ux%u q %u-%u -> %zu bytes\n", width, height,
		ctx->min_qp->val, ctx->max_qp->val, total);
	dev_dbg(venc->dev, "act:  PICBITS %#x MEANQP %#x INT %#x\n",
		readl(venc->regs + VENC_PICBITS),
		readl(venc->regs + VENC_MEANQP), venc->int_state);
	histb_venc_dump(venc, "act");

	return 0;
}

/* ------------------------------------------------------------------ */
/* mem2mem                                                             */
/* ------------------------------------------------------------------ */

static void histb_venc_device_run(void *priv)
{
	struct histb_venc_ctx *ctx = priv;
	struct histb_venc *venc = ctx->venc;
	struct vb2_v4l2_buffer *src, *dst;
	enum vb2_buffer_state state;
	int ret;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (!src || !dst) {
		v4l2_m2m_job_finish(venc->m2m_dev, ctx->fh.m2m_ctx);
		return;
	}

	v4l2_m2m_buf_copy_metadata(src, dst);
	/* This driver never holds a CAPTURE buffer across jobs. */
	src->flags &= ~V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;

	mutex_lock(&venc->hw_lock);
	ret = histb_venc_run(ctx, &src->vb2_buf, &dst->vb2_buf);
	mutex_unlock(&venc->hw_lock);

	if (ret)
		dev_dbg(venc->dev, "job refused: %d (%s)\n", ret,
			ctx->why ? : "unspecified");
	ctx->why = NULL;

	state = ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE;
	v4l2_m2m_buf_done_and_job_finish(venc->m2m_dev, ctx->fh.m2m_ctx, state);
}

/*
 * No job_abort(): device_run() is synchronous and leaves both buffers on the
 * ready lists until it completes, so an abort request from VIDIOC_STREAMOFF or
 * close only has to wait for the job already in flight to finish itself.
 */
static const struct v4l2_m2m_ops histb_venc_m2m_ops = {
	.device_run = histb_venc_device_run,
};

/* ------------------------------------------------------------------ */
/* vb2                                                                 */
/* ------------------------------------------------------------------ */

static int histb_venc_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
				  unsigned int *num_planes, unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct histb_venc_ctx *ctx = vq->drv_priv;
	const struct histb_venc_q *q = V4L2_TYPE_IS_OUTPUT(vq->type) ?
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

static int histb_venc_buf_prepare(struct vb2_buffer *vb)
{
	struct histb_venc_ctx *ctx = vb->vb2_queue->drv_priv;

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

static void histb_venc_buf_queue(struct vb2_buffer *vb)
{
	struct histb_venc_ctx *ctx = vb->vb2_queue->drv_priv;

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int histb_venc_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	return 0;
}

static void histb_venc_stop_streaming(struct vb2_queue *vq)
{
	struct histb_venc_ctx *ctx = vq->drv_priv;
	struct vb2_v4l2_buffer *buf;

	do {
		buf = V4L2_TYPE_IS_OUTPUT(vq->type) ?
		      v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx) :
		      v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (buf)
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
	} while (buf);
}

static const struct vb2_ops histb_venc_vb2_ops = {
	.queue_setup = histb_venc_queue_setup,
	.buf_prepare = histb_venc_buf_prepare,
	.buf_queue = histb_venc_buf_queue,
	.start_streaming = histb_venc_start_streaming,
	.stop_streaming = histb_venc_stop_streaming,
};

/* ------------------------------------------------------------------ */
/* Format ioctls                                                       */
/* ------------------------------------------------------------------ */

static int histb_venc_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	struct histb_venc *venc = video_get_drvdata(video_devdata(file));

	strscpy(cap->driver, HISTB_VENC_NAME, sizeof(cap->driver));
	strscpy(cap->card, HISTB_VENC_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(venc->dev));

	return 0;
}

static int histb_venc_enum_fmt_cap(struct file *file, void *priv,
				  struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_H264;

	return 0;
}

static int histb_venc_enum_fmt_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(histb_venc_out_fmts))
		return -EINVAL;

	f->pixelformat = histb_venc_out_fmts[f->index];

	return 0;
}

static bool histb_venc_is_out_fmt(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(histb_venc_out_fmts); i++)
		if (histb_venc_out_fmts[i] == fourcc)
			return true;

	return false;
}

/*
 * The reference gates sizes to a multiple of four within 176x144..1920x1088, so
 * a picture that is not a whole number of macroblocks is coded with the padding
 * columns and rows and cropped back by the parameter set. Both stride fields are
 * 16 bits wide, and the chroma pair plane follows the luma at one stride per
 * half-height row.
 */
static int histb_venc_out_geom(struct v4l2_pix_format *pix,
			       struct histb_venc_q *q)
{
	unsigned int width = round_down(pix->width, VENC_DIM_ALIGN);
	unsigned int height = round_down(pix->height, VENC_DIM_ALIGN);
	unsigned int bpl;

	if (!histb_venc_is_out_fmt(pix->pixelformat))
		return -EINVAL;
	if (width < VENC_MIN_WIDTH || width > VENC_MAX_WIDTH ||
	    height < VENC_MIN_HEIGHT || height > VENC_MAX_HEIGHT)
		return -EINVAL;

	bpl = max(width, round_up(pix->bytesperline ? : width,
				 VENC_ALIGN_FRAME));
	if (bpl > U16_MAX)
		return -EINVAL;

	q->fourcc = pix->pixelformat;
	q->width = width;
	q->height = height;
	q->bytesperline = bpl;
	q->size = (size_t)bpl * height * 3 / 2;

	pix->width = width;
	pix->height = height;
	pix->bytesperline = bpl;
	pix->sizeimage = q->size;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

/*
 * The ring the block writes is what the client is willing to accept, and the
 * picture it belongs to is the one the OUTPUT queue describes. The default is
 * the raw picture size, which is far above what an intra frame spends.
 */
static int histb_venc_cap_geom(const struct histb_venc_q *out,
			       struct v4l2_pix_format *pix,
			       struct histb_venc_q *q)
{
	unsigned int size;

	if (pix->pixelformat != V4L2_PIX_FMT_H264)
		return -EINVAL;
	if (!out->size)
		return -EINVAL;

	size = clamp(pix->sizeimage ? : max_t(unsigned int, out->size,
					      VENC_MIN_STREAM_SIZE),
		     (unsigned int)VENC_MIN_STREAM_SIZE,
		     (unsigned int)VENC_MAX_STREAM_SIZE);

	q->fourcc = pix->pixelformat;
	q->width = out->width;
	q->height = out->height;
	q->bytesperline = 0;
	q->size = round_up(size, VENC_ALIGN_BUFLEN);

	pix->width = out->width;
	pix->height = out->height;
	pix->bytesperline = 0;
	pix->sizeimage = q->size;
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int histb_venc_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct histb_venc_ctx *ctx = file_to_ctx(file);
	const struct histb_venc_q *q = V4L2_TYPE_IS_OUTPUT(f->type) ?
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
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;

	return 0;
}

static int histb_venc_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct histb_venc_ctx *ctx = file_to_ctx(file);
	struct histb_venc_q q;
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;
	if (vb2_is_busy(vq))
		return -EBUSY;

	/*
	 * The two queues are set independently, as on the decoder side: the
	 * picture geometry comes from OUTPUT and the ring bound from CAPTURE, and
	 * a CAPTURE format before an OUTPUT one is refused rather than guessed at.
	 */
	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		struct v4l2_pix_format cap = { 0 };

		ret = histb_venc_out_geom(&f->fmt.pix, &q);
		if (ret)
			return ret;
		ctx->out_q = q;
		/*
		 * The bitstream queue describes the same picture, so a new
		 * geometry re-derives it; what the client asked for as the bound
		 * carries over, being the one thing the picture does not dictate.
		 */
		cap.pixelformat = V4L2_PIX_FMT_H264;
		cap.sizeimage = ctx->cap_q.size;

		return histb_venc_cap_geom(&ctx->out_q, &cap, &ctx->cap_q);
	}

	ret = histb_venc_cap_geom(&ctx->out_q, &f->fmt.pix, &q);
	if (!ret)
		ctx->cap_q = q;

	return ret;
}

static int histb_venc_try_fmt(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	struct histb_venc_ctx *ctx = file_to_ctx(file);
	struct histb_venc_q q;

	if (V4L2_TYPE_IS_OUTPUT(f->type))
		return histb_venc_out_geom(&f->fmt.pix, &q);
	if (f->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	return histb_venc_cap_geom(&ctx->out_q, &f->fmt.pix, &q);
}

static const struct v4l2_ioctl_ops histb_venc_ioctl_ops = {
	.vidioc_querycap = histb_venc_querycap,
	.vidioc_enum_fmt_vid_cap = histb_venc_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out = histb_venc_enum_fmt_out,
	.vidioc_g_fmt_vid_cap = histb_venc_g_fmt,
	.vidioc_g_fmt_vid_out = histb_venc_g_fmt,
	.vidioc_s_fmt_vid_cap = histb_venc_s_fmt,
	.vidioc_s_fmt_vid_out = histb_venc_s_fmt,
	.vidioc_try_fmt_vid_cap = histb_venc_try_fmt,
	.vidioc_try_fmt_vid_out = histb_venc_try_fmt,
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

static int histb_venc_queue_init(void *priv, struct vb2_queue *src_vq,
				 struct vb2_queue *dst_vq);

static int histb_venc_open(struct file *file)
{
	struct histb_venc *venc = video_drvdata(file);
	struct histb_venc_ctx *ctx;
	struct v4l2_ctrl *min_qp, *max_qp, *bitrate;
	int ret;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return -ENOMEM;

	ctx->venc = venc;
	/* Both queues have one format each; the geometry is filled in by S_FMT. */
	ctx->out_q.fourcc = histb_venc_out_fmts[0];
	ctx->cap_q.fourcc = V4L2_PIX_FMT_H264;

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, 3);
	/*
	 * The quantiser window is the one control this block has no equivalent of:
	 * the pair is clamped into the QP range the register file gives, and the
	 * target-bits register is derived from the bit rate the client names.
	 */
	min_qp = v4l2_ctrl_new_std(&ctx->ctrl_hdl, NULL,
				   V4L2_CID_MPEG_VIDEO_H264_MIN_QP, VENC_QP_MIN,
				   VENC_QP_MAX, 1, VENC_QP_DEFAULT);
	max_qp = v4l2_ctrl_new_std(&ctx->ctrl_hdl, NULL,
				   V4L2_CID_MPEG_VIDEO_H264_MAX_QP, VENC_QP_MIN,
				   VENC_QP_MAX, 1, VENC_QP_DEFAULT);
	bitrate = v4l2_ctrl_new_std(&ctx->ctrl_hdl, NULL,
				    V4L2_CID_MPEG_VIDEO_BITRATE, 0,
				    100000000, 1000, VENC_BITRATE_DEFAULT);
	if (ctx->ctrl_hdl.error) {
		ret = ctx->ctrl_hdl.error;
		v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
		v4l2_fh_exit(&ctx->fh);
		kfree(ctx);
		return ret;
	}
	ctx->min_qp = min_qp;
	ctx->max_qp = max_qp;
	ctx->bitrate = bitrate;
	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(venc->m2m_dev, ctx,
					    histb_venc_queue_init);
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

static int histb_venc_release(struct file *file)
{
	struct histb_venc *venc = video_drvdata(file);
	struct v4l2_fh *fh = file->private_data;
	struct histb_venc_ctx *ctx = container_of(fh, struct histb_venc_ctx, fh);

	v4l2_fh_del(fh, file);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_exit(fh);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	if (ctx->stream)
		dma_free_coherent(venc->dev, ctx->stream_alloc, ctx->stream,
				  ctx->stream_dma);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations histb_venc_fops = {
	.owner = THIS_MODULE,
	.open = histb_venc_open,
	.release = histb_venc_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int histb_venc_queue_init(void *priv, struct vb2_queue *src_vq,
				 struct vb2_queue *dst_vq)
{
	struct histb_venc_ctx *ctx = priv;
	unsigned int i;
	struct vb2_queue *vq[2];
	int ret;

	/* Both queues hold a full picture per buffer, so nothing is shared. */
	vq[0] = src_vq;
	vq[1] = dst_vq;
	for (i = 0; i < ARRAY_SIZE(vq); i++) {
		vq[i]->io_modes = VB2_MMAP | VB2_DMABUF;
		vq[i]->drv_priv = ctx;
		vq[i]->ops = &histb_venc_vb2_ops;
		vq[i]->mem_ops = &vb2_dma_contig_memops;
		vq[i]->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		vq[i]->min_queued_buffers = 1;
		vq[i]->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		vq[i]->lock = &ctx->venc->vdev_lock;
		vq[i]->dev = ctx->venc->dev;
		/*
		 * The picture is copied in and the bitstream is copied out in the
		 * kernel, so both queues need a kernel mapping.
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

static const struct of_device_id histb_venc_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-venc" },
	{ .compatible = "hisilicon,hi3716cv200-venc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, histb_venc_of_match);

static int histb_venc_probe(struct platform_device *pdev)
{
	struct histb_venc *venc;
	struct video_device *vdev;
	int ret, irq;

	venc = devm_kzalloc(&pdev->dev, sizeof(*venc), GFP_KERNEL);
	if (!venc)
		return -ENOMEM;

	venc->dev = &pdev->dev;
	platform_set_drvdata(pdev, venc);
	mutex_init(&venc->hw_lock);
	mutex_init(&venc->vdev_lock);
	init_waitqueue_head(&venc->done);

	venc->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(venc->regs))
		return PTR_ERR(venc->regs);

	venc->clk = devm_clk_get(&pdev->dev, "venc");
	if (IS_ERR(venc->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(venc->clk),
				     "no clock\n");

	ret = clk_prepare_enable(venc->clk);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "cannot enable clock\n");

	venc->rst = devm_reset_control_get_exclusive(&pdev->dev, "venc");
	if (IS_ERR(venc->rst)) {
		ret = PTR_ERR(venc->rst);
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

	ret = v4l2_device_register(&pdev->dev, &venc->v4l2_dev);
	if (ret)
		goto err_clk;

	venc->m2m_dev = v4l2_m2m_init(&histb_venc_m2m_ops);
	if (IS_ERR(venc->m2m_dev)) {
		ret = PTR_ERR(venc->m2m_dev);
		v4l2_err(&venc->v4l2_dev, "failed to init mem2mem device\n");
		goto err_v4l2;
	}

	vdev = video_device_alloc();
	if (!vdev) {
		ret = -ENOMEM;
		goto err_m2m;
	}
	venc->vdev = vdev;
	vdev->v4l2_dev = &venc->v4l2_dev;
	vdev->fops = &histb_venc_fops;
	vdev->ioctl_ops = &histb_venc_ioctl_ops;
	vdev->lock = &venc->vdev_lock;
	vdev->release = video_device_release;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	strscpy(vdev->name, HISTB_VENC_NAME, sizeof(vdev->name));
	video_set_drvdata(vdev, venc);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&venc->v4l2_dev, "failed to register video device\n");
		goto err_vdev;
	}

	ret = histb_venc_reset(venc);
	if (ret)
		goto err_unregister;

	ret = devm_request_irq(&pdev->dev, irq, histb_venc_irq, 0,
			       HISTB_VENC_NAME, venc);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq\n");
		goto err_assert;
	}

	dev_info(&pdev->dev, "video encoder, version %#x\n",
		 readl(venc->regs + VENC_VERSION));

	return 0;

err_assert:
	reset_control_assert(venc->rst);
err_unregister:
	video_unregister_device(venc->vdev);
	venc->vdev = NULL;
	/*
	 * video_unregister_device() already drops the last reference, so the
	 * release callback frees the node here; video_device_release() below
	 * is only for a registration that never got that far.
	 */
	goto err_m2m;
err_vdev:
	video_device_release(vdev);
err_m2m:
	v4l2_m2m_release(venc->m2m_dev);
err_v4l2:
	v4l2_device_unregister(&venc->v4l2_dev);
err_clk:
	clk_disable_unprepare(venc->clk);
	return ret;
}

static void histb_venc_remove(struct platform_device *pdev)
{
	struct histb_venc *venc = platform_get_drvdata(pdev);

	video_unregister_device(venc->vdev);
	v4l2_m2m_release(venc->m2m_dev);
	v4l2_device_unregister(&venc->v4l2_dev);
	reset_control_assert(venc->rst);
	clk_disable_unprepare(venc->clk);
}

static struct platform_driver histb_venc_driver = {
	.probe = histb_venc_probe,
	.remove = histb_venc_remove,
	.driver = {
		.name = HISTB_VENC_NAME,
		.of_match_table = histb_venc_of_match,
	},
};
module_platform_driver(histb_venc_driver);

MODULE_DESCRIPTION("HiSilicon Hi3798MV100 H.264 encoder driver");
MODULE_LICENSE("GPL");
