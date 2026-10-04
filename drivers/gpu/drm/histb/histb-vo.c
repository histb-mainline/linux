// SPDX-License-Identifier: GPL-2.0
/*
 * DRM driver for the HiSTB VO (VDP v2) display controller,
 * found on hi3798mv100 / hi3796mv100 / hi3716dv100 SoCs.
 *
 * The VO block at 0xf8cc0000 provides two display channels.  This driver
 * exposes DHD0 (the HD channel) as a single CRTC with a primary plane fed
 * by the G0 graphics (RGB) layer, routed G0 -> GP0 -> MIXG0 -> CBM(MIX0)
 * -> DHD0 -> HDMI.  Scanout data leaves the VO towards the HDMI transmitter
 * selected through of_graph port@0; the downstream node is a drm_bridge
 * (hisilicon,histb-hdmi-1-4).
 *
 * Hardware facts extracted from the vendor SDK
 * (HiSTBLinuxV100R005C00SPC050, msp/drv/vo/vdp_v2_0):
 *  - hi3798mv100 uses the vdp_v2_0 generation with the "3798m" HAL
 *    (see vo/Makefile: hi3798mv100 -> HAL_DIR=3798m, VO_VER=vdp_v2_0).
 *  - VO register base 0xf8cc0000 (vdp_define.h DISP_BASE_ADDRESS).
 *  - The VO interrupt is number 122 in the SDK numbering
 *    (vdp_define.h DISP_INT_NUMBER), driven by the DHD0_VTTHD thresholds.
 *  - Clocking: the HD channel is fed by the CRG's HD PLL through a post
 *    divider and CRG54's vo_hd_clk_div (offset 0xd8), i.e. the channel
 *    clock is half the PLL output.  drivers/clk/hisilicon models that as
 *    "clk_vo_hd", which is also what the HDMI transmitter receives 1:1, so
 *    the two blocks cannot disagree about the pixel clock.  The driver
 *    enables the CRG54 gates by hand and pulses CRG54 bit 30 before it
 *    configures anything, so the display controller comes up from its reset
 *    defaults rather than from whatever the boot left behind.
 *
 * Tested on an EC6108V9C board (hi3798mv100): console and modes come out
 * 1:1 on a 1920x1080 panel through the on-chip HDMI transmitter.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/clients/drm_client_setup.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_of.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "histb-vo-regs.h"

struct histb_vo {
	struct device		*dev;
	void __iomem		*regs;

	struct clk		*clk_pix;	/* HD channel clock, shared with HDMI */
	struct clk		*clk_vo;	/* CRG54 gate group of the channel */

	struct reset_control	*rst;

	int			irq;

	struct histb_vo_display	*disp;	/* NULL unless the pipeline is bound */
};

/*
 * The DRM objects belong to the aggregate driver, not to the platform device:
 * they come into existence when the pipeline the device tree describes is up
 * and go away when it comes down, which can happen while this device stays
 * probed (the transmitter is a component of the aggregate).  devm cannot
 * cover that lifetime, so this block is allocated by the aggregate's bind and
 * released by its unbind.
 */
struct histb_vo_display {
	struct histb_vo		*vo;
	struct drm_device	*drm;

	struct drm_plane	primary;
	struct drm_crtc		crtc;
	struct drm_encoder	encoder;
};

static inline struct histb_vo *plane_to_vo(struct drm_plane *plane)
{
	return container_of(plane, struct histb_vo_display, primary)->vo;
}

static inline struct histb_vo *crtc_to_vo(struct drm_crtc *crtc)
{
	return container_of(crtc, struct histb_vo_display, crtc)->vo;
}

/*
 * The mode's pixel clock is the HD channel clock, which the CRG driver
 * builds from the HD PLL's post divider and CRG54's vo_hd_clk_div.  The
 * vendor format table always leaves vo_hd_clk_div at /2, so the PLL runs at
 * twice the pixel clock: 297MHz for 1080p60, 148.5MHz for 720p and the
 * 1080p24-30 family.  Requesting the channel rate is therefore enough for
 * the transmitter to see the same clock, since it takes vo_hd 1:1.
 */
static int histb_vo_set_pixclk(struct histb_vo *vo, unsigned int mode_khz)
{
	unsigned long rate = (unsigned long)mode_khz * 1000;
	int ret;

	ret = clk_set_rate(vo->clk_pix, rate);
	if (ret) {
		dev_err(vo->dev, "failed to set pixel clock to %lu Hz: %d\n",
			rate, ret);
		return ret;
	}

	dev_dbg(vo->dev, "pixel clock %lu Hz for mode clock %u kHz\n",
		clk_get_rate(vo->clk_pix), mode_khz);
	return 0;
}

static inline u32 vo_read(struct histb_vo *vo, u32 off)
{
	return readl_relaxed(vo->regs + off);
}

static inline void vo_write(struct histb_vo *vo, u32 off, u32 val)
{
	writel_relaxed(val, vo->regs + off);
}

/* ------------------------------------------------------------------ */
/* Plane (G0 graphics layer)                                           */
/* ------------------------------------------------------------------ */

/*
 * No per-pixel alpha is programmed in the atomic update path, so the
 * ARGB format is deliberately not exposed (drm core would warn about
 * alpha formats without blend mode support).
 */
static const u32 histb_vo_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* `value - 1`, clamped so that a zero porch/sync width never underflows */
static u32 histb_vo_m1(u32 v)
{
	return max(v, 1u) - 1;
}

static void histb_vo_g0_set(struct histb_vo *vo, const struct drm_framebuffer *fb,
			    struct drm_gem_dma_object *gem, bool enable)
{
	u32 ctrl, ifmt, palpha;

