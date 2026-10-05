/*
 * Copyright (c) 2026 Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson 2K0300 clock controller.
 *
 * The block holds the three PLLs of the SoC and the per bus frequency scalers.
 * This driver uses it read only: the boot loader has already locked the PLLs
 * when it hands over and reprogramming them from the kernel - the DDR PLL in
 * particular - would take the board down, so there is deliberately no
 * clock_control_configure() implementation here. What devices need is the rate
 * the hardware is actually running at, above all the APB clock the NS16550
 * console divides down for its baud rate.
 *
 * Clock tree (field layout from the reference manual and u-boot's
 * arch/loongarch/mach-loongson/include/mach/ls2k300/ls2k300.h):
 *
 *   ref_clk (fixed-clock from DT, 120 MHz on this board)
 *     node_pll = ref * LOOPC / REFC                        NODE_PLL_L
 *       cpu / scache / iodma = node_pll / ODIV * freqscale(NODE)
 *       gmac                 = node_pll / ODIV_GMAC
 *       i2s                  = node_pll / ODIV_I2S * freqscale(I2S)
 *     ddr_pll = ref * LOOPC / REFC                         DDR_PLL_L
 *       ddr = ddr_pll / ODIV
 *       net = ddr_pll / ODIV_NET
 *       devs = ddr_pll / ODIV_DEVS
 *         usb / apb / boot / sdio = devs * freqscale(...)
 *     pix_pll = ref * LOOPC / REFC                         PIX0_PLL
 *       pix = pix_pll / ODIV
 *
 * A freqscale field is one nibble of FREQ_SCALE; its bit 3 selects the mode:
 *
 *   mode 0: rate = parent * (value + 1) / 8
 *   mode 1: rate = parent / (value + 1)
 *
 * Cross check against what the PAI u-boot programs (REF 120 MHz, CORE 800 MHz,
 * DDR 800 MHz, APB 200 MHz, see u-boot's include/configs/loongson_2k300.h):
 *
 *   node_pll = 120 * 80 / 6 = 1600 MHz, cpu = 1600 / 2 = 800 MHz
 *   ddr_pll  = 120 * 80 / 3 = 3200 MHz, ddr = 3200 / 4 = 800 MHz
 *   devs     = 3200 / 16    =  200 MHz - the APB clock, which is also the rate
 *   measured on the console UART of this board.
 */

#define DT_DRV_COMPAT loongson_ls2k0300_clk

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/dt-bindings/clock/loongson,ls2k0300-clk.h>
#include <zephyr/arch/common/sys_io.h>
#include <zephyr/sys/util.h>

/* Register offsets inside the clock block */
#define LS2K_CLK_NODE_PLL_L	0x00u
#define LS2K_CLK_NODE_PLL_H	0x04u
#define LS2K_CLK_DDR_PLL_L	0x08u
#define LS2K_CLK_DDR_PLL_H	0x0cu
#define LS2K_CLK_PIX0_PLL	0x10u
#define LS2K_CLK_FREQ_SCALE	0x20u
#define LS2K_CLK_EN		0x24u

/* First level divider fields, identical for all three PLLs */
#define LS2K_CLK_ODIV_SHIFT	24u
#define LS2K_CLK_ODIV_MASK	0x7fu
#define LS2K_CLK_LOOPC_SHIFT	15u
#define LS2K_CLK_LOOPC_MASK	0x1ffu
#define LS2K_CLK_REFC_SHIFT	8u
#define LS2K_CLK_REFC_MASK	0x7fu

/* Second level dividers: plain seven bit fields, no frequency scaler */
#define LS2K_CLK_L2_MASK	0x7fu
#define LS2K_CLK_NODE1_GMAC_SHIFT	0u	/* NODE_PLL_H */
#define LS2K_CLK_NODE1_I2S_SHIFT	8u
#define LS2K_CLK_DDR1_NET_SHIFT		0u	/* DDR_PLL_H */
#define LS2K_CLK_DDR1_DEVS_SHIFT	8u

