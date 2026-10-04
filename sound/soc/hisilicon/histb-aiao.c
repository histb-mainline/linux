// SPDX-License-Identifier: GPL-2.0
/*
 * Hi3798MV100 AIAO I2S transmit port.
 *
 * The TX port is its own bus master: it walks a ring buffer in DRAM
 * (TX_BUFF_SADDR/TX_BUFF_SIZE) and advances TX_BUFF_RPTR as it pulls samples,
 * so no SoC DMA channel is involved.  The producer side is TX_BUFF_WPTR,
 * written after the samples land in the buffer.
 *
 * Both the period boundary and the PCM position come from the tick below, not
 * from the hardware, and that matches how the vendor drives this port in
 * production: aiao_v1_1/Makefile:132-134 selects the hrtimer glue whenever ALSA
 * AO support is enabled, its callback (alsa/dsp_mix_v2/hiaiao_dma.c:226) calls
 * snd_pcm_period_elapsed() per tick, and its pointer callback returns the
 * counted position rather than a register (:246, :642-665).
 *
 * Neither hardware source works as a period boundary on its own here:
 *
 *  - TX_INT_TRANS_FINISH (U_TX_INT_ENA bit0, hal_aiao_priv.h:872-891) is
 *    supposed to assert once per TX_TRANS_SIZE bytes played, but with
 *    TX_TRANS_SIZE = one period it never reached the GIC on EC6108V9C: the port
 *    stopped after about 1.5 kB of play with tx_disable_done set while
 *    TX_INT_RAW, TX_INT_STATUS and the COM aggregation all read 0.  Shrinking
 *    TX_TRANS_SIZE to 64 bytes made the interrupt fire within the same second,
 *    and a sweep over TX_TRANS_SIZE showed the count rising only for the small
 *    values (0x20/0x40/0x100/0x400 fire, 0x800 and 0x1000 do not).
 *  - TX_BUFF_RPTR does track the audio timeline - it gains 192 kB/s and stays
 *    one to two periods behind TX_BUFF_WPTR, and TX_WS_CNT (0x2320) counts
 *    transmitted words at the same 48 kHz - but feeding a register position to
 *    the core while a timer announces the periods makes the core's
 *    interrupt-side expectation (one period per elapsed) disagree with it, and
 *    the stream then stalls with a write timeout.
 *
 * Port 3 is the one whose lanes reach the on-chip SiI9134 HDMI transmitter
 * (SDK ao/drv_ao_op.c:64-70 resolves HDMI0 + I2S to AIAO_PORT_TX3 in this
 * package).  Register layout, field encodings and the clock divider follow
 * msp/drv/aiao/aiao_v1_1 in the vendor SDK.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <sound/memalloc.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#define AIAO_SWITCH_TX_BCLK		0x002c
#define  SWITCH_TX_BCLK_SEL(n)		(0xfU << ((n) * 4))
/* The RX clock generators are sources 0..7, the TX ones 8..15. */
#define AIAO_TX_CRG_SRC(n)		(8 + (n))

#define AIAO_STATUS			0x0030
#define  AIAO_SRST_RDY			BIT(0)

#define AIAO_TX_CRG(n)			(0x0140 + (n) * 8)
#define  AIAO_CRG_MCLK			0x00
#define   CRG_MCLK_DIV			GENMASK(26, 0)
#define  AIAO_CRG_CTRL			0x04
#define   CRG_BCLK_DIV			GENMASK(3, 0)
#define   CRG_FSCLK_DIV			GENMASK(6, 4)
#define   CRG_CKEN			BIT(8)