	if (!enable) {
		ctrl = vo_read(vo, G0_CTRL);
		ctrl &= ~G0_CTRL_SURFACE_EN;
		vo_write(vo, G0_CTRL, ctrl);
		vo_write(vo, G0_UPD, G0_UPD_REGUP);
		return;
	}

	/*
	 * Pixel format.  The A-prefixed hardware codes consume the buffer's
	 * alpha byte, so an XRGB framebuffer must not be described as
	 * ARGB_8888: every pixel would carry alpha 0x00 and the layer would
	 * composite as fully transparent (verified on the board by filling
	 * the boot logo's buffer with 0x00ffffff, which blanks the panel).
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
	 * bitext / read_mode / upd_mode are part of the SDK's layer init
	 * (drv_hifb_adp.c:1158-1172) and all set in the state that actually
	 * drives the panel (G1_CTRL = 0xcc000368).  Left at 0, read_mode means
	 * VDP_RMODE_INTERFACE: the fetch follows the interface's field coding
	 * instead of running progressive.
	 */
	ctrl = vo_read(vo, G0_CTRL);
	ctrl &= ~(G0_CTRL_IFMT | G0_CTRL_BITEXT | G0_CTRL_READ_MODE |
		  G0_CTRL_UPD_MODE);
	ctrl |= FIELD_PREP(G0_CTRL_IFMT, ifmt) |
		FIELD_PREP(G0_CTRL_BITEXT, G0_BITEXT_3RD) |
		G0_CTRL_READ_MODE | G0_CTRL_UPD_MODE;
	vo_write(vo, G0_CTRL, ctrl);

	/* opaque surface, no colour key, no per-pixel alpha */
	ctrl = vo_read(vo, G0_CBMPARA);
	ctrl &= ~(G0_CBMPARA_GALPHA | G0_CBMPARA_PALPHA_EN |
		  G0_CBMPARA_KEY_EN | G0_CBMPARA_KEY_MODE);
	ctrl |= FIELD_PREP(G0_CBMPARA_GALPHA, 0xff) | palpha;
	vo_write(vo, G0_CBMPARA, ctrl);

	vo_write(vo, G0_ADDR, (u32)(gem->dma_addr + fb->offsets[0]));
	/* G0_STRIDE is programmed in units of 16 bytes */
	vo_write(vo, G0_STRIDE,
		 FIELD_PREP(G0_STRIDE_MASK, fb->pitches[0] / 16));
	vo_write(vo, G0_IRESO,
		 FIELD_PREP(G0_IRESO_IW, fb->width - 1) |
		 FIELD_PREP(G0_IRESO_IH, fb->height - 1));

	/* source position 0,0 and 1:1 placement on the display */
	vo_write(vo, G0_DFPOS, 0);
	vo_write(vo, G0_DLPOS,
		 FIELD_PREP(G0_DLPOS_X, fb->width - 1) |
		 FIELD_PREP(G0_DLPOS_Y, fb->height - 1));
	vo_write(vo, G0_VFPOS, 0);
	vo_write(vo, G0_VLPOS,
		 FIELD_PREP(G0_DLPOS_X, fb->width - 1) |
		 FIELD_PREP(G0_DLPOS_Y, fb->height - 1));

	/*
	 * GP0 passes the G0 surface through: same resolution, full-screen
	 * display window (DFPOS 0,0 / DLPOS (w-1, h-1), 12-bit x[11:0]
	 * y[23:12] fields -- boot leaves this block zeroed because the
	 * fastboot path never displays).
	 */
	vo_write(vo, GP0_IRESO,
		 FIELD_PREP(G0_IRESO_IW, fb->width - 1) |
		 FIELD_PREP(G0_IRESO_IH, fb->height - 1));
	vo_write(vo, GP0_ORESO,
		 FIELD_PREP(GP0_ORESO_OW, fb->width - 1) |
		 FIELD_PREP(GP0_ORESO_OH, fb->height - 1));
	vo_write(vo, GP0_DFPOS, 0);
	vo_write(vo, GP0_DLPOS,
		 FIELD_PREP(G0_DLPOS_X, fb->width - 1) |
		 FIELD_PREP(G0_DLPOS_Y, fb->height - 1));
	vo_write(vo, GP0_VFPOS, 0);
	vo_write(vo, GP0_VLPOS,
		 FIELD_PREP(G0_DLPOS_X, fb->width - 1) |
		 FIELD_PREP(G0_DLPOS_Y, fb->height - 1));

	/*
	 * The zoom engine walks the source with its own step registers, so
	 * GP0_IRESO == GP0_ORESO does not mean 1:1: left alone, the engine keeps
	 * scaling by whatever the bootloader asked for (the fastboot logo is
	 * 1280x720 -> 3840x2160, which reads back as hratio 0x055555 / vratio
	 * 0x555, and made our output fill the screen three times over).  Same
	 * formula as the SDK's VDP_DISP_SetSrZmeHorRatio(): ratio = in * precision
	 * / out, with ZME_HPREC (1<<20) and ZME_VPREC (1<<12) from
	 * hal/3798m/vdp_define.h:141-145.
	 */
	ctrl = vo_read(vo, GP0_ZME_HSP);
	ctrl = (ctrl & ~GP0_ZME_HSP_RATIO) |
	       FIELD_PREP(GP0_ZME_HSP_RATIO, GP0_ZME_HPREC);
	vo_write(vo, GP0_ZME_HSP, ctrl);

