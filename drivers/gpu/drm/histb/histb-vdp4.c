// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * DRM driver for the HiSTB VDP v4 display engine, the variant found on the
 * Hi3798MV200.
 *
 * The VDP block at 0xf8cc0000 provides two display channels. This driver
 * exposes DHD0 (the HD channel) as a single CRTC with a primary plane fed by
 * the G0 graphics (RGB) layer, admitted to the channel by the priority slot of
 * the DHD0 mixer word (G0 -> CBM MIX0 -> DHD0) -> HDMI. Scanout data leaves the
 * VDP towards the HDMI transmitter selected through of_graph port@0; the
 * downstream node is that transmitter's bridge.
 *
 * Clocking: the channel takes its pixel clock through CRG54 -- vo_hd_clk_sel
 * (bit 16) picks the source and vo_hd_clk_div (bits 19:18) divides it; this part
 * has no gate for the HD channel (bits 5:4 are reserved in the user guide, even
 * though the Hi3798MV210 decode of the same register names bit 4 vo_hd_cken).
 * The clock the driver asks for is the display FPLL in PERI_CRG_PLL10/PLL11 over
 * those two fields, which drivers/clk computes, and the transmitter is fed from
 * the same clock, so the two blocks cannot disagree about the pixel clock.
 *
 * Enable order is a hardware constraint, not taste: an access to the register
 * file while the HD domain is clocked and a reset request of the block is held
 * stalls the bus with the transaction outstanding. So the driver first puts
 * both requests (CRG211 bit 29 for the block, CRG54 bit 30 for the VOU) into
 * their running state while nothing clocks the domain yet, then raises the
 * clocks, and only then touches the register file.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/sysfs.h>

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

#include "histb-vdp4-regs.h"

/* The clocks in struct histb_vdp4.clks, in the order they are raised. */
enum histb_vdp4_clk {
	HISTB_VDP4_CLK_PIX,	/* HD channel pixel clock, shared with HDMI */
	HISTB_VDP4_CLK_BUS,	/* CRG54 bus gate of the channel */
	HISTB_VDP4_CLK_SDATE,	/* CRG54 gate of the channel's timing encoder */
	HISTB_VDP4_CLK_NUM,
};

/*
 * PERI_CRG54's VOU reset request.  The register file is only reachable with
 * this bit set: cleared while the HD domain runs, the next access to the block
 * locks the bus rather than returning a value.  Measured on the board from the
 * vendor bootloader, where CRG54=0x400d0019 reads VDPVER1=0x20170929 and
 * 0x000d0019 (that one bit back to 0, nothing else changed) hangs the read.
 * So the driver only ever raises it, and never pulses it down again: the pulse
 * cannot be made safe once the transmitter shares the clock.
 */
#define CRG54			0x0d8
#define  CRG54_VOU_SRST_REQ	BIT(30)

struct histb_vdp4 {
	struct device		*dev;
	void __iomem		*regs;

	struct clk_bulk_data	clks[HISTB_VDP4_CLK_NUM];

	struct reset_control	*rst_vdp;

	struct regmap		*crg;

	void __iomem		*mark;

	int			irq;
	unsigned long		vblank_seen;

	struct histb_vdp4_display	*disp;	/* NULL unless the pipeline is bound */
};

/*
 * The DRM objects belong to the aggregate driver, not to the platform device:
 * they come into existence when the pipeline the device tree describes is up
 * and go away when it comes down, which can happen while this device stays
 * probed (the transmitter is a component of the aggregate). devm cannot cover
 * that lifetime, so this block is allocated by the aggregate's bind and
 * released by its unbind.
 */
struct histb_vdp4_display {
	struct histb_vdp4		*vdp;
	struct drm_device	*drm;

	struct drm_plane	primary;
	struct drm_crtc		crtc;
	struct drm_encoder	encoder;
};

static inline struct histb_vdp4 *plane_to_vdp(struct drm_plane *plane)
{
	return container_of(plane, struct histb_vdp4_display, primary)->vdp;
}

static inline struct histb_vdp4 *crtc_to_vdp(struct drm_crtc *crtc)
{
	return container_of(crtc, struct histb_vdp4_display, crtc)->vdp;
}

/*
 * The mode's pixel clock is the HD channel clock.  The CRG driver builds it
 * from the display FPLL in PERI_CRG_PLL10/PLL11 and CRG54's select and divider
 * for the channel, and it prefers to pass the PLL's output straight through, so
 * requesting the channel rate here is what programs the PLL: 297MHz for
 * 3840x2160@30, which is what the board's vendor system runs.  The transmitter
 * takes that clock 1:1 -- the CRG driver pins the ratio bit at 1:1 in the
 * clock's .prepare, because with 1:2 the VDP register file cannot be reached
 * at all on this part.
 */
static int histb_vdp4_set_pixclk(struct histb_vdp4 *vdp, unsigned int mode_khz)
{
	unsigned long rate = (unsigned long)mode_khz * 1000;
	struct clk *clk = vdp->clks[HISTB_VDP4_CLK_PIX].clk;
	int ret;

	ret = clk_set_rate(clk, rate);
	if (ret) {
		dev_err(vdp->dev, "failed to set pixel clock to %lu Hz: %d\n",
			rate, ret);
		return ret;
	}

	dev_dbg(vdp->dev, "pixel clock %lu Hz for mode clock %u kHz\n",
		clk_get_rate(clk), mode_khz);
	return 0;
}

static inline u32 vo_read(struct histb_vdp4 *vdp, u32 off)
{
	return readl_relaxed(vdp->regs + off);
}

static inline void vo_write(struct histb_vdp4 *vdp, u32 off, u32 val)
{
	writel_relaxed(val, vdp->regs + off);
}

/* ------------------------------------------------------------------ */
/* Plane (G0 graphics layer)                                           */
/* ------------------------------------------------------------------ */

/*
 * No per-pixel alpha is programmed in the atomic update path, so the ARGB
 * format is deliberately not exposed (drm core would warn about alpha formats
 * without blend mode support).
 */
static const u32 histb_vdp4_formats[] = {
	DRM_FORMAT_XRGB8888,
};

/* `value - 1`, clamped so that a zero porch/sync width never underflows */
static u32 histb_vdp4_m1(u32 v)
{
	return max(v, 1u) - 1;
}

static void histb_vdp4_g0_set(struct histb_vdp4 *vdp, const struct drm_framebuffer *fb,
			      struct drm_gem_dma_object *gem, bool enable)
{
	u32 ctrl, ifmt;

	if (!enable) {
		ctrl = vo_read(vdp, L0_CTRL);
		ctrl &= ~L0_EN;
		vo_write(vdp, L0_CTRL, ctrl);
		vo_write(vdp, L0_REGUP, 1);
		return;
	}

	/*
	 * Pixel format. The A-prefixed hardware codes consume the buffer's
	 * alpha byte, so an XRGB framebuffer must not be described as ARGB8888:
	 * every pixel would carry alpha 0x00 and the layer would composite as
	 * fully transparent.
	 */
	if (fb->format->format == DRM_FORMAT_ARGB8888)
		ifmt = GFX0_IFMT_ARGB8888;
	else
		ifmt = GFX0_IFMT_KRGB888;

	/*
	 * Fetch unit: gate and address map before anything that has to reach the
	 * DMA, then the request depth the vendor's own chip init passes.
	 */
	ctrl = vo_read(vdp, GFX0_CTRL);
	ctrl |= GFX0_CK_GT_EN | GFX0_ADDR_MAP_EN;
	vo_write(vdp, GFX0_CTRL, ctrl);
	vo_write(vdp, GFX0_REQ_CTRL,
		 FIELD_PREP(GFX0_REQ_CTRL_MASK, GFX0_REQ_CTRL_INIT));
	/*
	 * Two more configured words of this block on a displaying system, written
	 * here rather than by hand because a live write into a running channel was
	 * measured to change nothing (section 158c).
	 */
	vo_write(vdp, GFX0_CTRL, vo_read(vdp, GFX0_CTRL) | GFX0_CTRL_SEEN);
	vo_write(vdp, GFX0_REQ_EXT, GFX0_REQ_EXT_LIVE);

	ctrl = vo_read(vdp, GFX0_FMT);
	ctrl = (ctrl & ~(GFX0_IFMT | GFX0_BITEXT)) |
	       FIELD_PREP(GFX0_IFMT, ifmt) |
	       FIELD_PREP(GFX0_BITEXT, GFX0_BITEXT_3RD);
	vo_write(vdp, GFX0_FMT, ctrl);

	vo_write(vdp, GFX0_ADDR, (u32)(gem->dma_addr + fb->offsets[0]));
	/*
	 * Stride: the two halves are the luma and chroma strides and are equal
	 * because the surface is single-plane RGB.  The unit (16 bytes) is mv100's
	 * and has not been re-read on this part, so a wrong-scale picture starts
	 * here.
	 */
	vo_write(vdp, GFX0_STRIDE,
		 FIELD_PREP(GFX0_LSTRIDE, fb->pitches[0] / 16) |
		 FIELD_PREP(GFX0_CSTRIDE, fb->pitches[0] / 16));
	vo_write(vdp, GFX0_IRESO,
		 FIELD_PREP(GFX0_RESO_W, fb->width - 1) |
		 FIELD_PREP(GFX0_RESO_H, fb->height - 1));
	vo_write(vdp, GFX0_ORESO,
		 FIELD_PREP(GFX0_RESO_W, fb->width - 1) |
		 FIELD_PREP(GFX0_RESO_H, fb->height - 1));

