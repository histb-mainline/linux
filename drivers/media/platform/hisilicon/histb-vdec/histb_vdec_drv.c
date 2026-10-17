// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver core for the HiSilicon Hi3798MV100 stateless video decoder (VDH).
 *
 * The VDM decodes one frame at a time from a downstream message pool that the
 * driver fills with fully parsed syntax submitted through V4L2 stateless
 * controls. A job is the pair of one OUTPUT (coded slice data) and one CAPTURE
 * (16x16-tiled NV21) buffer; dev->hw_lock is held from job start to job end and
 * a watchdog recovers the hardware through a reset pulse when the frame-done
 * interrupt never arrives.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <media/media-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "histb_vdec.h"

/* Protocol descriptors exported by the codec files. */

/*
 * The codec descriptors are extern objects, so C can not aggregate them into
 * the by-value histb_vdec_fmts[] array the driver contract declares; the list
 * is carried as pointers instead.
 */
static const struct histb_vdec_fmt_desc *const histb_vdec_fmt_list[] = {
	&histb_vdec_h264_fmt,
	&histb_vdec_hevc_fmt,
	&histb_vdec_mpeg2_fmt,
	&histb_vdec_mpeg4_fmt,
	&histb_vdec_h263_fmt,
	/*
	 * VP8 is implemented (histb_vdec_vp8.c) but deliberately not
	 * registered: the VDM seeds its macroblock-layer bool decoder from a
	 * live 24-bit residue plus a count that depends on the symbol history
	 * (measured on the board: with both injected, the reference frame
	 * reproduces fully), and struct v4l2_vp8_entropy_coder_state carries
	 * only 8 bits of value, so no driver can compute the seed from the
	 * control. Doing it properly needs that control to carry the full live
	 * coder value and the count its header parse stops at.
	 */
};

/*
 * struct histb_vdec_dev and struct histb_vdec_ctx are fixed by the shared
 * driver contract; the record of the frame in flight, the locks they do not
 * carry and the per-open format state live in these wrappers, which own the
 * allocations.
 */
struct histb_vdec_job {
	struct histb_vdec_ctx	*ctx;
	unsigned int		gen;
};

struct histb_vdec_core {
	struct histb_vdec_dev	vdec;
	/* guards vdec.job_running and @job against the IRQ thread */
	spinlock_t		job_lock;
	struct histb_vdec_job	job;
	unsigned int		job_gen;
	/* serialises V4L2 ioctls: video_device and vb2 queue lock */
	struct mutex		vdev_lock;
};

struct histb_vdec_fh {
	struct histb_vdec_ctx	ctx;
	struct v4l2_format	out_fmt;
	struct v4l2_format	cap_fmt;
};

static struct histb_vdec_core *histb_vdec_core_of(struct histb_vdec_dev *dev)
{
	return container_of(dev, struct histb_vdec_core, vdec);
}

static struct histb_vdec_dev *histb_vdec_dev_of(struct histb_vdec_ctx *ctx)
{
	struct v4l2_device *v4l2_dev = ctx->fh.vdev->v4l2_dev;

	return container_of(v4l2_dev, struct histb_vdec_dev, v4l2_dev);
}

static struct histb_vdec_fh *histb_vdec_fh_of(struct histb_vdec_ctx *ctx)
{
	return container_of(ctx, struct histb_vdec_fh, ctx);
}

static struct histb_vdec_ctx *file_to_ctx(struct file *filp)
{
	return &container_of(filp->private_data, struct histb_vdec_fh,
			     ctx.fh)->ctx;
}

/* ---------------------------------------------------------------- formats */

static unsigned int histb_vdec_stride(const struct histb_vdec_ctx *ctx,
				      unsigned int width)
{
	return ctx->desc->stride(width);
}

static unsigned int histb_vdec_frame_size(const struct histb_vdec_ctx *ctx,
					  unsigned int width,
					  unsigned int height)
{
	return histb_vdec_frame_bytes(histb_vdec_stride(ctx, width),
				      ALIGN(height, ctx->desc->grid_align));
}

static void histb_vdec_set_frame_size(struct histb_vdec_ctx *ctx,
				      struct v4l2_format *f,
				      unsigned int width, unsigned int height)
{
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;

	pix->width = width;
	pix->height = height;
	pix->plane_fmt[0].bytesperline = histb_vdec_stride(ctx, width);
	pix->plane_fmt[0].sizeimage = histb_vdec_frame_size(ctx, width, height);
}

static void histb_vdec_reset_fmts(struct histb_vdec_ctx *ctx)
{
	struct histb_vdec_fh *fh = histb_vdec_fh_of(ctx);
	struct v4l2_format *out = &fh->out_fmt;
	struct v4l2_format *cap = &fh->cap_fmt;

	ctx->desc = histb_vdec_fmt_list[0];

	memset(out, 0, sizeof(*out));
	out->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	out->fmt.pix_mp.pixelformat = histb_vdec_fmt_list[0]->fourcc;
	out->fmt.pix_mp.field = V4L2_FIELD_NONE;
	out->fmt.pix_mp.num_planes = 1;
	out->fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	out->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	out->fmt.pix_mp.quantization = V4L2_QUANTIZATION_DEFAULT;
	out->fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	histb_vdec_set_frame_size(ctx, out, HISTB_VDEC_MAX_WIDTH, 1088);
	out->fmt.pix_mp.plane_fmt[0].sizeimage = HISTB_VDEC_OUTPUT_SIZE_DEFAULT;

	memset(cap, 0, sizeof(*cap));
	cap->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	cap->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV21_16L16;
	cap->fmt.pix_mp.field = V4L2_FIELD_NONE;
	cap->fmt.pix_mp.num_planes = 1;
	cap->fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	cap->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	cap->fmt.pix_mp.quantization = V4L2_QUANTIZATION_DEFAULT;
	cap->fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_DEFAULT;
	histb_vdec_set_frame_size(ctx, cap, out->fmt.pix_mp.width,
					  out->fmt.pix_mp.height);
}

