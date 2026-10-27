// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for the HiSTB VDP v2 display engine, found on hi3798mv100 /
 * hi3796mv100 / hi3716dv100 SoCs.
 *
 * The VDP block at 0xf8cc0000 provides two display channels. This driver
 * exposes DHD0 (the HD channel) as a single CRTC with a primary plane fed by
 * the G0 graphics (RGB) layer, routed G0 -> GP0 -> MIXG0 -> CBM(MIX0) -> DHD0
 * -> HDMI. Scanout data leaves the VDP towards the HDMI transmitter selected
 * through of_graph port@0; the downstream node is a drm_bridge
 * (hisilicon,hi3798mv100-hdmi).
 *
 * Clocking: the HD channel is fed by the CRG's HD PLL through a post divider
 * and CRG54's vo_hd_clk_div (offset 0xd8), i.e. the channel clock is half the
 * PLL output. drivers/clk/hisilicon models that as "clk_vo_hd", which is also
 * what the HDMI transmitter receives 1:1, so the two blocks cannot disagree
 * about the pixel clock. The driver enables the CRG54 gates by hand and pulses
 * CRG54 bit 30 before it configures anything, so the display controller comes
 * up from its reset defaults rather than from the state it was handed over in.
 *
 * WBC_HD0 taps that mixed output and can write one frame back to memory;
 * it is exposed as a DRM writeback connector, and only produces data while
 * the channel is running.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/slab.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_crtc.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_plane.h>
#include <drm/drm_util.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_of.h>
#include <drm/drm_print.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>
#include <drm/drm_writeback.h>

#include "histb-vdp2-regs.h"

/*
 * Where the GP0 zoom engine keeps its step registers, and what a 1:1 step is.
 * The two SoCs this driver serves agree on every other offset the driver
 * touches; this block is the one place they differ (see the note in
 * histb-vdp2-regs.h), so it is selected by the of_device_id rather than by a
 * compile-time choice.
 */
struct histb_vdp2_zme {
	u32	hsp;		/* horizontal step register */
	u32	vsr;		/* vertical step register */
	u32	hoffset;	/* horizontal phase; the second one, if any, is at +4 */
	u32	voffset;	/* vertical phase */
	u32	hratio;		/* field mask inside hsp */
	u32	vratio;		/* field mask inside vsr */
	bool	hoffset_pair;
	unsigned long	hprec;	/* the ratio that means 1:1 */
	unsigned long	vprec;
};

static const struct histb_vdp2_zme histb_vdp2_zme_mv100 = {
	.hsp = GP0_ZME_HSP,
	.vsr = GP0_ZME_VSR,
	.hoffset = GP0_ZME_HLOFFSET,
	.voffset = GP0_ZME_VOFFSET,
	.hratio = GP0_ZME_HSP_RATIO,
	.vratio = GP0_ZME_VSR_RATIO,
	.hoffset_pair = true,
	.hprec = GP0_ZME_HPREC,
	.vprec = GP0_ZME_VPREC,
};

/*
 * The precisions here are board readings, not table arithmetic: a channel the
 * vendor fastboot left scanning holds 0x00001000 in both step registers, so
 * this part counts in 4.12 both ways.
 */
static const struct histb_vdp2_zme histb_vdp2_zme_cv200 = {
	.hsp = CV200_GP0_ZME_HSP,
	.vsr = CV200_GP0_ZME_VSR,
	.hoffset = CV200_GP0_ZME_HOFFSET,
	.voffset = CV200_GP0_ZME_VOFFSET,
	.hratio = CV200_GP0_ZME_HSP_RATIO,
	.vratio = CV200_GP0_ZME_VSR_RATIO,
	.hoffset_pair = false,
	.hprec = CV200_GP0_ZME_HPREC,
	.vprec = CV200_GP0_ZME_VPREC,
};

/*
 * Per-chip differences in the *output shaping* path, established by reading the
 * same registers on a part that is displaying: the vendor's live values on
 * Hi3716CV200 are VO_MUX 0x9 (sddate_sel and hdmi_sel set, ie its primary
 * display is channel 1), GP0_CTRL 0x80001230 (CSC order, no zoom stage) and
 * DHD0_CTRL 0x80008010 (the HDMI_MODE bit is absent, interface B is set),
 * while the Hi3798MV100 pipeline this driver first ran on had 0x8000C010
 * with both bits and its mux already routed by the boot.
 */
struct histb_vdp2_chip {
	const struct histb_vdp2_zme	*zme;
	u32				gp0_order;
	bool				dhd_intf_b;
	bool				dhd_hdmi_mode;
	bool				mux_hdmi_dhd0;
	bool				hdate_hdmi_out;
	/*
	 * Expose the write-back taps: WBC_HD0 (the composited HD channel, block
	 * at 0xac00) and WBC_GP0 (graphics layer, 0xa800). Hi3798MV100 has both
	 * board-verified. Hi3716CV200's own register file has the same two blocks
	 * at the same offsets with the same bit positions, accumulated from
	 * S_VOU_CV200_REGS_TYPE in vdp_v1_0/hal/cv200/vdp_reg.h, and its cast path
	 * drives WBC_HD0 the way this driver does, so the map is shared.
	 */
	bool				has_wbc;
	/*
	 * Run the GP0 CSC stage as the component-line unrotation plus the
	 * limited-range shaping. That contract was established on Hi3716CV200
	 * (the G0 fetch hands its lines over rotated, see the comment in
	 * histb_vdp2_pipeline_init()); Hi3798MV100 was verified displaying with
	 * the stage bypassed, so on it the stage must stay off.
	 */
	bool				gp0_csc_cycle;
};

static const struct histb_vdp2_chip histb_vdp2_mv100 = {
	.zme = &histb_vdp2_zme_mv100,
	.gp0_order = GP0_ORDER_ZME_CSC,
	.dhd_intf_b = true,
	.dhd_hdmi_mode = true,
	.mux_hdmi_dhd0 = false,
	.has_wbc = true,
	.gp0_csc_cycle = false,
};

static const struct histb_vdp2_chip histb_vdp2_cv200 = {
	.zme = &histb_vdp2_zme_cv200,
	.gp0_order = GP0_ORDER_CSC,
	.dhd_intf_b = true,
	/*
	 * hdmi_mode (bit14) is the interface's data-type decision and this part
	 * leaves it clear while carrying RGB -- measured on a channel that was
	 * displaying: 0x80008010, where this driver's 0x8000C010 had it set.
	 */
	.dhd_hdmi_mode = false,
	.mux_hdmi_dhd0 = true,
	.hdate_hdmi_out = true,
	.has_wbc = true,
};

struct histb_vdp2 {
	struct device		*dev;
	void __iomem		*regs;

	struct clk		*clk_pix;	/* HD channel clock, shared with HDMI */
	struct clk		*clk_vo;	/* CRG54 gate group of the channel */

	struct reset_control	*rst;
	const struct histb_vdp2_zme	*zme;
	const struct histb_vdp2_chip	*chip;

	int			irq;

	struct histb_vdp2_display	*disp;	/* NULL unless the pipeline is bound */
};

/*
 * The DRM objects belong to the aggregate driver, not to the platform device:
 * they come into existence when the pipeline the device tree describes is up
 * and go away when it comes down, which can happen while this device stays
 * probed (the transmitter is a component of the aggregate). devm cannot cover
 * that lifetime, so this block is allocated by the aggregate's bind and
 * released by its unbind.
 */
struct histb_vdp2_display {
	struct histb_vdp2		*vdp;
	struct drm_device	*drm;

	struct drm_plane	primary;
	struct drm_crtc		crtc;
	struct drm_encoder	encoder;

	struct drm_encoder	wb_enc;
	struct drm_writeback_connector	wb;
	struct drm_encoder	wb_gp0_enc;
	struct drm_writeback_connector	wb_gp0;
	/*
	 * One capture at a time: wb_pending says a job is written to memory, and
	 * wb_vblanks counts the frames that have gone by since it was armed.
	 * Only the commit tail sets wb_pending, only the interrupt handler
	 * clears it.
	 */
	bool			wb_pending;
	unsigned int		wb_vblanks;
	/* which block the armed job writes through, and whose connector to signal */
	u16			wb_base;
	struct drm_writeback_connector *wb_running;
};

static inline struct histb_vdp2 *plane_to_vdp(struct drm_plane *plane)
{
	return container_of(plane, struct histb_vdp2_display, primary)->vdp;
}

static inline struct histb_vdp2 *crtc_to_vdp(struct drm_crtc *crtc)
{
	return container_of(crtc, struct histb_vdp2_display, crtc)->vdp;
}

/*
 * The mode's pixel clock is the HD channel clock, which the CRG driver builds
 * from the HD PLL's post divider and CRG54's vo_hd_clk_div. With vo_hd_clk_div
 * at /2 the PLL runs at twice the pixel clock: 297MHz for 1080p60, 148.5MHz for
 * 720p and the 1080p24-30 family. Requesting the channel rate is therefore
 * enough for the transmitter to see the same clock, since it takes vo_hd 1:1.
 */
static int histb_vdp2_set_pixclk(struct histb_vdp2 *vdp, unsigned int mode_khz)
{
	unsigned long rate = (unsigned long)mode_khz * 1000;
	int ret;

	ret = clk_set_rate(vdp->clk_pix, rate);
	if (ret) {
		dev_err(vdp->dev, "failed to set pixel clock to %lu Hz: %d\n",
			rate, ret);
		return ret;
	}

	dev_dbg(vdp->dev, "pixel clock %lu Hz for mode clock %u kHz\n",
		clk_get_rate(vdp->clk_pix), mode_khz);
	return 0;
}

