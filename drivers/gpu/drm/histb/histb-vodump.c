// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only snapshot of the hi3798mv100 display pipeline as the vendor
 * fastboot left it while scanning out its boot logo.
 *
 * The point of this file is that its snapshot path never writes a device
 * register: it captures a configuration that is known to put a picture on
 * the HDMI output, so drivers/gpu/drm/histb can be diffed against it.
 * scr writes a pattern into the scan-out buffer, which is plain DRAM, and
 * poke/peek exist for register A/B experiments once the snapshot has been
 * taken -- they are the only paths that touch device state.
 *
 * Run it again at any time with:
 *   echo 1 > /sys/module/histb_vodump/parameters/dump
 *   echo <pattern> > /sys/module/histb_vodump/parameters/scr
 */

#include <linux/bitfield.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sizes.h>
#include <linux/vmalloc.h>

#include "histb-vo-regs.h"

#define VO_BASE		0xf8cc0000
#define VO_SIZE		0x10000
#define HDMI_BASE	0xf8ce0000
#define CRG_BASE	0xf8a22000
#define CRG_SIZE	0x200
#define AIAO_BASE	0xf8cd0000
#define AIAO_SIZE	0x10000

/* Every register the VO driver touches, plus the state counters. */
static const u16 vo_offs[] = {
	VO_CTRL, VO_MUX, VO_INTSTA, G0_CTRL, G0_ADDR, G0_STRIDE, G0_IRESO,
	G0_CBMPARA, G0_DFPOS, G0_DLPOS, G0_UPD, GP0_CTRL, GP0_IRESO, GP0_ORESO,
	GP0_UPD, CBM_ATTR, CBM_MIX1, CBM_MIX2, DHD0_CTRL, DHD0_VSYNC,
	DHD0_HSYNC1, DHD0_HSYNC2, DHD0_VPLUS, DHD0_PWR, DHD0_SYNC_INV,
	DHD0_DATA_SEL, DHD0_VTTHD, DHD0_STATE, DHD0_DEBUG, DHD0_DEBUG_STATE,
	HDATE_STATE, GP0_GALPHA, GP0_DFPOS, GP0_DLPOS, GP0_BK, GP0_ALPHA,
	CBM_BKG1, HDATE_EN, HDATE_POLA_CTRL, HDATE_VIDEO_FORMAT,
	HDATE_OUT_CTRL,
};

static const u16 sd_offs[] = {
	VO_DAC_MUX, VO_MUX_TESTSYNC, VO_MUX_TESTDATA, CBM_BKG2,
	SD_CTRL, SD_VSYNC, SD_HSYNC1, SD_HSYNC2, SD_VPLUS, SD_PWR,
	SD_VTTHD3, SD_VTTHD, SD_SYNC_INV, SD_DATA_SEL,
};

static void __iomem *vo_base, *hdmi_base, *crg_base, *aiao_base;

static void vodump_list(const char *tag, const u16 *offs, int n)
{
	static char buf[700];	/* too big for the init-call stack */
	int i, len = 0;

	for (i = 0; i < n; i++)
		len += scnprintf(buf + len, sizeof(buf) - len, "%s%04x:%08x",
				 i ? " " : "", offs[i],
				 readl(vo_base + offs[i]));

	pr_info("vodump %s %s\n", tag, buf);
}

static void vodump_scan(const char *tag, void __iomem *base, u32 size)
{
	static char buf[96];	/* too big for the init-call stack */
	u32 off;
	int len = 0;

	buf[0] = '\0';
	for (off = 0; off < size; off += 4) {
		u32 val = readl(base + off);

		if (!val)
			continue;
		len += scnprintf(buf + len, sizeof(buf) - len, " %04x:%08x",
				 off, val);
		if (len > 64) {
			pr_info("vodump %s%s\n", tag, buf);
			buf[0] = '\0';
			len = 0;
		}
	}
	if (len)
		pr_info("vodump %s%s\n", tag, buf);
}

