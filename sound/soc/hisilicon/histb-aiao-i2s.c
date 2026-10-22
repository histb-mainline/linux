// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * HiSTB AIAO I2S transmit and receive ports.
 *
 * The TX port is its own bus master: it walks a ring buffer in DRAM
 * (TX_BUFF_SADDR/TX_BUFF_SIZE) and advances TX_BUFF_RPTR as it pulls samples,
 * so no SoC DMA channel is involved. The producer side is TX_BUFF_WPTR, written
 * after the samples land in the buffer.
 *
 * The RX port is the same engine turned around: it stores samples into its ring
 * and advances RX_BUFF_WPTR as it goes, while RX_BUFF_RPTR is the reader mark
 * the application commits. The port freezes instead of wrapping when the ring
 * fills, and resumes when that mark moves.
 *
 * Both the period boundary and the PCM position come from the tick below, not
 * from the hardware. Neither hardware source works as a period boundary on its
 * own:
 *
 *  - the transfer-finish interrupt is meant to assert once the port has
 *    played the number of bytes in its transfer-size threshold, but with
 *    that threshold set to one period it never reaches the GIC: the port
 *    stops after about 1.5 kB with TX_DISABLE_DONE set while the raw and
 *    aggregated interrupt status both read 0. It does fire for small
 *    thresholds, and not from 0x800 bytes upwards, so a period-sized
 *    transfer cannot be the tick source.
 *  - TX_BUFF_RPTR does track the audio timeline: it gains 192 kB/s and
 *    stays one to two periods behind TX_BUFF_WPTR, and the word counter at
 *    0x2320 counts transmitted words at the same 48 kHz. But feeding a
 *    register position to the core while a timer announces the periods
 *    makes the core's interrupt-side expectation (one period per elapsed)
 *    disagree with it, and the stream stalls with a write timeout.
 *  - on the receive side neither RX_WS_CNT nor RX_BCLK_CNT reports the
 *    clock the port is sampling with: they stay at zero while the ring
 *    fills at full rate.
 *
 * Port 3 is the one whose lanes reach the on-chip SiI9134 HDMI transmitter. The
 * register layout, field encodings and the clock divider are those of the AIAO
 * block as this SoC wires it.
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

#define AIAO_SWITCH_RX_BCLK		0x0028
#define AIAO_SWITCH_TX_BCLK		0x002c
#define  SWITCH_BCLK_SEL(n)		(0xfU << ((n) * 4))
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

#define AIAO_RX(n)			(0x1000 + (n) * 0x100)
#define  RX_IF_ATTRI			0x00
#define   RX_MODE			GENMASK(1, 0)
#define   RX_PRECISION			GENMASK(3, 2)
#define   RX_CH_NUM			GENMASK(5, 4)
#define   RX_SD_OFFSET			GENMASK(15, 8)
#define   RX_SD_SOURCE_SEL		GENMASK(23, 20)
#define  RX_DSP_CTRL			0x04
#define   RX_BYPASS_EN			BIT(27)
#define   RX_ENABLE			BIT(28)
#define   RX_DISABLE_DONE		BIT(29)
#define  RX_BUFF_SADDR			0x80
#define  RX_BUFF_SIZE			0x84
#define  RX_BUFF_WPTR			0x88
#define  RX_BUFF_RPTR			0x8c
#define  RX_BUFF_ALFULL_TH		0x90
#define  RX_TRANS_SIZE			0x94

#define AIAO_TX(n)			(0x2000 + (n) * 0x100)
#define  TX_IF_ATTRI			0x00
#define   TX_MODE			GENMASK(1, 0)
#define   TX_PRECISION			GENMASK(3, 2)
#define   TX_CH_NUM			GENMASK(5, 4)
#define   TX_SD_OFFSET			GENMASK(15, 8)
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

/*
 * The port divides the AIAO PLL itself. The PLL runs at 491.52 MHz and mclk is
 * encoded as a 27-bit fraction of half of that, which gives 0x00333333 for 48
 * kHz and 0x002f0a3d for 44.1 kHz at 256*fs.
 */
