/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson 2K0300 pin mux.
 *
 * Every GPIO pin owns two bits in the GPIO_CFG registers at 0x1600_0490, 16
 * pins per 32 bit register: pin n lives in GPIO_CFG(n / 16) at bit
 * 2 * (n % 16). GPIO_CFG2 therefore holds GPIO32..GPIO47 and with it the
 * console pins GPIO40 (uart0_rx) and GPIO41 (uart0_tx), which the vendor
 * device tree configures as
 *
 *   pinctrl-single,bits = <0x8 0x000f0000 0x000f0000>;
 *
 * i.e. both fields set to 3, the primary function.
 *
 * The value selects the function of the pin: 0 GPIO, 1 first alternate,
 * 2 second alternate, 3 primary function. A state is described in the device
 * tree as (pin, function) pairs, see pinctrl_soc.h and the binding
 * dts/bindings/pinctrl/loongson,ls2k0300-pinmux.yaml.
 *
 * Note that u-boot leaves the console pins configured, which is why the
 * console works before this driver ever runs - but only for the pins u-boot
 * touched: uart0_rx was observed stuck in the "GPIO" setting, so the console
 * transmitted but never received, and the shell could not be typed into.
 *
 * The SoC provides this function directly instead of a driver instance: the
 * pinctrl framework only needs pinctrl_configure_pins() plus the controller's
 * reg address, both of which the generated pinctrl device config carries.
 */

#include <zephyr/arch/common/sys_io.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/sys/util.h>

/*
 * The mux block belongs to the SoC, not to the pin, so its address is taken
 * from the pin controller node here rather than from the third argument of
 * pinctrl_configure_pins(): the framework fills that one with the *consumer's*
 * register base (uart0's, for the console state), which is not where the mux
 * fields live.
 */
#define LS2K_PINMUX_BASE DT_REG_ADDR(DT_NODELABEL(pinctrl))

/* GPIO_CFG registers hold 16 two bit pin fields each */
#define LS2K_PINS_PER_REG	16U
#define LS2K_REG_STRIDE		4U
#define LS2K_PIN_FIELD_MASK	0x3U

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	ARG_UNUSED(reg);

	for (uint8_t i = 0U; i < pin_cnt; i++) {
		mem_addr_t addr = LS2K_PINMUX_BASE +
				  ((uint32_t)pins[i].pin / LS2K_PINS_PER_REG) *
					  LS2K_REG_STRIDE;
		uint32_t shift = ((uint32_t)pins[i].pin % LS2K_PINS_PER_REG) * 2U;
		uint32_t value = sys_read32(addr);

		value &= ~(LS2K_PIN_FIELD_MASK << shift);
		value |= ((uint32_t)pins[i].function & LS2K_PIN_FIELD_MASK) << shift;

		sys_write32(value, addr);
	}

	return 0;
}