	ctrl = vo_read(vo, GP0_ZME_VSR);
	ctrl = (ctrl & ~GP0_ZME_VSR_RATIO) |
	       FIELD_PREP(GP0_ZME_VSR_RATIO, GP0_ZME_VPREC);
	vo_write(vo, GP0_ZME_VSR, ctrl);

	/* scaler phase starts at the first source pixel */
	vo_write(vo, GP0_ZME_HLOFFSET, 0);
	vo_write(vo, GP0_ZME_HCOFFSET, 0);
	vo_write(vo, GP0_ZME_VOFFSET, 0);
	vo_write(vo, GP0_UPD, GP0_UPD_REGUP);

	/* layer enable + shadow-register update */
	ctrl = vo_read(vo, G0_CTRL);
	ctrl |= G0_CTRL_SURFACE_EN;
	vo_write(vo, G0_CTRL, ctrl);
	vo_write(vo, G0_UPD, G0_UPD_REGUP);
}

static void histb_vo_plane_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_commit *state)
{
	struct histb_vo *vo = plane_to_vo(plane);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_gem_dma_object *gem;
	struct drm_framebuffer *fb = new_state->fb;

	if (!fb)
		return;

	gem = drm_fb_dma_get_gem_obj(fb, 0);
	if (!gem)
		return;

	histb_vo_g0_set(vo, fb, gem, new_state->visible);
}

static void histb_vo_plane_atomic_disable(struct drm_plane *plane,
					  struct drm_atomic_commit *state)
{
	struct histb_vo *vo = plane_to_vo(plane);

	histb_vo_g0_set(vo, NULL, NULL, false);
}

static int histb_vo_plane_atomic_check(struct drm_plane *plane,
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
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	if (new_state->crtc_w > 4095 || new_state->crtc_h > 4095)
		return -EINVAL;

	/*
	 * This is what fills in new_state->visible, and atomic_update acts on
	 * it.  Skipping the helper left visible at false, so every plane commit
	 * switched the surface off again; the panel only stayed lit because
	 * histb_vo_crtc_atomic_enable() re-programs the layer by hand.
	 * can_position is false because the driver always places the surface at
	 * 0,0 with the framebuffer's own size.
	 */
	return drm_atomic_helper_check_plane_state(new_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   false, true);
}

static const struct drm_plane_helper_funcs histb_vo_plane_helper_funcs = {
	/*
	 * No DRM_GEM_SHADOW_PLANE_HELPER_FUNCS here: they upcast the state with
	 * container_of() to struct drm_shadow_plane_state, which only the
	 * matching DRM_GEM_SHADOW_PLANE_FUNCS duplicate() allocates.
	 */
	.atomic_check		= histb_vo_plane_atomic_check,
	.atomic_update		= histb_vo_plane_atomic_update,
	.atomic_disable		= histb_vo_plane_atomic_disable,
};

static const struct drm_plane_funcs histb_vo_plane_funcs = {
	.update_plane		= drm_atomic_helper_update_plane,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.destroy		= drm_plane_cleanup,
	.reset			= drm_atomic_helper_plane_reset,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
};

/* ------------------------------------------------------------------ */
/* CRTC (DHD0 channel)                                                 */
/* ------------------------------------------------------------------ */

/*
 * One-time display pipeline setup, the equivalent of VDP_DRIVER_Initial()
 * in the SDK's vdp_driver.c: route GP0 into the DHD0 mixer, set the AXI
 * outstanding limits and enable the VO clock gate.
 */