#define AIAO_TX(n)			(0x2000 + (n) * 0x100)
#define  TX_IF_ATTRI			0x00
#define   TX_MODE			GENMASK(1, 0)
#define   TX_PRECISION			GENMASK(3, 2)
#define   TX_CH_NUM			GENMASK(5, 4)
#define   TX_SD_OFFSET			GENMASK(15, 8)
#define   TX_TRACKMODE			GENMASK(18, 16)
#define   TX_SD_SOURCE_SEL		GENMASK(23, 20)
#define  TX_DSP_CTRL			0x04
#define   TX_BYPASS_EN			BIT(27)
#define   TX_ENABLE			BIT(28)
#define   TX_DISABLE_DONE		BIT(29)
#define  TX_BUFF_SADDR			0x80
#define  TX_BUFF_SIZE			0x84
#define  TX_BUFF_WPTR			0x88
#define  TX_BUFF_RPTR			0x8c
#define  TX_BUFF_ALEMPTY_TH		0x90
#define  TX_TRANS_SIZE			0x94

/* The port's clock generator shares its index with the port. */
#define AIAO_HDMI_TX_PORT		3

/*
 * The port divides the AIAO PLL itself.  The SDK taps it at 491.52 MHz and
 * encodes mclk as a 27-bit fraction of half of that (g_u32MclkPLLTab under
 * AIAO_PLL_492MHZ, hal_aiao_priv.c:191-208), which reproduces the table's
 * 0x00333333 for 48 kHz and 0x002f0a3d for 44.1 kHz at 256*fs.
 */
#define AIAO_PLL_HZ			245760000
#define AIAO_MCLK_FS			256

/* bclk = 64 * fs for stereo I2S; 4 and 64 are the SDK's HDMI dividers. */
#define AIAO_BCLK_DIV_IDX		3	/* g_u16BclkDivTab index of 4 */
#define AIAO_FSCLK_DIV_IDX		3	/* g_u16FsDivTab index of 64 */

#define AIAO_BUFFER_ALIGN		128
#define AIAO_PERIOD_BYTES_MAX		4096
#define AIAO_BUFFER_BYTES		(8 * AIAO_PERIOD_BYTES_MAX)

struct histb_aiao {
	void __iomem *base;
	void __iomem *crg;
	void __iomem *tx;
	struct clk *clk;
	struct reset_control *rst;
	struct snd_pcm_substream *substream;
	struct hrtimer tick;
	ktime_t period;
	snd_pcm_uframes_t pos;
	bool running;
};

static void aiao_set_clock(struct histb_aiao *aiao, unsigned int rate)
{
	u64 mclk = (u64)rate * AIAO_MCLK_FS * BIT(26);

	writel(FIELD_GET(CRG_MCLK_DIV, DIV_ROUND_CLOSEST_ULL(mclk, AIAO_PLL_HZ)),
	       aiao->crg + AIAO_CRG_MCLK);
	writel(FIELD_PREP(CRG_BCLK_DIV, AIAO_BCLK_DIV_IDX) |
	       FIELD_PREP(CRG_FSCLK_DIV, AIAO_FSCLK_DIV_IDX) | CRG_CKEN,
	       aiao->crg + AIAO_CRG_CTRL);
}

static int aiao_hw_params(struct snd_pcm_substream *substream,
			  struct snd_pcm_hw_params *params,
			  struct snd_soc_dai *dai)
{
	struct histb_aiao *aiao = snd_soc_dai_get_drvdata(dai);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int period = params_period_bytes(params);
	u32 val;

	aiao->substream = substream;

	if (params_buffer_bytes(params) >= BIT(24))
		return -EINVAL;

	aiao->period = ns_to_ktime(div_u64((u64)params_period_size(params) *
					   NSEC_PER_SEC, params_rate(params)));

	/*
	 * Route this port's bit clock and word select to its own clock
	 * generator.  AIAO_HW_SetIfAttr() does this for the master leg too
	 * (hal_aiao_priv.c:1603-1607 -> AIAO_LOW_SetI2SDulicateClk,
	 * :2029-2040); without it the port is enabled and clocked but never
	 * advances TX_BUFF_RPTR.
	 */
	val = readl(aiao->base + AIAO_SWITCH_TX_BCLK);
	val &= ~SWITCH_TX_BCLK_SEL(AIAO_HDMI_TX_PORT);
	val |= FIELD_PREP(SWITCH_TX_BCLK_SEL(AIAO_HDMI_TX_PORT),
			  AIAO_TX_CRG_SRC(AIAO_HDMI_TX_PORT));
	writel(val, aiao->base + AIAO_SWITCH_TX_BCLK);

