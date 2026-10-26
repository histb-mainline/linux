// SPDX-License-Identifier: GPL-2.0
/*
 * HiSTB AIAO sound card.
 *
 * The AIAO block plays to the on-chip HDMI transmitter, and its receive port
 * taps back the serial stream that transmit port puts on the wire. The capture
 * side has no codec to pair with - nothing else listens to it - and only a
 * machine driver can ask the core for its dummy codec on such a link, which is
 * why this card cannot be described with simple-audio-card.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

static const struct {
	const char *node;
	const char *name;
	const char *stream;
	unsigned int dai_fmt;
	bool capture_only;
} histb_aiao_links[] = {
	{
		.node = "playback",
		.name = "aiao-hdmi",
		.stream = "hdmi",
		/*
		 * The port drives bit clock and frame sync; the transmitter
		 * only receives.
		 */
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_CBC_CFC,
	},
	{
		.node = "capture",
		.name = "aiao-in",
		.stream = "loopback",
		.capture_only = true,
	},
	{
		.node = "dac-playback",
		.name = "aiao-dac",
		.stream = "dac-playback",
		/*
		 * The DAC is the clock consumer here, and the transmit port
		 * drives both, same as the HDMI side.
		 */
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_CBC_CFC,
	},
	{
		.node = "dac-capture",
		.name = "aiao-dac-in",
		.stream = "dac-loopback",
		.capture_only = true,
	},
};

struct histb_aiao_card {
	struct snd_soc_card card;
	struct snd_soc_dai_link link[ARRAY_SIZE(histb_aiao_links)];
	struct snd_soc_dai_link_component platform[ARRAY_SIZE(histb_aiao_links)];
};

static int histb_aiao_parse_link(struct platform_device *pdev,
				 struct device_node *node,
				 struct snd_soc_dai_link *link,
				 struct snd_soc_dai_link_component *platform,
				 const char *stream, unsigned int dai_fmt,
				 bool capture_only)
{
	struct device *dev = &pdev->dev;
	struct device_node *cpu, *codec;
	int ret;

	cpu = of_get_child_by_name(node, "cpu");
	if (!cpu)
		return dev_err_probe(dev, -ENODEV, "%pOF has no cpu\n", node);

	ret = snd_soc_of_get_dai_link_cpus(dev, cpu, link);
	of_node_put(cpu);
	if (ret)
		return dev_err_probe(dev, ret, "%pOF has no cpu DAI\n", node);

	codec = of_get_child_by_name(node, "codec");
	if (codec) {
		ret = snd_soc_of_get_dai_link_codecs(dev, codec, link);
		of_node_put(codec);
		if (ret)
			return dev_err_probe(dev, ret, "%pOF has no codec DAI\n", node);
	} else {
		/* The tap has nothing on the other end. */
		link->codecs = &snd_soc_dummy_dlc;
		link->num_codecs = 1;
	}

	link->platforms = platform;
	link->num_platforms = 1;
	snd_soc_dlc_use_cpu_as_platform(link->platforms, link->cpus);

	link->stream_name = stream;
	link->dai_fmt = dai_fmt;
	link->capture_only = capture_only;

	return 0;
}

/*
 * Each DAI the parse helpers resolve comes with a reference on its node.
 * The component arrays those references live in are devm-allocated while the
 * links are parsed, so this action has to be registered *after* them: devres
 * unwinds in reverse order, and registering it first would run it on already
 * freed arrays.
 */
static void histb_aiao_put_dlc(void *data)
{
	struct snd_soc_card *card = data;
	int i;

	for (i = 0; i < card->num_links; i++) {
		snd_soc_of_put_dai_link_cpus(&card->dai_link[i]);
		snd_soc_of_put_dai_link_codecs(&card->dai_link[i]);
	}
}

static int histb_aiao_card_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_aiao_card *priv;
	struct device_node *np = dev->of_node;
	struct snd_soc_card *card;
	struct snd_soc_dai_link *link;
	struct snd_soc_dai_link_component *platform;
	int i, ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	card = &priv->card;
	card->dev = dev;
	card->driver_name = "histb-aiao";
	card->dai_link = priv->link;

	ret = snd_soc_of_parse_card_name(card, "model");
	if (ret)
		return dev_err_probe(dev, ret, "no model name\n");

	/*
	 * A part without one of these taps leaves its node out of the tree, so
	 * the table is a menu rather than a checklist. The count grows as links
	 * are filled in, so a link that fails half-parsed still has the
	 * references it already took released by the explicit put below.
	 */
	for (i = 0; i < ARRAY_SIZE(histb_aiao_links); i++) {
		struct device_node *node = of_get_child_by_name(np,
								histb_aiao_links[i].node);

		if (!node)
			continue;

		link = &priv->link[card->num_links];
		platform = &priv->platform[card->num_links];
		link->name = histb_aiao_links[i].name;
		card->num_links++;
		ret = histb_aiao_parse_link(pdev, node, link, platform,
					    histb_aiao_links[i].stream,
					    histb_aiao_links[i].dai_fmt,
					    histb_aiao_links[i].capture_only);
		of_node_put(node);
		if (ret) {
			histb_aiao_put_dlc(card);
			return ret;
		}
	}

	if (!card->num_links)
		return dev_err_probe(dev, -ENODEV, "no DAI link in %pOF\n", np);

	ret = devm_add_action_or_reset(dev, histb_aiao_put_dlc, card);
	if (ret)
		return dev_err_probe(dev, ret, "no cleanup action\n");

	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		return dev_err_probe(dev, ret, "could not register the card\n");

	return 0;
}

static const struct of_device_id histb_aiao_card_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv100-aiao-card" },
	{ .compatible = "hisilicon,hi3716cv200-aiao-card" },
	{ .compatible = "hisilicon,hi3798mv200-aiao-card" },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_aiao_card_of_match);

static struct platform_driver histb_aiao_card_driver = {
	.probe = histb_aiao_card_probe,
	.driver = {
		.name = "histb-aiao-card",
		.of_match_table = histb_aiao_card_of_match,
	},
};
module_platform_driver(histb_aiao_card_driver);

MODULE_DESCRIPTION("HiSTB AIAO sound card");
MODULE_LICENSE("GPL");
