// SPDX-License-Identifier: GPL-2.0
/*
 * Keymap for the Skyworth YK-series set-top-box remote (NEC extended).
 *
 * Scancodes transcribed from the vendor table for this remote.  The command
 * byte needs no byte-order conversion: the vendor's own HiSilicon-remote table
 * and mainline's rc-hisi-poplar.c agree on it key for key (0x92/0x93/0xcc for
 * digits 1/2/3), mainline only adding the address.  The two address bytes are
 * taken from that table as-is and are not yet confirmed by a decoded press.
 */

#include <linux/array_size.h>
#include <linux/module.h>
#include <media/rc-map.h>

static struct rc_map_table skyworth_yk[] = {
	{ 0x0e0e0c, KEY_POWER },
	{ 0x0e0e17, KEY_IMAGES },
	{ 0x0e0e18, KEY_AUDIO },
	{ 0x0e0e4f, KEY_VIDEO },
	{ 0x0e0e01, KEY_1 },
	{ 0x0e0e02, KEY_2 },
	{ 0x0e0e03, KEY_3 },
	{ 0x0e0e04, KEY_4 },
	{ 0x0e0e05, KEY_5 },
	{ 0x0e0e06, KEY_6 },
	{ 0x0e0e07, KEY_7 },
	{ 0x0e0e08, KEY_8 },
	{ 0x0e0e09, KEY_9 },
	{ 0x0e0e00, KEY_0 },
	{ 0x0e0e0b, KEY_FN_1 },
	{ 0x0e0e0a, KEY_FN_2 },
	{ 0x0e0e1a, KEY_FN_B },
	{ 0x0e0e1c, KEY_FN_D },
	{ 0x0e0e16, KEY_INFO },
	{ 0x0e0e11, KEY_MENU },
	{ 0x0e0e53, KEY_FN_E },
	{ 0x0e0e0f, KEY_FN_F },
	{ 0x0e0e46, KEY_OK },
	{ 0x0e0e42, KEY_UP },
	{ 0x0e0e44, KEY_LEFT },
	{ 0x0e0e45, KEY_RIGHT },
	{ 0x0e0e43, KEY_DOWN },
	{ 0x0e0e5b, KEY_BACK },
	{ 0x0e0e51, KEY_FN_S },
	{ 0x0e0e12, KEY_CHANNELUP },
	{ 0x0e0e13, KEY_CHANNELDOWN },
	{ 0x0e0e14, KEY_VOLUMEUP },
	{ 0x0e0e15, KEY_VOLUMEDOWN },
	{ 0x0e0e0d, KEY_MUTE },
	{ 0x0e0e40, KEY_REWIND },
	{ 0x0e0e49, KEY_PLAYPAUSE },
	{ 0x0e0e4a, KEY_STOP },
	{ 0x0e0e41, KEY_FORWARD },
	{ 0x0e0e47, KEY_PREVIOUS },
	{ 0x0e0e48, KEY_NEXT },
};

static struct rc_map_list skyworth_yk_map = {
	.map = {
		.scan = skyworth_yk,
		.size = ARRAY_SIZE(skyworth_yk),
		.rc_proto = RC_PROTO_NECX,
		.name = RC_MAP_SKYWORTH_YK,
	}
};

static int __init init_rc_map_skyworth_yk(void)
{
	return rc_map_register(&skyworth_yk_map);
}

static void __exit exit_rc_map_skyworth_yk(void)
{
	rc_map_unregister(&skyworth_yk_map);
}

module_init(init_rc_map_skyworth_yk)
module_exit(exit_rc_map_skyworth_yk)

MODULE_AUTHOR("David Yang <mmyangfl@gmail.com>");
MODULE_DESCRIPTION("Skyworth YK remote controller keytable");
MODULE_LICENSE("GPL");
