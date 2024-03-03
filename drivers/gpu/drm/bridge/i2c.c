// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2023 David Yang
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/string.h>


#define HDMI_CORE_SYS_VND_IDL              0x0
#define HDMI_CORE_SYS_DEV_IDL              0x8
#define HDMI_CORE_SYS_DEV_IDH              0xC
#define HDMI_CORE_SYS_DEV_REV              0x10
#define HDMI_CORE_SYS_SRST                 0x14
#define   BIT_TX_SW_RST                     (0x01)
#define   BIT_TX_FIFO_RST                   (0x02)
#define HDMI_CORE_CTRL1                    0x20
#define   BIT_TX_PD                         (0x01)
#define   BIT_BSEL24BITS                    (0x04)
#define   BIT_TX_CLOCK_RISING_EDGE          (0x02)
#define HDMI_CORE_SYS_SYS_STAT             0x24
#define   BIT_HDMI_PSTABLE                  (0X01)
#define   BIT_HPD_PIN                       (0x02)
#define   BIT_RSEN                          (0x04)
#define HDMI_CORE_SYS_DATA_CTRL            0x34
#define   BIT_AUD_MUTE                      (0x02)
#define   BIT_VID_BLANK                     (0x04)
#define HDMI_CORE_SYS_VID_ACEN             0x124
#define HDMI_CORE_SYS_VID_MODE             0x128
#define HDMI_CORE_SYS_VID_CTRL             0x120
#define HDMI_CORE_SYS_INTR_STATE           0x1C0
#define   BIT_INTR                          (0x01)
#define HDMI_CORE_SYS_INTR1                0x1C4
#define   BIT_INTR1_SOFT                    (0x80)
#define   BIT_INTR1_HPD                     (0x40)
#define   BIT_INTR1_RSEN                    (0x20)
#define   BIT_INTR1_DROP_SAMPLE             (0x10)
#define   BIT_INTR1_BI_PHASE_ERR            (0x08)
#define   BIT_INTR1_RI_128                  (0x04)
#define   BIT_INTR1_OVER_RUN                (0x02)
#define   BIT_INTR1_UNDER_RUN               (0x01)
#define HDMI_CORE_SYS_INTR2                0x1C8
#define   BIT_INTR2_BCAP_DONE               (0x80)
#define   BIT_INTR2_SPDIF_PAR               (0x40)
#define   BIT_INTR2_ENC_DIS                 (0x20)
#define   BIT_INTR2_PREAM_ERR               (0x10)
#define   BIT_INTR2_CTS_CHG                 (0x08)
#define   BIT_INTR2_ACR_OVR                 (0x04)
#define   BIT_INTR2_TCLK_STBL               (0x02)
#define   BIT_INTR2_VSYNC_REC               (0x01)
#define HDMI_CORE_SYS_INTR3                0x1CC
#define   BIT_INTR3_RI_ERR3                 (0x80)
#define   BIT_INTR3_RI_ERR2                 (0x40)
#define   BIT_INTR3_RI_ERR1                 (0x20)
#define   BIT_INTR3_RI_ERR0                 (0x10)
#define   BIT_INTR3_DDC_CMD_DONE            (0x08)
#define   BIT_INTR3_DDC_FIFO_HALF           (0x04)
#define   BIT_INTR3_DDC_FIFO_FULL           (0x02)
#define   BIT_INTR3_DDC_FIFO_EMPTY          (0x01)
#define HDMI_CORE_SYS_INTR4                0x1D0
#define   BIT_INTR4_CEC                     (0x08)
#define   BIT_INTR4_DSD_INVALID             (0x01)
#define HDMI_CORE_SYS_INTR1_MASK           0x1D4
#define HDMI_CORE_SYS_INTR2_MASK           0x1D8
#define HDMI_CORE_SYS_INTR3_MASK           0x1DC
#define   MASK_AUTO_RI_9134_SPECIFIC        (0xB0)
#define   MASK_AUTO_KSV_READY               (0x80)
#define HDMI_CORE_SYS_INTR4_MASK           0x1E0
#define   BIT_INT_Ri_CHECK                  (0x04)