	/*
	 * I2S, 16-bit, two channels, one bit of delay before the first sample
	 * and the port's own serial-data lanes selected -- the SDK's HDMI
	 * default attribute (hal_aiao.c:1532-1567) with source and CRG resolved
	 * to TX3 (hal_aiao.c:1661-1666).
	 */
	writel(FIELD_PREP(TX_MODE, 0) | FIELD_PREP(TX_PRECISION, 1) |
	       FIELD_PREP(TX_CH_NUM, 1) | FIELD_PREP(TX_SD_OFFSET, 1) |
	       FIELD_PREP(TX_SD_SOURCE_SEL, AIAO_HDMI_TX_PORT),
	       aiao->tx + TX_IF_ATTRI);
	/* The port's mute/volume stage stays bypassed; bit0 there would silence. */
	writel(TX_BYPASS_EN, aiao->tx + TX_DSP_CTRL);

	writel(runtime->dma_addr, aiao->tx + TX_BUFF_SADDR);
	writel(params_buffer_bytes(params), aiao->tx + TX_BUFF_SIZE);
	writel(0, aiao->tx + TX_BUFF_WPTR);
	writel(0, aiao->tx + TX_BUFF_RPTR);
	/*
	 * The port clears TX_ENABLE and sets TX_DISABLE_DONE once the data still
	 * queued behind its prefetch pointer drops below this threshold, so it has
	 * to be an emptiness mark rather than a period: with the SDK's HDMI default
	 * of one period (hal_aiao_func.c:166) the port stopped with 2048 of 3072
	 * committed bytes still queued.
	 */
	writel(frames_to_bytes(runtime, 1), aiao->tx + TX_BUFF_ALEMPTY_TH);
	writel(period, aiao->tx + TX_TRANS_SIZE);

	aiao_set_clock(aiao, params_rate(params));

	return 0;
}

/*
 * Stopping must never wait for a running tick: .trigger holds the stream lock
 * and period_elapsed() takes that same lock, so the flag above ends the timer
 * and expiry happens in soft context - the pattern sound/drivers/dummy.c:385-423
 * uses for the same reason.
 */
static enum hrtimer_restart aiao_period_tick(struct hrtimer *timer)
{
	struct histb_aiao *aiao = container_of(timer, struct histb_aiao, tick);
	struct snd_pcm_substream *substream = aiao->substream;
	struct snd_pcm_runtime *runtime;

	if (!substream || !READ_ONCE(aiao->running))
		return HRTIMER_NORESTART;

	runtime = substream->runtime;
	aiao->pos += runtime->period_size;
	if (aiao->pos >= runtime->buffer_size)
		aiao->pos -= runtime->buffer_size;
	snd_pcm_period_elapsed(substream);

	/* period_elapsed() can have run .trigger(STOP) through the core. */
	if (!READ_ONCE(aiao->running))
		return HRTIMER_NORESTART;

	hrtimer_forward_now(timer, aiao->period);
	return HRTIMER_RESTART;
}

static int aiao_trigger(struct snd_pcm_substream *substream, int cmd,
			struct snd_soc_dai *dai)
{
	struct histb_aiao *aiao = snd_soc_dai_get_drvdata(dai);
	u32 val;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		WRITE_ONCE(aiao->running, true);
		hrtimer_start(&aiao->tick, aiao->period, HRTIMER_MODE_REL_SOFT);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(aiao->running, false);
		hrtimer_try_to_cancel(&aiao->tick);
		writel(TX_BYPASS_EN, aiao->tx + TX_DSP_CTRL);
		/*
		 * The port reports when its pipeline has really drained.  Wait for it
		 * without scheduling: .trigger runs with the stream lock held
		 * (measured on EC6108V9C: the sleeping poll_timeout made the timer
		 * softirq report "bad: scheduling from the idle thread!").
		 */
		ret = readl_relaxed_poll_timeout_atomic(aiao->tx + TX_DSP_CTRL, val,
							val & TX_DISABLE_DONE,
							10, 50000);
		if (ret)
			dev_warn(dai->dev, "tx port did not report idle (0x%08x)\n",
				 val);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t aiao_pointer(struct snd_soc_component *component,
				      struct snd_pcm_substream *substream)
{
	struct histb_aiao *aiao = snd_soc_component_get_drvdata(component);