static void histb_vdec_reset_capture_fmt(struct histb_vdec_ctx *ctx)
{
	struct histb_vdec_fh *fh = histb_vdec_fh_of(ctx);
	const struct v4l2_pix_format_mplane *out = &fh->out_fmt.fmt.pix_mp;
	struct v4l2_format *cap = &fh->cap_fmt;

	cap->fmt.pix_mp.field = V4L2_FIELD_NONE;
	/* Propagate the colourspace information of the bitstream to capture. */
	cap->fmt.pix_mp.colorspace = out->colorspace;
	cap->fmt.pix_mp.ycbcr_enc = out->ycbcr_enc;
	cap->fmt.pix_mp.quantization = out->quantization;
	cap->fmt.pix_mp.xfer_func = out->xfer_func;
	histb_vdec_set_frame_size(ctx, cap, out->width, out->height);
}

static const struct histb_vdec_fmt_desc *
histb_vdec_find_fmt(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(histb_vdec_fmt_list); i++) {
		if (histb_vdec_fmt_list[i]->fourcc == fourcc)
			return histb_vdec_fmt_list[i];
	}

	return NULL;
}

static int histb_vdec_querycap(struct file *file, void *priv,
			       struct v4l2_capability *cap)
{
	struct histb_vdec_dev *dev = video_get_drvdata(video_devdata(file));

	strscpy(cap->driver, HISTB_VDEC_NAME, sizeof(cap->driver));
	strscpy(cap->card, HISTB_VDEC_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 dev_name(dev->dev));
	return 0;
}

static int histb_vdec_enum_fmt_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_NV21_16L16;
	strscpy(f->description, "16x16-tiled NV21", sizeof(f->description));
	return 0;
}

static int histb_vdec_enum_fmt_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(histb_vdec_fmt_list))
		return -EINVAL;

	f->pixelformat = histb_vdec_fmt_list[f->index]->fourcc;
	return 0;
}

static int histb_vdec_try_fmt_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct histb_vdec_ctx *ctx = file_to_ctx(file);
	struct histb_vdec_fh *fh = histb_vdec_fh_of(ctx);
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;
	unsigned int width, height;

	if (pix->pixelformat != V4L2_PIX_FMT_NV21_16L16)
		pix->pixelformat = V4L2_PIX_FMT_NV21_16L16;

	/* A decoded frame can not be smaller than the stream resolution. */
	width = min_t(u32, pix->width, HISTB_VDEC_MAX_WIDTH);
	height = min_t(u32, pix->height, HISTB_VDEC_MAX_HEIGHT);
	width = ALIGN(width, 16);
	height = ALIGN(height, 16);
	width = max(width, fh->out_fmt.fmt.pix_mp.width);
	height = max(height, fh->out_fmt.fmt.pix_mp.height);
	pix->width = width;
	pix->height = height;

	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = 1;
	histb_vdec_set_frame_size(ctx, f, pix->width, pix->height);
	return 0;
}

static int histb_vdec_try_fmt_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix = &f->fmt.pix_mp;

	if (!histb_vdec_find_fmt(pix->pixelformat))
		pix->pixelformat = histb_vdec_fmt_list[0]->fourcc;

	pix->field = V4L2_FIELD_NONE;
	pix->num_planes = 1;
	pix->width = ALIGN(min_t(u32, pix->width, HISTB_VDEC_MAX_WIDTH), 16);
	pix->height = ALIGN(min_t(u32, pix->height, HISTB_VDEC_MAX_HEIGHT), 16);

	/*
	 * The bitstream is consumed by the stream fetcher, not a plane
	 * formatter; the nominal size only bounds the buffer allocation and
	 * userspace may size it below the default.
	 */
	if (!pix->plane_fmt[0].sizeimage)
		pix->plane_fmt[0].sizeimage = HISTB_VDEC_OUTPUT_SIZE_DEFAULT;
	return 0;
}

static int histb_vdec_g_fmt_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct histb_vdec_ctx *ctx = file_to_ctx(file);

	*f = histb_vdec_fh_of(ctx)->cap_fmt;
	return 0;
}

static int histb_vdec_g_fmt_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct histb_vdec_ctx *ctx = file_to_ctx(file);

	*f = histb_vdec_fh_of(ctx)->out_fmt;
	return 0;
}

static int histb_vdec_s_fmt_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct histb_vdec_ctx *ctx = file_to_ctx(file);
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
			     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ret = histb_vdec_try_fmt_cap(file, priv, f);
	if (ret)
		return ret;

	histb_vdec_fh_of(ctx)->cap_fmt = *f;
	return 0;
}

static int histb_vdec_s_fmt_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct histb_vdec_ctx *ctx = file_to_ctx(file);
	struct histb_vdec_fh *fh = histb_vdec_fh_of(ctx);
	const struct histb_vdec_fmt_desc *desc;
	struct vb2_queue *vq, *peer_vq;
	int ret;

	/*
	 * A protocol change resets the per-protocol hardware state, and the
	 * frame slot geometry follows the stream resolution, so neither can
	 * change once the queues are in use.
	 */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
			     V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (vb2_is_streaming(vq) ||
	    (vb2_is_busy(vq) &&
	     f->fmt.pix_mp.pixelformat != fh->out_fmt.fmt.pix_mp.pixelformat))
		return -EBUSY;

	peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
				  V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(peer_vq))
		return -EBUSY;

	/*
	 * An unrecognised fourcc is snapped to the first descriptor by
	 * ->try_fmt() and reported back through the same buffer, which is what
	 * rkvdec does too (rkvdec.c:706); the client is expected to read the
	 * negotiated format. A bench that asked for HEVC through a misspelled
	 * fourcc therefore got 'S264' and drove the H.264 codec ops quietly.
	 */
	ret = histb_vdec_try_fmt_out(file, priv, f);
	if (ret)
		return ret;

	desc = histb_vdec_find_fmt(f->fmt.pix_mp.pixelformat);
	/*
	 * Defensive: histb_vdec_try_fmt_out() above snapped the client's fourcc
	 * onto a registered descriptor, so this lookup only returns NULL if a
	 * negotiable format were ever missing from histb_vdec_fmt_list[];
	 * refuse rather than store a NULL desc whose codec ops every later job
	 * would dereference.
	 */
	if (!desc)
		return -EINVAL;

	if (ctx->desc != desc) {
		ctx->desc = desc;
		/* The dpb -> pmv slot map is per-protocol. */
		memset(ctx->pmv_map, 0, sizeof(ctx->pmv_map));
		ctx->pmv_next = 0;
	}

	fh->out_fmt = *f;
	histb_vdec_reset_capture_fmt(ctx);
	return 0;
}

