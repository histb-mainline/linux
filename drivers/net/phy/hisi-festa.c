// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/*
 * Driver for HiSilicon Festa PHYs
 *
 * The Festa PHYs are the integrated 10/100/1000 PHYs of HiSilicon STB SoCs.
 * Their behaviour comes from microcode that is either held in an external
 * EEPROM or injected over MDIO; this driver only implements the latter, so a
 * PHY which already has its image loaded is left untouched.
 *
 * Copyright (c) 2023 David Yang
 */

#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/phy.h>

#define PHY_ID_HISILICON_FESTAV200	0x20669813
#define PHY_ID_HISILICON_FESTAV220	0x20669823
#define PHY_ID_HISILICON_FESTAV300	0x20669833
#define PHY_ID_HISILICON_FESTAV320	0x20669843
#define PHY_ID_HISILICON_FESTAV330	0x20669853
#define PHY_ID_HISILICON_FESTAV331	0x20669863

#define MII_EXPMD			0x1d	/* Expanded memory data */
#define MII_EXPMA			0x1e	/* Expanded memory address */

/* Expanded memory addresses used by the microcode download */
#define FESTA_UCODE_START		0x33f8
#define FESTA_UCODE_STUB		0x33f9
#define FESTA_UCODE_ENTRY		0x3400
/* Kept in the entry point while the rest of the image is loaded */
#define FESTA_UCODE_HOLD		0x39
#define FESTA_UCODE_MAGIC0		0x01
#define FESTA_UCODE_MAGIC1		0xcc

/* Written to FESTA_UCODE_STUB before the image itself */
static const u8 festa_ucode_prologue[] = { 0xbd, 0x34, 0x00, 0x39 };

/*
 * The expanded memory window is addressed one byte at a time: MII_EXPMA
 * selects the byte, MII_EXPMD carries it.
 */
static int festa_write_byte(struct phy_device *phydev, unsigned int addr,
			    u8 val)
{
	int ret;

	lockdep_assert_held(&phydev->mdio.bus->mdio_lock);

	ret = __phy_write(phydev, MII_EXPMA, addr);
	if (ret < 0)
		return ret;

	return __phy_write(phydev, MII_EXPMD, val);
}

static int festa_write_bytes(struct phy_device *phydev, unsigned int addr,
			     const u8 *data, size_t len)
{
	int ret;

	while (len--) {
		ret = festa_write_byte(phydev, addr++, *data++);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * The PHY only takes microcode while powered down, and the entry point is
 * released last so that the core cannot run a partially loaded image.
 */
static int festa_load_fw(struct phy_device *phydev,
			 const struct firmware *fw)
{
	int ret;
	int err;

	phy_lock_mdio_bus(phydev);

	ret = __phy_set_bits(phydev, MII_BMCR, BMCR_PDOWN);
	if (ret < 0)
		goto out;

	ret = festa_write_bytes(phydev, FESTA_UCODE_STUB, festa_ucode_prologue,
				ARRAY_SIZE(festa_ucode_prologue));
	if (ret < 0)
		goto power_up;

	ret = festa_write_byte(phydev, FESTA_UCODE_ENTRY, FESTA_UCODE_HOLD);
	if (ret < 0)
		goto power_up;

	ret = festa_write_bytes(phydev, FESTA_UCODE_ENTRY + 1,
				fw->data + 1, fw->size - 1);
	if (ret < 0)
		goto power_up;

	ret = festa_write_byte(phydev, FESTA_UCODE_ENTRY, fw->data[0]);
	if (ret < 0)
		goto power_up;

	ret = festa_write_byte(phydev, FESTA_UCODE_START, 1);

power_up:
	err = __phy_clear_bits(phydev, MII_BMCR, BMCR_PDOWN);
	if (!ret)
		ret = err;
out:
	phy_unlock_mdio_bus(phydev);

	return ret;
}

static int festa_patch_fw(struct phy_device *phydev)
{
	const struct firmware *fw;
	char fw_name[64];
	int ret;

	snprintf(fw_name, sizeof(fw_name), "hisilicon/festa.%08x.ucode",
		 phydev->phy_id);

	ret = firmware_request_nowarn(&fw, fw_name, &phydev->mdio.dev);
	if (ret < 0) {
		/* No image installed, the PHY keeps whatever it runs now */
		if (ret == -ENOENT)
			return 0;
		phydev_err(phydev, "failed to request firmware %s: %d\n",
			   fw_name, ret);
		return ret;
	}

	if (fw->size < 2 || fw->data[0] != FESTA_UCODE_MAGIC0 ||
	    fw->data[1] != FESTA_UCODE_MAGIC1) {
		phydev_err(phydev, "%s does not look like valid firmware, refusing to load\n",
			   fw_name);
		ret = -EINVAL;
		goto out;
	}

	ret = festa_load_fw(phydev, fw);
	if (ret < 0) {
		phydev_err(phydev, "failed to download firmware %s: %d\n",
			   fw_name, ret);
		goto out;
	}

	phydev_info(phydev, "loaded firmware %s\n", fw_name);

out:
	release_firmware(fw);
	return ret;
}

static int festa_config_init(struct phy_device *phydev)
{
	return festa_patch_fw(phydev);
}

#define HISI_FESTA_DRIVER(_id, _name)				\
	{							\
		PHY_ID_MATCH_MODEL(_id),			\
		.name		= (_name),			\
		.config_init	= festa_config_init,	\
	}

static struct phy_driver festa_drivers[] = {
	HISI_FESTA_DRIVER(PHY_ID_HISILICON_FESTAV200,
			  "HiSilicon Festa v200/v210"),
	HISI_FESTA_DRIVER(PHY_ID_HISILICON_FESTAV220, "HiSilicon Festa v220"),
	HISI_FESTA_DRIVER(PHY_ID_HISILICON_FESTAV300, "HiSilicon Festa v300"),
	HISI_FESTA_DRIVER(PHY_ID_HISILICON_FESTAV320, "HiSilicon Festa v320"),
	HISI_FESTA_DRIVER(PHY_ID_HISILICON_FESTAV330, "HiSilicon Festa v330"),
	HISI_FESTA_DRIVER(PHY_ID_HISILICON_FESTAV331, "HiSilicon Festa v331"),
};

module_phy_driver(festa_drivers);

static const struct mdio_device_id __maybe_unused festa_mdio_ids[] = {
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV200) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV220) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV300) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV320) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV330) },
	{ PHY_ID_MATCH_MODEL(PHY_ID_HISILICON_FESTAV331) },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(mdio, festa_mdio_ids);
MODULE_DESCRIPTION("HiSilicon Festa PHY driver");
MODULE_LICENSE("Dual MIT/GPL");