static inline u32 vo_read(struct histb_vdp2 *vdp, u32 off)
{
	return readl_relaxed(vdp->regs + off);
}

static inline void vo_write(struct histb_vdp2 *vdp, u32 off, u32 val)
{
	writel_relaxed(val, vdp->regs + off);
}

/*
 * FIELD_PREP() cannot be used here: the ratio masks come from chip data, and
 * that macro requires a build-time constant mask.
 */
static u32 vo_rmw_field(struct histb_vdp2 *vdp, u32 off, u32 mask, u32 val)
{
	u32 ctrl = vo_read(vdp, off);

	val = (val << __ffs(mask)) & mask;
	ctrl = (ctrl & ~mask) | val;
	vo_write(vdp, off, ctrl);

	return ctrl;
}

/* ------------------------------------------------------------------ */
/* Plane (G0 graphics layer)                                           */
/* ------------------------------------------------------------------ */

/*
 * No per-pixel alpha is programmed in the atomic update path, so the ARGB
 * format is deliberately not exposed (drm core would warn about alpha formats
 * without blend mode support).
 */
static const u32 histb_vdp2_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* `value - 1`, clamped so that a zero porch/sync width never underflows */
static u32 histb_vdp2_m1(u32 v)
{
	return max(v, 1u) - 1;
}

static void histb_vdp2_g0_set(struct histb_vdp2 *vdp, struct drm_framebuffer *fb,
			    struct drm_gem_dma_object *gem, bool enable)
{
	u32 ctrl, ifmt, palpha;

	if (!enable) {
		ctrl = vo_read(vdp, G0_CTRL);
		ctrl &= ~G0_CTRL_SURFACE_EN;
		vo_write(vdp, G0_CTRL, ctrl);
		vo_write(vdp, G0_UPD, G0_UPD_REGUP);
		return;
	}

	/*
	 * Pixel format. The A-prefixed hardware codes consume the buffer's
	 * alpha byte, so an XRGB framebuffer must not be described as ARGB8888:
	 * every pixel would carry alpha 0x00 and the layer would composite as
	 * fully transparent.
	 */
	switch (fb->format->format) {
	case DRM_FORMAT_ARGB8888:
		ifmt = G0_IFMT_ARGB8888;
		palpha = G0_CBMPARA_PALPHA_EN;
		break;
	default:
		ifmt = G0_IFMT_KRGB888;
		palpha = 0;
		break;
	}

	/*
	 * bitext, read_mode and upd_mode all have to be set for a layer that
	 * drives a panel. Left at 0, read_mode means that the fetch follows the
	 * interface's field coding instead of running progressive.
	 */
	ctrl = vo_read(vdp, G0_CTRL);
	ctrl &= ~(G0_CTRL_IFMT | G0_CTRL_BITEXT | G0_CTRL_READ_MODE |
		  G0_CTRL_UPD_MODE);
	ctrl |= FIELD_PREP(G0_CTRL_IFMT, ifmt) |
		FIELD_PREP(G0_CTRL_BITEXT, G0_BITEXT_3RD) |
		G0_CTRL_READ_MODE | G0_CTRL_UPD_MODE;
	vo_write(vdp, G0_CTRL, ctrl);

	/* opaque surface, no colour key, no per-pixel alpha */
	ctrl = vo_read(vdp, G0_CBMPARA);
	ctrl &= ~(G0_CBMPARA_GALPHA | G0_CBMPARA_PALPHA_EN |
		  G0_CBMPARA_KEY_EN | G0_CBMPARA_KEY_MODE);
	ctrl |= FIELD_PREP(G0_CBMPARA_GALPHA, 0xff) | palpha;
	vo_write(vdp, G0_CBMPARA, ctrl);

	vo_write(vdp, G0_ADDR, (u32)(gem->dma_addr + fb->offsets[0]));
	/* G0_STRIDE is programmed in units of 16 bytes */
	vo_write(vdp, G0_STRIDE,
		 FIELD_PREP(G0_STRIDE_MASK, fb->pitches[0] / 16));
	vo_write(vdp, G0_IRESO,
		 FIELD_PREP(G0_IRESO_IW, fb->width - 1) |
		 FIELD_PREP(G0_IRESO_IH, fb->height - 1));

	/* source position 0,0 and 1:1 placement on the display */
	vo_write(vdp, G0_DFPOS, 0);
	vo_write(vdp, G0_DLPOS,
		 FIELD_PREP(G0_DLPOS_X, fb->width - 1) |
		 FIELD_PREP(G0_DLPOS_Y, fb->height - 1));
	vo_write(vdp, G0_VFPOS, 0);
	vo_write(vdp, G0_VLPOS,
		 FIELD_PREP(G0_DLPOS_X, fb->width - 1) |
		 FIELD_PREP(G0_DLPOS_Y, fb->height - 1));

	/*
	 * GP0 passes the G0 surface through: same resolution, full-screen
	 * display window (DFPOS 0,0 / DLPOS (w-1, h-1), 12-bit x[11:0] y[23:12]
	 * fields).
	 */
	vo_write(vdp, GP0_IRESO,
		 FIELD_PREP(G0_IRESO_IW, fb->width - 1) |
		 FIELD_PREP(G0_IRESO_IH, fb->height - 1));
	vo_write(vdp, GP0_ORESO,
		 FIELD_PREP(GP0_ORESO_OW, fb->width - 1) |
		 FIELD_PREP(GP0_ORESO_OH, fb->height - 1));
	vo_write(vdp, GP0_DFPOS, 0);
	vo_write(vdp, GP0_DLPOS,
		 FIELD_PREP(G0_DLPOS_X, fb->width - 1) |
		 FIELD_PREP(G0_DLPOS_Y, fb->height - 1));
	vo_write(vdp, GP0_VFPOS, 0);
	vo_write(vdp, GP0_VLPOS,
		 FIELD_PREP(G0_DLPOS_X, fb->width - 1) |
		 FIELD_PREP(G0_DLPOS_Y, fb->height - 1));

	/*
	 * The zoom engine walks the source with its own step registers, so
	 * GP0_IRESO == GP0_ORESO does not mean 1:1: left alone, the engine
	 * keeps scaling by whatever was programmed for the previous user of the
	 * block, which can make the output cover the screen more than once. The
	 * formula is ratio = in * precision / out, and the register layout the
	 * ratios live in is what differs between the two SoCs of this family --
	 * hence the chip data rather than the macros.
	 */
	ctrl = vo_rmw_field(vdp, vdp->zme->hsp, vdp->zme->hratio, vdp->zme->hprec);
	ctrl = vo_rmw_field(vdp, vdp->zme->vsr, vdp->zme->vratio, vdp->zme->vprec);

	/* scaler phase starts at the first source pixel */
	vo_write(vdp, vdp->zme->hoffset, 0);
	if (vdp->zme->hoffset_pair)
		vo_write(vdp, vdp->zme->hoffset + 4, 0);
	vo_write(vdp, vdp->zme->voffset, 0);
	vo_write(vdp, GP0_UPD, GP0_UPD_REGUP);

	/* layer enable + shadow-register update */
	ctrl = vo_read(vdp, G0_CTRL);
	ctrl |= G0_CTRL_SURFACE_EN;
	vo_write(vdp, G0_CTRL, ctrl);
	vo_write(vdp, G0_UPD, G0_UPD_REGUP);
}

static void histb_vdp2_plane_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_commit *state)
{
	struct histb_vdp2 *vdp = plane_to_vdp(plane);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_gem_dma_object *gem;
	struct drm_framebuffer *fb = new_state->fb;

	if (!fb)
		return;

	gem = drm_fb_dma_get_gem_obj(fb, 0);
	if (!gem)
		return;

	histb_vdp2_g0_set(vdp, fb, gem, new_state->visible);
}

static void histb_vdp2_plane_atomic_disable(struct drm_plane *plane,
					  struct drm_atomic_commit *state)
{
	struct histb_vdp2 *vdp = plane_to_vdp(plane);

	histb_vdp2_g0_set(vdp, NULL, NULL, false);
}

static int histb_vdp2_plane_atomic_check(struct drm_plane *plane,
				       struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_state->fb)
		return 0;

	/* G0_STRIDE is programmed in units of 16 bytes */
	if (new_state->fb->pitches[0] % 16)
		return -EINVAL;

	/* the G0_IRESO / GP0_ORESO width fields are 12 bits wide */
	if (new_state->fb->width > 4095)
		return -EINVAL;

	if (!new_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, new_state->crtc);
	/* The new-state getter is a slot read, so it yields NULL. */
	if (!crtc_state)
		return PTR_ERR(crtc_state);

	if (new_state->crtc_w > 4095 || new_state->crtc_h > 4095)
		return -EINVAL;

	/*
	 * This is what fills in new_state->visible, and atomic_update acts on
	 * it: without the helper visible stays false, so every plane commit
	 * switches the surface off again. can_position is false because the
	 * driver always places the surface at 0,0 with the framebuffer's own
	 * size.
	 */
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, true);
}

static const struct drm_plane_helper_funcs histb_vdp2_plane_helper_funcs = {
	/*
	 * No DRM_GEM_SHADOW_PLANE_HELPER_FUNCS here: they upcast the state with
	 * container_of() to struct drm_shadow_plane_state, which only the
	 * matching DRM_GEM_SHADOW_PLANE_FUNCS duplicate() allocates.
	 */
	.atomic_check		= histb_vdp2_plane_atomic_check,
	.atomic_update		= histb_vdp2_plane_atomic_update,
	.atomic_disable		= histb_vdp2_plane_atomic_disable,
};

