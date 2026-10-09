/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson 2K0300 SPI master controller ("ls-spi", user manual chapter 9),
 * ported from the register file the vendor's Linux driver
 * (drivers/spi/spi-loongson-core.c) and mainline use - the same block
 * mainline matches as "loongson,ls2k1000-spi".
 *
 * The block is small and completely synchronous: a single byte-wide FIFO
 * port (write transmits, read receives), a status register whose rfempty bit
 * says a byte has come back, and a clock divider built from SPCR.spr and
 * SPER.spre, whose twelve combinations divide the input clock by 2, 4, 8,
 * 16 ... 4096 (table 9-7). With the APB/DEVS bus clock this board runs
 * (200 MHz) that puts SCLK between 100 MHz and about 49 kHz.
 *
 * Everything here is polled, like the vendor driver: the per byte loop
 * writes the FIFO, waits for rfempty to clear and reads the byte back - the
 * block has an interrupt line, but its only meaning is "the RX byte is
 * there", which the status register already says.
 *
 * The chip selects are the SFC_SOFTCS register: its low nibble selects which
 * of the four csn lines software drives ("csen"), its high nibble holds the
 * line values ("csn"). One subtlety from the parameter register: bit 0
 * (memory_en) is set out of reset, because the same block doubles as the
 * flash XIP engine - while it is set the csn lines do not answer to
 * SFC_SOFTCS, so it is cleared for the duration of a message (the vendor
 * driver does the same, and restores it afterwards).
 *
 * What this driver is and is not:
 *
 *   - 8 bit words only: the FIFO port is one byte wide and the vendor driver
 *     demands words in whole bytes as well.
 *   - MSB first, master mode, full duplex, no loopback: there is no loopback
 *     or word order bit in the register file, so those requests are refused
 *     rather than silently ignored.
 *   - both directions of a transceive run as full duplex: a missing tx buffer
 *     sends zeros, a missing rx buffer discards what comes back - which is
 *     the "write then read" shape of every SPI flash and sensor transaction.
 *   - up to four hardware chip selects through SFC_SOFTCS; a device that
 *     spells out "cs-gpios" gets a GPIO chip select through the usual SPI
 *     context instead.
 */

#define DT_DRV_COMPAT loongson_ls_spi

/*
 * spi_context.h logs a few lines through LOG_*, and this port's logging has
 * the known multi-argument formatting defect, so the module level is pinned
 * to zero here: every LOG_* call in this translation unit (the context's
 * included) compiles out and the driver reports through printk instead.
 */
#define LOG_LEVEL 0
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_loongson_ls2k0300);

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/sys_io.h>
#include <errno.h>

#include "spi_context.h"

/* Register offsets, byte wide (manual table 9-2) */
#define LS2K_SPI_SPCR    0x00u
#define LS2K_SPI_SPSR    0x01u
#define LS2K_SPI_FIFO    0x02u
#define LS2K_SPI_SPER    0x03u
#define LS2K_SPI_SFC_PARAM 0x04u
#define LS2K_SPI_SFC_SOFTCS 0x05u
#define LS2K_SPI_SFC_TIMING 0x06u

/* SPCR bits */
#define LS2K_SPI_SPCR_SPIE	BIT(7)
#define LS2K_SPI_SPCR_SPE	BIT(6)
#define LS2K_SPI_SPCR_CPOL	BIT(3)
#define LS2K_SPI_SPCR_CPHA	BIT(2)
#define LS2K_SPI_SPCR_SPR	GENMASK(1, 0)

/* SPSR bits */
#define LS2K_SPI_SPSR_SPIF	BIT(7) /* write 1 to clear */
#define LS2K_SPI_SPSR_WCOL	BIT(6) /* write 1 to clear */
#define LS2K_SPI_SPSR_WFFULL	BIT(3)
#define LS2K_SPI_SPSR_WFEMPTY	BIT(2)
#define LS2K_SPI_SPSR_RFFULL	BIT(1)
#define LS2K_SPI_SPSR_RFEMPTY	BIT(0)