	/*
	 * Layer stage: source at 0,0 and 1:1 placement on the display.  The layer
	 * words pack two 16-bit halves, not the mv200 header's 12-bit fields.
	 */
	vo_write(vdp, L0_VIR_START, 0);
	vo_write(vdp, L0_DISP_START, 0);
	ctrl = FIELD_PREP(L0_POS_X, fb->width - 1) |
	       FIELD_PREP(L0_POS_Y, fb->height - 1);
	vo_write(vdp, L0_VIR_END, ctrl);
	vo_write(vdp, L0_DISP_END, ctrl);

	/* opaque layer, update-on, then the shadow-register latch */
	ctrl = vo_read(vdp, L0_CTRL);
	ctrl = (ctrl & ~L0_GALPHA) | L0_EN | L0_UPD_MODE |
	       FIELD_PREP(L0_GALPHA, 0xff);
	vo_write(vdp, L0_CTRL, ctrl);
	vo_write(vdp, L0_REGUP, 1);

	/*
	 * Commit the shadowed graphics configuration the way the vendor does it:
	 * VDP_GFX_SetParaUpd() ORs bit(17 + layer) into the shared word at
	 * base+0xf00, and without that pulse the address/stride/format/geometry
	 * written above stay in the shadow set.
	 */
	vo_write(vdp, PARA_UPD,
		 vo_read(vdp, PARA_UPD) | PARA_UPD_GFX(0));

	/* board test instrument, not for submission: what the fetch is aimed at */
	dev_info(vdp->dev,
		 "g0 fetch: addr 0x%08x pitch %u %ux%u fourcc %p4cc cpu va %px\n",
		 (u32)(gem->dma_addr + fb->offsets[0]), fb->pitches[0], fb->width,
		 fb->height, &fb->format->format, gem->vaddr);
}

static void histb_vdp4_plane_atomic_update(struct drm_plane *plane,
					 struct drm_atomic_commit *state)
{
	struct histb_vdp4 *vdp = plane_to_vdp(plane);
	struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
	struct drm_gem_dma_object *gem;
	struct drm_framebuffer *fb = new_state->fb;

	if (!fb)
		return;

	gem = drm_fb_dma_get_gem_obj(fb, 0);
	if (!gem)
		return;

	histb_vdp4_g0_set(vdp, fb, gem, new_state->visible);
}

static void histb_vdp4_plane_atomic_disable(struct drm_plane *plane,
					  struct drm_atomic_commit *state)
{
	struct histb_vdp4 *vdp = plane_to_vdp(plane);

	histb_vdp4_g0_set(vdp, NULL, NULL, false);
}

static int histb_vdp4_plane_atomic_check(struct drm_plane *plane,
				       struct drm_atomic_commit *state)
{
	struct drm_plane_state *new_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;

	if (!new_state->fb)
		return 0;

	/* stride unit is mv100's (16 bytes) until a working system says otherwise */
	if (new_state->fb->pitches[0] % 16)
		return -EINVAL;