static const struct drm_plane_funcs histb_vdp2_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

/* ------------------------------------------------------------------ */
/* Write-back (WBC_DHD0, WBC_GP0)                                      */
/* ------------------------------------------------------------------ */

/*
 * The block copies whatever it taps and never colour-converts it: over eight
 * colour bars a planar target carries the red sample in the luma byte and
 * green and blue in the Cb and Cr ones, so those entries are byte packings of
 * an RGB image rather than YCbCr. A consumer that wants the frame as it is
 * displayed takes the graphics tap instead, which writes 32-bit pixels with
 * the lanes in B, G, R, X order and X filled with 0xff - XBGR8888.
 */
struct histb_wb_fmt {
	u32	format;
	u8	wbc_format;
	u8	planes;
	bool	uv_order;	/* chroma order of the planar formats */
	u16	base;		/* which write-back block produces it */
};

static const struct histb_wb_fmt histb_wb_fmts[] = {
	{ DRM_FORMAT_NV12, WBC_FORMAT_SP420, 2, true, WBC_BASE_DHD0 },
	{ DRM_FORMAT_NV21, WBC_FORMAT_SP420, 2, false, WBC_BASE_DHD0 },
	{ DRM_FORMAT_NV16, WBC_FORMAT_SP422, 2, true, WBC_BASE_DHD0 },
	{ DRM_FORMAT_NV61, WBC_FORMAT_SP422, 2, false, WBC_BASE_DHD0 },
	{ DRM_FORMAT_UYVY, WBC_FORMAT_PKG_UYVY, 1, false, WBC_BASE_DHD0 },
	{ DRM_FORMAT_YUYV, WBC_FORMAT_PKG_YUYV, 1, false, WBC_BASE_DHD0 },
	{ DRM_FORMAT_YVYU, WBC_FORMAT_PKG_YVYU, 1, false, WBC_BASE_DHD0 },
	{ DRM_FORMAT_XBGR8888, WBC_FORMAT_ARGB8888, 1, false, WBC_BASE_GP0 },
};

static const u32 histb_wb_format_list[] = {
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
	DRM_FORMAT_NV16,
	DRM_FORMAT_NV61,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_YVYU,
};

static const u32 histb_wb_gp0_format_list[] = {
	DRM_FORMAT_XBGR8888,
};

static const struct histb_wb_fmt *histb_wb_fmt_find(u32 format)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(histb_wb_fmts); i++)
		if (histb_wb_fmts[i].format == format)
			return &histb_wb_fmts[i];

	return NULL;
}

/* the capture is one frame of what the channel mixes, read
 * progressively as the vendor's snapshot path does
 * (drv_disp_hal.c:2583), at 1:1
 */
static void histb_wb_setup(struct histb_vdp2 *vdp, const struct histb_wb_fmt *fmt,
			   struct drm_framebuffer *fb)
{
	struct drm_gem_dma_object *gem;
	dma_addr_t addr;
	u32 ctrl, stride, reso;
	int plane;

	u32 base = fmt->base;

	ctrl = vo_read(vdp, base + WBC_OFS_CTRL);
	ctrl &= ~(WBC_CTRL_FORMAT | WBC_CTRL_MODE | WBC_CTRL_3D_MODE);
	ctrl |= FIELD_PREP(WBC_CTRL_FORMAT, fmt->wbc_format) |
		FIELD_PREP(WBC_CTRL_MODE, WBC_MODE_PROGRESSIVE);
	if (fmt->planes == 2) {
		ctrl &= ~WBC_CTRL_UV_ORDER;
		if (fmt->uv_order)
			ctrl |= WBC_CTRL_UV_ORDER;
	}
	/* the caller enables, once the target is fully described */
	vo_write(vdp, base + WBC_OFS_CTRL, ctrl);

	if (base == WBC_BASE_GP0) {
		/* 10-bit surfaces dither down to the 8-bit target the block writes */
		vo_write(vdp, WBC_GP0_DITHER_CTRL,
			 FIELD_PREP(WBC_GP0_DITHER_MD, WBC_GP0_DITHER_SPA_8));
	}

	stride = FIELD_PREP(WBC_STRIDE_L, fb->pitches[0]) |
		 FIELD_PREP(WBC_STRIDE_C, fb->pitches[fmt->planes - 1]);
	vo_write(vdp, base + WBC_OFS_STRIDE, stride);

	reso = FIELD_PREP(WBC_ORESO_W, fb->width - 1) |
	       FIELD_PREP(WBC_ORESO_H, fb->height - 1);
	vo_write(vdp, base + WBC_OFS_ORESO, reso);

	/*
	 * The crop window has to be stated as well: both edges default to zero,
	 * and a window that ends at the first pixel has nothing to write.
	 */
	vo_write(vdp, base + WBC_OFS_FCROP, 0);
	vo_write(vdp, base + WBC_OFS_LCROP,
		 FIELD_PREP(WBC_LCROP_W, fb->width - 1) |
		 FIELD_PREP(WBC_LCROP_H, fb->height - 1));

	/*
	 * The channel tap scales through its own ZME before writing, so a 1:1
	 * capture still has to state the ratio: both directions carry the value
	 * that means "one output line per input line" with the filters off.
	 * The graphics tap has no ZME of its own.
	 */
	if (base == WBC_BASE_DHD0) {
		vo_write(vdp, WBC_DHD0_ZME_HSP, WBC_ZME_HPREC);
		vo_write(vdp, WBC_DHD0_ZME_HLOFFSET, 0);
		vo_write(vdp, WBC_DHD0_ZME_HCOFFSET, 0);
		vo_write(vdp, WBC_DHD0_ZME_VSR, WBC_ZME_VPREC);
		vo_write(vdp, WBC_DHD0_ZME_VOFFSET, 0);
	}

	for (plane = 0; plane < fmt->planes; plane++) {
		gem = drm_fb_dma_get_gem_obj(fb, plane);
		if (!gem)
			return;
		addr = gem->dma_addr + fb->offsets[plane];
		/* a packed capture ignores the chroma address */
		vo_write(vdp, base + (plane ? WBC_OFS_CADDR : WBC_OFS_YADDR),
			 addr);
	}
}

static void histb_wb_start(struct histb_vdp2_display *disp,
			   const struct histb_wb_fmt *fmt,
			   struct drm_framebuffer *fb,
			   struct drm_writeback_connector *wb)
{
	struct histb_vdp2 *vdp = disp->vdp;
	u32 base = fmt->base;

	histb_wb_setup(vdp, fmt, fb);
	vo_write(vdp, base + WBC_OFS_CTRL,
		 vo_read(vdp, base + WBC_OFS_CTRL) | WBC_CTRL_EN);

	/* the block latches everything it was pointed at with one update bit */
	vo_write(vdp, base + WBC_OFS_UPD, WBC_UPD_REGUP);
	disp->wb_base = base;
	disp->wb_running = wb;
	WRITE_ONCE(disp->wb_pending, true);
	disp->wb_vblanks = 0;
}

static void histb_wb_stop(struct histb_vdp2 *vdp, u16 base)
{
	u32 ctrl = vo_read(vdp, base + WBC_OFS_CTRL);

	ctrl &= ~WBC_CTRL_EN;
	vo_write(vdp, base + WBC_OFS_CTRL, ctrl);
	vo_write(vdp, base + WBC_OFS_UPD, WBC_UPD_REGUP);
}

/*
 * The write-back's own frame-done status bit is not unmasked here: it latches
 * in the status register either way, so the driver can report whether the bit
 * tracks the capture while keeping the line quiet. Completion is taken from
 * the channel's vblank, which is what the block's own timed wait counts.
 */
static void histb_wb_update(struct histb_vdp2_display *disp, u32 all, u32 sta)
{
	struct histb_vdp2 *vdp = disp->vdp;

	if (!READ_ONCE(disp->wb_pending))
		return;

	if (!(sta & VO_INT_DHD0_VTTHD1))
		return;

	if (++disp->wb_vblanks < 2)
		return;

	dev_info_ratelimited(vdp->dev, "wbc: frame written, vte status %s\n",
			     all & VO_INT_DWBC0_VTTHD ? "set" : "clear");

	histb_wb_stop(vdp, disp->wb_base);
	WRITE_ONCE(disp->wb_pending, false);
	drm_writeback_signal_completion(disp->wb_running, 0);
	disp->wb_running = NULL;
}

static int histb_wb_enc_atomic_check(struct drm_encoder *encoder,
				     struct drm_crtc_state *crtc_state,
				     struct drm_connector_state *conn_state)
{
	struct drm_device *drm = encoder->dev;
	struct drm_framebuffer *fb;
	const struct histb_wb_fmt *fmt;
	const struct drm_display_mode *mode = &crtc_state->mode;
	struct histb_vdp2_display *disp;
	struct drm_connector_state *cs_dhd0, *cs_gp0;
	int jobs = 0;

	disp = container_of(crtc_state->crtc, struct histb_vdp2_display, crtc);

	fb = conn_state->writeback_job ? conn_state->writeback_job->fb : NULL;
	if (!conn_state->writeback_job || !conn_state->writeback_job->fb)
		return 0;

	/*
	 * One tap can be armed per commit: the two write-back blocks share the
	 * channel's vblank, so a second job would have no completion of its own.
	 */
	cs_dhd0 = drm_atomic_get_new_connector_state(crtc_state->state,
						    &disp->wb.base);
	if (IS_ERR(cs_dhd0))
		return PTR_ERR(cs_dhd0);
	cs_gp0 = drm_atomic_get_new_connector_state(crtc_state->state,
						    &disp->wb_gp0.base);
	if (IS_ERR(cs_gp0))
		return PTR_ERR(cs_gp0);
	jobs = !!(cs_dhd0 && cs_dhd0->writeback_job) +
	       !!(cs_gp0 && cs_gp0->writeback_job);
	if (jobs > 1) {
		drm_dbg_kms(drm, "wbc: only one write-back job fits per commit\n");
		return -EBUSY;
	}

