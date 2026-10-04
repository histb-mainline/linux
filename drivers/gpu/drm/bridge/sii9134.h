/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2023 David Yang
 *
 * Silicon Image SiI9134 HDMI 1.4 transmitter core.
 */

#ifndef __SII9134_H__
#define __SII9134_H__

#include <linux/mutex.h>

#include <drm/drm_bridge.h>
#include <sound/hdmi-codec.h>

struct sii9134;
struct device;
struct drm_bridge;
struct drm_bridge_funcs;

extern const struct drm_bridge_funcs sii9134_bridge_funcs;
struct drm_connector;
struct i2c_adapter;
struct platform_device;
struct regmap;

/**
 * struct sii9134_phy_ops - PHY programming supplied by the SoC glue
 *
 * The TMDS PHY is not part of the SiI9134 register set this core drives: the
 * integrations it is used in wire a separate 32-bit register window to it,
 * whose bring-up values and swing table are per SoC and appear in no SiI
 * datasheet.
 *
 * @init:	program the PHY once, before the first mode set
 * @power:	TMDS output on (@true) or off (@false)
 * @swing:	select the output drive strength for a TMDS clock in kHz
 * @dvi_enc_in_ctrl:	the DVI/TMDS encoder lives in the SiI controller rather
 *			than in the PHY, so the core has to clear
 *			TEST_TX.DVI_ENC_BYPASS. Measured on hi3798mv100, where the
 *			link only comes up with it cleared. For another part: check
 *			its HDMI Tx chapter for which side owns TEST_TX.
 */
struct sii9134_phy_ops {
	void (*init)(struct sii9134 *sii);
	void (*power)(struct sii9134 *sii, bool on);
	void (*swing)(struct sii9134 *sii, unsigned int tmds_khz);
	bool dvi_enc_in_ctrl;
};

/**
 * struct sii9134 - SiI9134 HDMI transmitter core
 *
 * The SoC glue (hi3798mv100-hdmi.c) owns the MMIO region and the clocks/resets.
 * Register access goes through three regmaps, all 8-bit registers on a 4-byte
 * stride:
 *
 *  - @map:   bank 0, I2C slave 0x72 in the original Silicon Image design
 *            (TPI / system / timing / interrupt / master-DDC registers),
 *            MMIO offset 0x0000..0x03ff
 *  - @map1:  bank 1, I2C slave 0x7a (audio / ACR / infoframe packet
 *            memory), MMIO offset 0x0400..0x07ff
 *  - @map_phy: the integrated TMDS PHY window, 32-bit registers,
 *            programmed by the glue through @phy
 *
 * Connector convention: this driver does NOT create a drm_connector; the
 * downstream (display, DRM master) side creates one with
 * drm_bridge_connector_init(). The bridge implements DRM_BRIDGE_OP_DETECT /
 * DRM_BRIDGE_OP_EDID; DRM_BRIDGE_OP_HPD is left off because the INT output
 * cannot be made to deassert while a sink holds HPD, so the DRM core polls
 * detect instead (see the SoC glue).
 */
struct sii9134 {
	struct device *dev;

	struct regmap *map;
	struct regmap *map1;
	struct regmap *map_phy;

	struct drm_bridge bridge;

	/* the transmitter's own master DDC, registered as an I2C bus */
	struct i2c_adapter *ddc;

	int irq;

	/*
	 * Wrapper-provided CRG reset halves. They are taken in a split order:
	 * assert both, release the PHY, configure the PHY, and only then
	 * release the controller. Configuring the PHY while the controller is
	 * still held is what makes the difference between a link that comes up
	 * and one that stays dead. Both are optional.
	 */
	void (*reset_hold)(struct sii9134 *sii);
	void (*reset_release)(struct sii9134 *sii);

	/* Required: the PHY programming of this integration. */
	const struct sii9134_phy_ops *phy;

	/*
	 * Single lock serialising every read-modify-write on shared registers
	 * (modeset, audio, infoframe, HPD): separate locks per callback lose
	 * updates on INF_CTRL1 / DATA_CTRL_ADDR.
	 */
	struct mutex lock;

	/*
	 * hdmi-codec state. @connector is borrowed from the downstream bridge
	 * connector (created by the DRM master) and cached while the pipeline
	 * is enabled; it is used to export the ELD to ALSA.
	 */
	struct drm_connector *connector;
	hdmi_codec_plugged_cb plugged_cb;
	struct device *codec_dev;

	/*
	 * Video state the audio side has to respect: the ACR N value is not a
	 * function of the sample rate alone -- 4K formats use a table of their
	 * own -- so a mode change has to recompute it while the stream stays up.
	 * @audio_fs_code is the SiI Fs code of the last configured stream, or -1
	 * before the first one; both fields are written and read under @lock.
	 */
	bool video_4k;
	int audio_fs_code;

	/*
	 * Whether the sink the last modeset saw is HDMI (@true) or a plain DVI
	 * panel (@false); DVI has no audio channel and no infoframe reader, so
	 * the audio side has to know. Until the first modeset this is true,
	 * because a stream opened before the display came up should not be
	 * silently dropped.
	 */
	bool hdmi_sink;

	struct platform_device *audio_pdev;
};

int sii9134_probe(struct sii9134 *sii9134, int irq);
void sii9134_remove(struct sii9134 *sii9134);
void sii9134_bridge_add(struct sii9134 *sii9134);
void sii9134_bridge_remove(struct sii9134 *sii9134);

#endif
