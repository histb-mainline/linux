// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSilicon STB SoCs DMA Controller
 *
 * Copyright (c) 2024 David Yang
 */
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/string.h>

#include "virt-dma.h"

/******** hardware definitions ********/

#define DMAC_INT_STATUS		0x00  /* interrupt status */
#define DMAC_INT_TC_STATUS	0x04  /* transmission completion interrupt status */
#define DMAC_INT_TC_CLR		0x08  /* transmission completion interrupt clear */
#define DMAC_INT_ERR_STATUS	0x0c  /* error interrupt status */
#define DMAC_INT_ERR_CLR	0x10  /* error interrupt clear */
#define DMAC_RAW_INT_TC_STATUS	0x14
#define DMAC_RAW_INT_ERR_STATUS	0x18
#define DMAC_ENBLD_CHNS		0x1c  /* enabled channels */
#define DMAC_SOFT_BRST_REQ	0x20  /* software burst request */
#define DMAC_SOFT_SGLE_REQ	0x24  /* software single request */
#define DMAC_SOFT_LST_BRST_REQ	0x28  /* software last burst request */
#define DMAC_SOFT_LST_SGLE_REQ	0x2c  /* software last single request */
#define DMAC_CFG		0x30
#define  DMAC_CFG_M2			BIT(2)  /* use big endian for Master 2 */
#define  DMAC_CFG_M1			BIT(1)  /* use big endian for Master 1 */
#define  DMAC_CFG_EN			BIT(0)
#define DMAC_SYNC		0x34

#define DMAC_CHAN_SRC_ADDR(n)		(0x100 + 0x20 * (n) + 0x00)
#define DMAC_CHAN_DST_ADDR(n)		(0x100 + 0x20 * (n) + 0x04)
#define DMAC_CHAN_LLI(n)		(0x100 + 0x20 * (n) + 0x08)  /* channel link list item */
#define  DMAC_CHAN_LLI_W0			BIT(1)
#define  DMAC_CHAN_LLI_NEXT			GENMASK(31, 2)  /* addr / 4, 0: disable */
#define  DMAC_CHAN_LLI_MST			BIT(0)  /* master to be loaded, 0: 1, 1: 2 */
#define DMAC_CHAN_CTRL(n)		(0x100 + 0x20 * (n) + 0x0c)
#define  DMAC_CHAN_CTRL_INT			BIT(31)  /* interrupt for this node */
#define  DMAC_CHAN_CTRL_PROT			GENMASK(30, 28)  /* bus HPROT value */
#define   DMAC_CHAN_CTRL_PROT_CACHEABLE			BIT(30)
#define   DMAC_CHAN_CTRL_PROT_BUFFERABLE		BIT(29)
#define   DMAC_CHAN_CTRL_PROT_PRIVILEGED		BIT(28)
#define  DMAC_CHAN_CTRL_DST_INC			BIT(27)
#define  DMAC_CHAN_CTRL_SRC_INC			BIT(26)
#define  DMAC_CHAN_CTRL_DST_MST			BIT(25)
#define  DMAC_CHAN_CTRL_SRC_MST			BIT(24)
#define  DMAC_CHAN_CTRL_DST_WIDTH		GENMASK(23, 21)
#define  DMAC_CHAN_CTRL_SRC_WIDTH		GENMASK(20, 18)
#define   DMAC_WIDTH_8					0
#define   DMAC_WIDTH_16					1
#define   DMAC_WIDTH_32					2
#define  DMAC_CHAN_CTRL_DST_BSIZE		GENMASK(17, 15)
#define  DMAC_CHAN_CTRL_SRC_BSIZE		GENMASK(14, 12)
#define   DMAC_BSIZE_1					0
#define   DMAC_BSIZE_4					1
#define   DMAC_BSIZE_8					2
#define   DMAC_BSIZE_16					3
#define   DMAC_BSIZE_32					4
#define   DMAC_BSIZE_64					5
#define   DMAC_BSIZE_128				6
#define   DMAC_BSIZE_256				7
#define  DMAC_CHAN_CTRL_TX			GENMASK(11, 0)
#define DMAC_CHAN_CFG(n)		(0x100 + 0x20 * (n) + 0x10)
#define  DMAC_CHAN_CFG_W0			(GENMASK(31, 19) | BIT(10) | BIT(5))
#define  DMAC_CHAN_CFG_HALT			BIT(18)  /* halt, ignore further requests */
#define  DMAC_CHAN_CFG_ACTIVE			BIT(17)  /* active, data exists */
#define  DMAC_CHAN_CFG_LOCK			BIT(16)  /* lock bus */
#define  DMAC_CHAN_CFG_INT_TC_MASK		BIT(15)
#define  DMAC_CHAN_CFG_INT_ERR_MASK		BIT(14)
#define  DMAC_CHAN_CFG_FLOW			GENMASK(13, 11)  /* src, dst, controller */
#define   DMAC_FLOW_MMC					0  /* memory -> memory (DMAC) */
#define   DMAC_FLOW_MDC					1  /* memory -> peripheral (DMAC) */
#define   DMAC_FLOW_SMC					2  /* peripheral -> memory (DMAC) */
#define   DMAC_FLOW_SDC					3  /* peripheral -> peripheral (DMAC) */
#define   DMAC_FLOW_SDD					4  /* peripheral -> peripheral (dst) */
#define   DMAC_FLOW_MDD					5  /* memory -> peripheral (dst) */
#define   DMAC_FLOW_SMS					6  /* peripheral -> memory (src) */
#define   DMAC_FLOW_SDS					7  /* peripheral -> peripheral (src) */
#define  DMAC_CHAN_CFG_DST_PERI			GENMASK(9, 6)
#define  DMAC_CHAN_CFG_SRC_PERI			GENMASK(4, 1)
#define  DMAC_CHAN_CFG_EN			BIT(0)

#define DMAC_CHAN_NUM		4u
/* max transfer count of a single node, in units of bus width */
#define DMAC_CHAN_MAX_TX	4095u

/* one transfer list item, fetched from memory by the controller */
struct histb_dma_item {
	__le32 src_addr;
	__le32 dst_addr;
	__le32 lli;
	__le32 ctrl;
} __packed __aligned(4);

