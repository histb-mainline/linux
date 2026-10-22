// SPDX-License-Identifier: GPL-2.0-only
/*
 * HiSTB TianLai internal audio DAC (ADAC).
 *
 * The analog output stage of the SoC: it takes the I2S stream that an AIAO
 * transmit port puts on its serial lane and drives a speaker/line output. The
 * lane is not selectable -- ADAC0/ADAC1 have no source-select bit, and the
 * vendor port map is DAC0 -> AIAO transmit port 2
 * (aiao_v1_1/ao/drv_ao_op.c:33-50), so the machine driver has to pair this
 * codec with that port.
 *
 * What the driver touches is deliberately the vendor's own set. The live V500
 * HAL (hi3716cv200 builds with it -- aiao_v1_1/Makefile:116) writes the
 * popfree bits, the two power-down bits, the two mute bits and the two volume
 * fields of ADAC0, and the soft-mute pair plus sample_sel of ADAC1, and
 * nothing else: clkdgesel, clksel2, adj_refbf, adj_ctcm, adj_dac, rst,
 * data_bits and mute_rate have no writer in that file, and the board shows them
 * populated as found (ADAC1 read 0x00E06755 with data_bits=3 before anything
 * of mine ran). Those bits are therefore left exactly as the bootloader left
 * them rather than invented for.
 *
 * The volume scale is raw register code, 0..127 per channel. Nothing in this
 * tree or the SDK states its direction or a dB mapping, so the control
 * publishes no TLV rather than a guessed one.
 */
#include <linux/array_size.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <sound/soc.h>

#define HISTB_ADAC_LOUT		0x00
#define  ADAC_POPFREER		BIT(24)
#define  ADAC_POPFREEL		BIT(25)
#define  ADAC_PD_VREF		BIT(27)
#define  ADAC_MUTE_DACR		BIT(28)
#define  ADAC_MUTE_DACL		BIT(29)
#define  ADAC_PD_DACR		BIT(30)
#define  ADAC_PD_DACL		BIT(31)
#define  ADAC_POWEROFF		(ADAC_PD_DACL | ADAC_PD_DACR | ADAC_MUTE_DACL | \
				 ADAC_MUTE_DACR | ADAC_PD_VREF | ADAC_POPFREEL | \
				 ADAC_POPFREER)

#define HISTB_ADAC_CTRL		0x04
#define  ADAC_SAMPLE_SEL	GENMASK(21, 19)
#define  ADAC_SUNMUTER		BIT(28)
#define  ADAC_SUNMUTEL		BIT(29)
#define  ADAC_SMUTER		BIT(30)
#define  ADAC_SMUTEL		BIT(31)

struct histb_adac {
	struct regmap *regmap;
	struct clk *clk;
};

/*
 * The rate bucket the vendor HAL selects (Digfi_DacSetSampleRate). The field
 * covers one of five bands and the band is shared by the neighbouring rates, so
 * 48 kHz and 44.1 kHz are the same code.
 */
static int histb_adac_sample_sel(unsigned int rate)
{
	if (rate <= 12000)
		return 0;
	if (rate <= 24000)
		return 1;
	if (rate <= 48000)
		return 2;
	if (rate <= 96000)
		return 3;

	return 4;
}

static int histb_adac_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct histb_adac *adac = snd_soc_dai_get_drvdata(dai);

	return regmap_update_bits(adac->regmap, HISTB_ADAC_CTRL, ADAC_SAMPLE_SEL,
				  FIELD_PREP(ADAC_SAMPLE_SEL,
					     histb_adac_sample_sel(params_rate(params))));
}

/*
 * Power-up, in the vendor's order (Digfi_DacPoweup without the
 * HI_SND_MUTECTL_SUPPORT branch, which is the one that has no timing hooks to
 * guess at): soft-mute first so nothing reaches the pins while the analog
 * stage comes up, popfree (the click-suppression ramp) around the reference and
 * driver power-on, then release both mutes.
 */
/*
 * Soft-unmute is an RMW over just the four mute bits: sample_sel lives in the
 * same register, so writing the whole register here would undo whatever rate
 * .hw_params picked.
 */
static void histb_adac_soft_unmute(struct histb_adac *adac)
{
	regmap_update_bits(adac->regmap, HISTB_ADAC_CTRL,
			   ADAC_SMUTEL | ADAC_SMUTER | ADAC_SUNMUTEL | ADAC_SUNMUTER,
			   ADAC_SUNMUTEL | ADAC_SUNMUTER);
}