#define AIAO_PLL_HZ			245760000
#define AIAO_MCLK_FS			256

/* bclk = 64 * fs for stereo I2S; both fields are table indices */
#define AIAO_BCLK_DIV_IDX		3	/* index 3 of the bclk divider table selects 4 */
#define AIAO_FSCLK_DIV_IDX		3	/* index 3 of the fs divider table selects 64 */

#define AIAO_PERIOD_BYTES_MAX		4096
#define AIAO_BUFFER_BYTES		(8 * AIAO_PERIOD_BYTES_MAX)

struct histb_aiao_port {
	void __iomem *regs;
	void __iomem *crg;
	unsigned int id;	/* index within its own RX or TX band */
	unsigned int tap;	/* receiver: TX band index it samples */
	unsigned int partner;	/* index into histb_aiao::port, or HISTB_AIAO_NO_PARTNER */
	bool rx;		/* this port lives in the receive band */
	struct snd_pcm_substream *substream;
	struct hrtimer tick;
	ktime_t period;
	snd_pcm_uframes_t pos;
	unsigned int rate;
	bool running;
	bool announced;
};

/*
 * The port numbers are not free choice: they are what the block is wired to.
 * The receive port taps the serial stream the HDMI port puts on the wire, so
 * the two share the transmit port's clock generator: capture runs at the rate
 * the tapped port plays at. The internal DAC listens to the serial lane of
 * transmit port 2 -- the vendor mapping is DAC0 -> AIAO_PORT_TX2 in
 * aiao_v1_1/ao/drv_ao_op.c:33-50, and ADAC0/ADAC1 have no source-select bit of
 * their own, so that one is hard-wired -- and its own generator is index 2.
 */
#define AIAO_HDMI_TX_PORT		3
#define AIAO_HDMI_RX_PORT		0
#define AIAO_DAC_TX_PORT		2
#define AIAO_DAC_RX_PORT		1

enum {
	HISTB_AIAO_HDMI_TX,
	HISTB_AIAO_HDMI_RX,
	HISTB_AIAO_DAC_TX,
	HISTB_AIAO_DAC_RX,
	HISTB_AIAO_NR_PORTS,
};

#define HISTB_AIAO_NO_PARTNER		HISTB_AIAO_NR_PORTS

struct histb_aiao {
	void __iomem *base;
	struct clk *clk;
	struct reset_control *rst;
	struct histb_aiao_port port[HISTB_AIAO_NR_PORTS];
};

/*
 * A DAI is one port: the DAI index registered in histb_aiao_dais[] is the index
 * into histb_aiao::port, in the same order. Selection has to be by DAI, not by
 * stream, now that the block drives more than one transmit port.
 */
static struct histb_aiao_port *histb_aiao_substream_port(struct histb_aiao *aiao,
					 struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);

	return &aiao->port[snd_soc_rtd_to_cpu(rtd, 0)->id];
}

static struct histb_aiao_port *histb_aiao_other(struct histb_aiao *aiao,
					 struct histb_aiao_port *port)
{
	if (port->partner == HISTB_AIAO_NO_PARTNER)
		return NULL;

	return &aiao->port[port->partner];
}

static void histb_aiao_set_clock(struct histb_aiao_port *port, unsigned int rate)
{
	u64 mclk = (u64)rate * AIAO_MCLK_FS * BIT(26);

	writel(FIELD_GET(CRG_MCLK_DIV, DIV_ROUND_CLOSEST_ULL(mclk, AIAO_PLL_HZ)),
	       port->crg + AIAO_CRG_MCLK);
	writel(FIELD_PREP(CRG_BCLK_DIV, AIAO_BCLK_DIV_IDX) |
	       FIELD_PREP(CRG_FSCLK_DIV, AIAO_FSCLK_DIV_IDX) | CRG_CKEN,
	       port->crg + AIAO_CRG_CTRL);
}