static void histb_vo_pipeline_init(struct histb_vo *vo)
{
	unsigned int i;
	u32 ctrl;

	/*
	 * Open the VO clock gate FIRST: every write issued while vo_ck_gt_en
	 * is clear can be dropped by the gated clock domain.  The previous
	 * order (gate last) left VOAXICTRL at its reset value on the board --
	 * the AXI read side never got its outstanding limits and the
	 * composition sampled black at the debug tap.
	 */
	ctrl = vo_read(vo, VO_CTRL);
	ctrl |= VO_CTRL_CK_GT_EN;
	vo_write(vo, VO_CTRL, ctrl);

	/* AXI outstanding limits, VDP_SetRdOutStd()/VDP_SetWrOutStd() defaults:
	 * read bus 0/1 = 7, write = 4.
	 */
	ctrl = vo_read(vo, VOAXICTRL);
	ctrl &= ~(VO_AXI_M0_RD0_OSTD | VO_AXI_M0_RD1_OSTD | VO_AXI_M0_WR_OSTD);
	ctrl |= FIELD_PREP(VO_AXI_M0_RD0_OSTD, 7) |
		FIELD_PREP(VO_AXI_M0_RD1_OSTD, 7) |
		FIELD_PREP(VO_AXI_M0_WR_OSTD, 4);
	vo_write(vo, VOAXICTRL, ctrl);

	/*
	 * CBM_ATTR routes each surface to one of the two mixers; the SDK labels
	 * the bits in the code that sets them (3798m/vdp_driver.c:113-118) and
	 * VDP_DRIVER_Initial() leaves 0xc (sur_attr2|sur_attr3) there.  That is
	 * also exactly what the panel shows while it is displaying a picture, so
	 * reproduce it verbatim rather than guessing at variants.
	 */
	vo_write(vo, CBM_ATTR, CBM_ATTR_SUR2 | CBM_ATTR_SUR3);

	/*
	 * Mixer background (Cr[9:0] Cb[19:10] Y[29:20]).  Left at its reset 0
	 * the uncovered area is YCbCr(0,0,0), which panels render as solid
	 * green; Y=64 with both chroma at mid-scale is black.  The boot state
	 * that scans out its logo has exactly this value.
	 */
	vo_write(vo, CBM_BKG1, CBM_BKG_BLACK);

	/*
	 * The bootloader leaves its logo layers enabled and they sit at a
	 * higher mixer priority than ours, so turn off every graphics layer
	 * except the one the plane drives.
	 */
	for (i = 0; i < 5; i++) {
		u32 off = G0_CTRL + i * GFX_LAYER_STRIDE;

		if (off == G0_CTRL)
			continue;
		ctrl = vo_read(vo, off);
		if (!(ctrl & G0_CTRL_SURFACE_EN))
			continue;
		vo_write(vo, off, ctrl & ~G0_CTRL_SURFACE_EN);
		vo_write(vo, off + (G0_UPD - G0_CTRL), G0_UPD_REGUP);
	}

	/*
	 * Two mixers sit between the surfaces and the channel: MIXG0 takes the
	 * graphics layers, CBM_MIX1 is DHD0's.  Both hold layer_id + 1 per
	 * priority slot (vdp_driver.c:4519-4534), and the boot's live logo state
	 * has 0x21 in each -- G0 in MIXG0 slot 0, GP0 in DHD0's slot 1.
	 */
	vo_write(vo, MIXG0_MIX,
		 FIELD_PREP(MIX_PRIO0, MIXG0_LAYER_G0) |
		 FIELD_PREP(MIX_PRIO1, MIXG0_LAYER_G1));
	vo_write(vo, CBM_MIX1,
		 FIELD_PREP(MIX_PRIO0, CBM_LAYER_VP0) |
		 FIELD_PREP(MIX_PRIO1, CBM_LAYER_GP0));

	/*
	 * GP0 sits between the G0 fetch and the mixer (DHD0's table admits
	 * prio0 = VP0, prio1 = GP0 -- SDK VDP_DRIVER_Initial(),
	 * 3798m/vdp_driver.c:135-136), so the surface has to transit a post
	 * block before the mixer sees it.  Which stage that output taps is one
	 * of the three whole-register values VDP_GP_SetIpOrder() can write
	 * (3798m/vdp_driver.c:5427-5467); the picture-producing boot state uses
	 * ORDER_ZME_CSC with read_mode set, and the scaling step registers are
	 * programmed for 1:1 by histb_vo_g0_set().
	 */
	vo_write(vo, GP0_CTRL, GP0_ORDER_ZME_CSC | GP0_CTRL_READ_MODE);
	vo_write(vo, GP0_GALPHA, 0xff);
	/*
	 * GP0's CSC stage stays bypassed: the interface carries RGB (see
	 * histb_vo_crtc_setup_timing()), so converting to YCbCr here would make
	 * the transmitter colour-convert its own input.  csc_en clears with the
	 * stage still in the order, which is what the SDK does on the RGB leg.
	 */
	vo_write(vo, GP0_CSC_IDC, 0);

	vo_write(vo, GP0_UPD, GP0_UPD_REGUP);
}