static int histb_adac_startup(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
	struct histb_adac *adac = snd_soc_dai_get_drvdata(dai);

	regmap_set_bits(adac->regmap, HISTB_ADAC_CTRL, ADAC_SMUTEL | ADAC_SMUTER);
	regmap_set_bits(adac->regmap, HISTB_ADAC_LOUT, ADAC_POPFREEL | ADAC_POPFREER);
	regmap_clear_bits(adac->regmap, HISTB_ADAC_LOUT, ADAC_PD_VREF);
	regmap_clear_bits(adac->regmap, HISTB_ADAC_LOUT, ADAC_PD_DACL | ADAC_PD_DACR);
	regmap_clear_bits(adac->regmap, HISTB_ADAC_LOUT, ADAC_POPFREEL | ADAC_POPFREER);
	regmap_clear_bits(adac->regmap, HISTB_ADAC_LOUT, ADAC_MUTE_DACL | ADAC_MUTE_DACR);
	histb_adac_soft_unmute(adac);

	return 0;
}

static void histb_adac_shutdown(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct histb_adac *adac = snd_soc_dai_get_drvdata(dai);

	regmap_set_bits(adac->regmap, HISTB_ADAC_CTRL, ADAC_SMUTEL | ADAC_SMUTER);
	/*
	 * One write for the whole analog power-down: every bit of the off state
	 * is asserted together, which is what the block shows cold (the board
	 * read ADAC0 = 0xF8400606 with pd_*, mute_* and pd_vref all set).
	 */
	regmap_set_bits(adac->regmap, HISTB_ADAC_LOUT, ADAC_POWEROFF);
}

static int histb_adac_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct histb_adac *adac = snd_soc_dai_get_drvdata(dai);

	if (mute)
		regmap_set_bits(adac->regmap, HISTB_ADAC_CTRL,
				ADAC_SMUTEL | ADAC_SMUTER);
	else
		histb_adac_soft_unmute(adac);

	return 0;
}

static const struct snd_soc_dai_ops histb_adac_ops = {
	.hw_params = histb_adac_hw_params,
	.startup = histb_adac_startup,
	.shutdown = histb_adac_shutdown,
	.mute_stream = histb_adac_mute_stream,
};

static const struct snd_soc_dapm_widget histb_adac_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("DAC Out"),
};

static const struct snd_soc_dapm_route histb_adac_dapm_routes[] = {
	{ "DAC Out", NULL, "DAC HiFi" },
};

static const struct snd_kcontrol_new histb_adac_controls[] = {
	/* Raw code, 0..127: see the header comment on why there is no TLV. */
	SOC_DOUBLE_R("DAC Volume", HISTB_ADAC_LOUT, 8, 0, 0x7f, 0),
};

static const struct regmap_config histb_adac_regmap = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static const struct snd_soc_component_driver histb_adac_component = {
	.controls = histb_adac_controls,
	.num_controls = ARRAY_SIZE(histb_adac_controls),
	.dapm_widgets = histb_adac_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(histb_adac_dapm_widgets),
	.dapm_routes = histb_adac_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(histb_adac_dapm_routes),
	.idle_bias_on = true,
};

static struct snd_soc_dai_driver histb_adac_dai = {
	.name = "histb-adac",
	.playback = {
		.stream_name = "DAC HiFi",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_192000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &histb_adac_ops,
};

static int histb_adac_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_adac *adac;
	void __iomem *base;
	int ret;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);

	adac = devm_kzalloc(dev, sizeof(*adac), GFP_KERNEL);
	if (!adac)
		return -ENOMEM;

	adac->regmap = devm_regmap_init_mmio(dev, base, &histb_adac_regmap);
	if (IS_ERR(adac->regmap))
		return dev_err_probe(dev, PTR_ERR(adac->regmap), "no regmap\n");

	adac->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(adac->clk))
		return dev_err_probe(dev, PTR_ERR(adac->clk), "no adac clock\n");

	ret = devm_snd_soc_register_component(dev, &histb_adac_component,
					      &histb_adac_dai, 1);
	if (ret)
		return dev_err_probe(dev, ret, "could not register the codec\n");

	platform_set_drvdata(pdev, adac);

	return 0;
}

static const struct of_device_id histb_adac_of_match[] = {
	{ .compatible = "hisilicon,hi3716cv200-adac" },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_adac_of_match);

static struct platform_driver histb_adac_driver = {
	.probe = histb_adac_probe,
	.driver = {
		.name = "histb-adac",
		.of_match_table = histb_adac_of_match,
	},
};
module_platform_driver(histb_adac_driver);

MODULE_DESCRIPTION("HiSTB TianLai internal audio DAC");
MODULE_LICENSE("GPL");