	return aiao->pos;
}

/* The port makes its own master clock from the AIAO PLL, so accept and ignore. */
static int aiao_set_sysclk(struct snd_soc_dai *dai, int clk_id,
			   unsigned int freq, int dir)
{
	return 0;
}

static const struct snd_soc_dai_ops aiao_dai_ops = {
	.hw_params = aiao_hw_params,
	.trigger = aiao_trigger,
	.set_sysclk = aiao_set_sysclk,
};

/*
 * The port's own ring engine, not a SoC DMA channel, moves the samples, so the
 * buffer shape is declared here rather than by the dmaengine helper.
 */
static const struct snd_pcm_hardware aiao_pcm_hardware = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_PAUSE | SNDRV_PCM_INFO_RESUME,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rate_min = 8000,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = AIAO_BUFFER_BYTES,
	.period_bytes_min = 256,
	.period_bytes_max = AIAO_PERIOD_BYTES_MAX,
	.periods_min = 2,
	.periods_max = AIAO_BUFFER_BYTES / AIAO_PERIOD_BYTES_MAX,
	.fifo_size = AIAO_PERIOD_BYTES_MAX,
};

static int aiao_pcm_open(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream)
{
	substream->runtime->hw = aiao_pcm_hardware;
	return 0;
}

/*
 * TX_BUFF_WPTR is the byte offset of the end of valid data in the ring: the port
 * plays up to it and clears tx_enable by itself as soon as its play pointer
 * catches up (measured: after one period of silence ahead of the producer,
 * DSP_CTRL went from 0x18000000 to 0x28000000 with the play pointer parked at
 * 0x1000).  So every commit of copied data also kicks the port.
 */
static int aiao_pcm_ack(struct snd_soc_component *component,
			struct snd_pcm_substream *substream)
{
	struct histb_aiao *aiao = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int size = frames_to_bytes(runtime, runtime->buffer_size);
	unsigned int room = size - frames_to_bytes(runtime, 1);
	u32 rptr = readl(aiao->tx + TX_BUFF_RPTR) & GENMASK(23, 0);
	u32 wptr = frames_to_bytes(runtime,
				   runtime->control->appl_ptr % runtime->buffer_size);
	u32 ahead;

	/*
	 * The ring has no full/empty code of its own: equal pointers read as
	 * empty, so a producer that fills the whole buffer would publish offset
	 * 0 and the port would stop there.  Keep the last frame unannounced; the
	 * next commit picks it up once the port has moved.
	 */
	ahead = (wptr + size - rptr) % size;
	if (ahead > room) {
		ahead = room;
		writel((rptr + room) % size, aiao->tx + TX_BUFF_WPTR);
	} else {
		writel(wptr, aiao->tx + TX_BUFF_WPTR);
	}
	if (ahead)
		writel(TX_ENABLE | TX_BYPASS_EN, aiao->tx + TX_DSP_CTRL);

	return 0;
}

static int aiao_pcm_prepare(struct snd_soc_component *component,
			    struct snd_pcm_substream *substream)
{
	struct histb_aiao *aiao = snd_soc_component_get_drvdata(component);

	aiao->pos = 0;
	writel(0, aiao->tx + TX_BUFF_WPTR);
	writel(0, aiao->tx + TX_BUFF_RPTR);
	writel(TX_BYPASS_EN, aiao->tx + TX_DSP_CTRL);

	return 0;
}