#define HDMI_CORE_SYS_TMDS_CTRL            0x208
#define HDMI_CORE_CTRL1_VEN_FOLLOWVSYNC    0x1
#define HDMI_CORE_CTRL1_HEN_FOLLOWHSYNC    0x1
#define HDMI_CORE_CTRL1_BSEL_24BITBUS      0x1
#define HDMI_CORE_CTRL1_EDGE_RISINGEDGE    0x1

#define HDMI_CORE_SYS_HDCP_CTRL            0x3c
#define   BIT_ENC_EN                        (0x01)
#define   BIT_RiREADY                       (0x02)
#define   BIT_RI_STARTED                    (0x01)
#define   BIT_CP_RESET_N                    (0x04)
#define   BIT_AN_STOP                       (0x08)
#define   BIT_RX_REPEATER                   (0x10)
#define   BIT_BKSV_ERROR                    (0x20)
#define   BIT_ENC_ON                        (0x40)

#define HDMI_CORE_SYS_HDCP_BKSV_ADDR       0x40
#define HDMI_CORE_SYS_HDCP_AN_ADDR         0x54
#define HDMI_CORE_SYS_HDCP_AKSV_ADDR       0x74
#define HDMI_CORE_SYS_HDCP_Ri_ADDR         0x88
#define HDMI_CORE_SYS_HDCP_RI_STAT         0x98
#define HDMI_CORE_SYS_HDCP_RI_CMD_ADDR     0x9c
#define HDMI_CORE_SYS_HDCP_RI_START        0xA0
#define HDMI_CORE_SYS_HDCP_RI_RX_1         0xA4
#define HDMI_CORE_SYS_HDCP_RI_RX_2         0xA8

#define HDMI_CORE_SYS_DE_DLY               0xC8
#define HDMI_CORE_SYS_DE_CTRL              0xCC
#define   BIT_DE_ENABLED                    (0x40)

#define HDMI_CORE_SYS_DE_TOP               0xD0
#define HDMI_CORE_SYS_DE_CNTL              0xD8
#define HDMI_CORE_SYS_DE_CNTH              0xDC
#define HDMI_CORE_SYS_DE_LINL              0xE0
#define HDMI_CORE_SYS_DE_LINH_1            0xE4
#define HDMI_CORE_SYS_INT_CNTRL            0x1E4
#define   BIT_INT_Ri_CHECK                  (0x04)
#define   BIT_INT_HOT_PLUG                  (0x40)
#define   BIT_BIPHASE_ERROR                 (0x08)
#define   BIT_DROP_SAMPLE                   (0x10)
#define   BIT_INT_VSYNC                     (0x01)
#define   BIT_INT_FPIXCHANGE                (0x02)
#define   BIT_INT_KSV_READY                 (0x80)
#define HDMI_CORE_SYS_VID_BLANK1           0x12C


/* HDMI IP Core Audio Video */
#define HDMI_CORE_AV_HDMI_CTRL             0xBC
#define HDMI_CORE_AV_DPD                   0xF4
#define HDMI_CORE_AV_PB_CTRL1              0xF8
#define HDMI_CORE_AV_PB_CTRL2              0xFC
#define HDMI_CORE_AV_AVI_TYPE              0x100
#define HDMI_CORE_AV_AVI_VERS              0x104
#define HDMI_CORE_AV_AVI_LEN               0x108
#define HDMI_CORE_AV_AVI_CHSUM             0x10C
#define HDMI_CORE_AV_AVI_DBYTE             0x110
#define HDMI_CORE_AV_AVI_DBYTE_ELSIZE      0x4

/* HDMI DDC E-DID */