/* SPER bits */
#define LS2K_SPI_SPER_ICNT	GENMASK(7, 6)
#define LS2K_SPI_SPER_MODE	BIT(2)
#define LS2K_SPI_SPER_SPRE	GENMASK(1, 0)

/* SFC_PARAM bits */
#define LS2K_SPI_PARAM_MEM_EN	BIT(0)

/* SFC_SOFTCS: low nibble per line "software controls it", high nibble values */
#define LS2K_SPI_SOFTCS_CSN(cs)	BIT(4 + (cs))
#define LS2K_SPI_SOFTCS_CSEN(cs)	BIT(cs)

/*
 * The clock divider, straight from the vendor driver's tables (which are
 * table 9-7 of the manual written out): for a divider of the input clock the
 * two bit fields together form a 2..4096 power of two ramp.
 */
static const uint8_t ls2k_spi_spre_lut[13] = { 0, 0, 0, 1, 0, 0, 1, 1, 1, 2, 2, 2, 2 };
static const uint8_t ls2k_spi_spr_lut[13] = { 0, 0, 1, 0, 2, 3, 1, 2, 3, 0, 1, 2, 3 };

/* Number of bytes polled per FIFO access before declaring the bus stuck */
#define LS2K_SPI_POLL_TRIES 100000u

struct spi_loongson_config {
	mem_addr_t base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pcfg;
};

struct spi_loongson_data {
	struct spi_context ctx;
	uint32_t clk_rate;
	uint32_t hz;    /* the speed currently programmed */
	uint16_t mode;  /* the SPI mode currently programmed */
	uint8_t sfc_param; /* the parameter register a message has to restore */
};

static inline uint8_t spi_loongson_read(const struct spi_loongson_config *cfg, uint32_t reg)
{
	return sys_read8(cfg->base + reg);
}

static inline void spi_loongson_write(const struct spi_loongson_config *cfg, uint32_t reg,
				      uint8_t value)
{
	sys_write8(value, cfg->base + reg);
}