	/* the fetch geometry words are 16-bit halves on this part (section 152a) */
	if (new_state->fb->width > 0xffff || new_state->fb->height > 0xffff)
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

static const struct drm_plane_helper_funcs histb_vdp4_plane_helper_funcs = {
	/*
	 * No DRM_GEM_SHADOW_PLANE_HELPER_FUNCS here: they upcast the state with
	 * container_of() to struct drm_shadow_plane_state, which only the
	 * matching DRM_GEM_SHADOW_PLANE_FUNCS duplicate() allocates.
	 */
	.atomic_check		= histb_vdp4_plane_atomic_check,
	.atomic_update		= histb_vdp4_plane_atomic_update,
	.atomic_disable		= histb_vdp4_plane_atomic_disable,
};

static const struct drm_plane_funcs histb_vdp4_plane_funcs = {
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
 * One-time display pipeline setup: route GP0 into the DHD0 mixer, set the AXI
 * outstanding limits and enable the block's clock gate.
 */
/*
 * Board test instrument, not for submission.  The mask picks which of the words
 * the displaying OEM holds -- and this driver never programs -- go in at
 * configuration time; one unbind/bind per bit therefore answers every one of
 * them from a single build.  The values are the OEM's live readings, taken with
 * it displaying (docs/mv200h-femac-reboot-2026-10-21.md sections 158/159).
 */
static const struct {
	u32 off;
	u32 val;
	const char *name;
} histb_vdp4_arms[] = {
	{ CYCBUF_GFX_BIND, 0x00000800, "cycbuf-gfx-bind" },
	{ 0x8608,	    0x00000001, "cycbuf-8608" },
	{ CYCBUF_MODE,	    0x00000010, "cycbuf-mode" },
	{ 0x8750,	    0x00000101, "cycbuf-8750" },
	{ 0x8a74,	    0x00f17a00, "fetch-8a74" },
	{ 0xca10,	    0x84010040, "intf-ca10" },
	{ 0xce00,	    0x008400dc, "date-coeff0" },
	{ 0xce54,	    0x00653421, "date-coeff21" },
	{ 0xcee4,	    0x80808181, "date-cee4" },
	{ 0xceec,	    0x00000113, "date-ceec" },
	{ 0xb000,	    0x00000013, "pq-coef-b000" },
	{ 0xd500,	    0x00000001, "pq-gmm-d500" },
	{ 0xb004,	    0x00001000, "csc1-diag-0" },
	{ 0xb014,	    0x00001000, "csc1-diag-1" },
	{ 0xb024,	    0x00001000, "csc1-diag-2" },
	{ 0xd514,	    0x00007528, "gmm-d514" },
	{ 0xd520,	    0x00007338, "gmm-d520" },
	{ 0xd528,	    0x00000005, "gmm-d528" },
	{ 0xd538,	    0x00000040, "gmm-d538" },
	{ 0xd53c,	    0x00000200, "gmm-d53c" },
};

/*
 * Board test instrument, not for submission: every word where the displaying OEM
 * holds a value this driver does not, taken from the whole-register-file diff
 * where both sides were read live (docs/mv200h-femac-reboot-2026-10-21.md section
 * 166b: 288 configuration differences, 0 gating differences).
 *
 * The low two bits of the parameter pick which class is applied:
 *   bit 0 -- configuration words: enables, update modes, formats, masks;
 *   bit 1 -- mode-carrying words: the OEM's own surface size, stride, position,
 *            channel timing and vertical thresholds.  Those are numbers for a
 *            1280x2160 surface and its timing, so they are wrong for this board
 *            if taken alone -- they exist here to complete the aggregate.
 * Words whose role is a memory address (the OEM's parameter-table and buffer
 * pointers, VDP+0xe04-0xe9c, 0x5000-0x500c, 0x6a04, 0x8250-0x8340, 0x8a54,
 * 0x9250-0x928c) are not in the table at all: copying one points an engine of
 * this board at memory our kernel owns.  Status and counter words are left out
 * for the mirror-image reason -- they are what the other system measures, not
 * something a driver programs.
 *
 * This exists because a live write into a running latched block changes nothing:
 * the same hand-armed state measured a white/black difference of 65 and then 5
 * (section 166g), so only a configuration-time arm can answer the question.  The
 * driver is built in, so the parameter has to arrive as
 * `histb_vdp4.vdp_oem_copy=1` on the kernel command line.
 */
struct histb_vdp4_copy {
	u32 off;
	u32 val;
	u8 mode;
};

static const struct histb_vdp4_copy histb_vdp4_oem_copy[] = {
	{ 0x00060, 0x000dd4eb, 0 },
	{ 0x00104, 0x00000000, 0 },
	{ 0x00120, 0x00200000, 0 },
	{ 0x00134, 0x00000370, 0 },
	{ 0x00150, 0x00004080, 0 },
	{ 0x02000, 0x600000ff, 1 },
	{ 0x02008, 0x086f0fff, 1 },
	{ 0x02018, 0x600000ff, 0 },
	{ 0x02084, 0x086f0fff, 1 },
	{ 0x0208c, 0x086f0fff, 1 },
	{ 0x02090, 0x00080200, 0 },
	{ 0x02094, 0x000000ff, 0 },
	{ 0x02098, 0x04080200, 0 },
	{ 0x0209c, 0x086f0fff, 1 },
	{ 0x020a4, 0x086f0fff, 1 },
	{ 0x020ac, 0x086f0fff, 1 },
	{ 0x020b0, 0x00080200, 0 },
	{ 0x020b4, 0x000000ff, 0 },
	{ 0x020b8, 0x04080200, 0 },
	{ 0x020bc, 0x086f0fff, 1 },
	{ 0x020c4, 0x086f0fff, 1 },
	{ 0x020cc, 0x086f0fff, 1 },
	{ 0x020d0, 0x00080200, 0 },
	{ 0x020d4, 0x000000ff, 0 },
	{ 0x020d8, 0x04080200, 0 },
	{ 0x020dc, 0x086f0fff, 1 },
	{ 0x020e4, 0x086f0fff, 1 },
	{ 0x020ec, 0x086f0fff, 1 },
	{ 0x020f0, 0x00080200, 0 },
	{ 0x020f4, 0x000000ff, 0 },
	{ 0x020f8, 0x04080200, 0 },
	{ 0x020fc, 0x086f0fff, 1 },
	{ 0x022c0, 0xc70aaaaa, 1 },
	{ 0x022d8, 0xc1880001, 0 },
	{ 0x022dc, 0x00000aaa, 0 },
	{ 0x022e0, 0x0000fe00, 1 },
	{ 0x022e4, 0x0000fe00, 1 },
	{ 0x022e8, 0x0437077f, 0 },
	{ 0x022f8, 0x0002e020, 1 },
	{ 0x022fc, 0x00030000, 1 },
	{ 0x02300, 0x0002e020, 1 },
	{ 0x02304, 0x00030000, 0 },
	{ 0x024c0, 0xc6078000, 1 },
	{ 0x024d8, 0xc1880001, 0 },
	{ 0x024dc, 0x00000800, 1 },
	{ 0x024e8, 0x086f0fff, 1 },
	{ 0x024f8, 0x0002e020, 1 },
	{ 0x024fc, 0x00030000, 1 },
	{ 0x02500, 0x0002e020, 1 },
	{ 0x02504, 0x00030000, 0 },
	{ 0x02600, 0x00000016, 0 },
	{ 0x03000, 0x600000ff, 0 },
	{ 0x03018, 0x600000ff, 0 },
	{ 0x04008, 0x086f0fff, 1 },
	{ 0x04010, 0x086f0fff, 1 },
	{ 0x04018, 0xa00000ff, 0 },
	{ 0x04084, 0x086f0fff, 1 },
	{ 0x0408c, 0x086f0fff, 1 },
	{ 0x04094, 0x000000ff, 0 },
	{ 0x0409c, 0x086f0fff, 1 },
	{ 0x040a4, 0x086f0fff, 1 },
	{ 0x040ac, 0x086f0fff, 1 },
	{ 0x040b4, 0x000000ff, 0 },
	{ 0x040bc, 0x086f0fff, 1 },
	{ 0x040c4, 0x086f0fff, 1 },
	{ 0x040cc, 0x086f0fff, 1 },
	{ 0x040d4, 0x000000ff, 0 },
	{ 0x040dc, 0x086f0fff, 1 },
	{ 0x040e4, 0x086f0fff, 1 },
	{ 0x040ec, 0x086f0fff, 1 },
	{ 0x040f4, 0x000000ff, 0 },
	{ 0x040fc, 0x086f0fff, 1 },
	{ 0x05800, 0x600000ff, 0 },
	{ 0x05808, 0x00f30002, 0 },
	{ 0x05818, 0x600000ff, 0 },
	{ 0x06000, 0x68000004, 1 },
	{ 0x06008, 0x0fff086f, 1 },
	{ 0x0680c, 0x086f0fff, 1 },
	{ 0x06810, 0x02cf04ff, 1 },
	{ 0x06884, 0x086f0fff, 1 },
	{ 0x0688c, 0x086f0fff, 1 },
	{ 0x0689c, 0x086f0fff, 1 },
	{ 0x068a4, 0x086f0fff, 1 },
	{ 0x068ac, 0x086f0fff, 1 },
	{ 0x068bc, 0x086f0fff, 1 },
	{ 0x068c4, 0x086f0fff, 1 },
	{ 0x068cc, 0x086f0fff, 1 },
	{ 0x068dc, 0x086f0fff, 1 },
	{ 0x068e4, 0x086f0fff, 1 },
	{ 0x068ec, 0x086f0fff, 1 },
	{ 0x068fc, 0x086f0fff, 1 },
	{ 0x06a00, 0x00010fff, 1 },
	{ 0x06a10, 0x0000086f, 1 },
	{ 0x06a14, 0xf1800000, 0 },
	{ 0x06a18, 0x00000555, 0 },
	{ 0x07028, 0x00000feb, 0 },
	{ 0x0702c, 0x7665642f, 0 },
	{ 0x07030, 0x04756e2f, 0 },
	{ 0x07034, 0x0020006c, 0 },
	{ 0x07038, 0x00202020, 0 },
	{ 0x0703c, 0x20202020, 0 },
	{ 0x07040, 0x20202020, 0 },
	{ 0x07044, 0x06302020, 0 },
	{ 0x07048, 0x00003636, 0 },
	{ 0x08000, 0x0000772f, 0 },
	{ 0x08004, 0x00000511, 0 },
	{ 0x08200, 0x00120011, 0 },
	{ 0x08204, 0x80000001, 0 },
	{ 0x08214, 0x04080200, 0 },
	{ 0x08220, 0x00000000, 0 },
	{ 0x08240, 0x00000002, 0 },
	{ 0x08244, 0x02cf04ff, 1 },
	{ 0x0824c, 0x02cf04ff, 1 },
	{ 0x08290, 0x05000500, 0 },
	{ 0x082c0, 0x00000001, 0 },
	{ 0x082ec, 0x0e5252a0, 0 },
	{ 0x082f4, 0x0000e100, 0 },
	{ 0x082f8, 0x00801000, 0 },
	{ 0x082fc, 0x00007080, 0 },
	{ 0x08300, 0x0000e100, 0 },
	{ 0x08304, 0x0002f548, 0 },
	{ 0x08308, 0x00007080, 0 },
	{ 0x0830c, 0x000177eb, 0 },
	{ 0x08310, 0x0000e100, 0 },
	{ 0x08314, 0x0002f95e, 0 },
	{ 0x08318, 0x00007080, 0 },
	{ 0x0831c, 0x00017eb4, 0 },
	{ 0x08320, 0x02cf04ff, 1 },
	{ 0x08324, 0x00730000, 0 },
	{ 0x08328, 0x00467b00, 0 },
	{ 0x0832c, 0x00475c00, 0 },
	{ 0x08330, 0x00120011, 0 },
	{ 0x08334, 0x00000000, 0 },
	{ 0x08404, 0x80000001, 0 },
	{ 0x08420, 0x00000000, 0 },
	{ 0x084c0, 0x00000001, 0 },
	{ 0x08524, 0x00710002, 0 },
	{ 0x08534, 0x00000000, 0 },
	{ 0x08a04, 0x00000000, 0 },
	{ 0x08a20, 0x00000000, 0 },
	{ 0x08a28, 0x0000ff00, 0 },
	{ 0x08a40, 0x00100068, 0 },
	{ 0x08a60, 0x00b000b0, 0 },
	{ 0x08a74, 0x00adfa00, 0 },
	{ 0x08a84, 0x008eb000, 0 },
	{ 0x08a88, 0x00010001, 0 },
	{ 0x08a94, 0x00adcd00, 0 },
	{ 0x08ac0, 0x00000001, 0 },
	{ 0x08b24, 0x00fb0002, 0 },
	{ 0x08b28, 0x0008edd0, 0 },
	{ 0x08b34, 0x00000000, 0 },
	{ 0x08c20, 0x00000000, 0 },
	{ 0x08cc0, 0x00000001, 0 },
	{ 0x08d24, 0x00f10002, 0 },
	{ 0x08d34, 0x00000000, 0 },
	{ 0x09200, 0x00000800, 0 },
	{ 0x09214, 0x00000004, 0 },
	{ 0x0b000, 0x00000013, 1 },
	{ 0x0b004, 0x00001000, 0 },
	{ 0x0b014, 0x00001000, 0 },
	{ 0x0b024, 0x00001000, 0 },
	{ 0x0b04c, 0x00000044, 0 },
	{ 0x0b050, 0x000007c0, 0 },
	{ 0x0b054, 0x00000600, 0 },
	{ 0x0b058, 0x00000600, 0 },
	{ 0x0b05c, 0x00000040, 0 },
	{ 0x0b060, 0x00000200, 0 },
	{ 0x0b064, 0x00000200, 0 },
	{ 0x0b084, 0x03ff03ff, 0 },
	{ 0x0b800, 0xf0000000, 0 },
	{ 0x0b804, 0x00400000, 0 },
	{ 0x0c004, 0x00002000, 1 },
	{ 0x0c208, 0x0051086f, 1 },
	{ 0x0c20c, 0x00000007, 1 },
	{ 0x0c210, 0x00d70fff, 1 },
	{ 0x0c214, 0x00000057, 1 },
	{ 0x0c220, 0x00090057, 1 },
	{ 0x0c224, 0x000180e1, 1 },
	{ 0x0c228, 0x87b98465, 1 },
	{ 0x0c808, 0x80000000, 1 },
	{ 0x0c810, 0x00000000, 1 },
	{ 0x0ca08, 0x00000006, 1 },
	{ 0x0ca10, 0x84010040, 1 },
	{ 0x0ce00, 0x008400dc, 1 },
	{ 0x0ce54, 0x00653421, 1 },
	{ 0x0ce58, 0x000000ba, 1 },
	{ 0x0cec8, 0x000007f9, 1 },
	{ 0x0cecc, 0x0000020e, 1 },
	{ 0x0ced4, 0x079f0014, 1 },
	{ 0x0ced8, 0x014d014d, 1 },
	{ 0x0cee4, 0x80808181, 1 },
	{ 0x0ceec, 0x00000113, 1 },
	{ 0x0d500, 0x00000001, 0 },
	{ 0x0d504, 0x000005e0, 0 },
	{ 0x0d508, 0x000013a8, 0 },
	{ 0x0d50c, 0x000001f8, 0 },
	{ 0x0d510, 0x00007cc8, 0 },
	{ 0x0d514, 0x00007528, 0 },
	{ 0x0d518, 0x00000e10, 0 },
	{ 0x0d51c, 0x00000e10, 0 },
	{ 0x0d520, 0x00007338, 0 },
	{ 0x0d524, 0x00007eb8, 0 },
	{ 0x0d528, 0x00000005, 0 },
	{ 0x0d538, 0x00000040, 0 },
	{ 0x0d53c, 0x00000200, 0 },
	{ 0x0d540, 0x00000200, 0 },
	{ 0x0d548, 0x000003ff, 0 },
	{ 0x0d600, 0x00000001, 0 },
	{ 0x0d604, 0x000005e0, 0 },
	{ 0x0d608, 0x000013a8, 0 },
	{ 0x0d60c, 0x000001f8, 0 },
	{ 0x0d610, 0x00007cc8, 0 },
	{ 0x0d614, 0x00007528, 0 },
	{ 0x0d618, 0x00000e10, 0 },
	{ 0x0d61c, 0x00000e10, 0 },
	{ 0x0d620, 0x00007338, 0 },
	{ 0x0d624, 0x00007eb8, 0 },
	{ 0x0d628, 0x00000005, 0 },
	{ 0x0d638, 0x00000040, 0 },
	{ 0x0d63c, 0x00000200, 0 },
	{ 0x0d640, 0x00000200, 0 },
	{ 0x0d648, 0x000003ff, 0 },
	{ 0x0d700, 0x00000001, 0 },
	{ 0x0d704, 0x000005e0, 0 },
	{ 0x0d708, 0x000013a8, 0 },
	{ 0x0d70c, 0x000001f8, 0 },
	{ 0x0d710, 0x00007cc8, 0 },
	{ 0x0d714, 0x00007528, 0 },
	{ 0x0d718, 0x00000e10, 0 },
	{ 0x0d71c, 0x00000e10, 0 },
	{ 0x0d720, 0x00007338, 0 },
	{ 0x0d724, 0x00007eb8, 0 },
	{ 0x0d728, 0x00000005, 0 },
	{ 0x0d738, 0x00000040, 0 },
	{ 0x0d73c, 0x00000200, 0 },
	{ 0x0d740, 0x00000200, 0 },
	{ 0x0d748, 0x000003ff, 0 },
};

/* the commit strobes, in the order the vendor's own helpers use them */
static const u32 histb_vdp4_copy_latches[] = {
	0x0f00, 0x4004, 0x5004, 0x6004, 0x6804, 0x8604, 0xc200,
};
#define COPY_PARA_UPD	((1 << 17) | (1 << 18) | (1 << 25))

static u32 vdp_oem_copy;
module_param(vdp_oem_copy, uint, 0);
MODULE_PARM_DESC(vdp_oem_copy, "board test instrument, not for submission: bit 0 applies the displaying OEM's configuration words and bit 1 its mode-carrying words at configuration time, when the blocks are not yet started -- live writes into a running latched window change nothing");

static void histb_vdp4_apply_oem_copy(struct histb_vdp4 *vdp)
{
	unsigned int applied = 0;
	int i;

	if (!vdp_oem_copy)
		return;

	for (i = 0; i < ARRAY_SIZE(histb_vdp4_oem_copy); i++) {
		const struct histb_vdp4_copy *w = &histb_vdp4_oem_copy[i];

		if (!(vdp_oem_copy & (w->mode ? BIT(1) : BIT(0))))
			continue;
		vo_write(vdp, w->off, w->val);
		applied++;
	}
	for (i = 0; i < ARRAY_SIZE(histb_vdp4_copy_latches); i++) {
		u32 off = histb_vdp4_copy_latches[i];
		/* 0xf00 is the shared parameter-update word with one bit per block;
		 * the others are a block's own one-bit commit strobe */
		u32 mask = off == 0x0f00 ? COPY_PARA_UPD : 1;
		u32 cur = vo_read(vdp, off);

		vo_write(vdp, off, cur | mask);
		vo_write(vdp, off, cur & ~mask);
	}
	dev_info(vdp->dev, "oem-copy: %u words applied, mask %#x\n",
		 applied, vdp_oem_copy);
}

static u32 vdp_arm;
module_param(vdp_arm, uint, 0644);
MODULE_PARM_DESC(vdp_arm, "board test instrument, not for submission: bit N of this mask programs one OEM-held VDP word at configuration time; the command line is the only way to do that without a re-mode-set, which this driver's transmitter path does not survive");

static ssize_t histb_vdp4_arm_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%#x\n", vdp_arm);
}

static ssize_t histb_vdp4_arm_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	int ret;