/******** driver definitions ********/

struct histb_dma_seg {
	dma_addr_t src;
	dma_addr_t dst;
	size_t len;
};

/* how a transfer is laid out on the bus */
struct histb_dma_xfer_cfg {
	bool src_inc;
	bool dst_inc;
	/* DMAC_WIDTH_* codes, source and destination must match */
	u8 src_width;
	u8 dst_width;
	/* DMAC_BSIZE_* code, 0: single transfer */
	u8 src_bsize;
	u8 dst_bsize;
	/* allow a trailing node of single bytes, for unaligned memcpy */
	bool tail8;
	/* value of the FLOW and PERI fields of DMAC_CHAN_CFG */
	u32 chan_cfg;
};

/* one transfer list item, and where the controller fetches it from */
struct histb_dma_item_ref {
	struct histb_dma_item *ptr;
	dma_addr_t addr;
};

struct histb_dma_desc {
	struct virt_dma_desc vdesc;

	/* first node, programmed directly into the channel registers */
	dma_addr_t src_addr;
	dma_addr_t dst_addr;
	u32 lli;
	u32 ctrl;
	/* FLOW and PERI fields of DMAC_CHAN_CFG */
	u32 chan_cfg;

	/*
	 * transfer list, used when the transfer spans multiple nodes;
	 * pool-backed so that it can be released from the completion
	 * tasklet
	 */
	struct histb_dma_item_ref *items;
	unsigned int nitems;
	struct dma_pool *items_pool;

	/* total bytes to transfer */
	size_t size;

	/* memset: fixed source buffer holding the fill value */
	int *value;
	/* device and pool to free value from */
	struct device *dev;
	struct dma_pool *pool;
};

struct histb_dma_chan {
	void __iomem *base;
	struct device *dev;
	unsigned char id;
	/* peripheral request line, from the DT dma specifier */
	unsigned char req_line;
	/* managed DMA pool of memset srcs */
	struct dma_pool *pool;
	/* managed DMA pool of transfer list items */
	struct dma_pool *item_pool;

	struct dma_slave_config config;

	struct virt_dma_chan vchan;

	struct histb_dma_desc *desc;
	/* the controller is clock-gated by runtime PM while idle */
	bool rpm_active;
};

struct histb_dma_priv {
	void __iomem *base;
	struct device *dev;

	struct reset_control *rst;
	struct clk_bulk_data *clks;
	unsigned int clks_n;
	int irq;

	struct dma_device dmadev;

	struct histb_dma_chan chans[DMAC_CHAN_NUM];
};

static struct histb_dma_desc *to_histb_dma_desc(struct virt_dma_desc *vdesc)
{
	return container_of(vdesc, struct histb_dma_desc, vdesc);
}

static struct histb_dma_chan *to_histb_dma_chan(struct dma_chan *dmachan)
{
	return container_of(dmachan, struct histb_dma_chan, vchan.chan);
}

static struct dma_chan *
histb_dma_of_xlate(struct of_phandle_args *spec, struct of_dma *ofdma);

/******** desc ********/

static void histb_dma_desc_free(struct histb_dma_desc *desc)
{
	for (unsigned int i = 0; i < desc->nitems; i++)
		dma_pool_free(desc->items_pool, desc->items[i].ptr,
			      desc->items[i].addr);
	kfree(desc->items);
	if (desc->value)
		dma_pool_free(desc->pool, desc->value, desc->src_addr);

	kfree(desc);
}

static struct histb_dma_desc *histb_dma_desc_new(void)
{
	return kzalloc(sizeof(struct histb_dma_desc), GFP_NOWAIT);
}

static struct histb_dma_desc *
histb_dma_desc_new_memset(struct histb_dma_chan *chan, unsigned char value)
{
	struct histb_dma_desc *desc = histb_dma_desc_new();
	int *buf;

	if (!desc)
		return NULL;

	buf = dma_pool_alloc(chan->pool, GFP_NOWAIT, &desc->src_addr);
	if (!buf) {
		kfree(desc);
		return NULL;
	}

	memset(buf, value, sizeof(*buf));
	desc->value = buf;
	desc->dev = chan->dev;
	desc->pool = chan->pool;

	return desc;
}

static void histb_dma_vdesc_free(struct virt_dma_desc *vdesc)
{
	histb_dma_desc_free(to_histb_dma_desc(vdesc));
}

/*
 * Fill a transfer with one segment per element of segs[], chaining LLI
 * items when a segment exceeds what a single node can transfer.
 *
 * Takes ownership of desc; on failure desc is freed and NULL returned.
 */
