/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2023 David Yang
 *
 * Silicon Image SiI9134 HDMI 1.4 transmitter core, as integrated in
 * HiSilicon STB SoCs (hi3798mv100 and friends).
 */

#ifndef __SII9134_H__
#define __SII9134_H__

#include <linux/mutex.h>

#include <drm/drm_bridge.h>
#include <sound/hdmi-codec.h>

struct device;
struct drm_bridge;
struct drm_bridge_funcs;

extern const struct drm_bridge_funcs sii9134_bridge_funcs;
struct drm_connector;
struct i2c_adapter;
struct platform_device;
struct regmap;

/**
 * struct sii9134 - SiI9134 HDMI transmitter core
 *
 * The wrapper (histb-hdmi-14.c) owns the MMIO region and the clocks/resets.
 * Register access goes through three regmaps, all 8-bit registers on a
 * 4-byte stride:
 *
 *  - @map:   bank 0, I2C slave 0x72 in the original Silicon Image design
 *            (TPI / system / timing / interrupt / master-DDC registers),
 *            MMIO offset 0x0000..0x03ff
 *  - @map1:  bank 1, I2C slave 0x7a (audio / ACR / infoframe packet
 *            memory), MMIO offset 0x0400..0x07ff
 *  - @map_phy: the on-chip HDMI PHY ("Taiyan" PHY on hi3798mv100),
 *            32-bit registers, MMIO offset 0x1800
 *
 * Connector convention: this driver does NOT create a drm_connector; the
 * downstream (VO, DRM master) side creates one with
 * drm_bridge_connector_init().  The bridge implements
 * DRM_BRIDGE_OP_DETECT / DRM_BRIDGE_OP_EDID; DRM_BRIDGE_OP_HPD is left off
 * because the INT output cannot be made to deassert while a sink holds
 * HPD, so the DRM core polls detect instead (see histb-hdmi-14.c).
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
	 * Wrapper-provided CRG reset halves, used in the SDK's order
	 * (si_hdmitx.c:164-200): SI_HW_ResetCtrl(1) + SI_HW_ResetPhy(1),
	 * release the PHY, SI_TX_PHY_INIT(), only then SI_HW_ResetCtrl(0).
	 * Configuring the PHY while the controller is still held is what
	 * makes the difference between a link that comes up and one that
	 * stays dead.  Both are optional.
	 */
	void (*reset_hold)(struct sii9134 *sii);
	void (*reset_release)(struct sii9134 *sii);

	/*
	 * Single lock serialising every read-modify-write on shared
	 * registers (modeset, audio, infoframe, HPD).  The vendor SDK is
	 * fully serialised; using separate modeset/audio locks here
	 * allowed lost updates on INF_CTRL1 / DATA_CTRL.
	 */
	struct mutex lock;

	/*
	 * hdmi-codec state.  @connector is borrowed from the downstream
	 * bridge connector (created by the DRM master) and cached while
	 * the pipeline is enabled; it is used to export the ELD to ALSA.
	 */
	struct drm_connector *connector;
	hdmi_codec_plugged_cb plugged_cb;
	struct device *codec_dev;

	/*
	 * Video state the audio side has to respect: the ACR N value is not a
	 * function of the sample rate alone, 4K formats use their own table
	 * (SDK si_audio.c:226-241), so a mode change has to recompute it while
	 * the stream stays up.  @audio_fs_code is the SiI Fs code of the last
	 * configured stream, or -1 before the first one; both fields are
	 * written and read under @lock.
	 */
	bool video_4k;
	int audio_fs_code;

	/*
	 * Whether the sink the last modeset saw is HDMI (@true) or a plain DVI
	 * panel (@false); DVI has no audio channel and no infoframe reader, so
	 * the audio side has to know.  Until the first modeset this is true,
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