	ret = kstrtou32(buf, 0, &vdp_arm);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(histb_vdp4_arm);

static void histb_vdp4_pipeline_init(struct histb_vdp4 *vdp)
{
	unsigned int i;
	unsigned int chn;
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

	/*
	 * AXI: leave the arbitration words at what a displaying system holds.  The
	 * value this driver used to build here (0x83ff, from a four-bit
	 * outstanding-limit decode) is not a state any working board has been read
	 * in, and the read channel priority a working board does set was never
	 * written at all.
	 */
	vo_write(vdp, VOAXICTRL, VOAXICTRL_LIVE);
	vo_write(vdp, VOAXICTRL1, VOAXICTRL_LIVE);
	vo_write(vdp, VO_MASTER_RD_PRIO, VO_MASTER_RD_PRIO_LIVE);

	/*
	 * Both channel blocks get the vendor's pre-timing default, not only the one
	 * this driver drives: VDP_DHD_DEFAULT stores 0x8011 at base+0xc200 and
	 * VDP_DSD_DEFAULT the same word at base+0xc400, read out of the OEM image's
	 * own text (docs/mv200h-femac-reboot-2026-10-21.md section 148).  The second
	 * block keeps its interface disabled, so this costs no output; it only stops
	 * that block sitting in a state the vendor never runs a board in.  The
	 * SYNC_INV and DATA_SEL writes that the mv200 C source pairs with this
	 * default are gone: no store has been seen at those positions in this part's
	 * channel block, and the 0xc000 layout they belong to is not the channel.
	 */
	for (chn = 0; chn < 2; chn++)
		vo_write(vdp, DHD0_CTRL + chn * HD_CHN_OFFSET, DHD_CTRL_DEFAULT);

	/*
	 * Which surfaces reach the channel is decided by the 4-bit priority slots of
	 * the channel's own mixer word, base+0x0208, and each surface block keeps its
	 * own mixer at +0x108 with its background at +0x100.  All seven words are
	 * written with the values a displaying OEM system holds live (ledger §156):
	 * background 0x04080200 in the channel and VP0 mixers, slots 0x21 in the
	 * channel and GP0 mixers, slots 0x10 in VP0's, and 0 in GP0's background and
	 * alpha.  Slot 0 = 1 is VP0 and slot 1 = 2 the graphics surface, so the live
	 * system runs the picture through the video and post-scale stages rather than
	 * into the channel directly -- which is why the two stage controls are written
	 * below too.  The meanings of the background triple and of the bits above the
	 * slot fields are not established; they are copied because a working board has
	 * exactly these values and not these.
	 */
	vo_write(vdp, CBM0_BKG, MIX_BKG_LIVE);
	vo_write(vdp, CBM0_SLOT, MIX_SLOT_LIVE);
	vo_write(vdp, MIXV0_BKG, MIX_BKG_LIVE);
	vo_write(vdp, MIXV0_SLOT, MIXV0_SLOT_LIVE);
	vo_write(vdp, MIXG0_BKG, MIXG0_BKG_LIVE);
	vo_write(vdp, MIXG0_BKALPHA, MIXG0_BKALPHA_LIVE);
	vo_write(vdp, MIXG0_SLOT, MIX_SLOT_LIVE);

	/*
	 * The two stages between the mixer and the interface, enabled with the live
	 * control words of the same displaying system: VP0 (base+0x4000 = 0xa00000ff,
	 * enable + update-on + opaque) and GP0 (base+0x6800 = 0xe00000ff, the same
	 * with no-sec set).  GP0's commit word at base+0x6804 reads 1 there, so it is
	 * pulsed; VP0's reads 0, so it is not.  The earlier "GP0 is deliberately not
	 * written" position rested on a BL scan that found no GP setter, and that
	 * method cannot see the vendor's indirect calls through its HAL table -- the
	 * live board supersedes it.
	 *
	 * Measured on the board (ledger §156b, boot with these words verified in the
	 * driver's own dump: `vp0 a00000ff gp0 e00000ff gpup 00000001 mixv 00000010
	 * mixg 00000021`): the channel still does not count lines and the transmitter's
	 * format detector still reads zero.  So these writes make us *agree with a
	 * working system*, they do not by themselves make the channel run, and nothing
	 * here should be read as the fix for the scan.  They stay because the OEM
	 * measures them this way while displaying, and leaving two pipeline stages off
	 * is a known divergence from every reference we have.
	 */
	vo_write(vdp, VP0_CTRL, VP0_LIVE_CTRL);
	vo_write(vdp, GP0_CTRL, GP0_LIVE_CTRL);
	vo_write(vdp, GP0_REGUP, 1);

	/*
	 * Board test instrument, not for submission: the words the displaying OEM
	 * holds and this driver has no code for, programmed here so they are in
	 * place before the channel starts rather than pasted into a running one.
	 */
	for (i = 0; i < ARRAY_SIZE(histb_vdp4_arms); i++) {
		if (!(vdp_arm & BIT(i)))
			continue;
		vo_write(vdp, histb_vdp4_arms[i].off, histb_vdp4_arms[i].val);
		dev_info(vdp->dev, "arm %u %s: off %#x val %08x reads %08x\n",
			 i, histb_vdp4_arms[i].name, histb_vdp4_arms[i].off,
			 histb_vdp4_arms[i].val, vo_read(vdp, histb_vdp4_arms[i].off));
	}
	/* the same words the displaying OEM holds, if the instrument is armed */
	histb_vdp4_apply_oem_copy(vdp);
}

/*
 * Board test instrument, not for submission: the vblank question is "did the
 * channel ever tick", and the board cannot hand /dev/mem to us here, so the
 * decision values go to the console.  Both HD channels are printed: the driver
 * owns the one HD_CHN names, and a non-zero value on the other says somebody
 * else still drives the output.
 */
static void histb_vdp4_dump(struct histb_vdp4 *vdp, const char *tag)
{
	dev_info(vdp->dev,
		 "vdpdump %s: VOCTRL %08x VO_MUX %08x INTMSK %08x MSKINTSTA %08x INTSTA %08x\n",
		 tag, vo_read(vdp, VO_CTRL), vo_read(vdp, VO_MUX),
		 vo_read(vdp, VO_INTMSK), vo_read(vdp, VO_MSKINTSTA),
		 vo_read(vdp, VO_INTSTA));
	dev_info(vdp->dev,
		 "vdpdump %s: HD_CHN %u state %08x faddr %08x fstride %08x ffmt %08x lctrl %08x cbm %08x cbkbkg %08x\n",
		 tag, HD_CHN, vo_read(vdp, HD_VBLANK_STATE),
		 vo_read(vdp, GFX0_ADDR), vo_read(vdp, GFX0_STRIDE),
		 vo_read(vdp, GFX0_FMT), vo_read(vdp, L0_CTRL),
		 vo_read(vdp, CBM0_SLOT), vo_read(vdp, CBM0_BKG));
	dev_info(vdp->dev,
		 "vdpdump %s: wbc %08x wbcup %08x cycbuf %08x cycmode %08x para %08x\n",
		 tag, vo_read(vdp, WBC0_CTRL),
		 vo_read(vdp, WBC0_REGUP), vo_read(vdp, CYCBUF_GFX_BIND),
		 vo_read(vdp, CYCBUF_MODE), vo_read(vdp, PARA_UPD));
	dev_info(vdp->dev,
		 "vdpdump %s: vp0 %08x gp0 %08x gpup %08x mixv %08x mixg %08x\n",
		 tag, vo_read(vdp, VP0_CTRL), vo_read(vdp, GP0_CTRL),
		 vo_read(vdp, GP0_REGUP), vo_read(vdp, MIXV0_SLOT),
		 vo_read(vdp, MIXG0_SLOT));
	dev_info(vdp->dev,
		 "vdpdump %s: DHD0 CTRL %08x PWR %08x VTTHD %08x VSYNC %08x VFB %08x HSYNC1 %08x HSYNC2 %08x\n",
		 tag, vo_read(vdp, DHD0_CTRL), vo_read(vdp, DHD0_PWR),
		 vo_read(vdp, DHD0_VTTHD), vo_read(vdp, DHD0_VSYNC),
		 vo_read(vdp, DHD0_VSYNC_VFB), vo_read(vdp, DHD0_HSYNC1),
		 vo_read(vdp, DHD0_HSYNC2));
	dev_info(vdp->dev,
		 "vdpdump %s: DHD1 CTRL %08x PWR %08x VTTHD %08x VSYNC %08x VFB %08x HSYNC1 %08x HSYNC2 %08x\n",
		 tag, vo_read(vdp, DHD0_CTRL + HD_CHN_OFFSET),
		 vo_read(vdp, DHD0_PWR + HD_CHN_OFFSET),
		 vo_read(vdp, DHD0_VTTHD + HD_CHN_OFFSET),
		 vo_read(vdp, DHD0_VSYNC + HD_CHN_OFFSET),
		 vo_read(vdp, DHD0_VSYNC_VFB + HD_CHN_OFFSET),
		 vo_read(vdp, DHD0_HSYNC1 + HD_CHN_OFFSET),
		 vo_read(vdp, DHD0_HSYNC2 + HD_CHN_OFFSET));
}

/*
 * Vertical thresholds, in the vendor's own arithmetic (the non-stereo branch of
 * PF_SetChnBaseTiming(), hal/common/drv_disp_hal.c:952-993 and
 * DISP_GFX_GetIsrPoint(), :734-802): thd1 is half a frame on the HD channel and
 * 60 % on the other, thd2 sits 1.5 ms worth of lines before the end of the
 * frame, thd3 a tenth of a frame. All three go in field-based mode, which is
 * what the vendor passes for any mode that is not frame-packed stereo
 * (DHD_VTXTHD_FIELD_MODE, hal/98mv200/hal_specific/vdp_software_selfdefine.h:3).
 * For 1280x720 at 60 Hz this is thd1 375, thd2 662, thd3 75.
 */
static void histb_vdp4_set_thresholds(struct histb_vdp4 *vdp, u32 total, u32 vfb,
				      u32 vrefresh)
{
	u32 period = vrefresh ? 100000 / (vrefresh * 100) : 1;
	u32 thd1, thd2, thd3, lead;

	/* the vendor divides by this directly; a zero would be its own bug */
	if (period == 0)
		period = 1;

	thd1 = total * (HD_CHN == 0 ? 50 : 60) / 100;
	thd3 = total / 10;

	/* 2 * total / period for the HD channel, 7 * total / (4 * period) there */
	lead = HD_CHN == 0 ? (total * 2) / period : (total * 7) / (period * 4);
	if (lead > vfb)
		lead -= vfb;
	thd2 = total - lead;

	/*
	 * thd1 and thd2 share one word and thd3 sits with an unused thd4 in the
	 * other (hi_reg_vdp.h: U_DHD0_VTTHD, U_DHD0_VTTHD3), so both are written
	 * whole.
	 */
	vo_write(vdp, HD_VTTHD,
		 FIELD_PREP(DHD_VTTHD1, thd1) | DHD_VTTHD1_MODE |
		 FIELD_PREP(DHD_VTTHD2, thd2) | DHD_VTTHD2_MODE);
	vo_write(vdp, HD_VTTHD3,
		 FIELD_PREP(DHD_VTTHD3, thd3) | DHD_VTTHD3_MODE);

	dev_dbg(vdp->dev, "thresholds: total %u vfb %u period %u -> thd %u/%u/%u\n",
		total, vfb, period, thd1, thd2, thd3);
}

static void histb_vdp4_crtc_setup_timing(struct histb_vdp4 *vdp,
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
	u32 total = tvbb + vact + tvfb;	/* lines per field, as the vendor counts */
	u32 ctrl, mux;

	/*
	 * Every word below is written whole rather than read-modify-written.  The
	 * reason changed with the address: at the 0xc000 block this driver used to
	 * program, a whole-word store left the same address reading 0 (and a
	 * displaying OEM system reads 0 there too), so that region gives no basis for
	 * merging anything.  At 0xc200 -- the block the OEM's own text programs -- the
	 * words read back what was written, both in the OEM and in our kernel (bench
	 * replay, docs/mv200h-femac-reboot-2026-10-21.md section 148), so a
	 * read-modify-write would now be possible; whole-word stores stay because
	 * that is the shape the vendor's channel reset uses and because the fields
	 * this function does not own (hmid, thd4) have no value to inherit.
	 *
	 * The order is the vendor's: reset the channel with its interface still
	 * off, lay the static words down (PF_ResetChn, drv_disp_hal.c:464), then
	 * the timing (VDP_DISP_OpenIntf, vdp_hal_chn.c:287), then the thresholds,
	 * then start it (PF_SetChnEnable -> VDP_DISP_SetIntfEnable).
	 */
	ctrl = DHD_CTRL_REGUP | DHD_CTRL_TWOCHN_DEBUG;
	if (!interlaced)
		ctrl |= DHD_CTRL_IOP;		/* progressive */
	/*
	 * The OEM leaves bit 23 raised on the channel that feeds its output
	 * (VDP_DISP_SetIntfRgbModeEn writes a different block, so this bit is the
	 * only place the "RGB to the interface" decision can live here).  It is the
	 * same intent this driver had with the mv200 header's hdmi_mode.
	 */
	ctrl |= DHD_CTRL_HDMI_MODE;
	vo_write(vdp, HD_CTRL, ctrl);

	/* horizontal: registers take value - 1 */
	vo_write(vdp, HD_HSYNC1,
		 FIELD_PREP(DHD_HSYNC1_HACT, histb_vdp4_m1(mode->hdisplay)) |
		 FIELD_PREP(DHD_HSYNC1_HBB, histb_vdp4_m1(hbb)));
	vo_write(vdp, HD_HSYNC2,
		 FIELD_PREP(DHD_HSYNC2_HFB, histb_vdp4_m1(hfb)) |
		 FIELD_PREP(DHD_HSYNC2_HMID, hmid ? hmid - 1 : 0));
	vo_write(vdp, HD_PWR,
		 FIELD_PREP(DHD_PWR_HPW, histb_vdp4_m1(hpw)) |
		 FIELD_PREP(DHD_PWR_VPW, histb_vdp4_m1(vpw)));

	/*
	 * Vertical: top field in VSYNC/VSYNC_VFB, bottom field in VPLUS/VPLUS_VFB.
	 * This part's channel splits vfb into its own word -- the OEM's
	 * VDP_DISP_OpenIntf stores (vact-1, vbb-1) in one 16/16 pair and vfb-1 in the
	 * next word (docs/mv200h-femac-reboot-2026-10-21.md section 148).
	 */
	vo_write(vdp, HD_VSYNC,
		 FIELD_PREP(DHD_VSYNC_VACT, histb_vdp4_m1(vact)) |
		 FIELD_PREP(DHD_VSYNC_VBB, histb_vdp4_m1(tvbb)));
	vo_write(vdp, HD_VSYNC_VFB, FIELD_PREP(DHD_VSYNC_VFB, histb_vdp4_m1(tvfb)));
	/*
	 * For a progressive mode the vendor's format table has Bvact/Bvbb/Bvfb all
	 * zero and VDP_DISP_OpenIntf() subtracts one unconditionally
	 * (vdp_hal_chn.c:337-343), so the word it leaves in the register is the
	 * underflowed all-ones value rather than a mirror of the top field -- except
	 * for the second half, which that code guards with "0 if the input was 0".
	 * Reproduced as the vendor writes it; the field widths here are 16 bits, so
	 * the underflow shape is 0xffff rather than the mv200 header's 0xfff.
	 */
	if (interlaced) {
		vo_write(vdp, HD_VPLUS,
			 FIELD_PREP(DHD_VPLUS_BVACT, histb_vdp4_m1(vact)) |
			 FIELD_PREP(DHD_VPLUS_BVBB, histb_vdp4_m1(bvbb)));
		vo_write(vdp, HD_VPLUS_VFB,
			 FIELD_PREP(DHD_VPLUS_BVFB, histb_vdp4_m1(tvfb)));
	} else {
		/*
		 * Progressive: a working system holds 0 here.  Writing the all-ones
		 * "there is no bottom field" shape instead gave the channel a bottom
		 * field whose geometry is 0xffff lines, which no displaying board has.
		 */
		vo_write(vdp, HD_VPLUS, 0);
		vo_write(vdp, HD_VPLUS_VFB, 0);
	}

	histb_vdp4_set_thresholds(vdp, total, tvfb, drm_mode_vrefresh(mode));

	/*
	 * Latch what was written: the same control word again, with regup in it,
	 * which is what VDP_DISP_SetRegUp() does (hal/98mv200/vdp_hal_chn.c:163).
	 * The interface is still disabled here; PF_SetChnBaseTiming() ends this way
	 * and only PF_SetChnEnable() raises intf_en afterwards.
	 */
	vo_write(vdp, HD_CTRL, ctrl);

	/*
	 * Which HD channel feeds the HDMI interface is a selector, not a fact:
	 * VO_MUX.hdmi_sel (bit 3) carries the channel id, so it has to agree with
	 * HD_CHN rather than inherit whatever the reset left there.  The vendor
	 * writes that id from the same value it programs the timing with
	 * (VDP_DISP_SetIntfMuxSel(), hal/98mv200/vdp_hal_chn.c:1438).
	 *
	 * VO_MUX does read back, and only this bit is ours: sddate_sel (bit 0)
	 * belongs to the SD path, which the chip init sets to 1
	 * (vdp_software_selfdefine.c:346), hence the read-modify-write.
	 */
	mux = vo_read(vdp, VO_MUX);
	/* board test instrument, not for submission: the value we inherited lets the
	 * dump say whether the selector already matched the channel being programmed.
	 */
	dev_info(vdp->dev, "vdpdump: VO_MUX inherited %08x (hdmi_sel=%u)\n", mux,
		 !!(mux & VO_MUX_HDMI_SEL));
	mux = (mux & ~VO_MUX_HDMI_SEL) | HD_MUX_HDMI_SEL;
	vo_write(vdp, VO_MUX, mux);

	/*
	 * Start the channel: the interface enable with the latch, the way
	 * PF_SetChnEnable() ends it -- VDP_DISP_SetIntfEnable() raises intf_en and
	 * calls SetRegUp (hal/98mv200/vdp_hal_chn.c:177, called from
	 * drv_disp_hal.c:1606).
	 *
	 * hdmi_mode goes up with it.  On this part that is bit 23, the bit the OEM
	 * leaves raised on the channel it displays with; the mv200 header calls bit 14
	 * hdmi_mode and its own chip init leaves it 0
	 * (DHD_YUV_TO_HDMI, hal/98mv200/hal_specific/vdp_software_selfdefine.c:15)
	 * because its source is video, while ours is the RGB framebuffer.  Deliberate
	 * departure from the C source, agreed with the running image, but not
	 * checkable with no sink on the line.
	 */
	vo_write(vdp, HD_CTRL, ctrl | DHD_CTRL_HDMI_MODE | DHD_CTRL_INTF_EN);

	/* board test instrument, not for submission: the numbers the driver
	 * computed, so the register dump above can be checked as arithmetic rather
	 * than eyeballed.
	 */
	dev_info(vdp->dev,
		 "vdpdump: mode %dx%d clk=%d htot %d vtot %d | hbb %d hfb %d hpw %d vbb %d vfb %d vpw %d total %d\n",
		 mode->hdisplay, mode->vdisplay, mode->clock, mode->htotal,
		 mode->vtotal, hbb, hfb, hpw, vbb, vfb, vpw, total);

	histb_vdp4_dump(vdp, "enable");
	msleep(1000);
	histb_vdp4_dump(vdp, "enable+1s");
}

static enum drm_mode_status histb_vdp4_crtc_mode_valid(struct drm_crtc *crtc,
						     const struct drm_display_mode *mode)
{
	struct histb_vdp4 *vdp = crtc_to_vdp(crtc);
	long rate;

	/* G0/GP0 position and resolution fields are 12 bits wide */
	if (mode->hdisplay > 4095 || mode->vdisplay > 4095)
		return MODE_BAD;

	/* 3840x2160 is the largest mode the pipeline reaches */
	if (mode->hdisplay > 3840 || mode->vdisplay > 2160)
		return MODE_BAD;

	if (mode->hdisplay % 2 || mode->vdisplay % 2)
		return MODE_BAD;

	/* the HD channel clock can only produce rates that divide out exactly */
	rate = clk_round_rate(vdp->clks[HISTB_VDP4_CLK_PIX].clk,
			      mode->clock * 1000);
	if (rate < 0)
		return MODE_ERROR;

	/*
	 * The clock's search only accepts a rate it can solve without remainder,
	 * while mode->clock is the nominal rate rounded to whole kHz -- a few
	 * hundred Hz away from what the timing adds up to. Allow half a percent;
	 * keep the arithmetic in Hz so a large mismatch cannot overflow and look
	 * like a pass.
	 */
	if (abs(rate - (long)mode->clock * 1000) > (long)mode->clock * 5)
		return MODE_NOCLOCK;

	return MODE_OK;
}

static void histb_vdp4_crtc_atomic_enable(struct drm_crtc *crtc,
					struct drm_atomic_commit *state)
{
	struct histb_vdp4 *vdp = crtc_to_vdp(crtc);
	struct drm_crtc_state *new_state = drm_atomic_get_new_crtc_state(state, crtc);
	const struct drm_display_mode *mode = &new_state->adjusted_mode;
	struct drm_plane_state *pstate;
	struct drm_gem_dma_object *gem;
	struct drm_framebuffer *fb;