	fmt = histb_wb_fmt_find(fb->format->format);
	if (!fmt) {
		drm_dbg_kms(drm, "wbc: format %p4cc is not written by this block\n",
			    &fb->format->format);
		return -EINVAL;
	}

	/*
	 * The format picks the block, so an encoder must not be handed a target
	 * that the other tap produces: the check below would otherwise arm the
	 * wrong block and report the other tap's pixels as this one's.
	 */
	if (fmt->base != (encoder == &disp->wb_enc ? WBC_BASE_DHD0 : WBC_BASE_GP0)) {
		drm_dbg_kms(drm, "wbc: %p4cc belongs to the other tap\n",
			    &fb->format->format);
		return -EINVAL;
	}

	/*
	 * The write-back takes the mixed picture at 1:1, so the target has to be
	 * the frame the channel is scanning out.
	 */
	if (fb->width != mode->hdisplay || fb->height != mode->vdisplay) {
		drm_dbg_kms(drm, "wbc: %ux%u target does not match %ux%u mode\n",
			    fb->width, fb->height, mode->hdisplay,
			    mode->vdisplay);
		return -EINVAL;
	}

	return 0;
}

static int histb_wb_conn_get_modes(struct drm_connector *connector)
{
	return drm_add_modes_noedid(connector, connector->dev->mode_config.max_width,
				    connector->dev->mode_config.max_height);
}

static const struct drm_encoder_helper_funcs histb_wb_enc_helper_funcs = {
	.atomic_check		= histb_wb_enc_atomic_check,
};

static const struct drm_connector_helper_funcs histb_wb_conn_helper_funcs = {
	.get_modes		= histb_wb_conn_get_modes,
};