static struct dma_async_tx_descriptor *
histb_dma_prep_segments(struct histb_dma_chan *chan,
			struct histb_dma_desc *desc,
			const struct histb_dma_seg *segs, unsigned int nsegs,
			const struct histb_dma_xfer_cfg *cfg,
			unsigned long flags)
{
	unsigned int beat = 1u << cfg->src_width;
	unsigned int n = 0, i;
	u32 ctrl, inc;

	if (!nsegs || cfg->src_width != cfg->dst_width)
		goto err_free;

	for (i = 0; i < nsegs; i++) {
		size_t len = segs[i].len;

		if (len == 0)
			goto err_free;

		if (len & (beat - 1) && !cfg->tail8)
			goto err_free;

		n += DIV_ROUND_UP(len & ~(beat - 1), DMAC_CHAN_MAX_TX * beat);
		if (len & (beat - 1))
			n++;	/* tail node */
	}

	desc->dev = chan->dev;
	desc->size = 0;
	for (i = 0; i < nsegs; i++)
		desc->size += segs[i].len;

	desc->items_pool = chan->item_pool;
	desc->items = kcalloc(n, sizeof(*desc->items), GFP_NOWAIT);
	if (!desc->items)
		goto err_free;
	for (i = 0; i < n; i++) {
		desc->items[i].ptr = dma_pool_alloc(chan->item_pool,
						    GFP_NOWAIT,
						    &desc->items[i].addr);
		if (!desc->items[i].ptr)
			goto err_free;
		/* desc_free unwinds exactly the items allocated so far */
		desc->nitems = i + 1;
	}

	inc = (cfg->src_inc ? DMAC_CHAN_CTRL_SRC_INC : 0) |
	      (cfg->dst_inc ? DMAC_CHAN_CTRL_DST_INC : 0);
	ctrl = DMAC_CHAN_CTRL_PROT | inc |
	       (cfg->src_bsize << 12) | (cfg->dst_bsize << 15) |
	       (cfg->dst_width << 21) | (cfg->src_width << 18);

	n = 0;
	for (i = 0; i < nsegs; i++) {
		dma_addr_t src = segs[i].src, dst = segs[i].dst;
		size_t len = segs[i].len, words = len / beat;

		while (words) {
			unsigned int tx = min_t(size_t, words, DMAC_CHAN_MAX_TX);
			struct histb_dma_item *item = desc->items[n++].ptr;

			item->src_addr = cpu_to_le32(src);
			item->dst_addr = cpu_to_le32(dst);
			item->lli = 0;
			item->ctrl = cpu_to_le32(ctrl | tx);
			src += tx * beat;
			dst += tx * beat;
			words -= tx;
		}

		if (len & (beat - 1)) {
			struct histb_dma_item *item = desc->items[n++].ptr;

			item->src_addr = cpu_to_le32(src);
			item->dst_addr = cpu_to_le32(dst);
			item->lli = 0;
			item->ctrl = cpu_to_le32(DMAC_CHAN_CTRL_PROT | inc |
						 (DMAC_WIDTH_8 << 21) |
						 (DMAC_WIDTH_8 << 18) |
						 (len & (beat - 1)));
		}
	}

	/* chain items and raise the interrupt on the last node only */
	for (i = 0; i < desc->nitems; i++) {
		struct histb_dma_item *item = desc->items[i].ptr;
		u32 val = le32_to_cpu(item->ctrl);

		if (i + 1 < desc->nitems)
			item->lli = cpu_to_le32((desc->items[i + 1].addr >> 2) |
						DMAC_CHAN_LLI_MST);
		else
			val |= DMAC_CHAN_CTRL_INT;
		item->ctrl = cpu_to_le32(val);
	}

	desc->src_addr = segs[0].src;
	desc->dst_addr = segs[0].dst;
	desc->chan_cfg = cfg->chan_cfg;
	/* a single node raised the interrupt directly from the registers */
	desc->ctrl = le32_to_cpu(desc->items[0].ptr->ctrl);
	if (desc->nitems > 1) {
		desc->ctrl &= ~DMAC_CHAN_CTRL_INT;
		desc->lli = (desc->items[1].addr >> 2) | DMAC_CHAN_LLI_MST;
	} else {
		desc->lli = 0;
	}

	return vchan_tx_prep(&chan->vchan, &desc->vdesc, flags);

err_free:
	histb_dma_desc_free(desc);
	return NULL;
}

/******** channel ********/

static void
_histb_dma_chan_debug(struct device *dev, unsigned int id, unsigned int n,
		      u32 src_addr, u32 dst_addr, u32 lli, u32 ctrl)
{
	static const unsigned int width[] = { 8, 16, 32, 0, 0, 0, 0, 0 };
	static const unsigned int bsize[] = { 1, 4, 8, 16, 32, 64, 128, 256 };

	dev_dbg(dev,
		"%u: %2u %c[%lu], %08x%c @ %2u-%2u -> %08x%c @ %2u-%2u * %lu\n",
		id, n, (ctrl & DMAC_CHAN_CTRL_INT) ? 'I' : ' ',
		(ctrl & DMAC_CHAN_CTRL_PROT) >> 28,
		src_addr, (ctrl & DMAC_CHAN_CTRL_SRC_INC) ? '+' : ' ',
		width[(ctrl & DMAC_CHAN_CTRL_SRC_WIDTH) >> 18],
		bsize[(ctrl & DMAC_CHAN_CTRL_SRC_BSIZE) >> 12],
		dst_addr, (ctrl & DMAC_CHAN_CTRL_DST_INC) ? '+' : ' ',
		width[(ctrl & DMAC_CHAN_CTRL_DST_WIDTH) >> 21],
		bsize[(ctrl & DMAC_CHAN_CTRL_DST_BSIZE) >> 15],
		ctrl & DMAC_CHAN_CTRL_TX);
}

static void histb_dma_chan_debug(struct histb_dma_chan *chan)
{
	static const char * const flow[] = {
		"MMC", "MDC", "SMC", "SDC", "SDD", "MDD", "SMS", "SDS"
	};

	void __iomem *base = chan->base;
	struct device *dev = chan->dev;
	unsigned int id = chan->id;

	u32 src_addr = readl_relaxed(base + DMAC_CHAN_SRC_ADDR(id));
	u32 dst_addr = readl_relaxed(base + DMAC_CHAN_DST_ADDR(id));
	u32 lli = readl_relaxed(base + DMAC_CHAN_LLI(id));
	u32 ctrl = readl_relaxed(base + DMAC_CHAN_CTRL(id));
	u32 cfg = readl_relaxed(base + DMAC_CHAN_CFG(id));

	dev_dbg(dev, "%u: flow %s, peri %lu -> %lu, %c%c%c%c%c%c\n", id,
		flow[(cfg & DMAC_CHAN_CFG_FLOW) >> 11],
		(cfg & DMAC_CHAN_CFG_SRC_PERI) >> 1,
		(cfg & DMAC_CHAN_CFG_DST_PERI) >> 6,
		(cfg & DMAC_CHAN_CFG_HALT) ? 'H' : ' ',
		(cfg & DMAC_CHAN_CFG_ACTIVE) ? 'A' : ' ',
		(cfg & DMAC_CHAN_CFG_LOCK) ? 'L' : ' ',
		(cfg & DMAC_CHAN_CFG_INT_TC_MASK) ? 'i' : ' ',
		(cfg & DMAC_CHAN_CFG_INT_ERR_MASK) ? 'e' : ' ',
		(cfg & DMAC_CHAN_CFG_EN) ? 'E' : ' ');

	_histb_dma_chan_debug(dev, id, 0, src_addr, dst_addr, lli, ctrl);
}