static int histb_vdec_enum_framesizes(struct file *file, void *priv,
				      struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index)
		return -EINVAL;

	if (fsize->pixel_format != V4L2_PIX_FMT_NV21_16L16 &&
	    !histb_vdec_find_fmt(fsize->pixel_format))
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise.min_width = 16;
	/*
	 * An H.264 picture one macroblock column wide cannot be delivered at
	 * all: histb_vdec_h264_run() refuses it because the shared luma write
	 * path needs two macroblock columns (measured on the board -- a 32-wide
	 * stream declared as one column reproduces the 16x16 failure, and the
	 * same 16x16 stream declared as two columns decodes byte-exactly; the
	 * 16x16 stream itself was then seen refused, kicks=0). So the coded
	 * format advertises the floor the jobs actually clear, while a decoded
	 * frame may still be 16 wide.
	 */
	if (fsize->pixel_format == V4L2_PIX_FMT_H264_SLICE)
		fsize->stepwise.min_width = 32;
	fsize->stepwise.max_width = HISTB_VDEC_MAX_WIDTH;
	fsize->stepwise.step_width = 16;
	fsize->stepwise.min_height = 16;
	fsize->stepwise.max_height = HISTB_VDEC_MAX_HEIGHT;
	fsize->stepwise.step_height = 16;
	return 0;
}

static int histb_vdec_g_selection(struct file *file, void *priv,
				  struct v4l2_selection *s)
{
	struct histb_vdec_ctx *ctx = file_to_ctx(file);
	const struct v4l2_pix_format_mplane *pix;

	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE &&
	    s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		return -EINVAL;

	switch (s->target) {
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		break;
	default:
		return -EINVAL;
	}

	/*
	 * The stateless controls carry no cropping information, so the coded
	 * size is the visible size here; tiles past it are never written.
	 */
	pix = &histb_vdec_fh_of(ctx)->cap_fmt.fmt.pix_mp;
	s->r.left = 0;
	s->r.top = 0;
	s->r.width = pix->width;
	s->r.height = pix->height;
	return 0;
}

/* ------------------------------------------------------------ vb2 queues */

static int histb_vdec_queue_setup(struct vb2_queue *vq,
				  unsigned int *num_buffers,
				  unsigned int *num_planes,
				  unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct histb_vdec_ctx *ctx = vb2_get_drv_priv(vq);
	struct histb_vdec_fh *fh = histb_vdec_fh_of(ctx);
	const struct v4l2_format *f;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		f = &fh->out_fmt;
	else
		f = &fh->cap_fmt;

	if (*num_planes) {
		if (*num_planes != 1)
			return -EINVAL;
		if (sizes[0] < f->fmt.pix_mp.plane_fmt[0].sizeimage)
			return -EINVAL;
		return 0;
	}

	/*
	 * The picture message names references through a fixed table of APC
	 * slots, so a capture queue deeper than that table could never have all
	 * of its buffers addressable as references. Clamping the count is
	 * enough here: VIDIOC_REQBUFS documents that the driver stores the
	 * number actually granted in count, which may be smaller than asked
	 * for, so the client sees the depth it got.
	 */
	if (!V4L2_TYPE_IS_OUTPUT(vq->type) &&
	    *num_buffers > HISTB_VDEC_MAX_APC_SLOTS)
		*num_buffers = HISTB_VDEC_MAX_APC_SLOTS;

	*num_planes = 1;
	sizes[0] = f->fmt.pix_mp.plane_fmt[0].sizeimage;
	return 0;
}

static int histb_vdec_buf_prepare(struct vb2_buffer *vb)
{
	struct histb_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct histb_vdec_fh *fh = histb_vdec_fh_of(ctx);
	const struct v4l2_format *f;

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type))
		f = &fh->out_fmt;
	else
		f = &fh->cap_fmt;

	if (vb2_plane_size(vb, 0) < f->fmt.pix_mp.plane_fmt[0].sizeimage)
		return -EINVAL;

	if (V4L2_TYPE_IS_OUTPUT(vb->vb2_queue->type)) {
		/*
		 * The bitstream segment descriptors carry the length in 24
		 * bits; anything larger would wrap around silently.
		 */
		if (vb2_get_plane_payload(vb, 0) > BIT(24) - 1)
			return -EINVAL;
		return 0;
	}

	/* CAPTURE buffers are always fully written by the VDM. */
	vb2_set_plane_payload(vb, 0, f->fmt.pix_mp.plane_fmt[0].sizeimage);

	return 0;
}

static int histb_vdec_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

static void histb_vdec_buf_queue(struct vb2_buffer *vb)
{
	struct histb_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void histb_vdec_buf_request_complete(struct vb2_buffer *vb)
{
	struct histb_vdec_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_hdl);
}

static int histb_vdec_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct histb_vdec_ctx *ctx = vb2_get_drv_priv(q);

	if (!V4L2_TYPE_IS_OUTPUT(q->type))
		return 0;

	if (ctx->desc->ops->start)
		return ctx->desc->ops->start(ctx);

	return 0;
}

static void histb_vdec_queue_cleanup(struct histb_vdec_ctx *ctx,
				     struct vb2_queue *q, u32 state)
{
	while (true) {
		struct vb2_v4l2_buffer *vbuf;

		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (!vbuf)
			break;

		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req,
					   &ctx->ctrl_hdl);
		v4l2_m2m_buf_done(vbuf, state);
	}
}

