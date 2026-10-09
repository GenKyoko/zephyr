/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson LS2K "byte controlled" GPIO driver (2K0300 and compatible IP).
 *
 * Register layout and semantics are taken from the mainline Linux driver
 * drivers/gpio/gpio-loongson-64bit.c, BYTE_CTRL_MODE (used by the 2K0300):
 *
 *   conf + pin : bit0 = 1 -> input, bit0 = 0 -> output
 *   out  + pin : bit0 = output level; the level is written first and the
 *                direction switched afterwards, as done by
 *                loongson_gpio_direction_output() in mainline
 *   in   + pin : bit0 = input level
 *
 * Every pin owns one byte in each region, so the driver addresses pins with
 * byte accesses. A controller instance covers "ngpios" pins starting at the
 * block pin index "loongson,pin-base"; the SoC device tree splits the IP
 * into several banks so that the Zephyr pin numbers of one instance stay
 * within the 32 bit port mask of the GPIO API.
 *
 * Interrupt support (inten/intpol/intedge/intsts regions) is not implemented
 * yet.
 */

#define DT_DRV_COMPAT loongson_ls2k0300_gpio

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define LIO_CONF_OFFSET 0x800U
#define LIO_OUT_OFFSET  0x900U
#define LIO_IN_OFFSET   0xa00U

struct gpio_loongson_cfg {
	/* Must be the first member: the GPIO API layer in drivers/gpio.h
	 * casts port->config to this type and reads port_pin_mask from it.
	 */
	struct gpio_driver_config common;
	mem_addr_t base;
	uint16_t pin_base;
	uint16_t ngpios;
};

/*
 * Must be the first member here too: z_impl_gpio_pin_configure() casts
 * port->data to struct gpio_driver_data * and updates data->invert on every
 * gpio_pin_configure() call. With a NULL device data pointer that store goes
 * to address 0, which on the 2K0300 - where address 0 needs a TLB entry the
 * kernel does not own - raises a TLB refill exception that lands in u-boot's
 * handler and kills the boot in the middle of the LED initialisation.
 */
struct gpio_loongson_data {
	struct gpio_driver_data common;
};

static inline void lio_write(const struct gpio_loongson_cfg *cfg, uint32_t offset,
			     gpio_pin_t pin, uint8_t value)
{
	sys_write8(value, cfg->base + offset + cfg->pin_base + pin);
}

static inline uint8_t lio_read(const struct gpio_loongson_cfg *cfg, uint32_t offset,
			       gpio_pin_t pin)
{
	return sys_read8(cfg->base + offset + cfg->pin_base + pin);
}

static int lio_pin_configure(const struct device *dev, gpio_pin_t pin,
			     gpio_flags_t flags)
{
	const struct gpio_loongson_cfg *cfg = dev->config;

	if (pin >= cfg->ngpios) {
		return -EINVAL;
	}

	/* No open drain / pull configuration in this IP */
	if ((flags & GPIO_SINGLE_ENDED) != 0 ||
	    (flags & (GPIO_PULL_UP | GPIO_PULL_DOWN)) != 0) {
		return -ENOTSUP;
	}

	if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0) {
		lio_write(cfg, LIO_OUT_OFFSET, pin, 1U);
	} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0) {
		lio_write(cfg, LIO_OUT_OFFSET, pin, 0U);
	}

	if ((flags & GPIO_INPUT) != 0) {
		lio_write(cfg, LIO_CONF_OFFSET, pin, 1U);
	} else if ((flags & GPIO_OUTPUT) != 0) {
		lio_write(cfg, LIO_CONF_OFFSET, pin, 0U);
	}

	return 0;
}

static int lio_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct gpio_loongson_cfg *cfg = dev->config;
	gpio_port_value_t val = 0U;

	for (uint32_t i = 0U; i < cfg->ngpios; i++) {
		if ((lio_read(cfg, LIO_IN_OFFSET, i) & 1U) != 0U) {
			val |= BIT(i);
		}
	}

	*value = val;

	return 0;
}