static void histb_vo_crtc_setup_timing(struct histb_vo *vo,
				       const struct drm_display_mode *mode)
{
	/* The DHD totals are ACT + BB + FB -- the sync pulse lives inside the
	 * back porch, and HPW/VPW only say how wide it is there (measured on
	 * the TX: programming a bare back porch gave H_RES 1208 / V_RES 800
	 * for a 1344x806 mode).
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
	 * vertical counters are per field.  The vendor's 1080i rows are exactly
	 * this halving with the odd line of the frame going to the bottom
	 * field's back porch: 1080i60 reads (Vact 540, Vbb 20, Vfb 2) for the
	 * top field and (540, 21, 2) for the bottom one, which sums to the
	 * 1125-line frame.
	 */
	u32 vact = interlaced ? mode->vdisplay / 2 : mode->vdisplay;
	u32 tvbb = interlaced ? vbb / 2 : vbb;
	u32 tvfb = interlaced ? vfb / 2 : vfb;
	u32 bvbb = vbb - tvbb;
	/*
	 * HMID is where the second field starts inside the line: the vendor
	 * table's 908 (1080i60) and 1128 (1080i50) are both htotal / 2 measured
	 * from the start of the line minus the blanking that precedes active
	 * video, i.e. the field boundary sits at half a frame line.
	 */
	u32 hmid = interlaced ? mode->htotal / 2 - hbb : 0;
	u32 vtthd, ctrl, inv = 0;

	ctrl = vo_read(vo, DHD0_CTRL);
	if (interlaced)
		ctrl &= ~DHD_CTRL_IOP;		/* field sequential */
	else
		ctrl |= DHD_CTRL_IOP;		/* progressive */
	/*
	 * hdmi_mode (bit14) and DATA_SEL.hdmi_data_sel are one decision: the SDK
	 * hands the same interface data type to VDP_DISP_SetHdmiMode() and
	 * VDP_DISP_SetDataSel() (drv_disp_hal.c:2331-2357 -> vdp_driver.c:2551,
	 * :4043), 1 meaning RGB on the HDMI pins and 0 YCbCr.
	 *
	 * The SDK's table picks by format family: RGB for the PC/861D-VESA block,
	 * YCbCr for the CEA block and for 4K (enum order hi_drv_video.h:748-832).
	 * This driver uses RGB for every format, because the YCbCr leg does not
	 * work as the SDK drives it here: measured through a capture card with
	 * hdmi_data_sel cleared, the transmitter fed YCbCr straight through
	 * (no converter, no down-sampler, link declared 4:4:4) at 1920x1080p,
	 * red came back (77,250,30), blue (177,0,0), black (0,134,0) -- the two
	 * chroma lanes are exchanged and Cb carries nothing, while grey stayed
	 * exact, so only the luma path is right.  The likeliest reason is the
	 * HDATE output-stage lane mux, which the SDK reprograms for its digital
	 * port (video1_sel=2, video2_sel=3, video3_sel=1, src_ctrl=2,
	 * vdp_driver.c:10589-10598) and this driver leaves as the boot state has
	 * it.  Not verified: programming those selects would be the way to
	 * make the YCbCr leg usable, and nothing on the RGB path needs it.
	 */
	ctrl |= DHD_CTRL_HDMI_MODE;
	ctrl |= DHD_CTRL_INTFB;			/* bit15 is set in the boot
						 * state that drives the panel
						 * (DHD0_CTRL = 0x80008010), even
						 * though the mv100 header marks
						 * it reserved
						 */
	ctrl |= DHD_CTRL_INTF_EN;		/* drive the HDMI interface */
	vo_write(vo, DHD0_CTRL, ctrl);

	/* horizontal: registers take value - 1 */
	vo_write(vo, DHD0_HSYNC1,
		 FIELD_PREP(DHD_HSYNC1_HACT, histb_vo_m1(mode->hdisplay)) |
		 FIELD_PREP(DHD_HSYNC1_HBB, histb_vo_m1(hbb)));
	vo_write(vo, DHD0_HSYNC2,
		 FIELD_PREP(DHD_HSYNC2_HFB, histb_vo_m1(hfb)) |
		 FIELD_PREP(DHD_HSYNC2_HMID, hmid ? hmid - 1 : 0));
	vo_write(vo, DHD0_PWR,
		 FIELD_PREP(DHD_PWR_HPW, histb_vo_m1(hpw)) |
		 FIELD_PREP(DHD_PWR_VPW, histb_vo_m1(vpw)));

	/* vertical: top field in VSYNC, bottom field in VPLUS; for
	 * progressive modes the bottom values mirror the top ones.
	 */
	vo_write(vo, DHD0_VSYNC,
		 FIELD_PREP(DHD_VSYNC_VACT, histb_vo_m1(vact)) |
		 FIELD_PREP(DHD_VSYNC_VBB, histb_vo_m1(tvbb)) |
		 FIELD_PREP(DHD_VSYNC_VFB, histb_vo_m1(tvfb)));
	vo_write(vo, DHD0_VPLUS,
		 FIELD_PREP(DHD_VPLUS_BVACT, histb_vo_m1(vact)) |
		 FIELD_PREP(DHD_VPLUS_BVBB, histb_vo_m1(bvbb)) |
		 FIELD_PREP(DHD_VPLUS_BVFB, histb_vo_m1(tvfb)));

	/* vblank interrupt threshold: 8/10 of the frame, as in the SDK's
	 * drv_display.c; frame based mode.
	 */
	vtthd = (tvbb + vact + tvfb) * 8 / 10;
	vo_write(vo, DHD0_VTTHD,
		 FIELD_PREP(DHD_VTTHD1, vtthd) | DHD_VTTHD1_MODE);

	/* sync polarity towards the HDMI transmitter: the *_INV bits invert
	 * the corresponding sync (SDK drv_disp_hal.c DISP_HAL_SetSyncInv);
	 * non-inverted output equals DRM positive sync.
	 */
	if (mode->flags & DRM_MODE_FLAG_NHSYNC)
		inv |= DHD_SYNC_HDMI_HS_INV;
	if (mode->flags & DRM_MODE_FLAG_NVSYNC)
		inv |= DHD_SYNC_HDMI_VS_INV;
	vo_write(vo, DHD0_SYNC_INV, inv);

	/*
	 * Set our bit of the interface data type and leave the rest as the
	 * reset state has it: VDP_DISP_SetDataSel() read-modifies-writes only
	 * hdmi_data_sel (vdp_driver.c:4051-4054), and on mv100 bit 1 is
	 * vga_data_sel while bit 2 is marked reserved (hi_reg_vdp.h:7253-7257).
	 * hdmi_mode above carries the same decision.
	 */
	ctrl = vo_read(vo, DHD0_DATA_SEL);
	ctrl |= DHD_DATA_SEL_HDMI;
	vo_write(vo, DHD0_DATA_SEL, ctrl);

	/* latch the timing registers */
	ctrl = vo_read(vo, DHD0_CTRL);
	ctrl |= DHD_CTRL_REGUP;
	vo_write(vo, DHD0_CTRL, ctrl);

	/*
	 * The DSD channel (0xc400..) is left exactly as the boot state has it:
	 * the HDMI interface is driven from DHD0, and VDP_DISP_SetIntfEnable()
	 * on channel 1 would only re-route a timing this port does not build.
	 */
}

static enum drm_mode_status histb_vo_crtc_mode_valid(struct drm_crtc *crtc,
						     const struct drm_display_mode *mode)
{
	struct histb_vo *vo = crtc_to_vo(crtc);
	long rate;

	/* G0/GP0 position and resolution fields are 12 bits wide */
	if (mode->hdisplay > 4095 || mode->vdisplay > 4095)
		return MODE_BAD;

	/* 3840x2160 is the boot's own mode, so the panel takes it */
	if (mode->hdisplay > 3840 || mode->vdisplay > 2160)
		return MODE_BAD;

	if (mode->hdisplay % 2 || mode->vdisplay % 2)
		return MODE_BAD;

	/* the HD PLL and channel divider can only produce certain rates */
	rate = clk_round_rate(vo->clk_pix, mode->clock * 1000);
	if (rate < 0)
		return MODE_ERROR;

	/*
	 * The clock can only produce the rates the vendor table holds, which
	 * are what each format's own timing adds up to -- a few hundred Hz
	 * away from the nominal clock the EDID rounds into mode->clock.
	 * Allow half a percent; keep the arithmetic in Hz so a large mismatch
	 * cannot overflow and look like a pass.
	 */
	if (abs(rate - (long)mode->clock * 1000) > (long)mode->clock * 5)
		return MODE_NOCLOCK;

	return MODE_OK;
}

