/*
 * Copyright (c) 2026 Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson 2K0300 PWM driver for the ATIM / GTIM timer blocks.
 *
 * Ported from the vendor Linux pair drivers/mfd/loongson-timers.c (the parent:
 * owns the register file, the clock, the interrupt and the DMA channels) and
 * drivers/pwm/pwm-ls-timer.c (the child: the PWM front end), reduced to what
 * this port needs:
 *
 *   - the driver binds to the child node ("loongson,ls2k-pwm-timer") and reaches
 *     the registers, the clock and the pinctrl through its parent node
 *     ("loongson,loongson-timers"), the same split the vendor device tree has;
 *   - the number of channels, the width of ARR and the presence of the break
 *     unit are probed at init the way the vendor driver probes the first two
 *     (write the CCER channel enable bits / ARR / BDTR.MOE and look at which
 *     bits stick). The blocks are not identical and the manual describes them
 *     separately: ATIM has the break and dead-time register with the main
 *     output enable plus complementary outputs, GTIM stops at CCR4 (tables
 *     18-2 and 19-2), and no counter width is documented for either;
 *   - output only: PSC and ARR set the period, CCRx the pulse, PWM mode 1 with
 *     output preload, plus polarity. All channels of one block share PSC and
 *     ARR, so a second channel with a different period is refused with -EBUSY
 *     instead of silently re-timing the channel that is already running (the
 *     vendor driver does the same).
 *
 * The complementary outputs of the ATIM (CH1N..CH3N) are driven together with
 * their channel, like the vendor driver does: configuring a channel enables
 * CCxE and, where the block has them, CCxNE. A channel listed in the
 * "loongson,complementary-only" property drives CHxN alone instead - the two
 * enables are independent bits - and a block that has no complementary outputs
 * (the GTIM) refuses to initialize if the property is set at all. The pins are
 * board wiring, and the dead time between the two outputs of a pair stays at
 * zero.
 *
 * Deliberately not ported, with what each would need:
 *
 *   - capture (PWM input mode): the vendor driver reads CCR1/CCR3 and CCR2/CCR4
 *     through the DMA engine in a burst and needs the capture interrupts; this
 *     port has neither a DMA driver nor the RTC-style capture plumbing yet;
 *   - break inputs ("loongson,breakinput" in the vendor binding) and the dead
 *     time generator: ATIM only and board specific; the vendor driver does not
 *     program BDTR.DTG either.
 *
 * Register offsets are the ones of chapter 18 (ATIM) and 19 (GTIM) of the 2K0300
 * user manual and match include/linux/mfd/loongson-timers.h.
 */

#define DT_DRV_COMPAT loongson_ls2k_pwm_timer

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define TIM_CR1   0x00U
#define TIM_EGR   0x14U
#define TIM_CCMR1 0x18U
#define TIM_CCMR2 0x1cU
#define TIM_CCER  0x20U
#define TIM_PSC   0x28U
#define TIM_ARR   0x2cU
#define TIM_BDTR  0x44U

/* Capture/compare register x, x in 1..4 */
#define TIM_CCR(x) (0x34U + (4U * ((x) - 1U)))

#define TIM_CR1_CEN  BIT(0)  /* counter enable */
#define TIM_CR1_ARPE BIT(7)  /* auto-reload preload enable */
#define TIM_EGR_UG   BIT(0)  /* update generation: latch PSC/ARR/CCRx now */

#define TIM_CCMR_PE        BIT(3)           /* output compare preload enable */
#define TIM_CCMR_OCxM_PWM1 (BIT(5) | BIT(6)) /* OCxM = 110: PWM mode 1 */
#define TIM_CCMR_SHIFT     8U
#define TIM_CCMR_MASK      0xffU

/* Channel x, x in 1..4 */
#define TIM_CCER_CCxE(x)  BIT(0U + (4U * ((x) - 1U)))
#define TIM_CCER_CCxP(x)  BIT(1U + (4U * ((x) - 1U)))
#define TIM_CCER_CCxNE(x) BIT(2U + (4U * ((x) - 1U)))
#define TIM_CCER_CCxNP(x) BIT(3U + (4U * ((x) - 1U)))
#define TIM_CCER_CC1NE    TIM_CCER_CCxNE(1)
#define TIM_CCER_CCXE     (BIT(0) | BIT(4) | BIT(8) | BIT(12))