static void histb_vdec_stop_streaming(struct vb2_queue *q)
{
	struct histb_vdec_ctx *ctx = vb2_get_drv_priv(q);

	/*
	 * The mem2mem core drains the running job before stop_streaming can
	 * run, so the hardware is idle here.
	 */
	if (V4L2_TYPE_IS_OUTPUT(q->type) && ctx->desc->ops->stop)
		ctx->desc->ops->stop(ctx);

	histb_vdec_queue_cleanup(ctx, q, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops histb_vdec_queue_ops = {
	.queue_setup = histb_vdec_queue_setup,
	.buf_prepare = histb_vdec_buf_prepare,
	.buf_out_validate = histb_vdec_buf_out_validate,
	.buf_queue = histb_vdec_buf_queue,
	.buf_request_complete = histb_vdec_buf_request_complete,
	.start_streaming = histb_vdec_start_streaming,
	.stop_streaming = histb_vdec_stop_streaming,
};

static int histb_vdec_queue_init(void *priv, struct vb2_queue *src_vq,
				 struct vb2_queue *dst_vq)
{
	struct histb_vdec_ctx *ctx = priv;
	struct histb_vdec_dev *dev = histb_vdec_dev_of(ctx);
	struct histb_vdec_core *core = histb_vdec_core_of(dev);
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &histb_vdec_queue_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	/*
	 * The driver reads the bitstream in the kernel (slice delimitation, the
	 * VP8 bool-decoder seed), so the plane must keep a kernel mapping;
	 * DMA_ATTR_NO_KERNEL_MAPPING would leave vb2_plane_vaddr() NULL.
	 */
	src_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &core->vdev_lock;
	src_vq->dev = dev->v4l2_dev.dev;
	src_vq->supports_requests = true;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &histb_vdec_queue_ops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	/* Frame slots are the hardware dpb; only the VDM touches them. */
	dst_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES |
			    DMA_ATTR_NO_KERNEL_MAPPING;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &core->vdev_lock;
	dst_vq->dev = dev->v4l2_dev.dev;
	/*
	 * The downstream message tables reference frames through the 16-slot
	 * APC/POC arrays; buffer index is the APC slot, so more buffers would
	 * be impossible to address. vb2 core rejects max_num_buffers <
	 * VB2_MAX_FRAME, so the cap is enforced in ->queue_setup() instead;
	 * that makes VIDIOC_CREATE_BUFS clamp to the queue's own buffer ceiling
	 * rather than reaching 32, the same shape the in-tree st/delta driver
	 * uses.
	 */
	dst_vq->min_queued_buffers = 4;

	return vb2_queue_init(dst_vq);
}

int histb_vdec_cap_dims(struct histb_vdec_ctx *ctx, unsigned int *w,
			unsigned int *h)
{
	const struct v4l2_pix_format_mplane *pix =
		&histb_vdec_fh_of(ctx)->cap_fmt.fmt.pix_mp;

	*w = pix->width;
	*h = pix->height;
	return 0;
}

/**
 * histb_vdec_buf_geom() - the frame-buffer layout the CAPTURE format promises
 * @ctx: decoding context
 * @stride: receives the tile stride in bytes
 * @lines: receives the luma row count the chroma plane sits behind
 *
 * Both numbers are the ones histb_vdec_set_frame_size() put into
 * @bytesperline and @sizeimage, so the tile write path can only land its
 * payload where a client reading the format says it is. Deriving them from the
 * coded picture instead (as every protocol used to) makes a negotiated
 * rectangle larger than the stream deliver a sheared frame: the uAPI ties the
 * reported geometry to the frame buffer itself
 * (Documentation/userspace-api/media/v4l/dev-stateless-decoder.rst:119-120),
 * and rkvdec programs its strides from the decoded format for the same reason
 * (rkvdec-h264.c:256-262). The engine's picture-size registers keep the coded
 * dimensions, so nothing is decoded beyond the coded picture; a picture larger
 * than the negotiation is refused by each protocol's own gate.
 */
void histb_vdec_buf_geom(struct histb_vdec_ctx *ctx, unsigned int *stride,
			 unsigned int *lines)
{
	unsigned int w, h;

	histb_vdec_cap_dims(ctx, &w, &h);
	*stride = histb_vdec_stride(ctx, w);
	*lines = ALIGN(h, ctx->desc->grid_align);
}

/* ------------------------------------------------------------ job plumbing */

/**
 * histb_vdec_job_take() - atomically claim the job slot
 * @dev: decoder device
 * @out: filled with the job record on success
 *
 * Completion paths (IRQ thread, watchdog, failed assembly) race for the single
 * slot; the first caller that finds it running wins and empties the record in
 * the same critical section, so late completers of a stale frame are a harmless
 * no-op. The completion itself runs lock-free: hw_lock only covers assembly,
 * and the mem2mem core owns the job transition.
 *
 * Return: true when the caller owns the completion.
 */
static bool histb_vdec_job_take(struct histb_vdec_dev *dev,
				struct histb_vdec_job *out)
{
	struct histb_vdec_core *core = histb_vdec_core_of(dev);
	unsigned long flags;

	spin_lock_irqsave(&core->job_lock, flags);
	if (!dev->job_running) {
		spin_unlock_irqrestore(&core->job_lock, flags);
		return false;
	}
	*out = core->job;
	dev->job_running = false;
	core->job = (struct histb_vdec_job){};
	spin_unlock_irqrestore(&core->job_lock, flags);
	return true;
}

/**
 * histb_vdec_job_complete() - return the buffers and close the job
 * @dev: decoder device
 * @job: job record claimed with histb_vdec_job_take()
 * @state: state to complete both buffers with
 *
 * The mem2mem core picks the buffers of this context's job off its ready lists,
 * so a completion that only called v4l2_m2m_buf_done() would leave the buffers
 * queued and the core would immediately start the same frame again.
 */
static void histb_vdec_job_complete(struct histb_vdec_dev *dev,
				    const struct histb_vdec_job *job,
				    enum vb2_buffer_state state)
{
	v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, job->ctx->fh.m2m_ctx,
					 state);
	pm_runtime_put_autosuspend(dev->dev);
}

static unsigned int histb_vdec_count_slices(u32 state)
{
	unsigned int num = FIELD_GET(VDM_STATE_SLICE_NUM, state);

	/*
	 * The upstream pool carries one 4-word report per decoded slice; the
	 * per-slice status fields are not interpreted yet, the count only feeds
	 * the debug output.
	 */
	return min_t(unsigned int, num, HISTB_VDEC_MAX_UP_SLICES);
}

/**
 * histb_vdec_irq_handler() - IRQ thread function, finishes the frame
 * @irq: interrupt number
 * @priv: decoder device
 *
 * Runs as the threaded half of the frame-done interrupt: reads the decoded
 * slice count from the VDM state word, cancels the watchdog and completes the
 * job. A completion without a job record belongs to the watchdog, which has
 * already finished the frame.
 */
irqreturn_t histb_vdec_irq_handler(int irq, void *priv)
{
	struct histb_vdec_dev *dev = priv;
	struct histb_vdec_job job;
	unsigned int slices;
	u32 state;

	/*
	 * Use the state the primary handler captured: clearing INT_STATE drops
	 * the latched frame-done condition, so a re-read here would already
	 * show an idle VDM and the job would never be completed.
	 */
	state = dev->irq_state;
	if (!(state & VDM_STATE_DEC_OVER))
		return IRQ_HANDLED;

	if (!histb_vdec_job_take(dev, &job))
		return IRQ_HANDLED;

	slices = histb_vdec_count_slices(state);
	dev_dbg(dev->dev, "frame done: %u slices%s\n", slices,
		(state & VDM_STATE_DEC_ERR) ? ", decode error" : "");

	/*
	 * A picture the engine itself flagged as failed leaves the VDM pipeline
	 * in a state the next frame cannot be programmed out of: measured on
	 * the board, an MPEG-2 picture that ended with DEC_ERR set made every
	 * subsequent frame hang (H.264 alone 4/4 clean, H.264 right after one
	 * MPEG-2 picture 8/8 timed out with the engine not reading a single
	 * macroblock), and only the CRG reset recovered it. Reset here so an
	 * errored stream cannot poison the next job, whatever opened it.
	 */
	if (state & VDM_STATE_DEC_ERR)
		histb_vdec_hw_reset(dev);

	cancel_delayed_work(&dev->watchdog);
	histb_vdec_job_complete(dev, &job,
				(state & VDM_STATE_DEC_ERR) ?
				VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	return IRQ_HANDLED;
}

/*
 * Claim the frame-done interrupt for the threaded handler: the VDM latches the
 * condition until the interrupt state register is cleared, which is also what
 * deasserts the level-triggered line.
 */
static bool histb_vdec_hw_irq_status(struct histb_vdec_dev *dev, u32 *state)
{
	u32 val = vdm_read(dev, VDM_STATE);

	if (!(val & VDM_STATE_DEC_OVER))
		return false;

	*state = val;
	vdm_write(dev, VDM_INT_STATE, 0xffffffff);
	return true;
}

static irqreturn_t histb_vdec_hardirq(int irq, void *priv)
{
	struct histb_vdec_dev *dev = priv;
	u32 state;

	if (!histb_vdec_hw_irq_status(dev, &state))
		return IRQ_NONE;

	dev->irq_state = state;
	return IRQ_WAKE_THREAD;
}

static void histb_vdec_watchdog(struct work_struct *work)
{
	struct histb_vdec_dev *dev =
		container_of(to_delayed_work(work), struct histb_vdec_dev,
			     watchdog);
	struct histb_vdec_job job;

	if (!histb_vdec_job_take(dev, &job))
		return;

	/* Claimed: the frame is ours to recover, and no IRQ thread can complete
	 * it from under us any more.
	 *
	 * The state and the per-picture counters have to be read here: the
	 * reset below wipes them, and a timeout whose engine never asked for a
	 * single read is a different bug from one that decoded the whole
	 * picture without ever raising end-of-video.
	 */
	dev_err(dev->dev,
		"frame decode timed out (state=%#010x int=%#010x cycle=%u rd=%u wr=%u), resetting the VDM\n",
		vdm_read(dev, VDM_STATE), vdm_read(dev, VDM_INT_STATE),
		vdm_read(dev, VDM_DEC_CYCLE_PER_PIC),
		vdm_read(dev, VDM_RD_REQ_PER_PIC),
		vdm_read(dev, VDM_WR_REQ_PER_PIC));
	histb_vdec_hw_reset(dev);
	histb_vdec_job_complete(dev, &job, VB2_BUF_STATE_ERROR);
}

static void histb_vdec_device_run(void *priv)
{
	struct histb_vdec_ctx *ctx = priv;
	struct histb_vdec_dev *dev = histb_vdec_dev_of(ctx);
	struct histb_vdec_core *core = histb_vdec_core_of(dev);
	struct histb_vdec_job job_record;
	struct vb2_v4l2_buffer *src, *dst;
	unsigned int gen;
	unsigned long flags;
	int ret;

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	if (WARN_ON(!src) || WARN_ON(!dst))
		return;

	ret = pm_runtime_resume_and_get(dev->dev);
	if (ret) {
		dev_dbg(dev->dev, "pm resume failed: %d\n", ret);
		goto err_finish_no_pm;
	}

	mutex_lock(&dev->hw_lock);

	spin_lock_irqsave(&core->job_lock, flags);
	if (WARN_ON(dev->job_running)) {
		spin_unlock_irqrestore(&core->job_lock, flags);
		mutex_unlock(&dev->hw_lock);
		pm_runtime_put_autosuspend(dev->dev);
		return;
	}
	dev->job_running = true;
	core->job_gen++;
	core->job = (struct histb_vdec_job){ ctx, core->job_gen };
	gen = core->job.gen;
	spin_unlock_irqrestore(&core->job_lock, flags);

	/*
	 * Apply the controls attached to the source buffer's request; the
	 * request is completed right away because the message assembly below
	 * has already consumed the control values.
	 */
	ret = v4l2_ctrl_request_setup(src->vb2_buf.req_obj.req,
				      &ctx->ctrl_hdl);
	if (ret) {
		dev_dbg(dev->dev, "request setup failed: %d\n", ret);
		goto err_job;
	}

	v4l2_m2m_buf_copy_metadata(src, dst);

	ret = ctx->desc->ops->run(ctx, src, dst);
	if (ret) {
		dev_dbg(dev->dev, "run refused: %d (out fourcc %4.4s)\n", ret,
			(char *)&ctx->desc->fourcc);
		/*
		 * A refusal must be readable without the buffer flag. Measured
		 * on the board, these streams come back with
		 * V4L2_BUF_FLAG_ERROR set on every capture buffer - the
		 * pictures that decoded byte-exactly included - so the flag
		 * cannot separate "the engine never saw this picture" from "the
		 * engine says it errored". Zeroing the payload does, and the
		 * buffer keeps its previous contents underneath: dropping them
		 * is the client's job once bytesused says nothing was written.
		 */
		vb2_set_plane_payload(&dst->vb2_buf, 0, 0);
		goto err_job;
	}

	/*
	 * The message assembly above consumed this request's control values, so
	 * its control object can be completed right away; the buffer object is
	 * completed when the job finishes.
	 */
	v4l2_ctrl_request_complete(src->vb2_buf.req_obj.req, &ctx->ctrl_hdl);

	/* hw_lock only guards the assembly and the doorbell; completion paths
	 * never take it, so it must not stay held across the wait.
	 */
	mutex_unlock(&dev->hw_lock);
	schedule_delayed_work(&dev->watchdog, HISTB_VDEC_TIMEOUT);
	dev_dbg(dev->dev, "job %u programmed\n", gen);
	return;

err_job:
	v4l2_ctrl_request_complete(src->vb2_buf.req_obj.req, &ctx->ctrl_hdl);
	mutex_unlock(&dev->hw_lock);
	if (histb_vdec_job_take(dev, &job_record))
		histb_vdec_job_complete(dev, &job_record,
					VB2_BUF_STATE_ERROR);
	return;

err_finish_no_pm:
	v4l2_m2m_buf_done_and_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx,
					 VB2_BUF_STATE_ERROR);
}

static const struct v4l2_m2m_ops histb_vdec_m2m_ops = {
	.device_run = histb_vdec_device_run,
};

/* --------------------------------------------------------------- controls */

static int histb_vdec_try_ctrl(struct v4l2_ctrl *ctrl)
{
	struct histb_vdec_ctx *ctx =
		container_of(ctrl->handler, struct histb_vdec_ctx, ctrl_hdl);

	if (ctx->desc->ops->try_ctrl)
		return ctx->desc->ops->try_ctrl(ctrl);

	return 0;
}

static const struct v4l2_ctrl_ops histb_vdec_ctrl_ops = {
	.try_ctrl = histb_vdec_try_ctrl,
};

/*
 * The control handlers of all file handles carry the controls of every
 * protocol, like rkvdec; a protocol switch then never needs a handler rebuild.
 */
static int histb_vdec_init_ctrls(struct histb_vdec_ctx *ctx)
{
	unsigned int i, j, nctrls = 0;
	int ret;

	for (i = 0; i < ARRAY_SIZE(histb_vdec_fmt_list); i++)
		nctrls += histb_vdec_fmt_list[i]->num_ctrls;

	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, nctrls);

	for (i = 0; i < ARRAY_SIZE(histb_vdec_fmt_list); i++) {
		for (j = 0; j < histb_vdec_fmt_list[i]->num_ctrls; j++) {
			struct v4l2_ctrl_config cfg =
				histb_vdec_fmt_list[i]->ctrls[j];

			cfg.ops = &histb_vdec_ctrl_ops;
			v4l2_ctrl_new_custom(&ctx->ctrl_hdl, &cfg, ctx);
			if (ctx->ctrl_hdl.error) {
				ret = ctx->ctrl_hdl.error;
				goto err_free_handler;
			}
		}
	}

	ret = v4l2_ctrl_handler_setup(&ctx->ctrl_hdl);
	if (ret)
		goto err_free_handler;

	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;
	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	return ret;
}