	/* the channel clock is what the HDMI transmitter receives, too */
	histb_vdp4_set_pixclk(vdp, mode->clock);

	histb_vdp4_pipeline_init(vdp);

	histb_vdp4_crtc_setup_timing(vdp, mode);

	/*
	 * The channel is enabled by histb_vdp4_crtc_setup_timing()'s control word:
	 * these registers do not read back, so a second read-modify-write here would
	 * write a word made up of zeros plus these two bits and clear iop, hdmi_mode
	 * and twochn_debug again.
	 */

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
			histb_vdp4_g0_set(vdp, fb, gem, pstate->visible);
	}

	drm_crtc_vblank_on(crtc);
}

static void histb_vdp4_crtc_atomic_disable(struct drm_crtc *crtc,
					 struct drm_atomic_commit *state)
{
	struct histb_vdp4 *vdp = crtc_to_vdp(crtc);

	drm_crtc_vblank_off(crtc);

	vo_write(vdp, HD_CTRL, DHD_CTRL_REGUP | DHD_CTRL_HDMI_MODE |
		   DHD_CTRL_TWOCHN_DEBUG);
}

static int histb_vdp4_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct histb_vdp4 *vdp = crtc_to_vdp(crtc);

	if (vdp->irq < 0)
		return -EOPNOTSUPP;

	/*
	 * A set bit of VOINTMSK lets its source through, so enabling the vblank
	 * source means setting it -- see the note on the name in
	 * histb-vdp4-regs.h.
	 *
	 * Written whole, not read-modify-written: the board was measured with this
	 * word at 0xffffffff while the channel was running, and every source then
	 * passes into VOMSKINTSTA, which is what made INTID 122 fire about 2100
	 * times a second (the two channel thresholds nobody consumes, VTTHD1 and
	 * VTTHD3, were among them).  This driver consumes one source, so it owns
	 * the word and leaves the other bits down.
	 */
	vo_write(vdp, VO_INTMSK, HD_INT_VBLANK);
	return 0;
}