#define HDMI_CORE_AV_AVI_DBYTE             0x110
#define HDMI_CORE_AV_AVI_DBYTE_ELSIZE      0x4
#define HDMI_IP_CORE_AV_AVI_DBYTE_NELEMS   15
#define HDMI_CORE_AV_SPD_DBYTE             0x190
#define HDMI_CORE_AV_SPD_DBYTE_ELSIZE      0x4
#define HDMI_CORE_AV_SPD_DBYTE_NELEMS      27
#define HDMI_CORE_AV_AUDIO_DBYTE           0x210
#define HDMI_CORE_AV_AUDIO_DBYTE_ELSIZE    0x4
#define HDMI_CORE_AV_AUDIO_DBYTE_NELEMS    10
#define HDMI_CORE_AV_MPEG_DBYTE            0x290
#define HDMI_CORE_AV_MPEG_DBYTE_ELSIZE     0x4
#define HDMI_CORE_AV_MPEG_DBYTE_NELEMS     27
#define HDMI_CORE_AV_GEN_DBYTE             0x300
#define HDMI_CORE_AV_GEN_DBYTE_ELSIZE      0x4
#define HDMI_CORE_AV_GEN_DBYTE_NELEMS      31
#define HDMI_CORE_AV_GEN2_DBYTE            0x380
#define HDMI_CORE_AV_GEN2_DBYTE_ELSIZE     0x4
#define HDMI_CORE_AV_GEN2_DBYTE_NELEMS     31
#define HDMI_CORE_AV_ACR_CTRL              0x4
#define HDMI_CORE_AV_FREQ_SVAL             0x8
#define HDMI_CORE_AV_N_SVAL1               0xC
#define HDMI_CORE_AV_N_SVAL2               0x10
#define HDMI_CORE_AV_N_SVAL3               0x14
#define HDMI_CORE_AV_CTS_SVAL1             0x18
#define HDMI_CORE_AV_CTS_SVAL2             0x1C
#define HDMI_CORE_AV_CTS_SVAL3             0x20
#define HDMI_CORE_AV_CTS_HVAL1             0x24
#define HDMI_CORE_AV_CTS_HVAL2             0x28
#define HDMI_CORE_AV_CTS_HVAL3             0x2C
#define HDMI_CORE_AV_AUD_MODE              0x50
#define HDMI_CORE_AV_SPDIF_CTRL            0x54
#define HDMI_CORE_AV_HW_SPDIF_FS           0x60
#define HDMI_CORE_AV_SWAP_I2S              0x64
#define HDMI_CORE_AV_SPDIF_ERTH            0x6C
#define HDMI_CORE_AV_I2S_IN_MAP            0x70
#define HDMI_CORE_AV_I2S_IN_CTRL           0x74
#define HDMI_CORE_AV_I2S_CHST0             0x78
#define HDMI_CORE_AV_I2S_CHST1             0x7C
#define HDMI_CORE_AV_I2S_CHST2             0x80
#define HDMI_CORE_AV_I2S_CHST4             0x84
#define HDMI_CORE_AV_I2S_CHST5             0x88
#define HDMI_CORE_AV_ASRC                  0x8C
#define   BIT_DOWNSAMPLE_RATIO              (0x02)
#define   BIT_DOWNSAMPLE_ENABLE_MASK        (0x01)
#define HDMI_CORE_AV_I2S_IN_LEN            0x90
#define   BIT_DEEPCOLOR_EN                  (0x40)
#define   BIT_TXHDMI_MODE                   (0x01)
#define   BIT_EN_AUDIO                      (0x01)
#define   BIT_LAYOUT                        (0x02)
#define   BIT_LAYOUT1                       (0x02)

#define HDMI_CORE_AV_AUDO_TXSTAT           0xC0
#define HDMI_CORE_AV_AUD_PAR_BUSCLK_1      0xCC
#define HDMI_CORE_AV_AUD_PAR_BUSCLK_2      0xD0
#define HDMI_CORE_AV_AUD_PAR_BUSCLK_3      0xD4
#define HDMI_CORE_AV_TEST_TXCTRL           0xF0
#define   BIT_DVI_ENC_BYPASS                (0x08)

#define HDMI_CORE_AV_DPD                   0xF4
#define HDMI_CORE_AV_PB_CTRL1              0xF8
#define HDMI_CORE_AV_PB_CTRL2              0xFC
#define   BIT_AVI_REPEAT                    (0x01)
#define   BIT_AVI_ENABLE                    (0x02)
#define   BIT_SPD_REPEAT                    (0x04)
#define   BIT_SPD_ENABLE                    (0x08)
#define   BIT_AUD_REPEAT                    (0x10)
#define   BIT_AUD_ENABLE                    (0x20)
#define   BIT_MPEG_REPEAT                   (0x40)
#define   BIT_MPEG_ENABLE                   (0x80)
#define   BIT_GENERIC_REPEAT                (0x01)
#define   BIT_GENERIC_ENABLE                (0x02)
#define   BIT_CP_REPEAT                     (0x04)
#define   BIT_CP_ENABLE                     (0x08)