/* --------------------------------------------------------------- ioctls */

static const struct v4l2_ioctl_ops histb_vdec_ioctl_ops = {
	.vidioc_querycap = histb_vdec_querycap,
	.vidioc_enum_framesizes = histb_vdec_enum_framesizes,

	.vidioc_enum_fmt_vid_cap = histb_vdec_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out = histb_vdec_enum_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = histb_vdec_g_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane = histb_vdec_g_fmt_out,
	.vidioc_try_fmt_vid_cap_mplane = histb_vdec_try_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = histb_vdec_try_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = histb_vdec_s_fmt_cap,
	.vidioc_s_fmt_vid_out_mplane = histb_vdec_s_fmt_out,
	.vidioc_g_selection = histb_vdec_g_selection,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_decoder_cmd = v4l2_m2m_ioctl_stateless_decoder_cmd,
	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_stateless_try_decoder_cmd,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

static int histb_vdec_open(struct file *filp)
{
	struct histb_vdec_dev *dev = video_drvdata(filp);
	struct histb_vdec_fh *fh;
	struct histb_vdec_ctx *ctx;
	int ret;

	fh = kzalloc_obj(*fh);
	if (!fh)
		return -ENOMEM;

	ctx = &fh->ctx;
	histb_vdec_reset_fmts(ctx);
	v4l2_fh_init(&ctx->fh, video_devdata(filp));

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx,
					    histb_vdec_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_free_fh;
	}

	ret = histb_vdec_init_ctrls(ctx);
	if (ret)
		goto err_release_m2m;

	v4l2_fh_add(&ctx->fh, filp);
	return 0;

err_release_m2m:
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
err_free_fh:
	v4l2_fh_exit(&ctx->fh);
	kfree(fh);
	return ret;
}