static const struct drm_connector_funcs histb_wb_conn_funcs = {
	.reset			= drm_atomic_helper_connector_reset,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static int histb_wb_one(struct histb_vdp2_display *disp,
			struct drm_encoder *enc, struct drm_writeback_connector *wb,
			const u32 *fmts, unsigned int n_fmts, const char *name)
{
	struct drm_device *drm = disp->drm;
	int ret;

	enc->possible_crtcs = drm_crtc_mask(&disp->crtc);
	ret = drmm_encoder_init(drm, enc, NULL, DRM_MODE_ENCODER_VIRTUAL, name);
	if (ret)
		return ret;
	drm_encoder_helper_add(enc, &histb_wb_enc_helper_funcs);

	drm_connector_helper_add(&wb->base, &histb_wb_conn_helper_funcs);

	return drmm_writeback_connector_init(drm, wb, &histb_wb_conn_funcs, enc,
					     fmts, n_fmts);
}

static int histb_wb_init(struct histb_vdp2_display *disp)
{
	unsigned int both;
	int ret;

	if (!disp->vdp->chip->has_wbc)
		return 0;

	/*
	 * The write-back has no enable sequence of its own: the block is armed
	 * from the CRTC's flush, so a job that arrives while the channel is off
	 * simply never sees a vblank.
	 */
	ret = histb_wb_one(disp, &disp->wb_enc, &disp->wb, histb_wb_format_list,
			   ARRAY_SIZE(histb_wb_format_list), "histb-vdp2-wb-enc");
	if (ret)
		return ret;

	ret = histb_wb_one(disp, &disp->wb_gp0_enc, &disp->wb_gp0,
			   histb_wb_gp0_format_list,
			   ARRAY_SIZE(histb_wb_gp0_format_list),
			   "histb-vdp2-wb-gp0-enc");
	if (ret)
		return ret;

	/*
	 * Capturing while displaying attaches both encoders to the one CRTC, and
	 * the core requires every encoder of a CRTC to list the others as
	 * cloneable before it accepts such a commit.
	 */
	/* every encoder of the CRTC has to list the others as cloneable */
	both = drm_encoder_mask(&disp->encoder) | drm_encoder_mask(&disp->wb_enc) |
	       drm_encoder_mask(&disp->wb_gp0_enc);
	disp->encoder.possible_clones = both;
	disp->wb_enc.possible_clones = both;
	disp->wb_gp0_enc.possible_clones = both;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Framebuffer creation                                                */
/* ------------------------------------------------------------------ */

static const struct drm_framebuffer_funcs histb_wb_fb_funcs = {
	.destroy		= drm_gem_fb_destroy,
	.create_handle		= drm_gem_fb_create_handle,
};

/*
 * The generic helper refuses a format that no plane can scan out. A write-back
 * target is written by WBC_HD0 and read by no plane at all, so the plane list
 * cannot describe it and those framebuffers are assembled here.
 */
static struct drm_framebuffer *
histb_vdp2_fb_create(struct drm_device *drm, struct drm_file *file,
		     const struct drm_format_info *info,
		     const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_gem_object *objs[DRM_FORMAT_MAX_PLANES] = { NULL };
	struct drm_framebuffer *fb;
	unsigned int i;
	int ret;

	if (drm_any_plane_has_format(drm, mode_cmd->pixel_format,
				     mode_cmd->modifier[0]))
		return drm_gem_fb_create(drm, file, info, mode_cmd);

	if (!histb_wb_fmt_find(mode_cmd->pixel_format)) {
		drm_dbg_kms(drm, "%p4cc is neither scanned out nor written back\n",
			    &mode_cmd->pixel_format);
		return ERR_PTR(-EINVAL);
	}

	for (i = 0; i < info->num_planes; i++) {
		unsigned int width = drm_format_info_plane_width(info,
							mode_cmd->width, i);
		unsigned int height = drm_format_info_plane_height(info,
							mode_cmd->height, i);
		unsigned int min_size;

		objs[i] = drm_gem_object_lookup(file, mode_cmd->handles[i]);
		if (!objs[i]) {
			drm_dbg_kms(drm, "no GEM object behind handle %u\n",
				    mode_cmd->handles[i]);
			ret = -ENOENT;
			goto err_put;
		}

		min_size = (height - 1) * mode_cmd->pitches[i] +
			   drm_format_info_min_pitch(info, i, width) +
			   mode_cmd->offsets[i];
		if (objs[i]->size < min_size) {
			drm_dbg_kms(drm, "buffer of %zu bytes is short of the %u needed for plane %u\n",
				    objs[i]->size, min_size, i);
			ret = -EINVAL;
			goto err_put;
		}
	}

	/*
	 * Plain slab allocation, not a drmm action: the destroy callback this
	 * framebuffer uses is drm_gem_fb_destroy(), which frees with kfree().
	 */
	fb = kzalloc_obj(*fb);
	if (!fb) {
		ret = -ENOMEM;
		goto err_put;
	}

	drm_helper_mode_fill_fb_struct(drm, fb, info, mode_cmd);
	for (i = 0; i < info->num_planes; i++)
		fb->obj[i] = objs[i];

	ret = drm_framebuffer_init(drm, fb, &histb_wb_fb_funcs);
	if (ret) {
		kfree(fb);
		goto err_put;
	}

	return fb;

err_put:
	while (i > 0) {
		--i;
		drm_gem_object_put(objs[i]);
	}
	return ERR_PTR(ret);
}

/* ------------------------------------------------------------------ */
/* CRTC (DHD0 channel)                                                 */
/* ------------------------------------------------------------------ */

/*
 * One-time display pipeline setup: route GP0 into the DHD0 mixer, set the AXI
 * outstanding limits and enable the block's clock gate.
 */
static void histb_vdp2_pipeline_init(struct histb_vdp2 *vdp)
{
	unsigned int i;
	u32 ctrl;

	/*
	 * Open the clock gate FIRST: every write issued while vo_ck_gt_en is
	 * clear can be dropped by the gated clock domain, which leaves the AXI
	 * side without its outstanding limits and the composition sampling
	 * black.
	 */
	ctrl = vo_read(vdp, VO_CTRL);
	ctrl |= VO_CTRL_CK_GT_EN;
	vo_write(vdp, VO_CTRL, ctrl);

	/* AXI outstanding limits: read bus 0/1 = 7, write = 4. */
	ctrl = vo_read(vdp, VOAXICTRL);
	ctrl &= ~(VO_AXI_M0_RD0_OSTD | VO_AXI_M0_RD1_OSTD | VO_AXI_M0_WR_OSTD);
	ctrl |= FIELD_PREP(VO_AXI_M0_RD0_OSTD, 7) |
		FIELD_PREP(VO_AXI_M0_RD1_OSTD, 7) |
		FIELD_PREP(VO_AXI_M0_WR_OSTD, 4);
	vo_write(vdp, VOAXICTRL, ctrl);

	/*
	 * CBM_ATTR routes each surface to one of the two mixers; the surfaces
	 * of the display channel use sur_attr2|sur_attr3, so 0xc.
	 */
	vo_write(vdp, CBM_ATTR, CBM_ATTR_SUR2 | CBM_ATTR_SUR3);

	/*
	 * Mixer background (Cr[9:0] Cb[19:10] Y[29:20]). Left at its reset 0
	 * the uncovered area is YCbCr(0,0,0), which panels render as solid
	 * green; Y=64 with both chroma at mid-scale is black.
	 */
	vo_write(vdp, CBM_BKG1, CBM_BKG_BLACK);

	/*
	 * Firmware can leave its own logo layers enabled, and they sit at a
	 * higher mixer priority than ours, so turn off every graphics layer
	 * except the one the plane drives.
	 */
	for (i = 0; i < 5; i++) {
		u32 off = G0_CTRL + i * GFX_LAYER_STRIDE;

		if (off == G0_CTRL)
			continue;
		ctrl = vo_read(vdp, off);
		if (!(ctrl & G0_CTRL_SURFACE_EN))
			continue;
		vo_write(vdp, off, ctrl & ~G0_CTRL_SURFACE_EN);
		vo_write(vdp, off + (G0_UPD - G0_CTRL), G0_UPD_REGUP);
	}

	/*
	 * Two mixers sit between the surfaces and the channel: MIXG0 takes the
	 * graphics layers, CBM_MIX1 is DHD0's. Both hold layer_id + 1 per
	 * priority slot and 0 for no slot, so G0 goes to MIXG0 slot 0 and GP0
	 * to DHD0's slot 1 (0x21 in each).
	 */
	vo_write(vdp, MIXG0_MIX,
		 FIELD_PREP(MIX_PRIO0, MIXG0_LAYER_G0) |
		 FIELD_PREP(MIX_PRIO1, MIXG0_LAYER_G1));
	vo_write(vdp, CBM_MIX1,
		 FIELD_PREP(MIX_PRIO0, CBM_LAYER_VP0) |
		 FIELD_PREP(MIX_PRIO1, CBM_LAYER_GP0));

	/*
	 * GP0 sits between the G0 fetch and the mixer (DHD0's table admits
	 * prio0 = VP0, prio1 = GP0), so the surface has to transit a post block
	 * before the mixer sees it. Which stages that output taps is one of the
	 * three whole-register values of GP0_CTRL; the working combination is
	 * GP0_ORDER_ZME_CSC with read_mode set, and the scaling step registers
	 * are programmed for 1:1 by histb_vdp2_g0_set().
	 */
	vo_write(vdp, GP0_CTRL, vdp->chip->gp0_order | GP0_CTRL_READ_MODE);
	vo_write(vdp, GP0_GALPHA, 0xff);
	/*
	 * The G0 fetch delivers its three components to this stage one position
	 * rotated, and nothing upstream can be changed to fix it: every 32bpp ifmt
	 * code this chip names only rotates the byte index of the pixel, and no
	 * 32-bit DRM format composes with that map into the honest assignment (both
	 * measured on the AR513W). This stage is the one place in the path that reorders the
	 * component lines, so it carries the inverse cycle, and it still performs no
	 * colour-space conversion -- the interface carries RGB (see
	 * histb_vdp2_crtc_setup_timing()) and the transmitter converts on its own.
	 *
	 * The gain and the output offsets are the range, not a colour tweak. The
	 * transmitter's own RGB->YCbCr converter emits full-range luma (board-measured:
	 * painted red arrives in the stream at Y = 54.0, the BT.709 full-range figure),
	 * while the AVI InfoFrame the bridge sends declares limited range -- so a receiver
	 * that obeys the declaration expands a signal that does not need it, and painted
	 * red reached the capture card at Y = 44.2 instead of 54.2. A common scale and a
	 * common offset on the three RGB lines fix that without disturbing the lane order:
	 * the luma weights sum to 1, so Y becomes 219/255*Y + 16 -- limited range exactly --
	 * and the Cb/Cr weights sum to 0, so the shared offset leaves chroma where it was.
	 * Those are also the vendor's own two numbers: their live GP0 carries the
	 * documented BT.709 row multiplied by 0.861 = 219/255, and IODC's output field
	 * holds 64, which is +16 codes at the SDK's s32DcPre = 4 (measured on the AR513W:
	 * this gain with odc 0 reads 35.81, with odc 64 it reads 54.72, odc 100 reads
	 * 64.77, and the unaltered block reads 44.20).
	 *
	 * Both halves are chip data, not family constants: every measurement behind
	 * them was taken on Hi3716CV200, and Hi3798MV100 was verified displaying with
	 * this stage bypassed, so there it stays bypassed.
	 */
	if (vdp->chip->gp0_csc_cycle) {
		vo_write(vdp, GP0_CSC_ODC, GP0_CSC_DC_ODC);
		vo_write(vdp, GP0_CSC_IODC, GP0_CSC_DC_IODC);
		vo_write(vdp, GP0_CSC_P0, GP0_CSC_CYC_P0);
		vo_write(vdp, GP0_CSC_P1, 0);
		vo_write(vdp, GP0_CSC_P2, GP0_CSC_CYC_P2);
		vo_write(vdp, GP0_CSC_P3, GP0_CSC_CYC_P3);
		vo_write(vdp, GP0_CSC_P4, 0);
		vo_write(vdp, GP0_CSC_IDC, GP0_CSC_EN);
	} else {
		vo_write(vdp, GP0_CSC_IDC, 0);
	}

	vo_write(vdp, GP0_UPD, GP0_UPD_REGUP);
}

static void histb_vdp2_crtc_setup_timing(struct histb_vdp2 *vdp,
				       const struct drm_display_mode *mode)
{
	/*
	 * The DHD totals are ACT + BB + FB -- the sync pulse lives inside the
	 * back porch, and HPW/VPW only say how wide it is there; programming a
	 * bare back porch leaves the transmitter counting too few lines.
	 */
	u32 hbb = mode->hsync_end - mode->hsync_start +	/* sync width */
		  mode->htotal - mode->hsync_end;	/* back porch */
	u32 hfb = mode->hsync_start - mode->hdisplay;	/* front porch */
	u32 hpw = mode->hsync_end - mode->hsync_start;	/* sync width */
	u32 vbb = mode->vsync_end - mode->vsync_start +
		  mode->vtotal - mode->vsync_end;
	u32 vfb = mode->vsync_start - mode->vdisplay;
	u32 vpw = mode->vsync_end - mode->vsync_start;
	bool interlaced = mode->flags & DRM_MODE_FLAG_INTERLACE;
	/*
	 * In an interlaced mode the channel emits one field at a time, so the
	 * vertical counters are per field. 1080i60 is (Vact 540, Vbb 20, Vfb 2)
	 * for the top field and (540, 21, 2) for the bottom one, which sums to
	 * the 1125-line frame.
	 */
	u32 vact = interlaced ? mode->vdisplay / 2 : mode->vdisplay;
	u32 tvbb = interlaced ? vbb / 2 : vbb;
	u32 tvfb = interlaced ? vfb / 2 : vfb;
	u32 bvbb = vbb - tvbb;
	/*
	 * HMID is where the second field starts inside the line, half a frame
	 * line in: 908 for 1080i60 and 1128 for 1080i50.
	 */
	u32 hmid = interlaced ? mode->htotal / 2 - hbb : 0;
	u32 vtthd, ctrl, inv = 0;

	ctrl = vo_read(vdp, DHD0_CTRL);
	if (interlaced)
		ctrl &= ~DHD_CTRL_IOP;		/* field sequential */
	else
		ctrl |= DHD_CTRL_IOP;		/* progressive */
	/*
	 * hdmi_mode (bit14) and DATA_SEL.hdmi_data_sel are one decision: both
	 * carry the interface data type, 1 meaning RGB on the HDMI pins and 0
	 * YCbCr.
	 *
	 * RGB is used for every format here. Passing YCbCr straight through
	 * also needs the output-stage lane mux (video1_sel / video2_sel /
	 * video3_sel / src_ctrl) reprogrammed to match; with the mux left at
	 * its reset values the two chroma lanes arrive exchanged and Cb carries
	 * nothing, so only the luma path is right. Nothing on the RGB path
	 * needs those selects.
	 */
	if (vdp->chip->dhd_hdmi_mode)
		ctrl |= DHD_CTRL_HDMI_MODE;
	/*
	 * bit15 is reserved in the mv100 layout, but a channel that drives a
	 * panel has it set on both parts: the Hi3716CV200 channel that was
	 * displaying measured 0x80008010, which has it and has hdmi_mode clear.
	 */
	if (vdp->chip->dhd_intf_b)
		ctrl |= DHD_CTRL_INTFB;
	ctrl |= DHD_CTRL_INTF_EN;		/* drive the HDMI interface */
	vo_write(vdp, DHD0_CTRL, ctrl);

	/* horizontal: registers take value - 1 */
	vo_write(vdp, DHD0_HSYNC1,
		 FIELD_PREP(DHD_HSYNC1_HACT, histb_vdp2_m1(mode->hdisplay)) |
		 FIELD_PREP(DHD_HSYNC1_HBB, histb_vdp2_m1(hbb)));
	vo_write(vdp, DHD0_HSYNC2,
		 FIELD_PREP(DHD_HSYNC2_HFB, histb_vdp2_m1(hfb)) |
		 FIELD_PREP(DHD_HSYNC2_HMID, hmid ? hmid - 1 : 0));
	vo_write(vdp, DHD0_PWR,
		 FIELD_PREP(DHD_PWR_HPW, histb_vdp2_m1(hpw)) |
		 FIELD_PREP(DHD_PWR_VPW, histb_vdp2_m1(vpw)));

	/* vertical: top field in VSYNC, bottom field in VPLUS; for progressive
	 * modes the bottom values mirror the top ones.
	 */
	vo_write(vdp, DHD0_VSYNC,
		 FIELD_PREP(DHD_VSYNC_VACT, histb_vdp2_m1(vact)) |
		 FIELD_PREP(DHD_VSYNC_VBB, histb_vdp2_m1(tvbb)) |
		 FIELD_PREP(DHD_VSYNC_VFB, histb_vdp2_m1(tvfb)));
	vo_write(vdp, DHD0_VPLUS,
		 FIELD_PREP(DHD_VPLUS_BVACT, histb_vdp2_m1(vact)) |
		 FIELD_PREP(DHD_VPLUS_BVBB, histb_vdp2_m1(bvbb)) |
		 FIELD_PREP(DHD_VPLUS_BVFB, histb_vdp2_m1(tvfb)));

	/* vblank interrupt threshold: 8/10 of the frame; frame based mode. */
	vtthd = (tvbb + vact + tvfb) * 8 / 10;
	vo_write(vdp, DHD0_VTTHD,
		 FIELD_PREP(DHD_VTTHD1, vtthd) | DHD_VTTHD1_MODE);

	/*
	 * sync polarity towards the HDMI transmitter: the *_INV bits invert the
	 * corresponding sync; non-inverted output equals DRM positive sync.
	 */
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		inv |= DHD_SYNC_HDMI_HS_INV;
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		inv |= DHD_SYNC_HDMI_VS_INV;
	vo_write(vdp, DHD0_SYNC_INV, inv);

	/*
	 * Set our bit of the interface data type and leave the rest as the
	 * reset state has it: bit 3 is hdmi_data_sel, bit 1 is vga_data_sel and
	 * bit 2 is reserved on mv100, so this has to be a read-modify-write.
	 * hdmi_mode above carries the same decision.
	 */
	ctrl = vo_read(vdp, DHD0_DATA_SEL);
	ctrl |= DHD_DATA_SEL_HDMI;
	vo_write(vdp, DHD0_DATA_SEL, ctrl);

	/* latch the timing registers */
	ctrl = vo_read(vdp, DHD0_CTRL);
	ctrl |= DHD_CTRL_REGUP;
	vo_write(vdp, DHD0_CTRL, ctrl);

	/*
	 * The DSD channel (0xc400..) is left alone: the HDMI interface is
	 * driven from DHD0, and enabling it on channel 1 would only re-route a
	 * timing this port does not build.
	 */
}

static enum drm_mode_status histb_vdp2_crtc_mode_valid(struct drm_crtc *crtc,
						     const struct drm_display_mode *mode)
{
	struct histb_vdp2 *vdp = crtc_to_vdp(crtc);
	long rate;

	/* G0/GP0 position and resolution fields are 12 bits wide */
	if (mode->hdisplay > 4095 || mode->vdisplay > 4095)
		return MODE_BAD;

	/* 3840x2160 is the largest mode the pipeline reaches */
	if (mode->hdisplay > 3840 || mode->vdisplay > 2160)
		return MODE_BAD;

	if (mode->hdisplay % 2 || mode->vdisplay % 2)
		return MODE_BAD;

	/* the HD PLL and channel divider can only produce certain rates */
	rate = clk_round_rate(vdp->clk_pix, mode->clock * 1000);
	if (rate < 0)
		return MODE_ERROR;

	/*
	 * The clock can only produce the rates its own table holds, which are
	 * what each format's timing adds up to -- a few hundred Hz away from
	 * the nominal clock the EDID rounds into mode->clock. Allow half a
	 * percent; keep the arithmetic in Hz so a large mismatch cannot
	 * overflow and look like a pass.
	 */
	if (abs(rate - (long)mode->clock * 1000) > (long)mode->clock * 5)
		return MODE_NOCLOCK;

	return MODE_OK;
}

/*
 * On Hi3716CV200 the composed frame reaches the HDMI transmitter through the
 * HD digital output port (HDATE), which resamples it with an 18-tap filter.
 * Left disabled the channel still counts frames and the transmitter's TMDS
 * clock still locks, but the transmitter measures no input timing and the
 * sink shows black. The taps and the format codes below are what a
 * displaying Hi3716CV200 carries.
 */
static const u32 histb_vdp2_hdate_coef[] = {
	0x00010000, 0x07fd0000, 0x00070000, 0x07f30000, 0x00160000, 0x07db0000,
	0x003d07ff, 0x078e0003, 0x014c07f2, 0x014c0218, 0x078e07f2, 0x003d0003,
	0x07db07ff,
	0x00160000, 0x07f30000, 0x00070000, 0x07fd0000, 0x00010000,
};

static void histb_vdp2_hdate_setup(struct histb_vdp2 *vdp,
				   const struct drm_display_mode *mode)
{
	unsigned int i, ctrl, ft = mode->hdisplay <= 1280 ? 2 : 4;

	BUILD_BUG_ON(ARRAY_SIZE(histb_vdp2_hdate_coef) !=
		     HDATE_SRC_13_COEF_HEAD + HDATE_SRC_13_COEF_TAIL);

	for (i = 0; i < HDATE_SRC_13_COEF_HEAD; i++)
		vo_write(vdp, HDATE_SRC_13_COEF1 + i * 4,
			 histb_vdp2_hdate_coef[i]);
	for (i = 0; i < HDATE_SRC_13_COEF_TAIL; i++)
		vo_write(vdp, HDATE_SRC_13_COEF14 + i * 4,
			 histb_vdp2_hdate_coef[HDATE_SRC_13_COEF_HEAD + i]);

	/*
	 * The port needs its horizontal sync inverted. With date_hs_inv clear
	 * the port counts lines but never locks a frame -- HDATE_STATE keeps
	 * frame_len at 0 -- and setting it makes the same read show the
	 * programmed line total.
	 */
	ctrl = vo_read(vdp, DHD0_SYNC_INV);
	vo_write(vdp, DHD0_SYNC_INV, ctrl | DHD_SYNC_DATE_HS_INV);

	vo_write(vdp, HDATE_POLA_CTRL, HDATE_POLA_IN);
	vo_write(vdp, HDATE_VIDEO_FORMAT,
		 0xa0 | FIELD_PREP(HDATE_VIDEO_FT, ft));
	/*
	 * The three source slots rotate by one: measured on a displaying board,
	 * where this word reads 0x1b0 (video1_sel 1, video2_sel 2, video3_sel 3).
	 */
	vo_write(vdp, HDATE_OUT_CTRL,
		 FIELD_PREP(HDATE_OUT_VIDEO1_SEL, 1) |
		 FIELD_PREP(HDATE_OUT_VIDEO2_SEL, 2) |
		 FIELD_PREP(HDATE_OUT_VIDEO3_SEL, 3));
	vo_write(vdp, HDATE_EN, HDATE_EN_HD);
}

static void histb_vdp2_crtc_atomic_enable(struct drm_crtc *crtc,
					struct drm_atomic_commit *state)
{
	struct histb_vdp2 *vdp = crtc_to_vdp(crtc);
	struct drm_crtc_state *new_state = drm_atomic_get_new_crtc_state(state, crtc);
	const struct drm_display_mode *mode = &new_state->adjusted_mode;
	struct drm_plane_state *pstate;
	struct drm_gem_dma_object *gem;
	struct drm_framebuffer *fb;
	u32 ctrl;

	/* the channel clock is what the HDMI transmitter receives, too */
	histb_vdp2_set_pixclk(vdp, mode->clock);

	histb_vdp2_pipeline_init(vdp);

	histb_vdp2_crtc_setup_timing(vdp, mode);

	/*
	 * VO_MUX bit3 names the channel the HDMI interface reads, it does not
	 * switch the interface on: the SDK writes hdmi_sel = the channel id of
	 * the display that owns the interface, and a displaying Hi3716CV200
	 * carries 1 because its primary display is channel 1. This driver builds
	 * its timing on DHD0, so the bit has to stay clear -- leaving the value
	 * the vendor board shows pointed the interface at a channel that was
	 * never programmed, and the transmitter measured no timing at all.
	 * Hi3798MV100 inherits its routing from its boot and is not touched.
	 */
	if (vdp->chip->mux_hdmi_dhd0) {
		ctrl = vo_read(vdp, VO_MUX);
		ctrl &= ~VO_MUX_HDMI_SEL;
		vo_write(vdp, VO_MUX, ctrl);
	}

	if (vdp->chip->hdate_hdmi_out)
		histb_vdp2_hdate_setup(vdp, mode);

	/* enable the channel */
	ctrl = vo_read(vdp, DHD0_CTRL);
	ctrl |= DHD_CTRL_INTF_EN | DHD_CTRL_REGUP;
	vo_write(vdp, DHD0_CTRL, ctrl);

	/*
	 * commit_planes runs before modeset_enables, and G0 drops register
	 * writes while the VDP pipeline is not up (the block then reads back
	 * its reset values), so (re)program the layer from the fb the commit
	 * just installed.
	 */
	pstate = drm_atomic_get_new_plane_state(state, crtc->primary);
	if (!pstate)
		pstate = crtc->primary->state;
	fb = pstate->fb;
	if (fb) {
		gem = drm_fb_dma_get_gem_obj(fb, 0);
		if (gem)
			histb_vdp2_g0_set(vdp, fb, gem, pstate->visible);
	}

	drm_crtc_vblank_on(crtc);
}

static void histb_vdp2_crtc_atomic_disable(struct drm_crtc *crtc,
					 struct drm_atomic_commit *state)
{
	struct histb_vdp2 *vdp = crtc_to_vdp(crtc);
	u32 ctrl;

	drm_crtc_vblank_off(crtc);

	if (vdp->chip->hdate_hdmi_out)
		vo_write(vdp, HDATE_EN, 0);

	ctrl = vo_read(vdp, DHD0_CTRL);
	ctrl &= ~DHD_CTRL_INTF_EN;
	ctrl |= DHD_CTRL_REGUP;
	vo_write(vdp, DHD0_CTRL, ctrl);
}

static int histb_vdp2_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct histb_vdp2 *vdp = crtc_to_vdp(crtc);

	if (vdp->irq < 0)
		return -EOPNOTSUPP;

	/* unmask the DHD0 threshold-1 interrupt */
	vo_write(vdp, VO_INTMSK,
		 vo_read(vdp, VO_INTMSK) & ~VO_INT_DHD0_VTTHD1);
	return 0;
}

static void histb_vdp2_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct histb_vdp2 *vdp = crtc_to_vdp(crtc);

	if (vdp->irq < 0)
		return;

	vo_write(vdp, VO_INTMSK,
		 vo_read(vdp, VO_INTMSK) | VO_INT_DHD0_VTTHD1);
}