static int lio_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
				   gpio_port_value_t value)
{
	const struct gpio_loongson_cfg *cfg = dev->config;

	for (uint32_t i = 0U; i < cfg->ngpios; i++) {
		if ((mask & BIT(i)) != 0U) {
			lio_write(cfg, LIO_OUT_OFFSET, (gpio_pin_t)i,
				  ((value & BIT(i)) != 0U) ? 1U : 0U);
		}
	}

	return 0;
}

static int lio_port_set_bits_raw(const struct device *dev, gpio_port_pins_t mask)
{
	return lio_port_set_masked_raw(dev, mask, mask);
}

static int lio_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t mask)
{
	return lio_port_set_masked_raw(dev, mask, 0U);
}

static int lio_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct gpio_loongson_cfg *cfg = dev->config;

	/*
	 * The IP has no toggle register: read the level out of the "out" region
	 * of every pin in the mask and write the inverse back. gpio_pin_toggle()
	 * reaches this through gpio_port_toggle_bits() without a NULL check, so
	 * this member is not optional for a GPIO driver.
	 */
	for (uint32_t i = 0U; i < cfg->ngpios; i++) {
		if ((pins & BIT(i)) != 0U) {
			uint8_t level = lio_read(cfg, LIO_OUT_OFFSET, (gpio_pin_t)i);

			lio_write(cfg, LIO_OUT_OFFSET, (gpio_pin_t)i,
				  ((level & 1U) != 0U) ? 0U : 1U);
		}
	}

	return 0;
}

#ifdef CONFIG_GPIO_GET_CONFIG
static int lio_pin_get_config(const struct device *dev, gpio_pin_t pin,
			      gpio_flags_t *flags)
{
	const struct gpio_loongson_cfg *cfg = dev->config;
	const struct gpio_loongson_data *data = dev->data;

	if ((pin >= cfg->ngpios) || (flags == NULL)) {
		return -EINVAL;
	}

	/* conf bit 0: 1 = input, 0 = output */
	*flags = ((lio_read(cfg, LIO_CONF_OFFSET, pin) & 1U) != 0U)
			 ? (gpio_flags_t)GPIO_INPUT
			 : (gpio_flags_t)GPIO_OUTPUT;

	/* The API layer keeps the active low mask in the device data */
	if ((data->common.invert & (gpio_port_pins_t)BIT(pin)) != 0U) {
		*flags |= GPIO_ACTIVE_LOW;
	}

	return 0;
}
#endif /* CONFIG_GPIO_GET_CONFIG */

static int lio_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
				       enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(pin);
	ARG_UNUSED(mode);
	ARG_UNUSED(trig);

	return -ENOTSUP;
}

static int lio_manage_callback(const struct device *dev, struct gpio_callback *cb,
			       bool set)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(set);

	return -ENOTSUP;
}

static int lio_init(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static const struct gpio_driver_api lio_api = {
	.pin_configure = lio_pin_configure,
#ifdef CONFIG_GPIO_GET_CONFIG
	.pin_get_config = lio_pin_get_config,
#endif
	.port_get_raw = lio_port_get_raw,
	.port_set_masked_raw = lio_port_set_masked_raw,
	.port_set_bits_raw = lio_port_set_bits_raw,
	.port_clear_bits_raw = lio_port_clear_bits_raw,
	.port_toggle_bits = lio_port_toggle_bits,
	.pin_interrupt_configure = lio_pin_interrupt_configure,
	.manage_callback = lio_manage_callback,
};

#define LIO_GPIO_INIT(n)							\
	static const struct gpio_loongson_cfg lio_cfg_##n = {			\
		.common = {							\
			.port_pin_mask = (gpio_port_pins_t)BIT_MASK(		\
				DT_INST_PROP(n, ngpios)),			\
		},								\
		.base = DT_INST_REG_ADDR(n),					\
		.pin_base = DT_INST_PROP_OR(n, loongson_pin_base, 0),		\
		.ngpios = DT_INST_PROP(n, ngpios),				\
	};									\
	static struct gpio_loongson_data lio_data_##n;				\
										\
	DEVICE_DT_INST_DEFINE(n, lio_init, NULL, &lio_data_##n, &lio_cfg_##n,	\
			      PRE_KERNEL_1, CONFIG_GPIO_INIT_PRIORITY, &lio_api);

DT_INST_FOREACH_STATUS_OKAY(LIO_GPIO_INIT)