static int histb_vdec_release(struct file *filp)
{
	struct histb_vdec_ctx *ctx = file_to_ctx(filp);
	struct histb_vdec_fh *fh = histb_vdec_fh_of(ctx);

	v4l2_fh_del(&ctx->fh, filp);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(fh);

	return 0;
}

static const struct v4l2_file_operations histb_vdec_fops = {
	.owner = THIS_MODULE,
	.open = histb_vdec_open,
	.release = histb_vdec_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

/* ------------------------------------------------------------- v4l2 init */

static int histb_vdec_request_validate(struct media_request *req)
{
	unsigned int count;

	count = vb2_request_buffer_cnt(req);
	/*
	 * A request drives one decode job, and a job is the pair of one OUTPUT
	 * and one CAPTURE buffer, so the request must bind an OUTPUT buffer to
	 * be queueable at all and never more than one: the engine is programmed
	 * through a single stream base and one slice chain per run, so two
	 * coded buffers cannot be mapped onto the one job this request
	 * schedules.
	 */
	if (!count)
		return -ENOENT;
	if (count > 1)
		return -EINVAL;

	return vb2_request_validate(req);
}

static const struct media_device_ops histb_vdec_media_ops = {
	.req_validate = histb_vdec_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static void histb_vdec_v4l2_cleanup(struct histb_vdec_dev *dev)
{
	media_device_unregister(&dev->mdev);
	video_unregister_device(dev->vdev);
	v4l2_m2m_unregister_media_controller(dev->m2m_dev);
	/*
	 * The unregister drained every file handle, so no new job can start;
	 * kill the watchdog before the mem2mem device it completes through goes
	 * away.
	 */
	cancel_delayed_work_sync(&dev->watchdog);
	v4l2_m2m_release(dev->m2m_dev);
	v4l2_device_unregister(&dev->v4l2_dev);
	media_device_cleanup(&dev->mdev);
}

static int histb_vdec_v4l2_init(struct histb_vdec_dev *dev)
{
	struct histb_vdec_core *core = histb_vdec_core_of(dev);
	struct video_device *vdev;
	int ret;

	ret = v4l2_device_register(dev->dev, &dev->v4l2_dev);
	if (ret) {
		dev_err(dev->dev, "failed to register v4l2 device\n");
		return ret;
	}

	dev->v4l2_dev.mdev = &dev->mdev;
	strscpy(dev->mdev.driver_name, HISTB_VDEC_NAME,
		sizeof(dev->mdev.driver_name));
	strscpy(dev->mdev.model, "HI3798MV100 VDH", sizeof(dev->mdev.model));
	snprintf(dev->mdev.bus_info, sizeof(dev->mdev.bus_info),
		 "platform:%s", dev_name(dev->dev));
	media_device_init(&dev->mdev);
	dev->mdev.dev = dev->dev;
	dev->mdev.ops = &histb_vdec_media_ops;

	dev->m2m_dev = v4l2_m2m_init(&histb_vdec_m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		ret = PTR_ERR(dev->m2m_dev);
		v4l2_err(&dev->v4l2_dev, "failed to init mem2mem device\n");
		goto err_unregister_v4l2;
	}

	vdev = video_device_alloc();
	if (!vdev) {
		ret = -ENOMEM;
		goto err_release_m2m;
	}
	dev->vdev = vdev;

	vdev->v4l2_dev = &dev->v4l2_dev;
	vdev->fops = &histb_vdec_fops;
	vdev->ioctl_ops = &histb_vdec_ioctl_ops;
	vdev->lock = &core->vdev_lock;
	vdev->release = video_device_release;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
	strscpy(vdev->name, HISTB_VDEC_NAME, sizeof(vdev->name));
	video_set_drvdata(vdev, dev);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "failed to register video device\n");
		video_device_release(vdev);
		dev->vdev = NULL;
		goto err_release_m2m;
	}

	/*
	 * The mem2m core creates the decoder entity and the device-node link;
	 * without them the media node has an empty topology and userspace
	 * cannot tie /dev/video0 to /dev/media0.
	 */
	ret = v4l2_m2m_register_media_controller(dev->m2m_dev, vdev,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "failed to register media controller\n");
		goto err_unregister_vdev;
	}

	ret = media_device_register(&dev->mdev);
	if (ret) {
		dev_err(dev->dev, "failed to register media device\n");
		goto err_unregister_mc;
	}

	return 0;