#define HDMI_CORE_AV_AVI_TYPE              0x100
#define HDMI_CORE_AV_AVI_VERS              0x104
#define HDMI_CORE_AV_AVI_LEN               0x108
#define HDMI_CORE_AV_AVI_CHSUM             0x10C
#define HDMI_CORE_AV_SPD_TYPE              0x180
#define HDMI_CORE_AV_SPD_VERS              0x184
#define HDMI_CORE_AV_SPD_LEN               0x188
#define HDMI_CORE_AV_SPD_CHSUM             0x18C
#define HDMI_CORE_AV_AUDIO_TYPE            0x200
#define HDMI_CORE_AV_AUDIO_VERS            0x204
#define HDMI_CORE_AV_AUDIO_LEN             0x208
#define HDMI_CORE_AV_AUDIO_CHSUM           0x20C
#define HDMI_CORE_AV_MPEG_TYPE             0x280
#define HDMI_CORE_AV_MPEG_VERS             0x284
#define HDMI_CORE_AV_MPEG_LEN              0x288
#define HDMI_CORE_AV_MPEG_CHSUM            0x28C
#define HDMI_CORE_AV_CP_BYTE1              0x37C
#define   BIT_CP_AVI_MUTE_SET               (0x01)
#define   BIT_CP_AVI_MUTE_CLEAR             (0x10)

#define HDMI_CORE_AV_CEC_ADDR_ID           0x3FC

#define HDMI_CORE_SYS_SYS_STAT_HPD         0x02

#define HDMI_IP_CORE_SYSTEM_INTR2_BCAP     0x80
#define HDMI_IP_CORE_SYSTEM_INTR3_RI_ERR   0xF0

/*hdmi phy */
#define HDMI_BEST_ACLK_DIG                 135000  //as KHz
#define HDMI_MAX_ACLK_DIG                  165000  //as KHz
#define HDMI_MIN_ACLK_DIG                  85000  //as KHz

#define HDMI_PHY_TDMS_CTL1                 0x0

#define HDMI_PHY_TDMS_CTL2                 0x04
#define HDMI_PHY_TDMS_CTL3                 0x08
#define HDMI_PHY_BIST_CNTL                 0x0C
#define HDMI_PHY_BIST_INSTRL               0x18
#define HDMI_PHY_TDMS_CNTL9                0x20

#define   ACLK_MULT_FACTOR_1                (0x0)
#define   ACLK_MULT_FACTOR_2                (0x4)
#define   ACLK_MULT_FACTOR_3                (0x2)
#define   ACLK_MULT_FACTOR_4                (0x5)
#define   ACLK_MULT_FACTOR_5                (0x3)
#define   ACLK_MULT_FACTOR_6                (0x6)
#define   ACLK_MULT_FACTOR_10               (0x7)

#define   BIT_ACLK_COUNT0                   (0x5)
#define   BIT_ACLK_COUNT1                   (0x6)
#define   BIT_ACLK_COUNT2                   (0x5)

#define FLD_MASK(start, end)    (((1 << ((start) - (end) + 1)) - 1) << (end))
#define FLD_VAL(val, start, end) (((val) << (end)) & FLD_MASK(start, end))
#define FLD_GET(val, start, end) (((val) & FLD_MASK(start, end)) >> (end))
#define FLD_MOD(orig, val, start, end) \
    (((orig) & ~FLD_MASK(start, end)) | FLD_VAL(val, start, end))

#define REG_FLD_MOD(base, idx, val, start, end) \
    write_reg(base, idx, \
        FLD_MOD(read_reg(base, idx), val, start, end))

#define RD_REG_32(COMP, REG)            read_reg(COMP, REG)
#define WR_REG_32(COMP, REG, VAL)       write_reg(COMP, REG, (u32)(VAL))