/* FREQ_SCALE nibbles: bit 3 is the mode, bits [2:0] the value */
#define LS2K_CLK_FS_MASK	0xfu
#define LS2K_CLK_FS_MODE	0x8u
#define LS2K_CLK_FS_VALUE	0x7u
#define LS2K_CLK_FS_NODE_SHIFT	0u
#define LS2K_CLK_FS_BOOT_SHIFT	8u
#define LS2K_CLK_FS_USB_SHIFT	12u
#define LS2K_CLK_FS_APB_SHIFT	16u
#define LS2K_CLK_FS_I2S_SHIFT	20u
#define LS2K_CLK_FS_SDIO_SHIFT	24u

/* LS2K_CLK_EN, the PLL output enables (bit numbers as in the manual) */
#define LS2K_CLK_EN_NODE	BIT(0)
#define LS2K_CLK_EN_BOOT	BIT(1)
#define LS2K_CLK_EN_USB		BIT(2)
#define LS2K_CLK_EN_APB		BIT(3)
#define LS2K_CLK_EN_SDIO	BIT(4)
#define LS2K_CLK_EN_PIX		BIT(6)

struct ls2k0300_clk_config {
	mem_addr_t base;
	const struct device *ref_clk;
};

static uint32_t ls2k0300_clk_read(const struct device *dev, uint32_t offset)
{
	const struct ls2k0300_clk_config *cfg = dev->config;

	return sys_read32(cfg->base + offset);
}

static uint32_t ls2k0300_clk_field(uint32_t value, uint32_t shift, uint32_t mask)
{
	return (value >> shift) & mask;
}

/* PLL output, i.e. the frequency before the second level divider */
static uint32_t ls2k0300_clk_pll(uint32_t pll_l, uint32_t ref_hz)
{
	uint32_t loopc = ls2k0300_clk_field(pll_l, LS2K_CLK_LOOPC_SHIFT,
					    LS2K_CLK_LOOPC_MASK);
	uint32_t refc = ls2k0300_clk_field(pll_l, LS2K_CLK_REFC_SHIFT,
					   LS2K_CLK_REFC_MASK);

	if ((loopc == 0u) || (refc == 0u)) {
		return 0u;
	}

	/* ref_hz * loopc does not fit into 32 bits */
	return (uint32_t)(((uint64_t)ref_hz * loopc) / refc);
}

static uint32_t ls2k0300_clk_div(uint32_t rate, uint32_t divider)
{
	return (divider == 0u) ? 0u : (rate / divider);
}

static uint32_t ls2k0300_clk_freqscale(uint32_t rate, uint32_t freq_scale, uint32_t shift)
{
	uint32_t field = ls2k0300_clk_field(freq_scale, shift, LS2K_CLK_FS_MASK);
	uint32_t value;

	/*
	 * A zero field means "scaler bypassed", not mode 0 with value 0 (which
	 * would be a divide by eight). Measured on this board: the APB clock is
	 * the undivided DEVS clock, 200 MHz, which is exactly the baud rate
	 * clock the console needs and which u-boot's own console divisor also
	 * assumes. So the hardware does not apply * (value + 1) / 8 in that
	 * state.
	 */
	if (field == 0u) {
		return rate;
	}

	value = (field & LS2K_CLK_FS_VALUE) + 1u;

	if ((field & LS2K_CLK_FS_MODE) == 0u) {
		return (uint32_t)(((uint64_t)rate * value) / 8u);
	}

	return rate / value;
}

/*
 * The SDIO scaler does not follow the nibble rules: the mainline driver divides
 * by the raw field and treats 0 and 1 both as "no division".
 */
static uint32_t ls2k0300_clk_freqscale_sdio(uint32_t rate, uint32_t freq_scale)
{
	uint32_t value = ls2k0300_clk_field(freq_scale, LS2K_CLK_FS_SDIO_SHIFT,
					    LS2K_CLK_FS_MASK);

	return (value == 0u) ? rate : (rate / value);
}

static int ls2k0300_clk_gate_bit(uint32_t id, uint32_t *bit)
{
	switch (id) {
	case LS2K0300_CLK_NODE:
		*bit = LS2K_CLK_EN_NODE;
		break;
	case LS2K0300_CLK_USB:
		*bit = LS2K_CLK_EN_USB;
		break;
	case LS2K0300_CLK_APB:
		*bit = LS2K_CLK_EN_APB;
		break;
	case LS2K0300_CLK_BOOT:
		*bit = LS2K_CLK_EN_BOOT;
		break;
	case LS2K0300_CLK_SDIO:
		*bit = LS2K_CLK_EN_SDIO;
		break;
	case LS2K0300_CLK_PIX:
		*bit = LS2K_CLK_EN_PIX;
		break;
	default:
		/* The PLLs themselves and the CPU / DDR / NET branches have no
		 * output enable.
		 */
		return -ENOTSUP;
	}

	return 0;
}