static void histb_vdp2_crtc_atomic_flush(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new_state = drm_atomic_get_new_crtc_state(state, crtc);
	struct histb_vdp2_display *disp = container_of(crtc,
						  struct histb_vdp2_display, crtc);
	struct drm_connector_state *conn_state;
	struct drm_framebuffer *fb;
	const struct histb_wb_fmt *fmt;

	struct drm_writeback_connector *wbs[2] = { &disp->wb, &disp->wb_gp0 };
	unsigned int tap;

	for (tap = 0; tap < ARRAY_SIZE(wbs); tap++) {
		struct drm_writeback_connector *wb = wbs[tap];

		conn_state = drm_atomic_get_new_connector_state(state, &wb->base);
		if (!conn_state || !conn_state->writeback_job ||
		    !conn_state->writeback_job->fb)
			continue;
		/*
		 * One capture at a time: a job that cannot be armed stays queued
		 * for the next commit rather than being dropped silently, which
		 * would leave the client waiting for a writeback fence.
		 */
		if (READ_ONCE(disp->wb_pending))
			break;
		fb = conn_state->writeback_job->fb;
		fmt = histb_wb_fmt_find(fb->format->format);
		if (!fmt)
			continue;
		histb_wb_start(disp, fmt, fb, wb);
		drm_writeback_queue_job(wb, conn_state);
	}

	/*
	 * The VDP latches the whole frame at once and the vblank IRQ only marks
	 * the count, so send the flip event right after the commit.
	 */
	if (new_state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, new_state->event);
		new_state->event = NULL;	/* consumed: keep hw_done quiet */
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static const struct drm_crtc_helper_funcs histb_vdp2_crtc_helper_funcs = {
	.mode_valid		= histb_vdp2_crtc_mode_valid,
	.atomic_enable		= histb_vdp2_crtc_atomic_enable,
	.atomic_disable		= histb_vdp2_crtc_atomic_disable,
	.atomic_flush		= histb_vdp2_crtc_atomic_flush,
};

static const struct drm_crtc_funcs histb_vdp2_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.destroy		= drm_crtc_cleanup,
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.enable_vblank		= histb_vdp2_crtc_enable_vblank,
	.disable_vblank		= histb_vdp2_crtc_disable_vblank,
};