static void histb_vdp4_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct histb_vdp4 *vdp = crtc_to_vdp(crtc);

	if (vdp->irq < 0)
		return;

	/*
	 * Ack before masking.  The latch is cleared by writing the status back to
	 * VOMSKINTSTA (the vendor's own ISR does that, hal/98mv200/vdp_hal_chn.c:1952),
	 * and a vblank that arrives while the mask bit is being dropped would
	 * otherwise stay latched with its source masked -- the handler then reads
	 * nothing, returns IRQ_NONE, and the level line never falls.  That is the
	 * second half of the storm measured above: VOINTMSK read 0xfffffffd with
	 * VOINTSTA still holding the vblank bit.
	 */
	vo_write(vdp, VO_MSKINTSTA, HD_INT_VBLANK);
	vo_write(vdp, VO_INTMSK, 0);
}

static void histb_vdp4_crtc_atomic_flush(struct drm_crtc *crtc,
				       struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new_state = drm_atomic_get_new_crtc_state(state, crtc);

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

static const struct drm_crtc_helper_funcs histb_vdp4_crtc_helper_funcs = {
	.mode_valid		= histb_vdp4_crtc_mode_valid,
	.atomic_enable		= histb_vdp4_crtc_atomic_enable,
	.atomic_disable		= histb_vdp4_crtc_atomic_disable,
	.atomic_flush		= histb_vdp4_crtc_atomic_flush,
};

static const struct drm_crtc_funcs histb_vdp4_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.destroy		= drm_crtc_cleanup,
	.reset			= drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state	= drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_crtc_destroy_state,
	.enable_vblank		= histb_vdp4_crtc_enable_vblank,
	.disable_vblank		= histb_vdp4_crtc_disable_vblank,
};