err_unregister_mc:
	v4l2_m2m_unregister_media_controller(dev->m2m_dev);
err_unregister_vdev:
	video_unregister_device(dev->vdev);
	dev->vdev = NULL;
err_release_m2m:
	v4l2_m2m_release(dev->m2m_dev);
err_unregister_v4l2:
	v4l2_device_unregister(&dev->v4l2_dev);
	media_device_cleanup(&dev->mdev);
	return ret;
}

/* ------------------------------------------------------------------- pm */

static int histb_vdec_runtime_suspend(struct device *dev)
{
	struct histb_vdec_core *core = dev_get_drvdata(dev);
	struct histb_vdec_dev *vdec = &core->vdec;

	clk_bulk_disable_unprepare(vdec->num_clks, vdec->clks);
	return 0;
}

static int histb_vdec_runtime_resume(struct device *dev)
{
	struct histb_vdec_core *core = dev_get_drvdata(dev);
	struct histb_vdec_dev *vdec = &core->vdec;

	return clk_bulk_prepare_enable(vdec->num_clks, vdec->clks);
}

static const struct dev_pm_ops histb_vdec_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(histb_vdec_runtime_suspend,
			   histb_vdec_runtime_resume, NULL)
};

/* ---------------------------------------------------------------- probe */

static int histb_vdec_pool_alloc(struct histb_vdec_dev *dev)
{
	struct histb_vdec_msgpool *pool = &dev->pool;

	/*
	 * pic slot, slice slots, upstream report slots and the protocol table
	 * area (CABAC M/N tables, tile info, ppfd and the band-line scratch
	 * blocks) must all fit in the single pool.
	 */
	BUILD_BUG_ON(ALIGN(HISTB_VDEC_SLOT_BYTES +
			   HISTB_VDEC_MAX_SLICE_SLOTS * HISTB_VDEC_SLOT_BYTES +
			   HISTB_VDEC_MAX_UP_SLICES *
			   HISTB_VDEC_UP_MSG_WORDS * 4, 64) +
		     HISTB_TABLE_OFF_SCRATCH(HISTB_VDEC_NUM_SCRATCH - 1) +
		     HISTB_VDEC_SCRATCH_SIZE > HISTB_VDEC_POOL_SIZE);

	pool->size = HISTB_VDEC_POOL_SIZE;
	pool->va = dma_alloc_wc(dev->dev, pool->size, &pool->dma, GFP_KERNEL);
	if (!pool->va)
		return -ENOMEM;

	pool->pic_off = 0;
	pool->slice_off = HISTB_VDEC_SLOT_BYTES;
	pool->up_off = pool->slice_off +
		       HISTB_VDEC_MAX_SLICE_SLOTS * HISTB_VDEC_SLOT_BYTES;
	pool->table_off = ALIGN(pool->up_off + HISTB_VDEC_MAX_UP_SLICES *
				HISTB_VDEC_UP_MSG_WORDS * 4, 64);

	memset(pool->va, 0, pool->size);
	return 0;
}