static int histb_dma_chan_halt(struct histb_dma_chan *chan)
{
	void __iomem *cfg = chan->base + DMAC_CHAN_CFG(chan->id);

	u32 val;
	int ret;

	val = readl_relaxed(cfg);
	if (!(val & DMAC_CHAN_CFG_EN))
		return 0;

	if (!(val & DMAC_CHAN_CFG_HALT)) {
		val &= ~DMAC_CHAN_CFG_W0;
		val |= DMAC_CHAN_CFG_HALT;
		writel(val, cfg);
	}

	ret = readl_relaxed_poll_timeout_atomic(cfg, val,
						!(val & DMAC_CHAN_CFG_ACTIVE),
						10, USEC_PER_SEC);
	if (ret)
		return ret;

	/*
	 * disable the channel so that it shows up disabled in
	 * ENBLD_CHNS, otherwise waiters would poll forever
	 */
	val = readl_relaxed(cfg);
	val &= ~(DMAC_CHAN_CFG_W0 | DMAC_CHAN_CFG_EN);
	writel(val, cfg);

	return 0;
}

static void histb_dma_chan_enable(struct histb_dma_chan *chan)
{
	void __iomem *cfg = chan->base + DMAC_CHAN_CFG(chan->id);

	u32 val;

	val = readl_relaxed(cfg);
	if (val & DMAC_CHAN_CFG_EN)
		return;

	if (val & DMAC_CHAN_CFG_HALT) {
		val &= ~DMAC_CHAN_CFG_HALT;
		writel_relaxed(val, cfg);
	}
}

static int histb_dma_chan_wait_disable(struct histb_dma_chan *chan)
{
	void __iomem *base = chan->base;
	unsigned int id = chan->id;

	u32 val;

	return readl_relaxed_poll_timeout_atomic(base + DMAC_ENBLD_CHNS, val,
						 !(val & BIT(id)),
						 10, USEC_PER_SEC);
}

/*
 * start transferring chan->desc, chan->desc.vdesc.lock should be held by caller
 */
static void histb_dma_chan_start(struct histb_dma_chan *chan)
{
	struct histb_dma_desc *desc = chan->desc;
	void __iomem *base = chan->base;
	unsigned int id = chan->id;

	u32 val;

	writel_relaxed(desc->src_addr, base + DMAC_CHAN_SRC_ADDR(id));
	writel_relaxed(desc->dst_addr, base + DMAC_CHAN_DST_ADDR(id));
	writel_relaxed(desc->lli, base + DMAC_CHAN_LLI(id));
	writel_relaxed(desc->ctrl, base + DMAC_CHAN_CTRL(id));

	/* a previous transfer may have left the channel halted */
	val = readl_relaxed(base + DMAC_CHAN_CFG(id));
	val &= ~(DMAC_CHAN_CFG_W0 | DMAC_CHAN_CFG_HALT);
	val |= DMAC_CHAN_CFG_INT_TC_MASK;
	val |= DMAC_CHAN_CFG_INT_ERR_MASK;
	val &= ~(DMAC_CHAN_CFG_FLOW | DMAC_CHAN_CFG_SRC_PERI |
		 DMAC_CHAN_CFG_DST_PERI);
	val |= desc->chan_cfg;
	val |= DMAC_CHAN_CFG_EN;
	writel_relaxed(val, base + DMAC_CHAN_CFG(id));

	histb_dma_chan_debug(chan);
}

/* memory to memory transfers: both sides increment, 32-bit beats */
static const struct histb_dma_xfer_cfg histb_dma_memcpy_cfg = {
	.src_inc	= true,
	.dst_inc	= true,
	.src_width	= DMAC_WIDTH_32,
	.dst_width	= DMAC_WIDTH_32,
	.tail8		= true,
	.chan_cfg	= DMAC_FLOW_MMC << 11,
};

/* memset: fixed source pointing at the fill value */
static const struct histb_dma_xfer_cfg histb_dma_memset_cfg = {
	.src_inc	= false,
	.dst_inc	= true,
	.src_width	= DMAC_WIDTH_32,
	.dst_width	= DMAC_WIDTH_32,
	.tail8		= true,
	.chan_cfg	= DMAC_FLOW_MMC << 11,
};

static struct dma_async_tx_descriptor *
histb_dma_prep_dma_memcpy(struct dma_chan *dmachan, dma_addr_t dst,
			  dma_addr_t src, size_t len, unsigned long flags)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	struct histb_dma_desc *desc;
	const struct histb_dma_seg seg = { .src = src, .dst = dst, .len = len };

	dev_dbg(chan->dev, "prep memcpy on channel %d: dst = %pad, src = %pad, len = %zu\n",
		chan->id, &dst, &src, len);

	desc = histb_dma_desc_new();
	if (!desc)
		return NULL;

	return histb_dma_prep_segments(chan, desc, &seg, 1,
				       &histb_dma_memcpy_cfg, flags);
}

static struct dma_async_tx_descriptor *
histb_dma_prep_dma_memset(struct dma_chan *dmachan, dma_addr_t dst, int value,
			  size_t len, unsigned long flags)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	struct histb_dma_desc *desc;
	struct histb_dma_seg seg;

	dev_dbg(chan->dev, "prep memset on channel %d: dst = %pad, value = %#x, len = %zu\n",
		chan->id, &dst, value, len);

	desc = histb_dma_desc_new_memset(chan, value);
	if (!desc)
		return NULL;

	seg.src = desc->src_addr;
	seg.dst = dst;
	seg.len = len;

	return histb_dma_prep_segments(chan, desc, &seg, 1,
				       &histb_dma_memset_cfg, flags);
}

static struct dma_async_tx_descriptor *
histb_dma_prep_dma_memset_sg(struct dma_chan *dmachan, struct scatterlist *sgl,
			     unsigned int sg_len, int value,
			     unsigned long flags)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	struct histb_dma_desc *desc;
	struct scatterlist *sg;
	struct histb_dma_seg *segs;
	struct dma_async_tx_descriptor *tx;
	unsigned int i;

	desc = histb_dma_desc_new_memset(chan, value);
	if (!desc)
		return NULL;

	segs = kcalloc(sg_len, sizeof(*segs), GFP_NOWAIT);
	if (!segs) {
		histb_dma_desc_free(desc);
		return NULL;
	}

	for_each_sg(sgl, sg, sg_len, i) {
		segs[i].src = desc->src_addr;
		segs[i].dst = sg_dma_address(sg);
		segs[i].len = sg_dma_len(sg);
	}

	tx = histb_dma_prep_segments(chan, desc, segs, sg_len,
				     &histb_dma_memset_cfg, flags);
	kfree(segs);

	return tx;
}