static void histb_vo_crtc_atomic_enable(struct drm_crtc *crtc,
					struct drm_atomic_commit *state)
{
	struct histb_vo *vo = crtc_to_vo(crtc);
	struct drm_crtc_state *new_state = drm_atomic_get_new_crtc_state(state, crtc);
	const struct drm_display_mode *mode = &new_state->adjusted_mode;
	struct drm_plane_state *pstate;
	struct drm_gem_dma_object *gem;
	struct drm_framebuffer *fb;
	u32 ctrl;

	/* the channel clock is what the HDMI transmitter receives, too */
	histb_vo_set_pixclk(vo, mode->clock);

	histb_vo_pipeline_init(vo);

	histb_vo_crtc_setup_timing(vo, mode);

	/* enable the channel */
	ctrl = vo_read(vo, DHD0_CTRL);
	ctrl |= DHD_CTRL_INTF_EN | DHD_CTRL_REGUP;
	vo_write(vo, DHD0_CTRL, ctrl);

	/*
	 * commit_planes runs before modeset_enables, and G0 drops register
	 * writes while the VO pipeline is not up (seen on hw: the whole G0
	 * block read back reset values after an early plane update), so
	 * (re)program the layer from the fb the commit just installed.
	 */
	pstate = drm_atomic_get_new_plane_state(state, crtc->primary);
	if (!pstate)
		pstate = crtc->primary->state;
	fb = pstate->fb;
	if (fb) {
		gem = drm_fb_dma_get_gem_obj(fb, 0);
		if (gem)
			histb_vo_g0_set(vo, fb, gem, pstate->visible);
	}

	drm_crtc_vblank_on(crtc);
}

static void histb_vo_crtc_atomic_disable(struct drm_crtc *crtc,
					 struct drm_atomic_commit *state)
{
	struct histb_vo *vo = crtc_to_vo(crtc);
	u32 ctrl;

	drm_crtc_vblank_off(crtc);

	ctrl = vo_read(vo, DHD0_CTRL);
	ctrl &= ~DHD_CTRL_INTF_EN;
	ctrl |= DHD_CTRL_REGUP;
	vo_write(vo, DHD0_CTRL, ctrl);
}

static int histb_vo_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct histb_vo *vo = crtc_to_vo(crtc);

	if (vo->irq < 0)
		return -EOPNOTSUPP;

	/* unmask the DHD0 threshold-1 interrupt */
	vo_write(vo, VO_INTMSK,
		 vo_read(vo, VO_INTMSK) & ~VO_INT_DHD0_VTTHD1);
	return 0;
}

static void histb_vo_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct histb_vo *vo = crtc_to_vo(crtc);

	if (vo->irq < 0)
		return;

	vo_write(vo, VO_INTMSK,
		 vo_read(vo, VO_INTMSK) | VO_INT_DHD0_VTTHD1);
}

static void histb_vo_crtc_atomic_flush(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new_state = drm_atomic_get_new_crtc_state(state, crtc);

	/*
	 * The VO latches the whole frame at once and the vblank IRQ only
	 * marks the count, so send the flip event right after the commit.
	 */
	if (new_state->event) {
		spin_lock_irq(&crtc->dev->event_lock);
		drm_crtc_send_vblank_event(crtc, new_state->event);
		new_state->event = NULL;	/* consumed: keep hw_done quiet */
		spin_unlock_irq(&crtc->dev->event_lock);
	}
}

static const struct drm_crtc_helper_funcs histb_vo_crtc_helper_funcs = {
	.mode_valid		= histb_vo_crtc_mode_valid,
	.atomic_enable		= histb_vo_crtc_atomic_enable,
	.atomic_disable		= histb_vo_crtc_atomic_disable,
	.atomic_flush		= histb_vo_crtc_atomic_flush,
};

static const struct drm_crtc_funcs histb_vo_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.destroy		= drm_crtc_cleanup,
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.enable_vblank		= histb_vo_crtc_enable_vblank,
	.disable_vblank		= histb_vo_crtc_disable_vblank,
};

