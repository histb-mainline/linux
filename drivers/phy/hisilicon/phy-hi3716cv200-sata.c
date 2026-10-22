// SPDX-License-Identifier: GPL-2.0-only
/*
 * SATA PHY driver for the HiSilicon Hi3716CV200.
 *
 * The PHY shares the SATA controller's register window, and the block answers
 * writes only after the two reset bits of its CRG word have seen an edge, so
 * the resets named by the DT node are pulsed before any PHY register is
 * touched. The sequence and the field values come from the vendor bring-up for
 * this chip (drivers/ata/hi_ahci_sys_s40_defconfig.c of the 3.10 SDK) and were
 * checked against the words the vendor system leaves behind on the AR513W.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#define SATA_PHY_CTRL			0x0a0
#define  PHY_CTRL_MPLL_MULT		GENMASK(8, 1)
#define  PHY_CTRL_MPLL_MULT_50M		FIELD_PREP(PHY_CTRL_MPLL_MULT, 0x3c)
#define  PHY_CTRL_REF_SSP_EN		BIT(9)
#define  PHY_CTRL_SSC_EN		BIT(10)
#define  PHY_CTRL_RESET			BIT(0)
/*
 * The vendor word also sets bits 30, 27 and 24; no source available for this
 * chip names them, so they travel with the value rather than as fields.
 */
#define  PHY_CTRL_VENDOR_UPPER		0x49000000

#define SATA_DATA_INVERT		0x0a4
#define  DATA_INVERT_TX_RX		BIT(3)

#define SATA_SPIN_UP			0x118
#define  SPIN_UP_CONFIG			0x00600000
#define  SPIN_UP_ENABLE			0x00600002

#define SATA_PORT_PHY_CTRL1		0x148
#define  AMP_GEN3			FIELD_PREP(GENMASK(21, 15), 0x68)
#define  AMP_GEN2			FIELD_PREP(GENMASK(14, 8), 0x5c)
#define  AMP_GEN1			FIELD_PREP(GENMASK(7, 1), 0x5c)

#define SATA_PORT_PHY_CTRL2		0x14c
#define  PRE_EMPH_GEN3			FIELD_PREP(GENMASK(17, 12), 0x20)
#define  PRE_EMPH_GEN2			FIELD_PREP(GENMASK(11, 6), 0x15)
#define  PRE_EMPH_GEN1			FIELD_PREP(GENMASK(5, 0), 0x5)
#define  SERDES_LANE_RESET		BIT(18)

#define SATA_PORT_PHY_CTRL		0x174
/*
 * Speed selection: the two-bit fields hix5hd2 decodes at 17:16, 19:18 and
 * 22:21 are driven to GEN1, then GEN3 with the speed-control bit at 20 set,
 * then GEN3 alone, then GEN2, which is how the vendor forces the setting to
 * take effect. Bits 27:25 are set in all four writes and are likewise not
 * named by any source available here.
 */
#define  PORT_SPEED_GEN1		0x0e100000
#define  PORT_SPEED_GEN3_ARMED		0x0e5a0000
#define  PORT_SPEED_GEN3		0x0e4a0000
#define  PORT_SPEED_GEN2		0x0e250000

struct hi3716cv200_sata_phy {
	void __iomem *base;
	struct regmap *peri;
	struct reset_control *rst;
	u32 power_offset;
	u32 power_bit;
};