/*
 * One nibble per port in the two switch registers, so the mask only exists once
 * the port is known at run time -- which rules out FIELD_PREP() (it requires a
 * build-time-constant mask) and this RMW is the replacement.
 */
static void histb_aiao_set_bclk_source(struct histb_aiao *aiao, unsigned int reg,
				       unsigned int port, unsigned int src)
{
	u32 mask = SWITCH_BCLK_SEL(port);
	u32 val = readl(aiao->base + reg);

	val = (val & ~mask) | ((src << (port * 4)) & mask);
	writel(val, aiao->base + reg);
}

static int histb_aiao_tx_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct histb_aiao *aiao,
				   struct histb_aiao_port *port)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int period = params_period_bytes(params);

	/*
	 * Route this port's bit clock and frame sync to its own clock
	 * generator; without that the port is enabled and clocked but never
	 * advances TX_BUFF_RPTR.
	 */
	histb_aiao_set_bclk_source(aiao, AIAO_SWITCH_TX_BCLK, port->id,
				   AIAO_TX_CRG_SRC(port->id));

	/*
	 * I2S, 16-bit, two channels, one bit of delay before the first sample,
	 * and the port's own serial-data lanes selected.
	 */
	writel(FIELD_PREP(TX_MODE, 0) | FIELD_PREP(TX_PRECISION, 1) |
	       FIELD_PREP(TX_CH_NUM, 1) | FIELD_PREP(TX_SD_OFFSET, 1) |
	       FIELD_PREP(TX_SD_SOURCE_SEL, port->id),
	       port->regs + TX_IF_ATTRI);
	/* The port's mute/volume stage stays bypassed; bit0 there would silence. */
	writel(TX_BYPASS_EN, port->regs + TX_DSP_CTRL);

	writel(runtime->dma_addr, port->regs + TX_BUFF_SADDR);
	writel(params_buffer_bytes(params), port->regs + TX_BUFF_SIZE);
	writel(0, port->regs + TX_BUFF_WPTR);
	writel(0, port->regs + TX_BUFF_RPTR);
	/*
	 * The port clears TX_ENABLE and sets TX_DISABLE_DONE once the data
	 * still queued behind its prefetch pointer drops below this threshold,
	 * so it has to be an emptiness mark rather than a period: with a
	 * period-sized threshold the port stops with 2048 of 3072 committed
	 * bytes still queued.
	 */
	writel(frames_to_bytes(runtime, 1), port->regs + TX_BUFF_ALEMPTY_TH);
	writel(period, port->regs + TX_TRANS_SIZE);

	return 0;
}

static int histb_aiao_rx_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct histb_aiao *aiao,
				   struct histb_aiao_port *port)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned int size = params_buffer_bytes(params);
	unsigned int period = params_period_bytes(params);

	/*
	 * Sample the tapped stream with the generator that produced it, so that
	 * receiver and transmitter share one clock domain: the captured frames
	 * then land where the transmitter put them.
	 */
	histb_aiao_set_bclk_source(aiao, AIAO_SWITCH_RX_BCLK, port->id,
				   AIAO_TX_CRG_SRC(port->tap));

	writel(FIELD_PREP(RX_MODE, 0) | FIELD_PREP(RX_PRECISION, 1) |
	       FIELD_PREP(RX_CH_NUM, 1) | FIELD_PREP(RX_SD_OFFSET, 1) |
	       FIELD_PREP(RX_SD_SOURCE_SEL, port->tap),
	       port->regs + RX_IF_ATTRI);
	writel(RX_BYPASS_EN, port->regs + RX_DSP_CTRL);

	writel(runtime->dma_addr, port->regs + RX_BUFF_SADDR);
	writel(size, port->regs + RX_BUFF_SIZE);
	writel(0, port->regs + RX_BUFF_WPTR);
	writel(0, port->regs + RX_BUFF_RPTR);
	writel(size - period, port->regs + RX_BUFF_ALFULL_TH);
	writel(period, port->regs + RX_TRANS_SIZE);

	return 0;
}