static irqreturn_t histb_vo_irq_handler(int irq, void *data)
{
	struct histb_vo *vo = data;
	struct histb_vo_display *disp = READ_ONCE(vo->disp);
	u32 all = vo_read(vo, VO_INTSTA);
	u32 sta = all & ~vo_read(vo, VO_INTMSK);

	if (!all)
		return IRQ_NONE;

	/*
	 * Ack every latched source, not just the vblank one: returning
	 * IRQ_NONE while a VO status bit stays set makes the core disable
	 * the line ("nobody cared") and vblank dies with it.  Sources the
	 * driver masks off still latch, so dispatch on the unmasked subset
	 * only -- otherwise the frame-threshold bits that nobody enables
	 * print a warning once per vblank.
	 */
	vo_write(vo, VO_MSKINTSTA, all);

	if (sta & ~VO_INT_DHD0_VTTHD1)
		dev_warn_ratelimited(vo->dev, "vo irq: unexpected status %08x\n",
				     sta);

	/* the line stays requested across an aggregate unbind, no CRTC then */
	if (disp && (sta & VO_INT_DHD0_VTTHD1))
		drm_crtc_handle_vblank(&disp->crtc);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Mode config / binding                                               */
/* ------------------------------------------------------------------ */

static const struct drm_mode_config_funcs histb_vo_mode_config_funcs = {
	.fb_create		= drm_gem_fb_create,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

static const struct drm_encoder_funcs histb_vo_encoder_funcs = {
	.destroy		= drm_encoder_cleanup,
};

static const struct drm_encoder_helper_funcs histb_vo_encoder_helper_funcs = {
};

DEFINE_DRM_GEM_DMA_FOPS(histb_vo_fops);

static const struct drm_driver histb_vo_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &histb_vo_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name			= "histb-vo",
	.desc			= "HiSTB VO / VDP display controller",
	.major			= 1,
	.minor			= 0,
};

static int histb_vo_bind(struct device *dev)
{
	struct histb_vo *vo = dev_get_drvdata(dev);
	struct histb_vo_display *disp;
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

	drm = drm_dev_alloc(&histb_vo_driver, dev);
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
	disp->vo = vo;
	disp->drm = drm;
	drm->dev_private = disp;
	WRITE_ONCE(vo->disp, disp);

	ret = drmm_mode_config_init(drm);
	if (ret) {
		dev_err_probe(dev, ret, "mode_config_init failed\n");
		goto err_put;
	}

	drm->mode_config.min_width	= 64;
	drm->mode_config.max_width	= 3840;
	drm->mode_config.min_height	= 64;
	drm->mode_config.max_height	= 2160;
	drm->mode_config.funcs		= &histb_vo_mode_config_funcs;

	/* one CRTC, so the vblank core must exist before the IRQ can be
	 * served and before any atomic commit waits on it
	 */
	ret = drm_vblank_init(drm, 1);
	if (ret) {
		dev_err_probe(dev, ret, "vblank_init failed\n");
		goto err_put;
	}

	ret = drm_universal_plane_init(drm, &disp->primary, 0,
				       &histb_vo_plane_funcs,
				       histb_vo_formats,
				       ARRAY_SIZE(histb_vo_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		dev_err_probe(dev, ret, "plane_init failed\n");
		goto err_put;
	}
	drm_plane_helper_add(&disp->primary, &histb_vo_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &disp->crtc, &disp->primary, NULL,
					&histb_vo_crtc_funcs, "histb-vo-crtc");
	if (ret) {
		dev_err_probe(dev, ret, "crtc_init failed\n");
		goto err_put;
	}
	drm_crtc_helper_add(&disp->crtc, &histb_vo_crtc_helper_funcs);
	drm_crtc_vblank_reset(&disp->crtc);

	disp->encoder.possible_crtcs = drm_crtc_mask(&disp->crtc);
	ret = drm_encoder_init(drm, &disp->encoder, &histb_vo_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, "histb-vo-encoder");
	if (ret) {
		dev_err_probe(dev, ret, "encoder_init failed\n");
		goto err_put;
	}
	drm_encoder_helper_add(&disp->encoder, &histb_vo_encoder_helper_funcs);

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

	/*
	 * The transmitter has no usable HPD output (it is a level signal that
	 * storms while a sink is attached), so the connector the bridge
	 * helper created asks for DRM_CONNECTOR_POLL_CONNECT|DISCONNECT.
	 * Without starting the poll work here nothing would ever re-detect a
	 * plug event.  The drmm variant stops with the DRM device.
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
	WRITE_ONCE(vo->disp, NULL);
	drm_dev_put(drm);
err_unbind:
	component_unbind_all(dev, NULL);
	return ret;
}

static void histb_vo_unbind(struct device *dev)
{
	struct histb_vo *vo = dev_get_drvdata(dev);
	struct histb_vo_display *disp = vo->disp;
	struct drm_device *drm = disp->drm;

	/* drm_dev_unplug() also unregisters; tear down while clocks are on */
	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);

	WRITE_ONCE(vo->disp, NULL);
	component_unbind_all(dev, NULL);
	drm_dev_put(drm);
}

/*
 * Who belongs to this pipeline is read out of the device tree, not out of the
 * order someone happens to echo into unbind: the transmitter on the far side
 * of our own graph endpoint is added as a component of this aggregate, so
 * removing its driver takes the DRM device down before that driver's bridge,
 * regmaps and MMIO are released.  Without it, unbinding the transmitter under
 * a live card lets the atomic paths dereference the freed bridge.
 */
static const struct component_master_ops histb_vo_master_ops = {
	.bind		= histb_vo_bind,
	.unbind		= histb_vo_unbind,
};

static int histb_vo_add_master(struct device *dev)
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

	return component_master_add_with_match(dev, &histb_vo_master_ops, match);
}

/*
 * The VOU reset request is CRG54[30].  A reset only reaches the block while it
 * is clocked, so the gates come up first: CRG54's own gate group is the clock
 * "vo" and the HD channel clock is "pix", both enabled by
 * histb_vo_runtime_resume().  Hold time and settle time mirror the SDK's
 * DISP_ResetCRG() (vdp_v2_0/hal/3798m/vdp_driver.c:13586), which the HAL calls
 * from DISP_HAL_Init() before it touches a single VDP register.
 */