static irqreturn_t histb_vdp2_irq_handler(int irq, void *data)
{
	struct histb_vdp2 *vdp = data;
	struct histb_vdp2_display *disp = READ_ONCE(vdp->disp);
	u32 all = vo_read(vdp, VO_INTSTA);
	u32 sta = all & ~vo_read(vdp, VO_INTMSK);

	if (!all)
		return IRQ_NONE;

	/*
	 * Ack every latched source, not just the vblank one: returning IRQ_NONE
	 * while a VDP status bit stays set makes the core disable the line
	 * ("nobody cared") and vblank dies with it. Sources the driver masks
	 * off still latch, so dispatch on the unmasked subset only -- otherwise
	 * the frame-threshold bits that nobody enables print a warning once per
	 * vblank.
	 */
	vo_write(vdp, VO_MSKINTSTA, all);

	if (sta & ~VO_INT_DHD0_VTTHD1)
		dev_warn_ratelimited(vdp->dev, "vdp irq: unexpected status %08x\n",
				     sta);

	/* the line stays requested across an aggregate unbind, no CRTC then */
	if (disp) {
		if (sta & VO_INT_DHD0_VTTHD1)
			drm_crtc_handle_vblank(&disp->crtc);
		histb_wb_update(disp, all, sta);
	}

	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Mode config / binding                                               */
/* ------------------------------------------------------------------ */

static const struct drm_mode_config_funcs histb_vdp2_mode_config_funcs = {
	.fb_create		= histb_vdp2_fb_create,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

static const struct drm_encoder_funcs histb_vdp2_encoder_funcs = {
	.destroy		= drm_encoder_cleanup,
};

static const struct drm_encoder_helper_funcs histb_vdp2_encoder_helper_funcs = {
};

DEFINE_DRM_GEM_DMA_FOPS(histb_vdp2_fops);

static const struct drm_driver histb_vdp2_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &histb_vdp2_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name			= "histb-vdp2",
	.desc			= "HiSTB VDP v2 display engine",
	.major			= 1,
	.minor			= 0,
};

static int histb_vdp2_bind(struct device *dev)
{
	struct histb_vdp2 *vdp = dev_get_drvdata(dev);
	struct histb_vdp2_display *disp;
	struct drm_device *drm;
	struct drm_bridge *bridge;
	struct drm_connector *connector;
	int ret;

	ret = component_bind_all(dev, NULL);
	if (ret)
		return dev_err_probe(dev, ret, "component_bind_all failed\n");

	/*
	 * Only the devm variant of this lookup exists, so each rebind adds a
	 * bridge reference tied to this device rather than to the aggregate.
	 * They are all dropped when the device goes away; the transmitter
	 * outlives the aggregate by construction (it is a component of it).
	 */
	bridge = devm_drm_of_get_bridge(dev, dev->of_node, 0, 0);
	if (IS_ERR(bridge)) {
		ret = PTR_ERR(bridge);
		dev_err_probe(dev, ret, "failed to find downstream HDMI bridge\n");
		goto err_unbind;
	}

	drm = drm_dev_alloc(&histb_vdp2_driver, dev);
	if (IS_ERR(drm)) {
		ret = PTR_ERR(drm);
		dev_err_probe(dev, ret, "drm_dev_alloc failed\n");
		goto err_unbind;
	}

	disp = drmm_kzalloc(drm, sizeof(*disp), GFP_KERNEL);
	if (!disp) {
		ret = -ENOMEM;
		goto err_put;
	}
	disp->vdp = vdp;
	disp->drm = drm;
	drm->dev_private = disp;
	WRITE_ONCE(vdp->disp, disp);

	ret = drmm_mode_config_init(drm);
	if (ret) {
		dev_err_probe(dev, ret, "mode_config_init failed\n");
		goto err_put;
	}

	drm->mode_config.min_width	= 64;
	drm->mode_config.max_width	= 3840;
	drm->mode_config.min_height	= 64;
	drm->mode_config.max_height	= 2160;
	drm->mode_config.funcs		= &histb_vdp2_mode_config_funcs;

	/* one CRTC, so the vblank core must exist before the IRQ can be served
	 * and before any atomic commit waits on it
	 */
	ret = drm_vblank_init(drm, 1);
	if (ret) {
		dev_err_probe(dev, ret, "vblank_init failed\n");
		goto err_put;
	}

	ret = drm_universal_plane_init(drm, &disp->primary, 0,
				       &histb_vdp2_plane_funcs,
				       histb_vdp2_formats,
				       ARRAY_SIZE(histb_vdp2_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		dev_err_probe(dev, ret, "plane_init failed\n");
		goto err_put;
	}
	drm_plane_helper_add(&disp->primary, &histb_vdp2_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &disp->crtc, &disp->primary, NULL,
					&histb_vdp2_crtc_funcs, "histb-vdp2-crtc");
	if (ret) {
		dev_err_probe(dev, ret, "crtc_init failed\n");
		goto err_put;
	}
	drm_crtc_helper_add(&disp->crtc, &histb_vdp2_crtc_helper_funcs);
	drm_crtc_vblank_reset(&disp->crtc);

	disp->encoder.possible_crtcs = drm_crtc_mask(&disp->crtc);
	ret = drm_encoder_init(drm, &disp->encoder, &histb_vdp2_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, "histb-vdp2-encoder");
	if (ret) {
		dev_err_probe(dev, ret, "encoder_init failed\n");
		goto err_put;
	}
	drm_encoder_helper_add(&disp->encoder, &histb_vdp2_encoder_helper_funcs);

	ret = drm_bridge_attach(&disp->encoder, bridge, NULL,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret) {
		dev_err_probe(dev, ret, "bridge_attach failed\n");
		goto err_put;
	}

	connector = drm_bridge_connector_init(drm, &disp->encoder);
	if (IS_ERR(connector)) {
		dev_err_probe(dev, PTR_ERR(connector),
			      "failed to init bridge connector\n");
		goto err_put;
	}
	drm_connector_attach_encoder(connector, &disp->encoder);

	ret = histb_wb_init(disp);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init the write-back connector\n");
		goto err_put;
	}

	/*
	 * The transmitter has no usable HPD output (it is a level signal that
	 * storms while a sink is attached), so the connector the bridge
	 * helper created asks for DRM_CONNECTOR_POLL_CONNECT|DISCONNECT.
	 * Without starting the poll work here nothing would ever re-detect a
	 * plug event. The drmm variant stops with the DRM device.
	 */
	drmm_kms_helper_poll_init(drm);

	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		dev_err_probe(dev, ret, "drm_dev_register failed\n");
		goto err_put;
	}

	drm_client_setup(drm, NULL);

	return 0;

err_put:
	WRITE_ONCE(vdp->disp, NULL);
	drm_dev_put(drm);
err_unbind:
	component_unbind_all(dev, NULL);
	return ret;
}