static irqreturn_t histb_vdp4_irq_handler(int irq, void *data)
{
	struct histb_vdp4 *vdp = data;
	struct histb_vdp4_display *disp = READ_ONCE(vdp->disp);
	u32 sta = vo_read(vdp, VO_MSKINTSTA);

	if (!sta)
		return IRQ_NONE;

	/*
	 * The vendor ISR takes its status from VOMSKINTSTA (the *masked* status,
	 * hal/98mv200/hal_specific/vdp_software_selfdefine.c:877) and acknowledges
	 * by writing that same value back to it (hal/98mv200/vdp_hal_chn.c:1952),
	 * so dispatch and ack are the same word here.  VOINTMSK is misnamed: a bit
	 * of 1 lets the source through, because the vendor's "enable" path ORs it
	 * in (VDP_DISP_SetIntMask) and its "disable" path clears it -- reading the
	 * raw VOINTSTA and filtering with ~VO_INTMSK therefore discarded exactly
	 * the sources that were enabled.
	 *
	 * Ack every latched source, not just the vblank one: returning IRQ_NONE
	 * while a VDP status bit stays set makes the core disable the line
	 * ("nobody cared") and vblank dies with it.
	 */
	vo_write(vdp, VO_MSKINTSTA, sta);

	if (sta & ~HD_INT_VBLANK)
		dev_warn_ratelimited(vdp->dev, "vdp irq: unexpected status %08x\n",
				     sta);

	/* the line stays requested across an aggregate unbind, no CRTC then */
	if (disp && (sta & HD_INT_VBLANK)) {
		drm_crtc_handle_vblank(&disp->crtc);
		/*
		 * Own-progress witness: "did the frame event arrive again" is the only
		 * question that distinguishes a channel that free-runs from one that
		 * asserted once at enable time -- and it must not lean on the panel or
		 * on a capture card, neither of which this board has attached.
		 */
		if (!(++vdp->vblank_seen % 50))
			dev_info(vdp->dev, "vdp vblank: %lu frames seen\n",
				 vdp->vblank_seen);
	}
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Mode config / binding                                               */
/* ------------------------------------------------------------------ */

static const struct drm_mode_config_funcs histb_vdp4_mode_config_funcs = {
	.fb_create		= drm_gem_fb_create,
	.atomic_check		= drm_atomic_helper_check,
	.atomic_commit		= drm_atomic_helper_commit,
};

static const struct drm_encoder_funcs histb_vdp4_encoder_funcs = {
	.destroy		= drm_encoder_cleanup,
};

static const struct drm_encoder_helper_funcs histb_vdp4_encoder_helper_funcs = {
};

DEFINE_DRM_GEM_DMA_FOPS(histb_vdp4_fops);

static const struct drm_driver histb_vdp4_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &histb_vdp4_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name			= "histb-vdp4",
	.desc			= "HiSTB VDP v4 display engine",
	.major			= 1,
	.minor			= 0,
};