static int aiao_pcm_new(struct snd_soc_component *component,
			struct snd_soc_pcm_runtime *rtd)
{
	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
				       rtd->card->snd_card->dev,
				       AIAO_BUFFER_BYTES, AIAO_BUFFER_BYTES);
	return 0;
}

static const struct snd_soc_component_driver aiao_component = {
	.open = aiao_pcm_open,
	.prepare = aiao_pcm_prepare,
	.ack = aiao_pcm_ack,
	.pointer = aiao_pointer,
	.pcm_new = aiao_pcm_new,
};

static struct snd_soc_dai_driver aiao_dai = {
	.name = "aiao-i2s",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &aiao_dai_ops,
};

static int histb_aiao_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_aiao *aiao;
	void __iomem *base;
	u32 val;
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	aiao = devm_kzalloc(dev, sizeof(*aiao), GFP_KERNEL);
	if (!aiao)
		return -ENOMEM;

	aiao->base = base;
	aiao->crg = base + AIAO_TX_CRG(AIAO_HDMI_TX_PORT);
	aiao->tx = base + AIAO_TX(AIAO_HDMI_TX_PORT);
	hrtimer_setup(&aiao->tick, aiao_period_tick, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);
	platform_set_drvdata(pdev, aiao);

	aiao->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(aiao->clk))
		return dev_err_probe(dev, PTR_ERR(aiao->clk), "no aiao clock\n");

	aiao->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(aiao->rst))
		return dev_err_probe(dev, PTR_ERR(aiao->rst), "bad reset\n");

	/*
	 * Drive the block through a reset of our own rather than accepting
	 * CRG70[4] as the boot left it, with the working clock already on so
	 * the pulse shifts through.  AIAO_STATUS.srst_rdy (bit0) follows the
	 * request, which is what the SDK polls in AIAO_HW_PowerOn()
	 * (hal_aiao_priv.c:620-684).
	 */
	ret = reset_control_assert(aiao->rst);
	if (ret)
		return dev_err_probe(dev, ret, "reset assert failed\n");

	if (readl_poll_timeout(aiao->base + AIAO_STATUS, val,
			       !(val & AIAO_SRST_RDY), 10, 1000))
		dev_warn(dev, "srst_rdy stayed high through the assert (%08x)\n",
			 val);
	else
		dev_dbg(dev, "reset asserted, status %08x\n", val);

	ret = reset_control_deassert(aiao->rst);
	if (ret)
		return dev_err_probe(dev, ret, "reset stays asserted\n");

	if (readl_poll_timeout(aiao->base + AIAO_STATUS, val,
			       val & AIAO_SRST_RDY, 10, 1000))
		dev_warn(dev, "srst_rdy stayed low after the release (%08x)\n",
			 val);
	else
		dev_dbg(dev, "reset released, status %08x\n", val);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "no 32-bit DMA\n");

	ret = devm_snd_soc_register_component(dev, &aiao_component, &aiao_dai, 1);
	if (ret)
		return dev_err_probe(dev, ret, "could not register the DAI\n");

	return 0;
}

static void histb_aiao_remove(struct platform_device *pdev)
{
	struct histb_aiao *aiao = platform_get_drvdata(pdev);

	WRITE_ONCE(aiao->running, false);
	hrtimer_cancel(&aiao->tick);
	writel(0, aiao->tx + TX_DSP_CTRL);
	writel(0, aiao->crg + AIAO_CRG_CTRL);
	reset_control_assert(aiao->rst);
}

static const struct of_device_id histb_aiao_of_match[] = {
	{ .compatible = "hisilicon,histb-aiao-i2s" },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_aiao_of_match);

static struct platform_driver histb_aiao_driver = {
	.probe = histb_aiao_probe,
	.remove = histb_aiao_remove,
	.driver = {
		.name = "histb-aiao-i2s",
		.of_match_table = histb_aiao_of_match,
	},
};
module_platform_driver(histb_aiao_driver);

MODULE_DESCRIPTION("Hi3798MV100 AIAO I2S transmit port");
MODULE_LICENSE("GPL");