static void histb_vdec_pool_free(struct histb_vdec_dev *dev)
{
	if (!dev->pool.va)
		return;

	dma_free_wc(dev->dev, dev->pool.size, dev->pool.va, dev->pool.dma);
	dev->pool.va = NULL;
}

static const struct of_device_id histb_vdec_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-vdec" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, histb_vdec_of_match);

static int histb_vdec_probe(struct platform_device *pdev)
{
	struct histb_vdec_core *core;
	struct histb_vdec_dev *dev;
	int ret;

	core = devm_kzalloc(&pdev->dev, sizeof(*core), GFP_KERNEL);
	if (!core)
		return -ENOMEM;

	dev = &core->vdec;
	dev->dev = &pdev->dev;
	platform_set_drvdata(pdev, core);
	mutex_init(&dev->hw_lock);
	mutex_init(&core->vdev_lock);
	spin_lock_init(&core->job_lock);
	INIT_DELAYED_WORK(&dev->watchdog, histb_vdec_watchdog);

	dev->vdm = devm_platform_ioremap_resource_byname(pdev, "vdm");
	if (IS_ERR(dev->vdm))
		return PTR_ERR(dev->vdm);

	dev->scd = devm_platform_ioremap_resource_byname(pdev, "scd");
	if (IS_ERR(dev->scd))
		return PTR_ERR(dev->scd);

	ret = devm_clk_bulk_get_all(&pdev->dev, &dev->clks);
	if (ret < 0)
		return ret;
	dev->num_clks = ret;

	/*
	 * Both reset lines have to be released: the subsystem-wide one keeps
	 * the whole VDM register window reading zero while asserted.
	 */
	dev->rst = devm_reset_control_array_get_exclusive(&pdev->dev);
	if (IS_ERR(dev->rst))
		return PTR_ERR(dev->rst);

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	dev->irq = ret;

	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(&pdev->dev, "failed to set DMA mask\n");
		return ret;
	}
	vb2_dma_contig_set_max_seg_size(&pdev->dev, DMA_BIT_MASK(32));

	/*
	 * Every buffer this engine uses has to be physically contiguous: the
	 * queues are dma-contig, and the message pool is handed to the hardware
	 * as one base address (VDM_AVM_ADDR). Bind the region named by
	 * "memory-region" before the first allocation so that they come out of
	 * the media pool instead of competing with every other client for the
	 * default CMA area. A DTB that draws no such pool is not an error
	 * (-ENODEV): the device then keeps using the default area.
	 */
	ret = devm_of_reserved_mem_device_init(&pdev->dev);
	if (ret && ret != -ENODEV) {
		dev_err(&pdev->dev, "failed to bind reserved memory\n");
		return ret;
	}

	ret = histb_vdec_pool_alloc(dev);
	if (ret)
		return ret;

	histb_vdec_hw_init_tables(dev);

	pm_runtime_set_autosuspend_delay(&pdev->dev, 100);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	/*
	 * The decoder comes up held in reset; release it, mask every interrupt
	 * source and only then register the handler.
	 */
	ret = reset_control_deassert(dev->rst);
	if (ret)
		goto err_disable_pm;

	ret = histb_vdec_hw_init(dev);
	if (ret)
		goto err_assert_reset;

	ret = devm_request_threaded_irq(&pdev->dev, dev->irq,
					histb_vdec_hardirq,
					histb_vdec_irq_handler,
					IRQF_ONESHOT, dev_name(&pdev->dev),
					dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq\n");
		goto err_assert_reset;
	}

	ret = histb_vdec_v4l2_init(dev);
	if (ret)
		goto err_assert_reset;

	histb_vdec_debugfs_init(dev);

	return 0;

err_assert_reset:
	reset_control_assert(dev->rst);
err_disable_pm:
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	histb_vdec_pool_free(dev);
	return ret;
}

static void histb_vdec_remove(struct platform_device *pdev)
{
	struct histb_vdec_core *core = platform_get_drvdata(pdev);
	struct histb_vdec_dev *dev = &core->vdec;

	/*
	 * Unregister first: open file handles drain their in-flight jobs
	 * through the watchdog before the work and the hardware go away.
	 */
	histb_vdec_v4l2_cleanup(dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
	reset_control_assert(dev->rst);
	histb_vdec_pool_free(dev);
}

static struct platform_driver histb_vdec_driver = {
	.probe = histb_vdec_probe,
	.remove = histb_vdec_remove,
	.driver = {
		.name = HISTB_VDEC_NAME,
		.of_match_table = histb_vdec_of_match,
		.pm = &histb_vdec_pm_ops,
	},
};
module_platform_driver(histb_vdec_driver);

MODULE_DESCRIPTION("HiSilicon Hi3798MV100 stateless video decoder driver");
MODULE_LICENSE("GPL");