static int histb_dma_slave_width(u32 addr_width, u8 *code)
{
	switch (addr_width) {
	case DMA_SLAVE_BUSWIDTH_1_BYTE:
		*code = DMAC_WIDTH_8;
		return 0;
	case DMA_SLAVE_BUSWIDTH_2_BYTES:
		*code = DMAC_WIDTH_16;
		return 0;
	case DMA_SLAVE_BUSWIDTH_4_BYTES:
		*code = DMAC_WIDTH_32;
		return 0;
	default:
		return -EINVAL;
	}
}

static int histb_dma_slave_bsize(u32 maxburst, u8 *code)
{
	/* burst encodings: 1, 4, 8 ... 256; there is no burst of 2 */
	if (maxburst == 0)
		maxburst = 1;

	if (maxburst == 1)
		*code = DMAC_BSIZE_1;
	else if (maxburst > 2 && is_power_of_2(maxburst) &&
		 maxburst <= 256)
		*code = fls(maxburst) - 2;
	else
		return -EINVAL;

	return 0;
}

static struct dma_async_tx_descriptor *
histb_dma_prep_slave_sg(struct dma_chan *dmachan, struct scatterlist *sgl,
			unsigned int sg_len, enum dma_transfer_direction dir,
			unsigned long flags, void *context)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	struct dma_slave_config *config = &chan->config;
	struct histb_dma_xfer_cfg cfg = { };
	struct histb_dma_desc *desc;
	struct histb_dma_seg *segs;
	struct dma_async_tx_descriptor *tx;
	struct scatterlist *sg;
	unsigned int i;
	int ret;

	if (!is_slave_direction(dir))
		return NULL;

	ret = histb_dma_slave_width(config->src_addr_width, &cfg.src_width) ?:
	      histb_dma_slave_width(config->dst_addr_width, &cfg.dst_width) ?:
	      histb_dma_slave_bsize(config->src_maxburst, &cfg.src_bsize) ?:
	      histb_dma_slave_bsize(config->dst_maxburst, &cfg.dst_bsize);
	if (ret)
		return NULL;

	if (cfg.src_width != cfg.dst_width)
		return NULL;

	desc = histb_dma_desc_new();
	if (!desc)
		return NULL;

	segs = kcalloc(sg_len, sizeof(*segs), GFP_NOWAIT);
	if (!segs) {
		histb_dma_desc_free(desc);
		return NULL;
	}

	for_each_sg(sgl, sg, sg_len, i) {
		if (dir == DMA_MEM_TO_DEV) {
			segs[i].src = sg_dma_address(sg);
			segs[i].dst = config->dst_addr;
		} else {
			segs[i].src = config->src_addr;
			segs[i].dst = sg_dma_address(sg);
		}
		segs[i].len = sg_dma_len(sg);
	}

	cfg.src_inc = dir == DMA_MEM_TO_DEV;
	cfg.dst_inc = dir == DMA_DEV_TO_MEM;
	if (dir == DMA_MEM_TO_DEV) {
		cfg.chan_cfg = (DMAC_FLOW_MDC << 11) |
			       (chan->req_line << 6);	/* DST_PERI */
	} else {
		cfg.chan_cfg = (DMAC_FLOW_SMC << 11) |
			       (chan->req_line << 1);	/* SRC_PERI */
	}

	tx = histb_dma_prep_segments(chan, desc, segs, sg_len, &cfg, flags);
	kfree(segs);

	return tx;
}

static int
histb_dma_slave_config(struct dma_chan *dmachan,
		       struct dma_slave_config *config)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	u8 tmp;
	int ret;

	/* reject unsupported settings here, not as a bare NULL in prep */
	ret = histb_dma_slave_width(config->src_addr_width, &tmp) ?:
	      histb_dma_slave_width(config->dst_addr_width, &tmp) ?:
	      histb_dma_slave_bsize(config->src_maxburst, &tmp) ?:
	      histb_dma_slave_bsize(config->dst_maxburst, &tmp);
	if (ret)
		return ret;

	chan->config = *config;

	return 0;
}

static int histb_dma_chan_set_halt(struct histb_dma_chan *chan, bool halt)
{
	void __iomem *reg = chan->base + DMAC_CHAN_CFG(chan->id);
	unsigned long flags;
	int ret = 0;
	u32 val;

	spin_lock_irqsave(&chan->vchan.lock, flags);

	/* pausing only makes sense while a transfer owns the channel */
	if (!chan->desc) {
		ret = -EPERM;
		goto out;
	}

	val = readl_relaxed(reg);
	if ((val & DMAC_CHAN_CFG_EN) &&
	    (bool)(val & DMAC_CHAN_CFG_HALT) == halt)
		goto out;

	val &= ~DMAC_CHAN_CFG_W0;
	if (halt)
		val |= DMAC_CHAN_CFG_HALT;
	else
		val &= ~DMAC_CHAN_CFG_HALT;
	writel_relaxed(val, reg);

out:
	spin_unlock_irqrestore(&chan->vchan.lock, flags);

	return ret;
}

static int histb_dma_device_pause(struct dma_chan *dmachan)
{
	return histb_dma_chan_set_halt(to_histb_dma_chan(dmachan), true);
}

static int histb_dma_device_resume(struct dma_chan *dmachan)
{
	return histb_dma_chan_set_halt(to_histb_dma_chan(dmachan), false);
}