static int ls2k0300_clk_get_rate(const struct device *dev, clock_control_subsys_t sys,
				 uint32_t *rate)
{
	const struct ls2k0300_clk_config *cfg = dev->config;
	uint32_t id = (uint32_t)(uintptr_t)sys;
	uint32_t node_l, ddr_l, pix_l, node_h, ddr_h, freq_scale;
	uint32_t ref_hz, node_pll, ddr_pll, devs, value;

	if (clock_control_get_rate(cfg->ref_clk, NULL, &ref_hz) != 0) {
		return -ENODEV;
	}

	node_l = ls2k0300_clk_read(dev, LS2K_CLK_NODE_PLL_L);
	ddr_l = ls2k0300_clk_read(dev, LS2K_CLK_DDR_PLL_L);
	pix_l = ls2k0300_clk_read(dev, LS2K_CLK_PIX0_PLL);
	node_h = ls2k0300_clk_read(dev, LS2K_CLK_NODE_PLL_H);
	ddr_h = ls2k0300_clk_read(dev, LS2K_CLK_DDR_PLL_H);
	freq_scale = ls2k0300_clk_read(dev, LS2K_CLK_FREQ_SCALE);

	node_pll = ls2k0300_clk_pll(node_l, ref_hz);
	ddr_pll = ls2k0300_clk_pll(ddr_l, ref_hz);
	devs = ls2k0300_clk_div(ddr_pll, ls2k0300_clk_field(ddr_h,
							    LS2K_CLK_DDR1_DEVS_SHIFT,
							    LS2K_CLK_L2_MASK));

	switch (id) {
	case LS2K0300_CLK_REF:
		value = ref_hz;
		break;
	case LS2K0300_CLK_NODE:
		value = node_pll;
		break;
	case LS2K0300_CLK_CPU:
	case LS2K0300_CLK_SCACHE:
	case LS2K0300_CLK_IODMA:
		value = ls2k0300_clk_div(node_pll,
					 ls2k0300_clk_field(node_l, LS2K_CLK_ODIV_SHIFT,
							    LS2K_CLK_ODIV_MASK));
		value = ls2k0300_clk_freqscale(value, freq_scale, LS2K_CLK_FS_NODE_SHIFT);
		break;
	case LS2K0300_CLK_GMAC:
		value = ls2k0300_clk_div(node_pll,
					 ls2k0300_clk_field(node_h, LS2K_CLK_NODE1_GMAC_SHIFT,
							    LS2K_CLK_L2_MASK));
		break;
	case LS2K0300_CLK_I2S:
		value = ls2k0300_clk_div(node_pll,
					 ls2k0300_clk_field(node_h, LS2K_CLK_NODE1_I2S_SHIFT,
							    LS2K_CLK_L2_MASK));
		value = ls2k0300_clk_freqscale(value, freq_scale, LS2K_CLK_FS_I2S_SHIFT);
		break;
	case LS2K0300_CLK_DDR_P:
		value = ddr_pll;
		break;
	case LS2K0300_CLK_DDR:
		value = ls2k0300_clk_div(ddr_pll,
					 ls2k0300_clk_field(ddr_l, LS2K_CLK_ODIV_SHIFT,
							    LS2K_CLK_ODIV_MASK));
		break;
	case LS2K0300_CLK_NET:
		value = ls2k0300_clk_div(ddr_pll,
					 ls2k0300_clk_field(ddr_h, LS2K_CLK_DDR1_NET_SHIFT,
							    LS2K_CLK_L2_MASK));
		break;
	case LS2K0300_CLK_DEVS:
		value = devs;
		break;
	case LS2K0300_CLK_USB:
		value = ls2k0300_clk_freqscale(devs, freq_scale, LS2K_CLK_FS_USB_SHIFT);
		break;
	case LS2K0300_CLK_APB:
		/* The APB bus clock: UART, CAN, I2C, SPI, NAND, PWM, RTC, ... */
		value = ls2k0300_clk_freqscale(devs, freq_scale, LS2K_CLK_FS_APB_SHIFT);
		break;
	case LS2K0300_CLK_BOOT:
		value = ls2k0300_clk_freqscale(devs, freq_scale, LS2K_CLK_FS_BOOT_SHIFT);
		break;
	case LS2K0300_CLK_SDIO:
		value = ls2k0300_clk_freqscale_sdio(devs, freq_scale);
		break;
	case LS2K0300_CLK_PIX_P:
		value = ls2k0300_clk_pll(pix_l, ref_hz);
		break;
	case LS2K0300_CLK_PIX:
		value = ls2k0300_clk_div(ls2k0300_clk_pll(pix_l, ref_hz),
					 ls2k0300_clk_field(pix_l, LS2K_CLK_ODIV_SHIFT,
							    LS2K_CLK_ODIV_MASK));
		break;
	default:
		/* CLK_GMACBP is derived in PIX_PLL_H, whose divider layout is not
		 * documented for this part.
		 */
		return -ENOTSUP;
	}

	*rate = value;

	return 0;
}