static void histb_vdp2_unbind(struct device *dev)
{
	struct histb_vdp2 *vdp = dev_get_drvdata(dev);
	struct histb_vdp2_display *disp = vdp->disp;
	struct drm_device *drm = disp->drm;

	/* drm_dev_unplug() also unregisters; tear down while clocks are on */
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	WRITE_ONCE(vdp->disp, NULL);
	component_unbind_all(dev, NULL);
	drm_dev_put(drm);
}

/*
 * Who belongs to this pipeline is read out of the device tree, not out of the
 * order someone happens to echo into unbind: the transmitter on the far side of
 * our own graph endpoint is added as a component of this aggregate, so removing
 * its driver takes the DRM device down before that driver's bridge, regmaps and
 * MMIO are released. Without it, unbinding the transmitter under a live card
 * lets the atomic paths dereference the freed bridge.
 */
static const struct component_master_ops histb_vdp2_master_ops = {
	.bind		= histb_vdp2_bind,
	.unbind		= histb_vdp2_unbind,
};

static int histb_vdp2_add_master(struct device *dev)
{
	struct component_match *match = NULL;
	struct device_node *endpoint;

	for_each_endpoint_of_node(dev->of_node, endpoint) {
		struct device_node *remote =
			of_graph_get_remote_port_parent(endpoint);

		if (remote && of_device_is_available(remote))
			drm_of_component_match_add(dev, &match,
						   component_compare_of, remote);
		of_node_put(remote);
	}

	return component_master_add_with_match(dev, &histb_vdp2_master_ops, match);
}

/*
 * The VOU reset request is CRG54[30]. A reset only reaches the block while it
 * is clocked, so the gates come up first: CRG54's own gate group is the clock
 * "vo" and the HD channel clock is "pix", both enabled by
 * histb_vdp2_runtime_resume(). The reset has to be held before it is released
 * and the block needs settling time afterwards, so the pulse comes with delays.
 */
static void histb_vdp2_hw_reset(struct histb_vdp2 *vdp)
{
	/*
	 * DHD0_CTRL is the readback that says the pulse landed: whoever used
	 * the display before us normally leaves the HD channel enabled, and a
	 * VOU reset clears it back to the default. Both reads happen outside
	 * the hold window, since a block in reset need not answer.
	 */
	dev_dbg(vdp->dev, "before reset: DHD0_CTRL %08x\n", vo_read(vdp, DHD0_CTRL));

	reset_control_assert(vdp->rst);
	usleep_range(5000, 5200);
	reset_control_deassert(vdp->rst);
	usleep_range(1000, 1200);

	dev_dbg(vdp->dev, "after reset: DHD0_CTRL %08x\n", vo_read(vdp, DHD0_CTRL));
}

static int histb_vdp2_runtime_suspend(struct device *dev)
{
	struct histb_vdp2 *vdp = dev_get_drvdata(dev);

	clk_disable_unprepare(vdp->clk_vo);
	clk_disable_unprepare(vdp->clk_pix);
	return 0;
}

static int histb_vdp2_runtime_resume(struct device *dev)
{
	struct histb_vdp2 *vdp = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(vdp->clk_pix);
	if (ret)
		return ret;

	ret = clk_prepare_enable(vdp->clk_vo);
	if (ret) {
		clk_disable_unprepare(vdp->clk_pix);
		return ret;
	}

	histb_vdp2_hw_reset(vdp);

	return 0;
}

static const struct dev_pm_ops histb_vdp2_pm_ops = {
	RUNTIME_PM_OPS(histb_vdp2_runtime_suspend, histb_vdp2_runtime_resume, NULL)
};

static int histb_vdp2_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_vdp2 *vdp;
	int ret;

	vdp = devm_kzalloc(dev, sizeof(*vdp), GFP_KERNEL);
	if (!vdp)
		return -ENOMEM;

	vdp->chip = of_device_get_match_data(dev);
	if (!vdp->chip)
		return -ENODEV;
	vdp->zme = vdp->chip->zme;

	vdp->dev = dev;
	platform_set_drvdata(pdev, vdp);

	vdp->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vdp->regs))
		return dev_err_probe(dev, PTR_ERR(vdp->regs), "ioremap failed\n");

	/*
	 * The HD channel clock, which the CRG driver builds from the HD PLL and
	 * CRG54 (see histb_vdp2_set_pixclk()), and CRG54's gate group for the
	 * rest of the channel path; the VOU reset only shifts while both of
	 * them run.
	 */
	vdp->clk_pix = devm_clk_get(dev, "pix");
	if (IS_ERR(vdp->clk_pix))
		return dev_err_probe(dev, PTR_ERR(vdp->clk_pix), "clk pix\n");

	vdp->clk_vo = devm_clk_get(dev, "vo");
	if (IS_ERR(vdp->clk_vo))
		return dev_err_probe(dev, PTR_ERR(vdp->clk_vo), "clk vo\n");

	vdp->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(vdp->rst))
		return dev_err_probe(dev, PTR_ERR(vdp->rst), "rst\n");

	/*
	 * Power the block up before anything reads or writes a VDP register:
	 * histb_vdp2_runtime_resume() enables the channel clock and takes the
	 * VOU through a reset of our own, so the register file starts from its
	 * documented defaults instead of from the state it was handed over in.
	 * The device then stays permanently powered for the whole probe/remove
	 * lifetime: the atomic paths and the vblank IRQ touch registers with no
	 * pm_runtime references of their own, and the shared pixel clock makes
	 * idle power-down of little use anyway. histb_vdp2_remove() drops the
	 * reference again; an aggregate unbind leaves the clocks up because the
	 * transmitter may bind again.
	 */
	pm_runtime_enable(dev);
	ret = pm_runtime_resume_and_get(dev);
	if (ret) {
		dev_err_probe(dev, ret, "pm_runtime_resume failed\n");
		goto err_pm_disable;
	}

	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err_probe(dev, ret, "dma_mask failed\n");
		goto err_pm_put;
	}

	/*
	 * Frame buffers have to be physically contiguous: this channel takes
	 * one base address (VO_G0_ADDR), and the chip has no MMU on the display
	 * side, so an imported buffer is refused unless its scatter/gather
	 * table covers the whole object. Bind the pool named by the
	 * "memory-region" property, which keeps them out of the default CMA
	 * area the decoder queues allocate from. A board whose DTB has no such
	 * property is not an error (-ENODEV): the device then keeps using the
	 * default pool.
	 */
	ret = devm_of_reserved_mem_device_init(dev);
	if (ret && ret != -ENODEV) {
		dev_err_probe(dev, ret, "memory-region init failed\n");
		goto err_pm_put;
	}

	/*
	 * Clear any latched status and mask every source before requesting the
	 * line: a pending bit arriving on an unmasked line makes the core
	 * disable the interrupt ("nobody cared"). drm_crtc_vblank_on() unmasks
	 * VTTHD1 later.
	 */
	vdp->irq = platform_get_irq_optional(pdev, 0);
	if (vdp->irq >= 0) {
		vo_write(vdp, VO_MSKINTSTA, ~0u);
		vo_write(vdp, VO_INTMSK, ~0u);

		ret = devm_request_irq(dev, vdp->irq, histb_vdp2_irq_handler,
				       IRQF_SHARED, dev_name(dev), vdp);
		if (ret) {
			dev_err_probe(dev, ret, "request_irq failed\n");
			goto err_pm_put;
		}
	}

	ret = histb_vdp2_add_master(dev);
	if (ret)
		goto err_pm_put;

	return 0;

err_pm_put:
	pm_runtime_put_sync(dev);
err_pm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static void histb_vdp2_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &histb_vdp2_master_ops);

	pm_runtime_put_sync_suspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

/*
 * drm_dev_register() documents that a KMS driver stops the pipeline at shutdown
 * and reboot: the channel would otherwise keep scanning a framebuffer whose
 * pages are about to be torn down underneath it.
 */
static void histb_vdp2_shutdown(struct platform_device *pdev)
{
	struct histb_vdp2 *vdp = platform_get_drvdata(pdev);
	struct histb_vdp2_display *disp = READ_ONCE(vdp->disp);

	if (disp)
		drm_atomic_helper_shutdown(disp->drm);
}

static const struct of_device_id histb_vdp2_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-vdp",
	  .data = &histb_vdp2_mv100 },
	/*
	 * Hi3716CV200 drives its output channel through the same register file: of
	 * the 48 registers this driver touches, every offset is identical on this
	 * part -- counted against msp/drv/vo/vdp_v1_0/hal/cv200/vdp_reg.h -- except
	 * the GP0 zoom block, whose cv200 offsets and 4.12 precisions are recorded
	 * in histb-vdp2-regs.h beside the board reading that fixed them.
	 */
	{ .compatible = "hisilicon,hi3716cv200-vdp",
	  .data = &histb_vdp2_cv200 },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_vdp2_of_match);

static struct platform_driver histb_vdp2_platform_driver = {
	.probe		= histb_vdp2_probe,
	.remove		= histb_vdp2_remove,
	.shutdown	= histb_vdp2_shutdown,
	.driver		= {
		.name			= "histb-vdp2",
		.of_match_table		= histb_vdp2_of_match,
		.pm			= pm_ptr(&histb_vdp2_pm_ops),
	},
};
module_platform_driver(histb_vdp2_platform_driver);

MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_DESCRIPTION("DRM driver for the HiSTB VDP v2 display engine (hi3798mv100)");
MODULE_LICENSE("GPL");