static enum dma_status
histb_dma_tx_status(struct dma_chan *dmachan, dma_cookie_t cookie,
		    struct dma_tx_state *txstate)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	struct histb_dma_desc *desc;
	struct virt_dma_desc *vd;
	enum dma_status status;
	unsigned long flags;
	u32 val;

	status = dma_cookie_status(dmachan, cookie, txstate);
	if (status == DMA_COMPLETE)
		return status;

	val = readl_relaxed(chan->base + DMAC_CHAN_CFG(chan->id));
	if ((val & (DMAC_CHAN_CFG_EN | DMAC_CHAN_CFG_HALT)) ==
	    (DMAC_CHAN_CFG_EN | DMAC_CHAN_CFG_HALT))
		return DMA_PAUSED;

	/*
	 * the controller gives no hint on which LLI node is being fetched,
	 * so an in-flight transfer is reported as not yet started
	 */
	spin_lock_irqsave(&chan->vchan.lock, flags);
	vd = vchan_find_desc(&chan->vchan, cookie);
	if (vd) {
		desc = to_histb_dma_desc(vd);
		if (txstate)
			dma_set_residue(txstate, desc->size);
	}
	spin_unlock_irqrestore(&chan->vchan.lock, flags);

	return status;
}

/*
 * Wake the controller up synchronously: alloc_chan_resources sleeps, so
 * the clock is guaranteed on before the first transfer is submitted.
 * An async resume here races with the channel start register writes
 * (they land on a dead bus and the transfer times out).
 */
static int histb_dma_pm_get(struct histb_dma_chan *chan)
{
	int ret;

	if (chan->rpm_active)
		return 0;

	ret = pm_runtime_resume_and_get(chan->dev);
	if (!ret)
		chan->rpm_active = true;

	return ret;
}

static void histb_dma_pm_put(struct histb_dma_chan *chan)
{
	if (chan->rpm_active) {
		chan->rpm_active = false;
		pm_runtime_mark_last_busy(chan->dev);
		pm_runtime_put_autosuspend(chan->dev);
	}
}

static int histb_dma_terminate_all(struct dma_chan *dmachan)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	LIST_HEAD(list);
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&chan->vchan.lock, flags);

	ret = histb_dma_chan_halt(chan);
	if (ret < 0) {
		spin_unlock_irqrestore(&chan->vchan.lock, flags);
		dev_err(chan->dev, "channel %d: cannot halt transfer\n",
			chan->id);
		return ret;
	}

	if (chan->desc) {
		vchan_terminate_vdesc(&chan->desc->vdesc);
		chan->desc = NULL;
	}

	vchan_get_all_descriptors(&chan->vchan, &list);

	spin_unlock_irqrestore(&chan->vchan.lock, flags);

	vchan_dma_desc_free_list(&chan->vchan, &list);

	return 0;
}

static void histb_dma_synchronize(struct dma_chan *dmachan)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);

	histb_dma_chan_wait_disable(chan);
	vchan_synchronize(&chan->vchan);
}

static void histb_dma_issue_pending(struct dma_chan *dmachan)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);

	unsigned long flags;

	spin_lock_irqsave(&chan->vchan.lock, flags);
	/*
	 *	chan->desc is NULL when the channel is available
	 */
	if (vchan_issue_pending(&chan->vchan) && !chan->desc) {
		struct virt_dma_desc *vnext = vchan_next_desc(&chan->vchan);

		chan->desc = to_histb_dma_desc(vnext);
		list_del(&vnext->node);
		histb_dma_chan_start(chan);
	}
	spin_unlock_irqrestore(&chan->vchan.lock, flags);
}

/******** dma_chan ********/

static int histb_dma_alloc_chan_resources(struct dma_chan *dmachan)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	int ret;

	/* keep the controller clocked while any channel is claimed */
	ret = histb_dma_pm_get(chan);
	if (ret)
		return ret;

	histb_dma_chan_enable(chan);
	return 0;
}

static void histb_dma_free_chan_resources(struct dma_chan *dmachan)
{
	struct histb_dma_chan *chan = to_histb_dma_chan(dmachan);
	unsigned long flags;

	/*
	 * If the channel refuses to stop, its descriptor items may still be
	 * fetched by the DMAC: freeing them (letting the pool reuse the
	 * memory) would feed the engine a corrupted chain, with runaway
	 * transfers scribbling over the register window.  Leak the
	 * descriptors and keep the clock on instead.
	 */
	if (histb_dma_chan_halt(chan)) {
		dev_err(chan->dev, "channel %d: wedged, leaking descriptors\n",
			chan->id);
		return;
	}

	/*
	 * The started descriptor is off every vchan list, so put it on the
	 * terminated list first: otherwise vchan_free_chan_resources()
	 * leaks it and leaves chan->desc dangling for the next claimant.
	 */
	spin_lock_irqsave(&chan->vchan.lock, flags);
	if (chan->desc) {
		vchan_terminate_vdesc(&chan->desc->vdesc);
		chan->desc = NULL;
	}
	spin_unlock_irqrestore(&chan->vchan.lock, flags);

	histb_dma_pm_put(chan);
	vchan_free_chan_resources(&chan->vchan);
}

static int
histb_dma_chan_init(struct histb_dma_chan *chan, struct histb_dma_priv *priv,
		    unsigned int id, struct dma_pool *pool,
		    struct dma_pool *item_pool)
{
	chan->base = priv->base;
	chan->id = id;
	chan->dev = priv->dev;
	chan->pool = pool;
	chan->item_pool = item_pool;

	chan->vchan.desc_free = histb_dma_vdesc_free;
	vchan_init(&chan->vchan, &priv->dmadev);

	return 0;
}

/******** irq ********/

/*
 * Complete the current descriptor and chain up
 * the next one, if any
 */
static void histb_dma_handle_chan(struct histb_dma_chan *chan)
{
	struct histb_dma_desc *old = chan->desc;
	struct virt_dma_desc *vnext;

	if (!old)
		return;

	vchan_cookie_complete(&old->vdesc);

	vnext = vchan_next_desc(&chan->vchan);
	if (!vnext) {		/* no more issued descriptors */
		chan->desc = NULL;
		return;
	}

	/* set new descriptor */
	chan->desc = to_histb_dma_desc(vnext);
	list_del(&vnext->node);

	histb_dma_chan_start(chan);	/* start the new descriptor */
}