#define BITS_32(in_NbBits) \
        ((((u32)1 << in_NbBits) - 1) | ((u32)1 << in_NbBits))

#define BITFIELD(in_UpBit, in_LowBit) \
        (BITS_32(in_UpBit) & ~((BITS_32(in_LowBit)) >> 1))

#define HDMI_CONNECT            0x01
#define HDMI_DISCONNECT         0x02
#define HDMI_INT_CEC            0x04
#define HDMI_FIRST_HPD          0x08
#define HDMI_BCAP               0x40
#define HDMI_RI_ERR             0x80
#define HDMI_RI_128_ERR         0x100

#define HDMI_EVENT_POWEROFF     0x00
#define HDMI_EVENT_POWERPHYOFF  0x01
#define HDMI_EVENT_POWERPHYON   0x02
#define HDMI_EVENT_POWERON      0x03

#define HDMI_AV_REG_OFFSET      0x400
#define HDMI_PHY_REG_OFFSET     0x1800
#define HDMI_CEC_REG_OFFSET     0x800


#define ID_VER_MAJOR		0x01
#define ID_VER_RELEASE		0x00
#define ID_VER_BUILD		0x33
#define ID_API_VER_H		0x00
#define ID_API_VER_L		0x00
#define ID_SII_PART			0x00
#define ID_CP_BOARD			0x00
#define ID_EEPROM_VER		0x06
#define ID_BUILD_FLAG		0x00


#define PHY_BASE	0x1800
#define PHY_OE			0x0
#define  PHY_TX_RST			BIT(0)
#define PHY_PWD 		0x1
#define  PHY_TX_EN 			BIT(0)
#define PHY_AUDIO 		0x2
#define PHY_PLL1		0x3
#define PHY_PLL2		0x4
#define  MASK_DEEPCOLOR			0x3
#define PHY_DRV 		0x5
#define PHY_CLK 		0x6
#define PHY_BIAS_GEN_CTRL1	0xa
#define PHY_BIAS_GEN_CTRL2	0xb


#define HDMI_CORE_SYS_SHA_CONTROL       (0xCC*4)  // 0x330ul  /* SHA Control */
#define   BIT_M0_READ_EN                    (0x08)  /* M0 readable (1=external, 0-default=internal) */
#define   BIT_SHA_DONE                      (0x02)  /* The SHA generator picked up the SHA GO START, write a "1" to clear before any new SHA GO START */
#define   BIT_SHA_GO_START                  (0x01)  /* Start the SHA generator */
#define HDMI_CORE_SYS_RI_CMD               0x9Cul
#define   BIT_RI_CMD_BCAP_EN                (0x02)  /* Enable polling of the BCAP "done" bit (KSV FIFO done 0x40[5]). */
#define   BIT_RI_CMD_RI_EN                  (0x01)  /* Enable Ri Check. Need to check the Ri On bit (0x026[0]) for firmware handshaking. */
#define HDMI_CORE_SYS_EPCM                (0xFA*4)  // 0x3E8ul  /* bit5 load ksv */


static void
histb_hdmi_14_rw(void __iomem *base, unsigned int offset, void *buf,
		 unsigned int len, bool write)
{
	base += 4 * offset;
	for (unsigned int i = 0; i < len; i++)
		if (write)
			writeb_relaxed(((u8 *) buf)[i], base + 4 * i);
		else
			((u8 *) buf)[i] = readb_relaxed(base + 4 * i);
	barrier();
}