/*
 * The HDMI TPI file is flattened: bank0 (I2C 0x72) at +0, bank1 (0x7a) at
 * +0x400, the on-chip PHY at +0x1800.  The range in between is not
 * implemented, so it is stepped over rather than read.
 */
static void vodump_hdmi(void)
{
	vodump_scan("tx-bank0", hdmi_base, 0x400);
	vodump_scan("tx-bank1", hdmi_base + 0x400, 0x400);
	vodump_scan("tx-phy", hdmi_base + 0x1800, 0x400);
}

static struct {
	void *kva;
	u32 phys;
	u32 stride;	/* bytes per line in memory */
	u32 width;	/* pixels per line the fetch engine walks */
	u32 height;
	u32 rows;	/* lines actually mapped */
	u32 base;	/* register block this layer was found at */
} fb;

/*
 * G0 at 0x6000 is only one of the graphics layers: the boot logo turns out
 * to be scanned out of the block at 0x6800 while 0x6000 sits disabled, so
 * the buffer has to be found by looking for whichever block is enabled.
 */
static const u32 gfx_bases[] = { 0x6000, 0x6800 };

static u32 fb_addr;
static u32 fb_stride;
static u32 fb_width;
static u32 fb_height;

module_param(fb_addr, uint, 0644);
MODULE_PARM_DESC(fb_addr, "override: physical address to scribble on (0 = auto-detect the enabled layer)");
module_param(fb_stride, uint, 0644);
MODULE_PARM_DESC(fb_stride, "override: bytes per line");
module_param(fb_width, uint, 0644);
MODULE_PARM_DESC(fb_width, "override: pixels per line");
module_param(fb_height, uint, 0644);
MODULE_PARM_DESC(fb_height, "override: lines");

/*
 * The scan-out buffer lives in the boot CMA area, which is ordinary (and on
 * this 1 GiB board, high) memory -- memremap() rejects RAM, so build a
 * vmapped alias of the page range instead.
 */