static int hi3716cv200_sata_phy_power_on(struct phy *phy)
{
	struct hi3716cv200_sata_phy *priv = phy_get_drvdata(phy);
	void __iomem *base = priv->base;

	regmap_update_bits(priv->peri, priv->power_offset, priv->power_bit,
			   priv->power_bit);

	/*
	 * Without this edge every write to the window is dropped: the block
	 * reads back as zero and HOST_CTL.AHCI_EN never sticks.
	 */
	reset_control_reset(priv->rst);

	writel(PHY_CTRL_MPLL_MULT_50M | PHY_CTRL_REF_SSP_EN | PHY_CTRL_SSC_EN |
	       PHY_CTRL_VENDOR_UPPER | PHY_CTRL_RESET, base + SATA_PHY_CTRL);
	msleep(20);
	writel(PHY_CTRL_MPLL_MULT_50M | PHY_CTRL_REF_SSP_EN | PHY_CTRL_SSC_EN |
	       PHY_CTRL_VENDOR_UPPER, base + SATA_PHY_CTRL);
	msleep(20);

	writel(AMP_GEN3 | AMP_GEN2 | AMP_GEN1, base + SATA_PORT_PHY_CTRL1);
	writel(PRE_EMPH_GEN3 | PRE_EMPH_GEN2 | PRE_EMPH_GEN1 |
	       SERDES_LANE_RESET, base + SATA_PORT_PHY_CTRL2);
	writel(PRE_EMPH_GEN3 | PRE_EMPH_GEN2 | PRE_EMPH_GEN1,
	       base + SATA_PORT_PHY_CTRL2);

	writel(DATA_INVERT_TX_RX, base + SATA_DATA_INVERT);

	writel(SPIN_UP_CONFIG, base + SATA_SPIN_UP);
	writel(SPIN_UP_ENABLE, base + SATA_SPIN_UP);

	writel(PORT_SPEED_GEN1, base + SATA_PORT_PHY_CTRL);
	writel(PORT_SPEED_GEN3_ARMED, base + SATA_PORT_PHY_CTRL);
	writel(PORT_SPEED_GEN3, base + SATA_PORT_PHY_CTRL);
	writel(PORT_SPEED_GEN2, base + SATA_PORT_PHY_CTRL);

	return 0;
}

static int hi3716cv200_sata_phy_power_off(struct phy *phy)
{
	struct hi3716cv200_sata_phy *priv = phy_get_drvdata(phy);

	reset_control_assert(priv->rst);
	regmap_update_bits(priv->peri, priv->power_offset, priv->power_bit, 0);

	return 0;
}

static const struct phy_ops hi3716cv200_sata_phy_ops = {
	.power_on	= hi3716cv200_sata_phy_power_on,
	.power_off	= hi3716cv200_sata_phy_power_off,
	.owner		= THIS_MODULE,
};

static int hi3716cv200_sata_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hi3716cv200_sata_phy *priv;
	struct phy_provider *provider;
	struct resource *res;
	struct phy *phy;
	void __iomem *base;
	u32 power[2];
	int ret;

	/*
	 * Mapped, not claimed: the ahci host owns this region and the PHY's
	 * registers live inside it.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;

	base = devm_ioremap(dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = base;
	priv->peri = syscon_regmap_lookup_by_phandle(dev->of_node,
						   "hisilicon,peri-syscon");
	if (IS_ERR(priv->peri))
		return dev_err_probe(dev, PTR_ERR(priv->peri),
				     "missing peri-syscon\n");

	ret = of_property_read_u32_array(dev->of_node, "hisilicon,power-reg",
					 power, ARRAY_SIZE(power));
	if (ret)
		return dev_err_probe(dev, ret, "missing power-reg\n");
	priv->power_offset = power[0];
	priv->power_bit = BIT(power[1]);

	priv->rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(priv->rst))
		return dev_err_probe(dev, PTR_ERR(priv->rst),
				     "missing SATA resets\n");

	phy = devm_phy_create(dev, NULL, &hi3716cv200_sata_phy_ops);
	if (IS_ERR(phy))
		return dev_err_probe(dev, PTR_ERR(phy), "failed to create PHY\n");

	phy_set_drvdata(phy, priv);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id hi3716cv200_sata_phy_of_match[] = {
	{ .compatible = "hisilicon,hi3716cv200-sata-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, hi3716cv200_sata_phy_of_match);

static struct platform_driver hi3716cv200_sata_phy_driver = {
	.probe	= hi3716cv200_sata_phy_probe,
	.driver	= {
		.name		= "hi3716cv200-sata-phy",
		.of_match_table	= hi3716cv200_sata_phy_of_match,
	},
};
module_platform_driver(hi3716cv200_sata_phy_driver);

MODULE_DESCRIPTION("HiSilicon Hi3716CV200 SATA PHY driver");
MODULE_LICENSE("GPL");