static int histb_vdp4_bind(struct device *dev)
{
	struct histb_vdp4 *vdp = dev_get_drvdata(dev);
	struct histb_vdp4_display *disp;
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

	drm = drm_dev_alloc(&histb_vdp4_driver, dev);
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
	drm->mode_config.funcs		= &histb_vdp4_mode_config_funcs;

	/* one CRTC, so the vblank core must exist before the IRQ can be served
	 * and before any atomic commit waits on it
	 */
	ret = drm_vblank_init(drm, 1);
	if (ret) {
		dev_err_probe(dev, ret, "vblank_init failed\n");
		goto err_put;
	}

	ret = drm_universal_plane_init(drm, &disp->primary, 0,
				       &histb_vdp4_plane_funcs,
				       histb_vdp4_formats,
				       ARRAY_SIZE(histb_vdp4_formats),
				       NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret) {
		dev_err_probe(dev, ret, "plane_init failed\n");
		goto err_put;
	}
	drm_plane_helper_add(&disp->primary, &histb_vdp4_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &disp->crtc, &disp->primary, NULL,
					&histb_vdp4_crtc_funcs, "histb-vdp4-crtc");
	if (ret) {
		dev_err_probe(dev, ret, "crtc_init failed\n");
		goto err_put;
	}
	drm_crtc_helper_add(&disp->crtc, &histb_vdp4_crtc_helper_funcs);
	drm_crtc_vblank_reset(&disp->crtc);

	disp->encoder.possible_crtcs = drm_crtc_mask(&disp->crtc);
	ret = drm_encoder_init(drm, &disp->encoder, &histb_vdp4_encoder_funcs,
			       DRM_MODE_ENCODER_TMDS, "histb-vdp4-encoder");
	if (ret) {
		dev_err_probe(dev, ret, "encoder_init failed\n");
		goto err_put;
	}
	drm_encoder_helper_add(&disp->encoder, &histb_vdp4_encoder_helper_funcs);

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
	 * The transmitter reports hot plug as a level and its interrupt is not
	 * wired, so the bridge connector the helper builds over the chain asks
	 * for DRM_CONNECTOR_POLL_CONNECT|DISCONNECT.  Without starting the poll
	 * work here nothing would ever re-detect a plug event.  The drmm
	 * variant stops with the DRM device.
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

static void histb_vdp4_unbind(struct device *dev)
{
	struct histb_vdp4 *vdp = dev_get_drvdata(dev);
	struct histb_vdp4_display *disp = vdp->disp;
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
static const struct component_master_ops histb_vdp4_master_ops = {
	.bind		= histb_vdp4_bind,
	.unbind		= histb_vdp4_unbind,
};

static int histb_vdp4_add_master(struct device *dev)
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

	/*
	 * component_master_add_with_match() reads match->num before anything
	 * else, so an aggregate with no available component must fail instead of
	 * handing it NULL. Disabling only the transmitter node is a valid DT
	 * configuration.
	 */
	if (!match)
		return -ENODEV;

	return component_master_add_with_match(dev, &histb_vdp4_master_ops, match);
}

/*
 * The VOU reset request is CRG54[30]. A reset only reaches the block while it
 * is clocked, so the gates come up first: CRG54's own gate group is the clock
 * "vo" and the HD channel clock is "pix", both enabled by
 * histb_vdp4_runtime_resume(). The reset has to be held before it is released
 * and the block needs settling time afterwards, so the pulse comes with delays.
 */
/*
 * Board test instrument, not for submission.  A bus lock during probe takes the
 * console with it, so the enable steps are counted into the trampoline's no-map
 * window, which the watchdog reset leaves intact: after the box is back in
 * fastboot, "md.l 0x200e000 8" says which step ran last and what the block
 * reported.  Word 4 is VOCTRL, 5 the channel control word before the VOU pulse,
 * 6 the same after it, 7 VO_INTMSK.
 */
#define MARK_PHYS	0x0200e000

static void histb_vdp4_mark(struct histb_vdp4 *vdp, u32 step, u32 off)
{
	if (!vdp->mark)
		return;

	writel(step, vdp->mark);
	if (off)
		writel(vo_read(vdp, off), vdp->mark + 16);
	wmb();			/* the reader is a different boot */
}

/*
 * Put the block into the reset state a clocked access survives and then hand it
 * back: the VOU request raised (see CRG54_VOU_SRST_REQ for the measurement) and
 * the block's own request pulsed.  The caller enables the clocks first, so this
 * runs with the HD domain clocked; the channel control readback at the end is the
 * first live access of the register file and it has to answer.
 */
static int histb_vdp4_reset_release(struct histb_vdp4 *vdp)
{
	int ret;

	ret = regmap_update_bits(vdp->crg, CRG54, CRG54_VOU_SRST_REQ,
				 CRG54_VOU_SRST_REQ);
	if (ret)
		return ret;

	/*
	 * Pulse the block's own reset before the register file is touched at all.
	 * HiSilicon's own mainline-facing port for this IP (linux-6.12.y,
	 * drivers/gpu/drm/hisilicon/histb/histb-vdp.c, Hi3798CV200) asserts, waits
	 * and then deasserts for one stated reason: to drop every display state
	 * inherited from the boot-logo pipeline.  This port only ever deasserted, so
	 * whatever the vendor fastboot left inside the VDP state machines has been
	 * carried into every round of this bring-up.
	 *
	 * The pulse is deliberately not interrupted by a read.  That driver polls
	 * VDPVERSION1/2 for reset completion, but on this part both words read the
	 * constant date stamp 0x20170929 (docs/mv200h-oem-live-2026-10-24.txt), so
	 * such a poll could only time out -- and reading the register file while a
	 * VDP reset is held is the access shape that already locked the bus once
	 * here (ledger 156d, with the wider VOU request).  mb() plus 10 us is that
	 * driver's own bracket around the reset edge.
	 */
	ret = reset_control_assert(vdp->rst_vdp);
	if (ret)
		return ret;
	mb();
	udelay(10);

	ret = reset_control_deassert(vdp->rst_vdp);
	if (ret)
		return ret;
	mb();
	udelay(10);

	histb_vdp4_mark(vdp, 5, HD_CTRL);

	return 0;
}

static int histb_vdp4_runtime_suspend(struct device *dev)
{
	struct histb_vdp4 *vdp = dev_get_drvdata(dev);

	/*
	 * Both reset requests stay as histb_vdp4_reset_release() left them:
	 * putting either back would leave a block held in reset while the
	 * transmitter still keeps the pixel clock running, and that is the
	 * combination that locks the bus.
	 */
	clk_bulk_disable_unprepare(HISTB_VDP4_CLK_NUM, vdp->clks);

	return 0;
}

static int histb_vdp4_runtime_resume(struct device *dev)
{
	struct histb_vdp4 *vdp = dev_get_drvdata(dev);
	int ret;

	histb_vdp4_mark(vdp, 1, 0);

	/*
	 * Clocks first, then the reset edge.  HiSilicon's own histb port enables the
	 * clocks before it asserts and deasserts the VDP reset, and that order is a
	 * hardware requirement rather than taste: a synchronous reset only propagates
	 * while the block is clocked, so pulsing it with the clocks down -- which is
	 * what this driver used to do -- can leave the state machines exactly as the
	 * boot loader handed them over.  The VOU request bit stays set through the
	 * pulse, as a displaying OEM system holds it (CRG54 reads 0x400d0019 there).
	 */
	ret = clk_bulk_prepare_enable(HISTB_VDP4_CLK_NUM, vdp->clks);
	if (ret)
		return ret;

	histb_vdp4_mark(vdp, 2, 0);

	ret = histb_vdp4_reset_release(vdp);
	if (ret)
		return ret;

	histb_vdp4_mark(vdp, 3, VO_CTRL);

	dev_info(dev, "vdp4mark: resumed, VOCTRL %08x HD_CTRL %08x INTMSK %08x\n",
		 vo_read(vdp, VO_CTRL), vo_read(vdp, HD_CTRL),
		 vo_read(vdp, VO_INTMSK));

	return 0;
}

static const struct dev_pm_ops histb_vdp4_pm_ops = {
	RUNTIME_PM_OPS(histb_vdp4_runtime_suspend, histb_vdp4_runtime_resume, NULL)
};

static int histb_vdp4_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_vdp4 *vdp;
	int ret;

	vdp = devm_kzalloc(dev, sizeof(*vdp), GFP_KERNEL);
	if (!vdp)
		return -ENOMEM;

	vdp->dev = dev;
	platform_set_drvdata(pdev, vdp);

	vdp->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vdp->regs))
		return dev_err_probe(dev, PTR_ERR(vdp->regs), "ioremap failed\n");

	vdp->mark = memremap(MARK_PHYS, 32, MEMREMAP_WB);
	if (vdp->mark)
		writel(0, vdp->mark);

	/*
	 * "pix" is the HD channel pixel clock that histb_vdp4_set_pixclk()
	 * programs through the display PLL and CRG54's select and divider; "bus"
	 * and "sdate" are the two CRG54 gates the channel runs through.  The VOU
	 * reset request only shifts while the block is clocked, so all three are
	 * raised before it is pulsed, and the SD channel gate (CRG54 bit 2) is
	 * left alone: an HD picture does not run through it, and the vendor's own
	 * system holds it clear.
	 */
	vdp->clks[HISTB_VDP4_CLK_PIX].id = "pix";
	vdp->clks[HISTB_VDP4_CLK_BUS].id = "bus";
	vdp->clks[HISTB_VDP4_CLK_SDATE].id = "sdate";
	ret = devm_clk_bulk_get(dev, HISTB_VDP4_CLK_NUM, vdp->clks);
	if (ret)
		return dev_err_probe(dev, ret, "clk\n");

	/*
	 * Two requests and one clock-generator field have to be in their running
	 * state before the register file is touched, and the order is a hardware
	 * constraint, not taste: histb_vdp4_reset_release() records what the
	 * board shows.  The VOU request is not a reset_control because the CRG
	 * offers no way to say that its bit means the opposite of what the reset
	 * framework's assert does, and getting that wrong on a clocked block
	 * locks the bus.
	 */
	vdp->rst_vdp = devm_reset_control_get_exclusive(dev, "vdp");
	if (IS_ERR(vdp->rst_vdp))
		return dev_err_probe(dev, PTR_ERR(vdp->rst_vdp), "vdp rst\n");

	vdp->crg = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "hisilicon,crg");
	if (IS_ERR(vdp->crg))
		return dev_err_probe(dev, PTR_ERR(vdp->crg), "no crg regmap\n");

	/*
	 * Power the block up before anything reads or writes a VDP register:
	 * histb_vdp4_runtime_resume() enables the channel clock and takes the
	 * VOU through a reset of our own, so the register file starts from its
	 * documented defaults instead of from the state it was handed over in.
	 * The device then stays permanently powered for the whole probe/remove
	 * lifetime: the atomic paths and the vblank IRQ touch registers with no
	 * pm_runtime references of their own, and the shared pixel clock makes
	 * idle power-down of little use anyway. histb_vdp4_remove() drops the
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
	 * one base address (VO_G0_ADDR), and the display path is addressed
	 * physically -- the VDP does have its own MMU (the vendor's VDP_SMMU_*
	 * family, at VDP+0xe000 here rather than the header's 0xf000) but this
	 * board's boot leaves its global bypass raised, read as SCR = 0x1, and
	 * the port keeps that state (port ledger section 164e).  So an imported
	 * buffer is refused unless its scatter/gather
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

		ret = devm_request_irq(dev, vdp->irq, histb_vdp4_irq_handler,
				       IRQF_SHARED, dev_name(dev), vdp);
		if (ret) {
			dev_err_probe(dev, ret, "request_irq failed\n");
			goto err_pm_put;
		}
	}

	/* board test instrument, not for submission */
	device_create_file(dev, &dev_attr_histb_vdp4_arm);

	ret = histb_vdp4_add_master(dev);
	if (ret)
		goto err_pm_put;

	return 0;

err_pm_put:
	pm_runtime_put_sync(dev);
err_pm_disable:
	pm_runtime_disable(dev);
	return ret;
}

static void histb_vdp4_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &histb_vdp4_master_ops);

	pm_runtime_put_sync_suspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);
}

/*
 * drm_dev_register() documents that a KMS driver stops the pipeline at shutdown
 * and reboot: the channel would otherwise keep scanning a framebuffer whose
 * pages are about to be torn down underneath it.
 */
static void histb_vdp4_shutdown(struct platform_device *pdev)
{
	struct histb_vdp4 *vdp = platform_get_drvdata(pdev);
	struct histb_vdp4_display *disp = READ_ONCE(vdp->disp);

	if (disp)
		drm_atomic_helper_shutdown(disp->drm);
}

static const struct of_device_id histb_vdp4_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv200-vdp" },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_vdp4_of_match);

static struct platform_driver histb_vdp4_platform_driver = {
	.probe		= histb_vdp4_probe,
	.remove		= histb_vdp4_remove,
	.shutdown	= histb_vdp4_shutdown,
	.driver		= {
		.name			= "histb-vdp4",
		.of_match_table		= histb_vdp4_of_match,
		.pm			= pm_ptr(&histb_vdp4_pm_ops),
	},
};
module_platform_driver(histb_vdp4_platform_driver);

MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_DESCRIPTION("DRM driver for the HiSTB VDP v4 display engine (hi3798mv100)");
MODULE_LICENSE("GPL");