static void *vodump_fb_vmap(u32 len)
{
	unsigned long pfn = PHYS_PFN(fb.phys);
	u32 first = fb.phys & ~PAGE_MASK;
	u32 npages = DIV_ROUND_UP(first + len, PAGE_SIZE);
	struct page **pages;
	void *kva;
	u32 i;

	pages = kvmalloc_array(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;

	for (i = 0; i < npages; i++) {
		if (!pfn_valid(pfn + i))
			break;
		pages[i] = pfn_to_page(pfn + i);
	}
	if (i != npages) {
		pr_info("vodump fb: pfn %08lx not managed\n", pfn + i);
		kvfree(pages);
		return NULL;
	}

	kva = vmap(pages, npages, VM_MAP, PAGE_KERNEL);
	kvfree(pages);

	return kva ? kva + first : NULL;
}

static void vodump_fb_read(void)
{
	u32 i, addr = 0, stride16 = 0, ireso = 0, vfpos = 0, vlpos = 0;

	fb.base = 0;
	for (i = 0; i < ARRAY_SIZE(gfx_bases); i++) {
		u32 c = readl(vo_base + gfx_bases[i] + 0x00);

		if (!(c & BIT(31)) || !readl(vo_base + gfx_bases[i] + 0x10))
			continue;
		if (!fb_addr && !fb.base) {
			fb.base = gfx_bases[i];
			addr = readl(vo_base + gfx_bases[i] + 0x10);
			stride16 = readl(vo_base + gfx_bases[i] + 0x1c) & G0_STRIDE_MASK;
			ireso = readl(vo_base + gfx_bases[i] + 0x20);
			vfpos = readl(vo_base + gfx_bases[i] + 0x88);
			vlpos = readl(vo_base + gfx_bases[i] + 0x8c);
		}
		pr_info("vodump layer %04x ctrl:%08x addr:%08x stride16:%x ireso:%08x pos:%08x/%08x\n",
			gfx_bases[i], c, readl(vo_base + gfx_bases[i] + 0x10),
			readl(vo_base + gfx_bases[i] + 0x1c), ireso, vfpos, vlpos);
	}

	if (fb_addr) {
		fb.base = 0;
		addr = fb_addr;
		stride16 = fb_stride / 16;
		ireso = 0;
		fb.width = fb_width;
		fb.height = fb_height;
		fb.rows = fb_height;
	} else {
		fb.width = FIELD_GET(G0_IRESO_IW, ireso) + 1;
		fb.height = FIELD_GET(G0_IRESO_IH, ireso) + 1;
		fb.rows = FIELD_GET(G0_DLPOS_Y, vlpos) -
			  FIELD_GET(G0_DLPOS_Y, vfpos) + 1;
	}

	fb.phys = addr;
	fb.stride = stride16 * 16;
	if (fb_stride)
		fb.stride = fb_stride;

	pr_info("vodump fb from:%04x phys:%08x stride:%u wxh:%ux%u rows:%u ireso:%08x\n",
		fb.base, fb.phys, fb.stride, fb.width, fb.height, fb.rows, ireso);

	if (!fb.phys || !fb.stride || !fb.width ||
	    (u64)fb.stride * fb.rows > SZ_16M) {
		pr_info("vodump fb: nothing to map\n");
		return;
	}

	fb.kva = vodump_fb_vmap(fb.stride * fb.rows);
	if (!fb.kva) {
		pr_info("vodump fb: vmap %08x+%u failed\n", fb.phys,
			fb.stride * fb.rows);
		return;
	}

	pr_info("vodump fb y0x0: %*phC\n", 32, fb.kva);
	pr_info("vodump fb y1x0: %*phC\n", 32, fb.kva + fb.stride);
	pr_info("vodump fb mid: %*phC\n", 16,
		fb.kva + (fb.rows / 2) * fb.stride + (fb.width / 2) * 4);
}

static void vodump_fb_write(u32 pattern)
{
	u32 x, y, val;

	if (!fb.kva) {
		pr_info("vodump scr: no buffer mapped\n");
		return;
	}

	for (y = 0; y < fb.rows; y++) {
		for (x = 0; x < fb.width; x++) {
			switch (pattern) {
			case 6:	/* 64px checkerboard, white on black */
				val = ((x >> 6) + (y >> 6)) & 1 ?
					0x00ffffff : 0x00000000;
				break;
			case 7:	/* quadrants: white, black, red, blue */
				if (y < fb.rows / 2)
					val = x < fb.width / 2 ? 0x00ffffff :
								0x00000000;
				else
					val = x < fb.width / 2 ? 0x00ff0000 :
								0x000000ff;
				break;
			default:
				val = pattern;
				break;
			}
			*(u32 *)(fb.kva + y * fb.stride + x * 4) = val;
		}
	}
	flush_kernel_vmap_range(fb.kva, fb.stride * fb.rows);
	pr_info("vodump scr: wrote %08x over %ux%u stride %u\n", pattern,
		fb.width, fb.rows, fb.stride);
}

static void vodump_full(void)
{
	vodump_list("vo", vo_offs, ARRAY_SIZE(vo_offs));
	vodump_list("sd", sd_offs, ARRAY_SIZE(sd_offs));
	vodump_scan("vdp", vo_base, VO_SIZE);
	vodump_scan("crg", crg_base, CRG_SIZE);
	vodump_hdmi();
	vodump_fb_read();
}

static int param_set_dump(const char *val, const struct kernel_param *kp)
{
	unsigned int n;
	int ret = kstrtouint(val, 0, &n);

	if (ret)
		return ret;
	vodump_full();
	return 0;
}

static int param_set_scr(const char *val, const struct kernel_param *kp)
{
	unsigned int n;
	int ret = kstrtouint(val, 0, &n);

	if (ret)
		return ret;
	vodump_fb_write(n);
	return 0;
}

static unsigned int poke_sel;

static int param_set_poke(const char *val, const struct kernel_param *kp)
{
	void __iomem *base = vo_base;
	unsigned int size = VO_SIZE;
	unsigned int off, n;

	if (sscanf(val, "%x %x", &off, &n) != 2)
		return -EINVAL;
	if (poke_sel == 1) {
		base = crg_base;
		size = CRG_SIZE;
	} else if (poke_sel == 2) {
		base = hdmi_base;
		size = SZ_8K;
	} else if (poke_sel == 3) {
		base = aiao_base;
		size = AIAO_SIZE;
	}
	if (!base || off + 4 > size)
		return -ERANGE;
	writel(n, base + off);
	pr_info("vodump poke: sel=%u [%04x] = %08x -> %08x\n", poke_sel, off, n,
		readl_relaxed(base + off));
	return 0;
}

static unsigned int peek_off;

static int param_set_peek(const char *val, const struct kernel_param *kp)
{
	void __iomem *base = vo_base;
	unsigned int size = VO_SIZE;
	unsigned int off;

	if (kstrtouint(val, 0, &off))
		return -EINVAL;
	if (poke_sel == 1) {
		base = crg_base;
		size = CRG_SIZE;
	} else if (poke_sel == 2) {
		base = hdmi_base;
		size = SZ_8K;
	} else if (poke_sel == 3) {
		base = aiao_base;
		size = AIAO_SIZE;
	}
	if (!base || off + 4 > size)
		return -ERANGE;
	peek_off = off;
	pr_info("vodump peek: sel=%u [%04x] = %08x\n", poke_sel, off,
		readl_relaxed(base + off));
	return 0;
}

static unsigned int dump_now = 1;
static unsigned int scr_pattern;
static unsigned int poke_val;

module_param_call(dump, param_set_dump, param_get_uint, &dump_now, 0644);
MODULE_PARM_DESC(dump, "1 snapshots at boot; echo 1 re-runs the snapshot");
module_param_call(scr, param_set_scr, param_get_uint, &scr_pattern, 0644);
MODULE_PARM_DESC(scr, "write a u32 pixel pattern into the scan-out buffer (6=checker, 7=quadrants)");
module_param_call(poke, param_set_poke, param_get_uint, &poke_val, 0644);
MODULE_PARM_DESC(poke, "write a register: echo '<offset> <value>' > poke (region from poke_sel)");
module_param(poke_sel, uint, 0644);
module_param_call(peek, param_set_peek, param_get_uint, &peek_off, 0644);
MODULE_PARM_DESC(peek, "read back one register: echo <offset> > peek (region from poke_sel)");
MODULE_PARM_DESC(poke_sel, "poke region: 0=VDP, 1=CRG, 2=HDMI TX, 3=AIAO");

/*
 * The named lists twice: once as soon as the mapping exists, once at the
 * end of init.  If the two disagree, something other than the display
 * drivers is disturbing the pipeline.
 */
static bool vodump_mapped;

static int __init histb_vodump_map(void)
{
	vo_base = ioremap(VO_BASE, VO_SIZE);
	hdmi_base = ioremap(HDMI_BASE, SZ_8K);
	crg_base = ioremap(CRG_BASE, CRG_SIZE);
	aiao_base = ioremap(AIAO_BASE, AIAO_SIZE);
	if (!vo_base || !hdmi_base || !crg_base || !aiao_base) {
		pr_err("vodump: ioremap failed\n");
		return -ENOMEM;
	}
	vodump_mapped = true;

	if (dump_now)
		vodump_list("early-vo", vo_offs, ARRAY_SIZE(vo_offs));

	return 0;
}

static int __init histb_vodump_init(void)
{
	if (!vodump_mapped)
		return 0;

	if (dump_now)
		vodump_full();

	return 0;
}
early_initcall(histb_vodump_map);
late_initcall(histb_vodump_init);

MODULE_DESCRIPTION("Read-only hi3798mv100 display snapshot");
MODULE_LICENSE("GPL");
