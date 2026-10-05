/*
 * Copyright (c) 2026 Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson 2K0300 pin mux, SoC side of the Zephyr pinctrl framework.
 *
 * Every GPIO pin owns two bits in the GPIO_CFG registers at 0x1600_0490 (see
 * pinctrl.c). A state node describes exactly one pin as a (pin, function) pair,
 * and a consumer lists one phandle per pin:
 *
 *   pinctrl {
 *           uart0_rx_default: uart0_rx_default { pinmux = <40 3>; };
 *           uart0_tx_default: uart0_tx_default { pinmux = <41 3>; };
 *   };
 *
 *   &uart0 {
 *           pinctrl-0 = <&uart0_rx_default &uart0_tx_default>;
 *           pinctrl-names = "default";
 *   };
 *
 * The first cell is the GPIO number, the second the value written into its two
 * bit field: 0 GPIO, 1 first alternate, 2 second alternate, 3 primary function.
 * One pair per state keeps the cell indices literal, which DT_PROP_BY_IDX
 * requires - it pastes the index into a generated symbol name and cannot take
 * an expression like "2 * idx".
 */

#ifndef ZEPHYR_SOC_LOONGSON_LS2K300_PINCTRL_SOC_H_
#define ZEPHYR_SOC_LOONGSON_LS2K300_PINCTRL_SOC_H_

#include <zephyr/devicetree.h>
#include <zephyr/types.h>

typedef struct pinctrl_soc_pin_t {
	uint8_t pin;
	uint8_t function;
} pinctrl_soc_pin_t;

/* One phandle of pinctrl-<n> is one pin: read its (pin, function) pair */
#define LS2K0300_DT_PIN(node_id, prop, idx)					\
	{									\
		.pin = DT_PROP_BY_IDX(DT_PHANDLE_BY_IDX(node_id, prop, idx),	\
				      pinmux, 0),				\
		.function = DT_PROP_BY_IDX(DT_PHANDLE_BY_IDX(node_id, prop, idx), \
					   pinmux, 1),				\
	},

#define Z_PINCTRL_STATE_PINS_INIT(node_id, prop)			\
	{ DT_FOREACH_PROP_ELEM(node_id, prop, LS2K0300_DT_PIN) }

#endif /* ZEPHYR_SOC_LOONGSON_LS2K300_PINCTRL_SOC_H_ */