static int spi_loongson_configure(const struct device *dev, const struct spi_config *spi_cfg)
{
	const struct spi_loongson_config *cfg = dev->config;
	struct spi_loongson_data *data = dev->data;
	uint16_t op = spi_cfg->operation;
	uint32_t div, idx;
	uint8_t spcr, sper;

	if (SPI_OP_MODE_GET(op) != SPI_OP_MODE_MASTER) {
		return -ENOTSUP;
	}

	if ((op & SPI_MODE_LOOP) != 0U || (op & SPI_TRANSFER_LSB) != 0U ||
	    (op & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		/* No loopback, word order or line selection in the register file */
		return -ENOTSUP;
	}

	if (SPI_WORD_SIZE_GET(op) != 8U) {
		/* The FIFO port is one byte wide */
		return -EINVAL;
	}

	if (spi_cfg->frequency == 0U) {
		return -EINVAL;
	}

	/* Table 9-7 through the vendor driver's lookup: the divider is clamped
	 * to 2..4096 and its rounded up power of two picks the field pair.
	 */
	div = ((data->clk_rate + (spi_cfg->frequency - 1U)) / spi_cfg->frequency);
	div = CLAMP(div, 2U, 4096U);
	idx = 32u - __builtin_clz(div - 1u); /* the vendor's fls(div - 1), 1..12 */

	spcr = sys_read8(cfg->base + LS2K_SPI_SPCR) & ~(LS2K_SPI_SPCR_SPR | LS2K_SPI_SPCR_CPOL |
						       LS2K_SPI_SPCR_CPHA);
	spcr |= ls2k_spi_spr_lut[idx];
	if ((op & SPI_MODE_CPOL) != 0U) {
		spcr |= LS2K_SPI_SPCR_CPOL;
	}
	if ((op & SPI_MODE_CPHA) != 0U) {
		spcr |= LS2K_SPI_SPCR_CPHA;
	}
	sys_write8(spcr, cfg->base + LS2K_SPI_SPCR);

	sper = sys_read8(cfg->base + LS2K_SPI_SPER) & ~LS2K_SPI_SPER_SPRE;
	sper |= ls2k_spi_spre_lut[idx];
	sys_write8(sper, cfg->base + LS2K_SPI_SPER);

	data->hz = spi_cfg->frequency;
	data->mode = op & SPI_MODE_MASK;

	return 0;
}

/*
 * The chip select bookkeeping of the vendor driver, verbatim: asserting a
 * line both selects it for software control (csen) and drives its value bit,
 * releasing only clears the value bit, and everything is a read-modify-write
 * so the other three lines keep their state.
 */
static void spi_loongson_cs(const struct device *dev, uint8_t cs, bool assert_)
{
	const struct spi_loongson_config *cfg = dev->config;
	uint8_t mask = (LS2K_SPI_SOFTCS_CSN(cs) | LS2K_SPI_SOFTCS_CSEN(cs)) & 0xffu;
	uint8_t val = assert_ ? mask : (LS2K_SPI_SOFTCS_CSEN(cs) & 0xffu);
	uint8_t reg = sys_read8(cfg->base + LS2K_SPI_SFC_SOFTCS) & (uint8_t)~mask;

	sys_write8(val | reg, cfg->base + LS2K_SPI_SFC_SOFTCS);
}

static int spi_loongson_transceive(const struct device *dev,
				   const struct spi_config *spi_cfg,
				   const struct spi_buf_set *tx_bufs,
				   const struct spi_buf_set *rx_bufs)
{
	const struct spi_loongson_config *cfg = dev->config;
	struct spi_loongson_data *data = dev->data;
	bool hw_cs = !spi_cs_is_gpio(spi_cfg);
	int ret = 0;

	spi_context_lock(&data->ctx, false, NULL, NULL, spi_cfg);

	data->ctx.config = spi_cfg;

	ret = spi_loongson_configure(dev, spi_cfg);
	if (ret != 0) {
		spi_context_release(&data->ctx, ret);
		return ret;
	}

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);

	/*
	 * Take the flash read engine out of the picture for the duration of
	 * the message: while SFC_PARAM.memory_en is set the csn lines ignore
	 * SFC_SOFTCS. Whatever was there before is restored afterwards.
	 */
	data->sfc_param = spi_loongson_read(cfg, LS2K_SPI_SFC_PARAM);
	spi_loongson_write(cfg, LS2K_SPI_SFC_PARAM,
			   data->sfc_param & (uint8_t)~LS2K_SPI_PARAM_MEM_EN);

	if (hw_cs) {
		spi_loongson_cs(dev, spi_cfg->slave, true);
	} else {
		spi_context_cs_control(&data->ctx, true);
	}

	while (spi_context_tx_on(&data->ctx) || spi_context_rx_on(&data->ctx)) {
		uint8_t tx_byte = 0U, rx_byte;
		uint32_t tries = LS2K_SPI_POLL_TRIES;

		if (spi_context_tx_buf_on(&data->ctx)) {
			tx_byte = *data->ctx.tx_buf;
		}

		spi_loongson_write(cfg, LS2K_SPI_FIFO, tx_byte);

		/* rfempty starts set; the byte coming back clears it */
		while ((spi_loongson_read(cfg, LS2K_SPI_SPSR) & LS2K_SPI_SPSR_RFEMPTY) != 0U) {
			if (--tries == 0U) {
				printk("SPI %s: rx byte never arrived (SPSR %02x)\n",
				       dev->name, spi_loongson_read(cfg, LS2K_SPI_SPSR));
				ret = -ETIMEDOUT;
				goto out_cs;
			}
		}

		rx_byte = spi_loongson_read(cfg, LS2K_SPI_FIFO);

		if (spi_context_rx_buf_on(&data->ctx)) {
			*data->ctx.rx_buf = rx_byte;
		}

		spi_context_update_tx(&data->ctx, 1, 1);
		spi_context_update_rx(&data->ctx, 1, 1);
	}

out_cs:
	/* Clear the completion flags the transfer may have set */
	spi_loongson_write(cfg, LS2K_SPI_SPSR, LS2K_SPI_SPSR_SPIF | LS2K_SPI_SPSR_WCOL);

	if (hw_cs) {
		spi_loongson_cs(dev, spi_cfg->slave, false);
	} else {
		spi_context_cs_control(&data->ctx, false);
	}

	spi_loongson_write(cfg, LS2K_SPI_SFC_PARAM, data->sfc_param);

	spi_context_complete(&data->ctx, dev, ret);

	ret = spi_context_wait_for_completion(&data->ctx);

	spi_context_release(&data->ctx, ret);

	return ret;
}