static int
histb_hdmi_14_i2c_access(struct i2c_adapter *adap, u16 addr,
			 unsigned short flags, char read_write, u8 command,
			 int size, union i2c_smbus_data *data)
{
	struct histb_hdmi_14_priv *priv = i2c_get_adapdata(adap);
	struct device *dev = priv->dev;
	void __iomem *base = priv->base;
	bool write = read_write == I2C_SMBUS_WRITE;
	int ret = 0;

	switch (addr) {
	case sii9134_ADDR0:
		break;
	case sii9134_ADDR1:
		base += 0x400;
		break;
	default:
		return -ENXIO;
	}

	if (size == I2C_SMBUS_QUICK)
		return 0;

	pm_runtime_get_sync(dev);

	switch (size) {
	case I2C_SMBUS_BYTE:
		histb_hdmi_14_rw(base, 0, &command, 1, write);
		break;
	case I2C_SMBUS_BYTE_DATA:
		histb_hdmi_14_rw(base, command, &data->byte, 1, write);
		break;
	case I2C_SMBUS_WORD_DATA:
		histb_hdmi_14_rw(base, command, &data->word, 2, write);
		break;
	case I2C_SMBUS_PROC_CALL:
		histb_hdmi_14_rw(base, command, &data->word, 2, true);
		histb_hdmi_14_rw(base, command, &data->word, 2, false);
		break;
	case I2C_SMBUS_BLOCK_DATA:
	case I2C_SMBUS_I2C_BLOCK_DATA:
		histb_hdmi_14_rw(base, command, &data->block[1], data->block[0],
				 write);
		break;
	default:
		dev_err(dev, "Unsupported transaction %d\n", size);
		ret = -EOPNOTSUPP;
	}

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static u32 histb_hdmi_14_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_NOSTART | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm histb_hdmi_14_i2c_algorithm = {
	.smbus_xfer = histb_hdmi_14_i2c_access,
	.functionality = histb_hdmi_14_i2c_func,
};

static int histb_hdmi_14_runtime_suspend(struct device *dev)
{
	struct histb_hdmi_14_priv *priv = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(priv->clks_num, priv->clks);

	return 0;
}

static int histb_hdmi_14_runtime_resume(struct device *dev)
{
	struct histb_hdmi_14_priv *priv = dev_get_drvdata(dev);

	return reset_control_assert(priv->rst) ?:
	       clk_bulk_prepare_enable(priv->clks_num, priv->clks) ?:
	       reset_control_deassert(priv->rst);
}

static const struct dev_pm_ops histb_hdmi_14_pm_ops = {
	.runtime_suspend = histb_hdmi_14_runtime_suspend,
	.runtime_resume = histb_hdmi_14_runtime_resume,
};

static void histb_hdmi_14_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_hdmi_14_priv *priv = platform_get_drvdata(pdev);

	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
	clk_bulk_disable_unprepare(priv->clks_num, priv->clks);
}

static int histb_hdmi_14_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct histb_hdmi_14_priv *priv;
	int ret;

	/* acquire resources */
	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	ret = devm_clk_bulk_get_all(dev, &priv->clks);
	if (ret < 0)
		return ret;
	priv->clks_num = ret;

	priv->rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(priv->rst))
		return PTR_ERR(priv->rst);

	priv->dev = dev;
	platform_set_drvdata(pdev, priv);
	dev_set_drvdata(dev, priv);

	/* bring up device */
	ret = histb_hdmi_14_runtime_resume(dev);
	if (ret)
		return ret;

	priv->adap = (typeof(priv->adap)) {
		.owner = THIS_MODULE,
		.algo = &histb_hdmi_14_i2c_algorithm,
		.algo_data = priv,
		.dev = {
			.of_node = dev->of_node,
			.parent = dev,
		}
	};
	i2c_set_adapdata(&priv->adap, priv);
	strscpy(priv->adap.name, dev->driver->name, sizeof(priv->adap.name));

	ret = devm_i2c_add_adapter(dev, &priv->adap);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(dev, MSEC_PER_SEC);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	return 0;
}

static const struct of_device_id histb_hdmi_14_of_match[] = {
	{ .compatible = "hisilicon,histb-hdmi-1-4", },
	{ }
};
MODULE_DEVICE_TABLE(of, histb_hdmi_14_of_match);

static struct platform_driver histb_hdmi_14_driver = {
	.probe = histb_hdmi_14_probe,
	.remove_new = histb_hdmi_14_remove,
	.driver = {
		.name = "histb-hdmi-14",
		.of_match_table = histb_hdmi_14_of_match,
		.pm = &histb_hdmi_14_pm_ops,
	},
};

module_platform_driver(histb_hdmi_14_driver);

MODULE_DESCRIPTION("HiSilicon STB HDMI 1.4 Tx Bus");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