static void histb_vo_hw_reset(struct histb_vo *vo)
{
	/*
	 * DHD0_CTRL is the readback that says the pulse landed: the boot leaves
	 * its HD channel enabled (measured 0x8000c010 with the logo on screen)
	 * and a VOU reset clears it back to the default.  Both reads happen
	 * outside the hold window, since a block in reset need not answer.
	 */
	dev_dbg(vo->dev, "before reset: DHD0_CTRL %08x\n", vo_read(vo, DHD0_CTRL));

	reset_control_assert(vo->rst);
	usleep_range(5000, 5200);
	reset_control_deassert(vo->rst);
	usleep_range(1000, 1200);

	dev_dbg(vo->dev, "after reset: DHD0_CTRL %08x\n", vo_read(vo, DHD0_CTRL));
}

static int histb_vo_runtime_suspend(struct device *dev)
{
	struct histb_vo *vo = dev_get_drvdata(dev);

	clk_disable_unprepare(vo->clk_vo);
	clk_disable_unprepare(vo->clk_pix);
	return 0;
}

static int histb_vo_runtime_resume(struct device *dev)
{
	struct histb_vo *vo = dev_get_drvdata(dev);
	int ret;

	ret = clk_prepare_enable(vo->clk_pix);
	if (ret)
		return ret;

	ret = clk_prepare_enable(vo->clk_vo);
	if (ret) {
		clk_disable_unprepare(vo->clk_pix);
		return ret;
	}

	histb_vo_hw_reset(vo);

	return 0;
}

static const struct dev_pm_ops histb_vo_pm_ops = {
	RUNTIME_PM_OPS(histb_vo_runtime_suspend, histb_vo_runtime_resume, NULL)
};

static int histb_vo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_vo *vo;
	int ret;

	vo = devm_kzalloc(dev, sizeof(*vo), GFP_KERNEL);
	if (!vo)
		return -ENOMEM;

	vo->dev = dev;
	platform_set_drvdata(pdev, vo);

	vo->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vo->regs))
		return dev_err_probe(dev, PTR_ERR(vo->regs), "ioremap failed\n");

	/*
	 * The HD channel clock, which the CRG driver builds from the HD PLL
	 * and CRG54 (see histb_vo_set_pixclk()), and CRG54's gate group for
	 * the rest of the channel path; the VOU reset only shifts while both
	 * of them run.
	 */
	vo->clk_pix = devm_clk_get(dev, "pix");
	if (IS_ERR(vo->clk_pix))
		return dev_err_probe(dev, PTR_ERR(vo->clk_pix), "clk pix\n");

	vo->clk_vo = devm_clk_get(dev, "vo");
	if (IS_ERR(vo->clk_vo))
		return dev_err_probe(dev, PTR_ERR(vo->clk_vo), "clk vo\n");

	vo->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(vo->rst))
		return dev_err_probe(dev, PTR_ERR(vo->rst), "rst\n");

	/*
	 * Power the block up before anything reads or writes a VDP register:
	 * histb_vo_runtime_resume() enables the channel clock and takes the
	 * VOU through a reset of our own, so the register file starts from
	 * its documented defaults instead of from whatever the boot left
	 * behind.  The device then stays permanently powered for the whole
	 * probe/remove lifetime: the atomic paths and the vblank IRQ touch
	 * registers with no pm_runtime references of their own, and the
	 * shared pixel clock makes idle power-down of little use anyway.
	 * histb_vo_remove() drops the reference again; an aggregate unbind
	 * leaves the clocks up because the transmitter may bind again.
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
	 * Clear any latched status and mask every source before requesting
	 * the line: a pending bit arriving on an unmasked line is the
	 * "nobody cared" storm this driver used to eat at ~2 s.
	 * drm_crtc_vblank_on() unmasks VTTHD1 later.
	 */
	vo->irq = platform_get_irq_optional(pdev, 0);
	if (vo->irq >= 0) {
		vo_write(vo, VO_MSKINTSTA, ~0u);
		vo_write(vo, VO_INTMSK, ~0u);

		ret = devm_request_irq(dev, vo->irq, histb_vo_irq_handler,
				       IRQF_SHARED, dev_name(dev), vo);
		if (ret) {
			dev_err_probe(dev, ret, "request_irq failed\n");
			goto err_pm_put;
		}
	}

	ret = histb_vo_add_master(dev);
	if (ret)
		goto err_pm_put;

	return 0;

err_pm_put:
	pm_runtime_put_sync(dev);
err_pm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static void histb_vo_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &histb_vo_master_ops);

	pm_runtime_put_sync_suspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

/*
 * drm_dev_register() documents that a KMS driver stops the pipeline at
 * shutdown and reboot: the channel would otherwise keep scanning a
 * framebuffer whose pages are about to be torn down underneath it.
 */
static void histb_vo_shutdown(struct platform_device *pdev)
{
	struct histb_vo *vo = platform_get_drvdata(pdev);
	struct histb_vo_display *disp = READ_ONCE(vo->disp);

	if (disp)
		drm_atomic_helper_shutdown(disp->drm);
}

static const struct of_device_id histb_vo_of_match[] = {
	{ .compatible = "hisilicon,histb-vo" },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_vo_of_match);

static struct platform_driver histb_vo_platform_driver = {
	.probe		= histb_vo_probe,
	.remove		= histb_vo_remove,
	.shutdown	= histb_vo_shutdown,
	.driver		= {
		.name			= "histb-vo",
		.of_match_table		= histb_vo_of_match,
		.pm			= pm_ptr(&histb_vo_pm_ops),
	},
};
module_platform_driver(histb_vo_platform_driver);

MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_DESCRIPTION("DRM driver for the HiSTB (hi3798mv100) VO/VDP display controller");
MODULE_LICENSE("GPL");