static int histb_aiao_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct histb_aiao *aiao = snd_soc_dai_get_drvdata(dai);
	struct histb_aiao_port *port = histb_aiao_substream_port(aiao, substream);
	struct histb_aiao_port *other = histb_aiao_other(aiao, port);
	unsigned int rate = params_rate(params);
	int ret;

	port->substream = substream;

	if (params_buffer_bytes(params) >= BIT(24))
		return -EINVAL;

	/* One generator feeds both directions, so they cannot run apart. */
	if (other && other->rate && other->rate != rate)
		return -EINVAL;
	port->rate = rate;

	port->period = ns_to_ktime(div_u64((u64)params_period_size(params) *
					   NSEC_PER_SEC, rate));

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		ret = histb_aiao_rx_hw_params(substream, params, aiao, port);
	else
		ret = histb_aiao_tx_hw_params(substream, params, aiao, port);
	if (ret)
		return ret;

	histb_aiao_set_clock(port, rate);

	return 0;
}

static int histb_aiao_hw_free(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct histb_aiao *aiao = snd_soc_dai_get_drvdata(dai);
	struct histb_aiao_port *port = histb_aiao_substream_port(aiao, substream);

	port->substream = NULL;
	port->rate = 0;

	return 0;
}

/*
 * TX_BUFF_WPTR is the byte offset of the end of valid data in the ring: the
 * port plays up to it, and the moment its play pointer catches up it clears
 * TX_ENABLE by itself and ignores further TX_ENABLE writes until both pointers
 * are re-primed. So the announced region must never be empty while a stream
 * runs: the core waits until the application is a whole buffer behind before it
 * calls that an underrun, and the ring runs dry long before, silently, with the
 * position kept on by our own tick.
 *
 * Commit the application pointer when it is that far ahead of the port, and
 * otherwise keep two periods in front of it. Past the application pointer the
 * ring holds what any other ring-buffer player would play there; the port stays
 * awake and the tick keeps committing, which is what an application that
 * stopped writing does not do.
 */
static void histb_aiao_tx_commit(struct histb_aiao_port *port,
				 struct snd_pcm_runtime *runtime)
{
	unsigned int size = frames_to_bytes(runtime, runtime->buffer_size);
	unsigned int room = size - frames_to_bytes(runtime, 1);
	unsigned int keep = frames_to_bytes(runtime, 2 * runtime->period_size);
	u32 rptr = readl(port->regs + TX_BUFF_RPTR) & GENMASK(23, 0);
	u32 ahead;

	ahead = frames_to_bytes(runtime,
				runtime->control->appl_ptr % runtime->buffer_size);
	ahead = (ahead + size - rptr) % size;
	if (ahead < keep)
		ahead = keep;
	if (ahead > room)
		ahead = room;

	/*
	 * The ring has no full/empty code of its own: equal pointers read as
	 * empty, so a producer that fills the whole buffer would publish offset
	 * 0 and the port would stop there. Keep the last frame unannounced; the
	 * next commit picks it up once the port has moved.
	 */
	writel((rptr + ahead) % size, port->regs + TX_BUFF_WPTR);
	writel(TX_ENABLE | TX_BYPASS_EN, port->regs + TX_DSP_CTRL);
	WRITE_ONCE(port->announced, true);
}

/*
 * Stopping must never wait for a running tick: .trigger holds the stream lock
 * and period_elapsed() takes that same lock, so the flag above ends the timer
 * and expiry happens in soft context - the pattern sound/drivers/dummy.c uses
 * for the same reason.
 */
static enum hrtimer_restart histb_aiao_period_tick(struct hrtimer *timer)
{
	struct histb_aiao_port *port = container_of(timer, struct histb_aiao_port, tick);
	struct snd_pcm_substream *substream = port->substream;
	struct snd_pcm_runtime *runtime;

	if (!substream || !READ_ONCE(port->running))
		return HRTIMER_NORESTART;