static irqreturn_t histb_dma_handle(int irq, void *dev_id)
{
	struct histb_dma_priv *priv = dev_id;
	u32 stat, tc, err;

	stat = readl_relaxed(priv->base + DMAC_INT_STATUS);
	if (!stat)
		return IRQ_NONE;

	tc = readl_relaxed(priv->base + DMAC_INT_TC_STATUS);
	if (tc)
		writel_relaxed(tc, priv->base + DMAC_INT_TC_CLR);

	err = readl_relaxed(priv->base + DMAC_INT_ERR_STATUS);
	if (err)
		writel_relaxed(err, priv->base + DMAC_INT_ERR_CLR);

	for (unsigned int i = 0; i < DMAC_CHAN_NUM; i++) {
		if (err & BIT(i)) {
			struct histb_dma_chan *chan = priv->chans + i;
			unsigned long flags;

			dev_err(priv->dev, "channel %u: transfer error\n", i);

			spin_lock_irqsave(&chan->vchan.lock, flags);
			histb_dma_chan_halt(chan);
			if (chan->desc) {
				struct histb_dma_desc *desc = chan->desc;

				/*
				 * Stop synchronously (a live DMAC refetching
				 * freed LLI memory would scribble kernel data
				 * into the register window), then hand the
				 * failed result to the client through the
				 * completion callback instead of dropping it
				 * silently, so its wait unblocks.
				 */
				chan->desc = NULL;
				desc->vdesc.tx_result.result = DMA_TRANS_READ_FAILED;
				desc->vdesc.tx_result.residue = desc->size;
				vchan_cookie_complete(&desc->vdesc);
			}
			spin_unlock_irqrestore(&chan->vchan.lock, flags);
		} else if (tc & BIT(i)) {
			struct histb_dma_chan *chan = priv->chans + i;
			unsigned long flags;

			spin_lock_irqsave(&chan->vchan.lock, flags);
			histb_dma_handle_chan(chan);
			spin_unlock_irqrestore(&chan->vchan.lock, flags);
		}
	}

	return IRQ_HANDLED;
}

/******** device ********/

static int __maybe_unused histb_dma_suspend(struct device *dev)
{
	struct histb_dma_priv *priv = dev_get_drvdata(dev);
	unsigned long flags;
	int ret;

	/*
	 * Refuse to power down while a transfer is running: the runtime
	 * suspend path clears every channel's EN bit, which would leave an
	 * in-flight descriptor uncompleted and the client waiting forever.
	 */
	for (unsigned int i = 0; i < DMAC_CHAN_NUM; i++) {
		struct histb_dma_chan *chan = &priv->chans[i];

		spin_lock_irqsave(&chan->vchan.lock, flags);
		if (chan->desc || !list_empty(&chan->vchan.desc_issued)) {
			spin_unlock_irqrestore(&chan->vchan.lock, flags);
			return -EBUSY;
		}
		spin_unlock_irqrestore(&chan->vchan.lock, flags);
	}

	ret = pm_runtime_force_suspend(dev);
	if (ret)
		return ret;

	clk_bulk_unprepare(priv->clks_n, priv->clks);

	return 0;
}

static int __maybe_unused histb_dma_resume(struct device *dev)
{
	struct histb_dma_priv *priv = dev_get_drvdata(dev);

	return clk_bulk_prepare(priv->clks_n, priv->clks) ?:
	       pm_runtime_force_resume(dev);
}

static int __maybe_unused histb_dma_runtime_suspend(struct device *dev)
{
	struct histb_dma_priv *priv = dev_get_drvdata(dev);

	u32 val;

	for (unsigned int id = 0; id < DMAC_CHAN_NUM; id++) {
		val = readl_relaxed(priv->base + DMAC_CHAN_CFG(id));
		val &= ~DMAC_CHAN_CFG_EN;
		val &= ~DMAC_CHAN_CFG_W0;
		writel_relaxed(val, priv->base + DMAC_CHAN_CFG(id));
	}
	val = readl_relaxed(priv->base + DMAC_CFG);
	writel_relaxed(val & ~DMAC_CFG_EN, priv->base + DMAC_CFG);

	clk_bulk_disable(priv->clks_n, priv->clks);

	return 0;
}

static int __maybe_unused histb_dma_runtime_resume(struct device *dev)
{
	struct histb_dma_priv *priv = dev_get_drvdata(dev);

	u32 val;
	int ret;

	ret = clk_bulk_enable(priv->clks_n, priv->clks);
	if (ret)
		return ret;

	val = readl_relaxed(priv->base + DMAC_CFG);
	if (!(val & DMAC_CFG_EN)) {
		val |= DMAC_CFG_M2;
		val |= DMAC_CFG_M1;
		val |= DMAC_CFG_EN;
		writel_relaxed(val, priv->base + DMAC_CFG);
	}

	writel_relaxed(~0, priv->base + DMAC_INT_ERR_CLR);
	writel_relaxed(~0, priv->base + DMAC_INT_TC_CLR);

	for (unsigned int id = 0; id < DMAC_CHAN_NUM; id++) {
		val = readl_relaxed(priv->base + DMAC_CHAN_CFG(id));
		val &= ~DMAC_CHAN_CFG_EN;
		val &= ~DMAC_CHAN_CFG_W0;
		writel_relaxed(val, priv->base + DMAC_CHAN_CFG(id));
	}

	return 0;
}

static const struct dev_pm_ops histb_dma_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(histb_dma_suspend, histb_dma_resume)
	SET_RUNTIME_PM_OPS(histb_dma_runtime_suspend,
			   histb_dma_runtime_resume, NULL)
};

static void histb_dma_remove(struct platform_device *pdev)
{
	struct histb_dma_priv *priv = platform_get_drvdata(pdev);
	struct device *dev = priv->dev;

	of_dma_controller_free(dev->of_node);

	/*
	 * Take the controller down while it is still clocked: stop the
	 * interrupt source, unregister the engine and drain the completion
	 * tasklets before anything resets or gates the block.
	 */
	pm_runtime_get_sync(dev);
	devm_free_irq(dev, priv->irq, priv);
	dma_async_device_unregister(&priv->dmadev);
	for (unsigned int i = 0; i < DMAC_CHAN_NUM; i++)
		tasklet_kill(&priv->chans[i].vchan.task);

	pm_runtime_put(dev);
	pm_runtime_disable(dev);
	if (!pm_runtime_status_suspended(dev))
		histb_dma_runtime_suspend(dev);
	pm_runtime_set_suspended(dev);
	pm_runtime_dont_use_autosuspend(dev);

	clk_bulk_disable_unprepare(priv->clks_n, priv->clks);
	reset_control_assert(priv->rst);
}

