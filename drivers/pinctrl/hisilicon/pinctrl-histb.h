/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <linux/bitops.h>

/* per-register bit definition */
#define HISTB_PIN_SCHMITT	BIT(14)
#define HISTB_PIN_PULLDOWN	BIT(13)
#define HISTB_PIN_PULLUP	BIT(12)
#define HISTB_PIN_SLEWRATE	BIT(8)
#define HISTB_PIN_DRV_MASK	GENMASK(7, 4)
#define HISTB_PIN_FUNC_MASK	GENMASK(2, 0)

/**
 * @histb_pin_desc flags
 *
 * @HISTB_PIN_FLAG_NOPU: This pin does not support "bias-pullup"
 * @HISTB_PIN_FLAG_NOPD: This pin does not support "bias-pulldown"
 * @HISTB_PIN_FLAG_NOSR: This pin does not support "slew-rate"
 * @HISTB_PIN_FLAG_SCHMITT: This pin supports setting input schmitt
 */
#define HISTB_PIN_FLAG_NOPU	BIT(0)
#define HISTB_PIN_FLAG_NOPD	BIT(1)
#define HISTB_PIN_FLAG_NOSR	BIT(2)
#define HISTB_PIN_FLAG_SCHMITT	BIT(3)

/* Reserve function 0 for GPIO */
#define HISTB_PIN_FUNCTION_GPIO		0
#define HISTB_PIN_FUNCTION_EOF		0xFFFFFFFF

/**
 * histb_pin_mux_desc - a descriptor for one function of a pin
 *
 * @func: the enumeration for the function
 * @bits: the bits pattern for this function
 */
struct histb_pin_mux_desc {
	u32 func;
	u32 bits;
};

/**
 * histb_pin_data - full info of a pin
 *
 * @drv_tbl: drive strength table, end with 0
 * @func_tbl: pinmux table, end with { }
 * @flags: pin flags, see HISTB_PIN_FLAG_* macros
 */
struct histb_pin_data {
	const u8 *drv_tbl;
	const struct histb_pin_mux_desc *func_tbl;
	u32 flags;
};

/**
 * HISTB_PIN() - a helper macro for initializing histb pin desc
 */
#define HISTB_PIN(_index, _name, _drv_tbl, _func_tbl, _flag) { \
	.number = _index, \
	.name = _name, \
	.drv_data = &(struct histb_pin_data){ \
		.drv_tbl = _drv_tbl, \
		.func_tbl = _func_tbl, \
		.flags = _flag, \
	} \
}

struct pinctrl_ops;
struct pinmux_ops;
struct pinconf_ops;

extern const struct pinctrl_ops histb_pinctrl_pctl_ops;
extern const struct pinmux_ops histb_pinctrl_pinmux_ops;
extern const struct pinconf_ops histb_pinctrl_pinconf_ops;