static int spi_loongson_release(const struct device *dev, const struct spi_config *spi_cfg)
{
	struct spi_loongson_data *data = dev->data;

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, spi_loongson_api) = {
	.transceive = spi_loongson_transceive,
#ifdef CONFIG_SPI_RTIO
	.iodev_submit = spi_rtio_iodev_default_submit,
#endif
	.release = spi_loongson_release,
};

static int spi_loongson_init(const struct device *dev)
{
	const struct spi_loongson_config *cfg = dev->config;
	struct spi_loongson_data *data = dev->data;
	uint8_t spcr;
	int ret;

	if (!device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_on(cfg->clock_dev, cfg->clock_subsys);
	if ((ret != 0) && (ret != -ENOTSUP)) {
		return ret;
	}

	ret = clock_control_get_rate(cfg->clock_dev, cfg->clock_subsys, &data->clk_rate);
	if ((ret != 0) || (data->clk_rate == 0U)) {
		return -EINVAL;
	}

	if (IS_ENABLED(CONFIG_PINCTRL)) {
		ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			return ret;
		}
	}

	/*
	 * The vendor driver's reginit: stop the block, clear the completion
	 * and overflow flags, start it again. MSTR (bit 4) holds itself at 1
	 * per the manual, and the flash timing register stays at its reset.
	 */
	spcr = spi_loongson_read(cfg, LS2K_SPI_SPCR) & (uint8_t)~LS2K_SPI_SPCR_SPE;
	spi_loongson_write(cfg, LS2K_SPI_SPCR, spcr);
	spi_loongson_write(cfg, LS2K_SPI_SPSR, LS2K_SPI_SPSR_SPIF | LS2K_SPI_SPSR_WCOL);
	spi_loongson_write(cfg, LS2K_SPI_SPCR, spcr | LS2K_SPI_SPCR_SPE);

	data->hz = 0U;
	data->mode = 0U;

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		return ret;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define SPI_LOONGSON_INIT(n)								\
	PINCTRL_DT_INST_DEFINE(n);							\
											\
	static struct spi_loongson_data spi_loongson_data_##n = {			\
		SPI_CONTEXT_INIT_LOCK(spi_loongson_data_##n, ctx),			\
		SPI_CONTEXT_INIT_SYNC(spi_loongson_data_##n, ctx),			\
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)			\
	};										\
											\
	static const struct spi_loongson_config spi_loongson_cfg_##n = {		\
		.base = DT_INST_REG_ADDR(n),						\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),			\
		.clock_subsys = (clock_control_subsys_t)DT_INST_PHA(n, clocks, clkid),	\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),				\
	};										\
											\
	SPI_DEVICE_DT_INST_DEFINE(n, spi_loongson_init, NULL, &spi_loongson_data_##n,	\
				  &spi_loongson_cfg_##n, POST_KERNEL,			\
				  CONFIG_SPI_INIT_PRIORITY, &spi_loongson_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_LOONGSON_INIT)