#define TIM_BDTR_MOE BIT(15) /* main output enable */

#define TIM_PSC_MAX 0xffffU

struct pwm_loongson_config {
	mem_addr_t base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pcfg;
	/* Channels that drive their complementary output (CHxN) alone, by bit */
	uint32_t complementary_only;
};

struct pwm_loongson_data {
	/* Protects the registers shared by all channels (PSC, ARR, CR1) */
	struct k_mutex lock;
	uint32_t clock_hz;
	uint32_t max_arr;
	uint8_t channels;
	/* ATIM has the break unit (and with it the main output enable), GTIM does not */
	bool has_bdtr;
	/* Only the ATIM has complementary outputs (CH1N..CH3N) */
	bool has_complementary;
};

static inline uint32_t pwm_loongson_read(const struct pwm_loongson_config *cfg,
					 uint32_t reg)
{
	return sys_read32(cfg->base + reg);
}

static inline void pwm_loongson_write(const struct pwm_loongson_config *cfg,
				      uint32_t reg, uint32_t value)
{
	sys_write32(value, cfg->base + reg);
}

static inline void pwm_loongson_set_bits(const struct pwm_loongson_config *cfg,
					 uint32_t reg, uint32_t mask)
{
	pwm_loongson_write(cfg, reg, pwm_loongson_read(cfg, reg) | mask);
}

static inline void pwm_loongson_clear_bits(const struct pwm_loongson_config *cfg,
					   uint32_t reg, uint32_t mask)
{
	pwm_loongson_write(cfg, reg, pwm_loongson_read(cfg, reg) & ~mask);
}

static uint32_t pwm_loongson_active_channels(const struct pwm_loongson_config *cfg)
{
	return pwm_loongson_read(cfg, TIM_CCER) & TIM_CCER_CCXE;
}