	runtime = substream->runtime;
	port->pos += runtime->period_size;
	if (port->pos >= runtime->buffer_size)
		port->pos -= runtime->buffer_size;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (READ_ONCE(port->announced) &&
		    !(readl(port->regs + TX_DSP_CTRL) & TX_ENABLE)) {
			/*
			 * The commit below keeps the ring announced even when
			 * the application stops writing, so getting here means
			 * the tick itself starved for longer than the ring can
			 * hold. The port latched off and will not take
			 * TX_ENABLE again; report the underrun, because the
			 * position is ours and nothing else here would ever
			 * tell the application that the stream went quiet.
			 */
			snd_pcm_stop_xrun(substream);
			return HRTIMER_NORESTART;
		}
		histb_aiao_tx_commit(port, runtime);
	}
	snd_pcm_period_elapsed(substream);

	/* period_elapsed() can have run .trigger(STOP) through the core. */
	if (!READ_ONCE(port->running))
		return HRTIMER_NORESTART;

	hrtimer_forward_now(timer, port->period);
	return HRTIMER_RESTART;
}

static int histb_aiao_trigger(struct snd_pcm_substream *substream, int cmd,
			      struct snd_soc_dai *dai)
{
	struct histb_aiao *aiao = snd_soc_dai_get_drvdata(dai);
	struct histb_aiao_port *port = histb_aiao_substream_port(aiao, substream);
	bool capture = substream->stream == SNDRV_PCM_STREAM_CAPTURE;
	void __iomem *dsp = port->regs + (capture ? RX_DSP_CTRL : TX_DSP_CTRL);
	u32 bypass = capture ? RX_BYPASS_EN : TX_BYPASS_EN;
	u32 enable = capture ? RX_ENABLE : TX_ENABLE;
	u32 done = capture ? RX_DISABLE_DONE : TX_DISABLE_DONE;
	u32 val;
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		/*
		 * The receiver stores as soon as it is started, unlike the
		 * transmitter which only kicks when data is committed to it:
		 * there is nothing to commit here, the samples arrive on the
		 * wire. The tick counts periods from this trigger, so leaving
		 * the port disabled until the first read would offset the ring
		 * by a period.
		 */
		if (capture)
			writel(enable | bypass, dsp);
		WRITE_ONCE(port->announced, false);
		WRITE_ONCE(port->running, true);
		hrtimer_start(&port->tick, port->period, HRTIMER_MODE_REL_SOFT);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(port->running, false);
		hrtimer_try_to_cancel(&port->tick);
		writel(bypass, dsp);
		/*
		 * The port reports when its pipeline has really drained. Wait
		 * for it without scheduling: .trigger runs with the stream lock
		 * held, and a sleeping poll there makes the timer softirq
		 * report "bad: scheduling from the idle thread!".
		 */
		ret = readl_relaxed_poll_timeout_atomic(dsp, val, val & done,
							10, 50000);
		if (ret)
			dev_warn(dai->dev, "%s port did not report idle (0x%08x)\n",
				 capture ? "rx" : "tx", val);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t histb_aiao_pointer(struct snd_soc_component *component,
					    struct snd_pcm_substream *substream)
{
	struct histb_aiao *aiao = snd_soc_component_get_drvdata(component);