static int ls2k0300_clk_gate(const struct device *dev, clock_control_subsys_t sys,
			     bool enable)
{
	const struct ls2k0300_clk_config *cfg = dev->config;
	uint32_t id = (uint32_t)(uintptr_t)sys;
	uint32_t bit, value;
	int ret;

	ret = ls2k0300_clk_gate_bit(id, &bit);
	if (ret != 0) {
		return ret;
	}

	value = sys_read32(cfg->base + LS2K_CLK_EN);
	sys_write32(enable ? (value | bit) : (value & ~bit), cfg->base + LS2K_CLK_EN);

	return 0;
}

static int ls2k0300_clk_on(const struct device *dev, clock_control_subsys_t sys)
{
	return ls2k0300_clk_gate(dev, sys, true);
}

static int ls2k0300_clk_off(const struct device *dev, clock_control_subsys_t sys)
{
	return ls2k0300_clk_gate(dev, sys, false);
}

static enum clock_control_status ls2k0300_clk_get_status(const struct device *dev,
							 clock_control_subsys_t sys)
{
	const struct ls2k0300_clk_config *cfg = dev->config;
	uint32_t id = (uint32_t)(uintptr_t)sys;
	uint32_t bit;

	if (ls2k0300_clk_gate_bit(id, &bit) != 0) {
		/* No output enable to read: the branch is either always on (the
		 * PLLs) or not a clock of this controller at all.
		 */
		return CLOCK_CONTROL_STATUS_ON;
	}

	return ((sys_read32(cfg->base + LS2K_CLK_EN) & bit) != 0u)
		       ? CLOCK_CONTROL_STATUS_ON
		       : CLOCK_CONTROL_STATUS_OFF;
}

static int ls2k0300_clk_init(const struct device *dev)
{
	const struct ls2k0300_clk_config *cfg = dev->config;

	if (!device_is_ready(cfg->ref_clk)) {
		return -ENODEV;
	}

	return 0;
}

static DEVICE_API(clock_control, ls2k0300_clk_api) = {
	.on = ls2k0300_clk_on,
	.off = ls2k0300_clk_off,
	.get_status = ls2k0300_clk_get_status,
	.get_rate = ls2k0300_clk_get_rate,
};

#define LS2K0300_CLK_INIT(n)							\
	static const struct ls2k0300_clk_config ls2k0300_clk_cfg_##n = {	\
		.base = DT_INST_REG_ADDR(n),					\
		.ref_clk = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),		\
	};									\
										\
	DEVICE_DT_INST_DEFINE(n, ls2k0300_clk_init, NULL, NULL,			\
			      &ls2k0300_clk_cfg_##n, PRE_KERNEL_1,		\
			      CONFIG_CLOCK_CONTROL_LOONGSON_LS2K0300_INIT_PRIORITY, \
			      &ls2k0300_clk_api);

DT_INST_FOREACH_STATUS_OKAY(LS2K0300_CLK_INIT)