static int pwm_loongson_set_cycles(const struct device *dev, uint32_t channel,
				   uint32_t period, uint32_t pulse, pwm_flags_t flags)
{
	const struct pwm_loongson_config *cfg = dev->config;
	struct pwm_loongson_data *data = dev->data;
	uint32_t reg_ch = channel + 1U;
	uint64_t prescaler, ticks, duty;
	uint32_t ccmr_reg, ccmr, mask, clear, shift;
	int ret = 0;

	if (channel >= data->channels) {
		return -EINVAL;
	}

	if ((period == 0U) || (pulse > period)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	/*
	 * period and pulse arrive in input clock cycles, see
	 * pwm_loongson_get_cycles_per_sec(). Pick the smallest prescaler that
	 * makes the period fit into ARR, exactly like the vendor driver:
	 *
	 *   prescaler = period / (max_arr + 1)
	 *   ticks     = period / (prescaler + 1)      -> ARR = ticks - 1
	 *   duty      = pulse  / (prescaler + 1)      -> CCRx
	 */
	prescaler = period / ((uint64_t)data->max_arr + 1U);
	if (prescaler > TIM_PSC_MAX) {
		ret = -EINVAL;
		goto out;
	}

	ticks = period / (prescaler + 1U);
	if (ticks == 0U) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * PSC and ARR are shared by all channels of the block: if another
	 * channel is running with a different period, this call would re-time
	 * it, so refuse instead.
	 */
	if ((pwm_loongson_active_channels(cfg) & ~TIM_CCER_CCxE(reg_ch)) != 0U) {
		if ((pwm_loongson_read(cfg, TIM_PSC) != prescaler) ||
		    (pwm_loongson_read(cfg, TIM_ARR) != ticks - 1U)) {
			ret = -EBUSY;
			goto out;
		}
	}

	pwm_loongson_write(cfg, TIM_PSC, (uint32_t)prescaler);
	pwm_loongson_write(cfg, TIM_ARR, (uint32_t)ticks - 1U);
	pwm_loongson_set_bits(cfg, TIM_CR1, TIM_CR1_ARPE);

	duty = pulse / (prescaler + 1U);
	pwm_loongson_write(cfg, TIM_CCR(reg_ch), (uint32_t)duty);

	/* Output compare, PWM mode 1, preload: one CCMR byte per channel */
	shift = (channel & 1U) * TIM_CCMR_SHIFT;
	ccmr = (TIM_CCMR_PE | TIM_CCMR_OCxM_PWM1) << shift;
	mask = TIM_CCMR_MASK << shift;
	ccmr_reg = (channel < 2U) ? TIM_CCMR1 : TIM_CCMR2;

	pwm_loongson_write(cfg, ccmr_reg,
			   (pwm_loongson_read(cfg, ccmr_reg) & ~mask) | ccmr);

	/*
	 * The ATIM gates all of its outputs with the break unit's main output
	 * enable; the GTIM stops at CCR4 and has neither the register nor the
	 * gate (manual table 19-2 against 18-2), so only set it where it exists.
	 */
	if (data->has_bdtr) {
		pwm_loongson_set_bits(cfg, TIM_BDTR, TIM_BDTR_MOE);
	}

	/* Polarity of the output, and of its complementary output if there is one */
	mask = TIM_CCER_CCxP(reg_ch);
	if (data->has_complementary) {
		mask |= TIM_CCER_CCxNP(reg_ch);
	}

	if ((flags & PWM_POLARITY_INVERTED) != 0U) {
		pwm_loongson_set_bits(cfg, TIM_CCER, mask);
	} else {
		pwm_loongson_clear_bits(cfg, TIM_CCER, mask);
	}

	/*
	 * Drive the output. A pulse of 0 leaves CCRx at 0, which keeps the pin at
	 * the inactive level while it is still driven - the behaviour the PWM API
	 * documents for a zero pulse - and a pulse equal to the period makes CCRx
	 * exceed ARR, so the pin stays active.
	 *
	 * On the ATIM the complementary output of the channel is enabled together
	 * with it, the way the vendor driver does it: the pair is driven by the
	 * same channel, and whether the CHxN pin is actually brought out is board
	 * wiring (pinctrl). The dead time between the two stays at its reset value
	 * of zero - BDTR.DTG is not programmed, and neither is it by the vendor.
	 */
	mask = TIM_CCER_CCxE(reg_ch);
	clear = 0U;

	if (data->has_complementary) {
		mask |= TIM_CCER_CCxNE(reg_ch);

		/*
		 * A channel listed in "loongson,complementary-only" drives its
		 * complementary pin alone - CCxE and CCxNE are independent
		 * enables, so clearing the first and setting the second puts
		 * the waveform on CHxN only. The other bit is cleared explicitly
		 * so that a channel can be switched over at run time.
		 */
		if ((cfg->complementary_only & BIT(channel)) != 0U) {
			mask = TIM_CCER_CCxNE(reg_ch);
			clear = TIM_CCER_CCxE(reg_ch);
		}
	}

	pwm_loongson_clear_bits(cfg, TIM_CCER, clear);
	pwm_loongson_set_bits(cfg, TIM_CCER, mask);
	pwm_loongson_set_bits(cfg, TIM_EGR, TIM_EGR_UG);
	pwm_loongson_set_bits(cfg, TIM_CR1, TIM_CR1_CEN);

out:
	k_mutex_unlock(&data->lock);

	return ret;
}

static int pwm_loongson_get_cycles_per_sec(const struct device *dev, uint32_t channel,
					   uint64_t *cycles)
{
	struct pwm_loongson_data *data = dev->data;

	if (channel >= data->channels) {
		return -EINVAL;
	}

	/*
	 * The unit of pwm_set_cycles() is the clock that feeds the prescaler, so
	 * that pwm_set_pulse_width_ns() and the PWM shell convert correctly; the
	 * prescaler itself is picked inside set_cycles().
	 */
	*cycles = data->clock_hz;

	return 0;
}

static DEVICE_API(pwm, pwm_loongson_api) = {
	.set_cycles = pwm_loongson_set_cycles,
	.get_cycles_per_sec = pwm_loongson_get_cycles_per_sec,
};

static int pwm_loongson_init(const struct device *dev)
{
	const struct pwm_loongson_config *cfg = dev->config;
	struct pwm_loongson_data *data = dev->data;
	uint32_t ccer_backup, ccer, arr_backup;
	int ret;

	if (!device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_get_rate(cfg->clock_dev, cfg->clock_subsys,
				     &data->clock_hz);
	if ((ret != 0) || (data->clock_hz == 0U)) {
		return -EINVAL;
	}

	/*
	 * The vendor driver refuses a clock faster than 1 GHz because its period
	 * arithmetic works in nanoseconds; this driver works in clock cycles, so
	 * the bound only has to keep 2^64 / clock_hz from wrapping.
	 */
	if (data->clock_hz > 1000000000U) {
		return -EINVAL;
	}

	k_mutex_init(&data->lock);

	if (IS_ENABLED(CONFIG_PINCTRL)) {
		ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			return ret;
		}
	}

	/*
	 * Probe the instance: the enable bits of a channel this block does not
	 * have (and the ARR bits above the counter width) read back as zero, so
	 * writing them and reading back is how the vendor driver finds both.
	 * The previous values are restored.
	 */
	ccer_backup = pwm_loongson_read(cfg, TIM_CCER);
	pwm_loongson_write(cfg, TIM_CCER, ccer_backup | TIM_CCER_CCXE | TIM_CCER_CC1NE);
	ccer = pwm_loongson_read(cfg, TIM_CCER);
	pwm_loongson_write(cfg, TIM_CCER, ccer_backup);

	data->channels = (uint8_t)__builtin_popcount(ccer & TIM_CCER_CCXE);
	if (data->channels == 0U) {
		return -ENODEV;
	}

	/*
	 * The same pass says whether the block has complementary outputs: only
	 * the ATIM does (CH1N..CH3N, manual table 18-2; the GTIM has no CCxNE
	 * bits at all). Where they exist, configuring a channel drives its
	 * complementary output along with the main one, like the vendor driver;
	 * the pin of that output is board wiring.
	 */
	data->has_complementary = (ccer & TIM_CCER_CC1NE) != 0U;

	/*
	 * A channel asked to drive its complementary output on a block that has
	 * none can never produce a waveform (CCxNE does not exist there), and
	 * that would be silent otherwise - refuse to initialize instead.
	 */
	if ((cfg->complementary_only != 0U) && !data->has_complementary) {
		return -ENOTSUP;
	}

	arr_backup = pwm_loongson_read(cfg, TIM_ARR);
	pwm_loongson_write(cfg, TIM_ARR, 0xffffffffU);
	data->max_arr = pwm_loongson_read(cfg, TIM_ARR);
	pwm_loongson_write(cfg, TIM_ARR, arr_backup);

	/*
	 * Does this block have the break unit? The ATIM does, with the main
	 * output enable in it, the GTIM does not (manual table 18-2 against
	 * 19-2). Probed the same way as the channel count, by writing the bit
	 * and seeing whether it sticks, so that no write goes to an offset the
	 * block does not implement. The bit is cleared again: it is set when a
	 * channel is configured.
	 */
	pwm_loongson_set_bits(cfg, TIM_BDTR, TIM_BDTR_MOE);
	data->has_bdtr = (pwm_loongson_read(cfg, TIM_BDTR) & TIM_BDTR_MOE) != 0U;
	pwm_loongson_clear_bits(cfg, TIM_BDTR, TIM_BDTR_MOE);

	return 0;
}

#define PWM_LOONGSON_INIT(n)								\
	PINCTRL_DT_DEFINE(DT_INST_PARENT(n));						\
											\
	static struct pwm_loongson_data pwm_loongson_data_##n;				\
											\
	static const struct pwm_loongson_config pwm_loongson_cfg_##n = {		\
		.base = DT_REG_ADDR(DT_INST_PARENT(n)),					\
		.clock_dev = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_INST_PARENT(n))),		\
		.clock_subsys =								\
			(clock_control_subsys_t)DT_PHA(DT_INST_PARENT(n), clocks,	\
						       clkid),				\
		.pcfg = PINCTRL_DT_DEV_CONFIG_GET(DT_INST_PARENT(n)),			\
		.complementary_only =							\
			DT_INST_PROP_OR(n, loongson_complementary_only, 0),		\
	};										\
											\
	DEVICE_DT_INST_DEFINE(n, pwm_loongson_init, NULL, &pwm_loongson_data_##n,	\
			      &pwm_loongson_cfg_##n, POST_KERNEL,			\
			      CONFIG_PWM_INIT_PRIORITY, &pwm_loongson_api);

DT_INST_FOREACH_STATUS_OKAY(PWM_LOONGSON_INIT)