	return histb_aiao_substream_port(aiao, substream)->pos;
}

/*
 * The port makes its own master clock from the AIAO PLL, so accept and ignore.
 */
static int histb_aiao_set_sysclk(struct snd_soc_dai *dai, int clk_id,
				 unsigned int freq, int dir)
{
	return 0;
}

static const struct snd_soc_dai_ops histb_aiao_dai_ops = {
	.hw_params = histb_aiao_hw_params,
	.hw_free = histb_aiao_hw_free,
	.trigger = histb_aiao_trigger,
	.set_sysclk = histb_aiao_set_sysclk,
};

/*
 * The port's own ring engine, not a SoC DMA channel, moves the samples, so the
 * buffer shape is declared here rather than by the dmaengine helper.
 */
static const struct snd_pcm_hardware histb_aiao_pcm_hardware = {
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

static int histb_aiao_pcm_open(struct snd_soc_component *component,
			       struct snd_pcm_substream *substream)
{
	substream->runtime->hw = histb_aiao_pcm_hardware;
	return 0;
}

static int histb_aiao_pcm_tx_ack(struct histb_aiao *aiao,
				 struct snd_pcm_substream *substream)
{
	histb_aiao_tx_commit(histb_aiao_substream_port(aiao, substream),
			     substream->runtime);

	return 0;
}

/*
 * RX_BUFF_RPTR is the reader mark: the port stores up to RX_BUFF_WPTR and stops
 * once the ring is full, so every report of consumed data has to commit it. A
 * reader that falls behind loses the samples the port could not store, and the
 * port restarts by itself as the mark catches up.
 */
static int histb_aiao_pcm_rx_ack(struct histb_aiao *aiao,
				 struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct histb_aiao_port *port = histb_aiao_substream_port(aiao, substream);
	u32 rptr = frames_to_bytes(runtime,
				   runtime->control->appl_ptr % runtime->buffer_size);

	writel(rptr, port->regs + RX_BUFF_RPTR);
	writel(RX_ENABLE | RX_BYPASS_EN, port->regs + RX_DSP_CTRL);

	return 0;
}

static int histb_aiao_pcm_ack(struct snd_soc_component *component,
			      struct snd_pcm_substream *substream)
{
	struct histb_aiao *aiao = snd_soc_component_get_drvdata(component);

	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		return histb_aiao_pcm_rx_ack(aiao, substream);

	return histb_aiao_pcm_tx_ack(aiao, substream);
}

static int histb_aiao_pcm_prepare(struct snd_soc_component *component,
				  struct snd_pcm_substream *substream)
{
	struct histb_aiao *aiao = snd_soc_component_get_drvdata(component);
	struct histb_aiao_port *port = histb_aiao_substream_port(aiao, substream);
	bool capture = substream->stream == SNDRV_PCM_STREAM_CAPTURE;
	unsigned int off = capture ? RX_BUFF_RPTR : TX_BUFF_RPTR;
	unsigned int woff = capture ? RX_BUFF_WPTR : TX_BUFF_WPTR;
	unsigned int dsp = capture ? RX_DSP_CTRL : TX_DSP_CTRL;
	u32 bypass = capture ? RX_BYPASS_EN : TX_BYPASS_EN;

	port->pos = 0;
	writel(0, port->regs + woff);
	writel(0, port->regs + off);
	writel(bypass, port->regs + dsp);

	return 0;
}

static int histb_aiao_pcm_new(struct snd_soc_component *component,
			      struct snd_soc_pcm_runtime *rtd)
{
	snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
				       rtd->card->snd_card->dev,
				       AIAO_BUFFER_BYTES, AIAO_BUFFER_BYTES);
	return 0;
}

static const struct snd_soc_component_driver histb_aiao_component = {
	.open = histb_aiao_pcm_open,
	.prepare = histb_aiao_pcm_prepare,
	.ack = histb_aiao_pcm_ack,
	.pointer = histb_aiao_pointer,
	.pcm_new = histb_aiao_pcm_new,
};

/*
 * The DAI index is the index into histb_aiao::port, so the two tables have to
 * stay in the order of the enum. The first entry keeps .id unset: the SoC core
 * then assigns the registration counter, which is 0 here anyway.
 */
static struct snd_soc_dai_driver histb_aiao_dais[] = {
	{
		.name = "aiao-i2s",
		.playback = {
			.stream_name = "Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &histb_aiao_dai_ops,
	},
	{
		.id = HISTB_AIAO_HDMI_RX,
		.name = "aiao-i2s-in",
		.capture = {
			.stream_name = "Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &histb_aiao_dai_ops,
	},
	{
		.id = HISTB_AIAO_DAC_TX,
		.name = "aiao-i2s-dac",
		.playback = {
			.stream_name = "DAC Playback",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &histb_aiao_dai_ops,
	},
	{
		.id = HISTB_AIAO_DAC_RX,
		.name = "aiao-i2s-dac-in",
		.capture = {
			.stream_name = "DAC Capture",
			.channels_min = 2,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.ops = &histb_aiao_dai_ops,
	},
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
	/*
	 * Which port is which. A receive port always runs off the generator of
	 * the transmit port it samples, so its crg points at that one.
	 */
	static const struct {
		unsigned int band;
		unsigned int tap;
		unsigned int partner;
		bool rx;
	} specs[HISTB_AIAO_NR_PORTS] = {
		[HISTB_AIAO_HDMI_TX] = { AIAO_HDMI_TX_PORT, AIAO_HDMI_TX_PORT,
					 HISTB_AIAO_HDMI_RX, false },
		[HISTB_AIAO_HDMI_RX] = { AIAO_HDMI_RX_PORT, AIAO_HDMI_TX_PORT,
					 HISTB_AIAO_HDMI_TX, true },
		[HISTB_AIAO_DAC_TX] = { AIAO_DAC_TX_PORT, AIAO_DAC_TX_PORT,
					HISTB_AIAO_DAC_RX, false },
		[HISTB_AIAO_DAC_RX] = { AIAO_DAC_RX_PORT, AIAO_DAC_TX_PORT,
					HISTB_AIAO_DAC_TX, true },
	};
	unsigned int i;

	for (i = 0; i < HISTB_AIAO_NR_PORTS; i++) {
		struct histb_aiao_port *port = &aiao->port[i];

		port->id = specs[i].band;
		port->tap = specs[i].tap;
		port->partner = specs[i].partner;
		port->rx = specs[i].rx;
		port->regs = base + (specs[i].rx ? AIAO_RX(port->id) :
						     AIAO_TX(port->id));
		port->crg = base + AIAO_TX_CRG(specs[i].rx ? port->tap :
						 port->id);
		hrtimer_setup(&port->tick, histb_aiao_period_tick, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL_SOFT);
	}
	platform_set_drvdata(pdev, aiao);

	aiao->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(aiao->clk))
		return dev_err_probe(dev, PTR_ERR(aiao->clk), "no aiao clock\n");

	aiao->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(aiao->rst))
		return dev_err_probe(dev, PTR_ERR(aiao->rst), "bad reset\n");

	/*
	 * Drive the block through a reset of our own rather than accepting
	 * CRG70[4] as it was left, with the working clock already on so the
	 * pulse shifts through. AIAO_STATUS.srst_rdy (bit0) follows the
	 * request, so it is the readback that proves the reset landed.
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

	ret = devm_snd_soc_register_component(dev, &histb_aiao_component,
					      histb_aiao_dais,
					      ARRAY_SIZE(histb_aiao_dais));
	if (ret)
		return dev_err_probe(dev, ret, "could not register the DAIs\n");

	return 0;
}

static void histb_aiao_remove(struct platform_device *pdev)
{
	struct histb_aiao *aiao = platform_get_drvdata(pdev);

	unsigned int i;

	for (i = 0; i < HISTB_AIAO_NR_PORTS; i++) {
		struct histb_aiao_port *port = &aiao->port[i];

		WRITE_ONCE(port->running, false);
		hrtimer_cancel(&port->tick);
		writel(0, port->regs + (port->rx ? RX_DSP_CTRL : TX_DSP_CTRL));
	}

	/* One generator per transmit port; the receivers share their tap's. */
	for (i = 0; i < HISTB_AIAO_NR_PORTS; i++)
		if (!aiao->port[i].rx)
			writel(0, aiao->port[i].crg + AIAO_CRG_CTRL);

	reset_control_assert(aiao->rst);
}

static const struct of_device_id histb_aiao_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-aiao-i2s" },
	{ .compatible = "hisilicon,hi3716cv200-aiao-i2s" },
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

MODULE_DESCRIPTION("HiSTB AIAO I2S transmit and receive ports");
MODULE_LICENSE("GPL");