static int histb_dma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	struct histb_dma_priv *priv;
	struct dma_device *dmadev;
	struct dma_pool *pool;
	struct dma_pool *item_pool;
	u32 val;
	int ret;

	/* acquire resources */
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* the registers and every LLI field are 32 bit, including under LPAE */
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	pool = dmam_pool_create("histb-dma-memset-src", dev, sizeof(int),
				sizeof(int), 0);
	if (!pool)
		return -ENOMEM;

	item_pool = dmam_pool_create("histb-dma-item", dev,
				     sizeof(struct histb_dma_item),
				     sizeof(struct histb_dma_item), 0);
	if (!item_pool)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	ret = devm_clk_bulk_get_all(dev, &priv->clks);
	if (ret < 0)
		return ret;
	priv->clks_n = ret;

	priv->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(priv->rst))
		return PTR_ERR(priv->rst);

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;
	priv->irq = ret;

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);

	/* bring up device */
	ret = reset_control_assert(priv->rst);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(priv->clks_n, priv->clks);
	if (ret)
		goto err_rst;
	ret = reset_control_deassert(priv->rst);
	if (ret)
		goto err_clk;

	val = readl_relaxed(priv->base + DMAC_CFG);
	if (!(val & DMAC_CFG_EN)) {
		writel_relaxed(val | DMAC_CFG_M2 | DMAC_CFG_M1 | DMAC_CFG_EN,
			       priv->base + DMAC_CFG);
		val = readl_relaxed(priv->base + DMAC_CFG);
		if (!(val & DMAC_CFG_EN)) {
			dev_err(dev, "cannot bring up device\n");
			ret = -ENODEV;
			goto err_clk;
		}
	}

	writel_relaxed(~0, priv->base + DMAC_INT_ERR_CLR);
	writel_relaxed(~0, priv->base + DMAC_INT_TC_CLR);

	/* register irq */
	ret = devm_request_irq(dev, priv->irq, histb_dma_handle,
			       IRQF_SHARED, pdev->name, priv);
	if (ret)
		goto err_clk;

	/* register dma engine */
	dmadev = &priv->dmadev;
	dma_cap_set(DMA_MEMCPY, dmadev->cap_mask);
	dma_cap_set(DMA_MEMSET, dmadev->cap_mask);
	dma_cap_set(DMA_SLAVE, dmadev->cap_mask);
	dmadev->copy_align = DMAENGINE_ALIGN_4_BYTES;
	dmadev->fill_align = DMAENGINE_ALIGN_4_BYTES;
	dmadev->dev = dev;
	dmadev->directions = BIT(DMA_MEM_TO_MEM) | BIT(DMA_MEM_TO_DEV) |
			     BIT(DMA_DEV_TO_MEM);
	dmadev->src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
				  BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
				  BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	dmadev->dst_addr_widths = dmadev->src_addr_widths;
	/* the controller gives no per-transfer progress readback */
	dmadev->residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;
	dmadev->max_burst = 256;
	dmadev->device_alloc_chan_resources = histb_dma_alloc_chan_resources;
	dmadev->device_free_chan_resources = histb_dma_free_chan_resources;
	dmadev->device_prep_dma_memcpy = histb_dma_prep_dma_memcpy;
	dmadev->device_prep_dma_memset = histb_dma_prep_dma_memset;
	dmadev->device_prep_dma_memset_sg = histb_dma_prep_dma_memset_sg;
	dmadev->device_prep_slave_sg = histb_dma_prep_slave_sg;
	dmadev->device_config = histb_dma_slave_config;
	dmadev->device_pause = histb_dma_device_pause;
	dmadev->device_resume = histb_dma_device_resume;
	dmadev->device_tx_status = histb_dma_tx_status;
	dmadev->device_terminate_all = histb_dma_terminate_all;
	dmadev->device_synchronize = histb_dma_synchronize;
	dmadev->device_issue_pending = histb_dma_issue_pending;
	INIT_LIST_HEAD(&dmadev->channels);

	for (unsigned int id = 0; id < DMAC_CHAN_NUM; id++)
		histb_dma_chan_init(&priv->chans[id], priv, id, pool,
				    item_pool);

	/* runtime PM has to be live before a client can claim a channel */
	pm_runtime_set_autosuspend_delay(dev, 500);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = dma_async_device_register(dmadev);
	if (ret)
		goto err_pm;

	ret = of_dma_controller_register(dev->of_node, histb_dma_of_xlate,
					 priv);
	if (ret)
		goto err_unregister;

	return 0;

err_unregister:
	dma_async_device_unregister(dmadev);
err_pm:
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
err_clk:
	clk_bulk_disable_unprepare(priv->clks_n, priv->clks);
err_rst:
	reset_control_assert(priv->rst);
	return ret;
}

static struct dma_chan *
histb_dma_of_xlate(struct of_phandle_args *spec, struct of_dma *ofdma)
{
	struct histb_dma_priv *priv = ofdma->of_dma_data;
	struct histb_dma_chan *chan;
	struct dma_chan *dmachan;

	if (spec->args_count != 1)
		return NULL;

	if (spec->args[0] > DMAC_CHAN_CFG_SRC_PERI >> 1)
		return NULL;

	dmachan = dma_get_any_slave_channel(&priv->dmadev);
	if (!dmachan)
		return NULL;

	chan = to_histb_dma_chan(dmachan);
	chan->req_line = spec->args[0];

	return dmachan;
}

static const struct of_device_id histb_dma_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-dmac", },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_dma_of_match);

static struct platform_driver histb_dma_driver = {
	.probe = histb_dma_probe,
	.remove = histb_dma_remove,
	.driver = {
		.name = "histb-dma",
		.of_match_table = histb_dma_of_match,
		.pm = &histb_dma_pm_ops,
	},
};

module_platform_driver(histb_dma_driver);

MODULE_DESCRIPTION("HiSilicon STB SoCs DMA Controller");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
