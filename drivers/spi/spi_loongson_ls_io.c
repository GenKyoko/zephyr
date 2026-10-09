/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson 2K0300 SPI-IO master controller ("ls-spi-io", user manual chapter
 * 10) - the register file of the SPI2/SPI3 blocks, ported from the vendor
 * Linux driver drivers/spi/spi-ls-io.c (compatible "loongson,ls-spi-io").
 *
 * This is a different design from the SPI0/SPI1 "loongson,ls-spi" pair: the
 * registers are 32 bit wide, a byte goes out by programming the transfer
 * count (CSTART with AUTOSUSPEND, so the block stops after exactly one
 * byte), the data register is byte wide, and there are no hardware chip
 * select lines at all - the vendor driver drives the chip selects
 * exclusively through "cs-gpios", and this port expects the same (a config
 * without a GPIO chip select runs the bus without any CS, which is what a
 * panel with CS tied low wants).
 *
 * The per byte sequence is the vendor's, verbatim: wait SR1.txa, write the
 * data register, set CR1 = spe | cstart | autosus, wait SR1.eot, clear EOT
 * (write 1), wait SR1.rxa, read the data register. Everything is polled -
 * the block has an interrupt line, but the status bits already say what the
 * driver needs to know at the granularity it works at.
 */

#define DT_DRV_COMPAT loongson_ls_spi_io

/*
 * spi_context.h logs a few lines through LOG_*, and this port's logging has
 * the known multi-argument formatting defect, so the module level is pinned
 * to zero here: every LOG_* call in this translation unit (the context's
 * included) compiles out and the driver reports through printk instead.
 */
#define LOG_LEVEL 0
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(spi_loongson_ls_io);

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/sys_io.h>
#include <errno.h>

#include "spi_context.h"

/* Register offsets, 32 bit wide except the data register (manual table 10-2) */
#define LS2K_SPIIO_CR1     0x00u
#define LS2K_SPIIO_CR2     0x04u
#define LS2K_SPIIO_CR3     0x08u
#define LS2K_SPIIO_IER     0x10u
#define LS2K_SPIIO_SR1     0x14u
#define LS2K_SPIIO_SR2     0x18u
#define LS2K_SPIIO_CFG1    0x20u
#define LS2K_SPIIO_CFG2    0x24u
#define LS2K_SPIIO_CFG3    0x28u
#define LS2K_SPIIO_DR      0x40u

/* CR1 bits */
#define LS2K_SPIIO_CR1_SPE     BIT(0)
#define LS2K_SPIIO_CR1_CSTART  BIT(1)
#define LS2K_SPIIO_CR1_AUTOSUS BIT(2)
#define LS2K_SPIIO_CR1_SSREV   BIT(8)

/* SR1 bits */
#define LS2K_SPIIO_SR1_RXA     BIT(0)
#define LS2K_SPIIO_SR1_TXA     BIT(1)
#define LS2K_SPIIO_SR1_TXE     BIT(5)
#define LS2K_SPIIO_SR1_EOT     BIT(15) /* write 1 to clear */

/* CFG1 bits */
#define LS2K_SPIIO_CFG1_CPOL    BIT(0)
#define LS2K_SPIIO_CFG1_CPHA    BIT(1)
#define LS2K_SPIIO_CFG1_LSBFRST BIT(7)
#define LS2K_SPIIO_CFG1_DSIZE(x) ((uint32_t)(x) << 8)

/* CFG2: divider = brint + brdec/64, programmed as brint << 8 */
#define LS2K_SPIIO_CFG2_BRINT(x) ((uint32_t)(x) << 8)

/* CFG3 bits */
#define LS2K_SPIIO_CFG3_MSTR BIT(0)
#define LS2K_SPIIO_CFG3_DIE  BIT(2)
#define LS2K_SPIIO_CFG3_DOE  BIT(3)

/* The transfer format this driver programs: 8 bit words */
#define LS2K_SPIIO_DSIZE 7u

/* Bytes polled per FIFO access before declaring the bus stuck */
#define LS2K_SPIIO_POLL_TRIES 100000u

struct spi_loongson_io_config {
	mem_addr_t base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pcfg;
};

struct spi_loongson_io_data {
	struct spi_context ctx;
	uint32_t clk_rate;
	uint32_t hz;   /* the speed currently programmed */
	uint16_t mode; /* the SPI mode currently programmed */
};

static inline uint32_t spi_io_read(const struct spi_loongson_io_config *cfg, uint32_t reg)
{
	return sys_read32(cfg->base + reg);
}

static inline void spi_io_write(const struct spi_loongson_io_config *cfg, uint32_t reg,
				uint32_t value)
{
	sys_write32(value, cfg->base + reg);
}

static int spi_io_configure(const struct device *dev, const struct spi_config *spi_cfg)
{
	const struct spi_loongson_io_config *cfg = dev->config;
	struct spi_loongson_io_data *data = dev->data;
	uint16_t op = spi_cfg->operation;
	uint32_t cfg1, div;

	if (SPI_OP_MODE_GET(op) != SPI_OP_MODE_MASTER) {
		return -ENOTSUP;
	}

	if ((op & SPI_MODE_LOOP) != 0U || (op & SPI_LINES_MASK) != SPI_LINES_SINGLE) {
		/* No loopback or line selection in the register file */
		return -ENOTSUP;
	}

	if (SPI_WORD_SIZE_GET(op) != 8U) {
		/* The data register is byte wide and the format is fixed to 8 bit */
		return -EINVAL;
	}

	if (spi_cfg->frequency == 0U) {
		return -EINVAL;
	}

	/* The vendor driver's divider: DIV_ROUND_UP clamped into 2..255,
	 * programmed into the brint field (its brdec fraction stays 0).
	 */
	div = (data->clk_rate + (spi_cfg->frequency - 1U)) / spi_cfg->frequency;
	div = CLAMP(div, 2U, 255U);
	spi_io_write(cfg, LS2K_SPIIO_CFG2, LS2K_SPIIO_CFG2_BRINT(div));

	cfg1 = LS2K_SPIIO_CFG1_DSIZE(LS2K_SPIIO_DSIZE);
	if ((op & SPI_MODE_CPOL) != 0U) {
		cfg1 |= LS2K_SPIIO_CFG1_CPOL;
	}
	if ((op & SPI_MODE_CPHA) != 0U) {
		cfg1 |= LS2K_SPIIO_CFG1_CPHA;
	}
	if ((op & SPI_TRANSFER_LSB) != 0U) {
		/* This block does have a word order bit, unlike the ls-spi pair */
		cfg1 |= LS2K_SPIIO_CFG1_LSBFRST;
	}
	spi_io_write(cfg, LS2K_SPIIO_CFG1, cfg1);

	data->hz = spi_cfg->frequency;
	data->mode = op & SPI_MODE_MASK;

	return 0;
}

/*
 * One byte, the vendor driver's sequence verbatim: wait for the transmit
 * buffer, write it, run exactly one transfer (CSTART with AUTOSUSPEND),
 * clear the end-of-transfer flag, wait for the receive buffer, read it.
 */
static int spi_io_xfer_byte(const struct device *dev, uint8_t tx_byte, uint8_t *rx_byte)
{
	const struct spi_loongson_io_config *cfg = dev->config;
	uint32_t tries = LS2K_SPIIO_POLL_TRIES;

	while ((spi_io_read(cfg, LS2K_SPIIO_SR1) & LS2K_SPIIO_SR1_TXA) == 0U) {
		if (--tries == 0U) {
			goto timeout;
		}
	}

	/* The data register is byte wide on both access sides */
	sys_write8(tx_byte, cfg->base + LS2K_SPIIO_DR);

	spi_io_write(cfg, LS2K_SPIIO_CR1,
		     LS2K_SPIIO_CR1_SPE | LS2K_SPIIO_CR1_CSTART | LS2K_SPIIO_CR1_AUTOSUS);

	tries = LS2K_SPIIO_POLL_TRIES;
	while ((spi_io_read(cfg, LS2K_SPIIO_SR1) & LS2K_SPIIO_SR1_EOT) == 0U) {
		if (--tries == 0U) {
			goto timeout;
		}
	}
	spi_io_write(cfg, LS2K_SPIIO_SR1, LS2K_SPIIO_SR1_EOT);

	tries = LS2K_SPIIO_POLL_TRIES;
	while ((spi_io_read(cfg, LS2K_SPIIO_SR1) & LS2K_SPIIO_SR1_RXA) == 0U) {
		if (--tries == 0U) {
			goto timeout;
		}
	}

	if (rx_byte != NULL) {
		*rx_byte = sys_read8(cfg->base + LS2K_SPIIO_DR);
	} else {
		(void)sys_read8(cfg->base + LS2K_SPIIO_DR);
	}

	return 0;

timeout:
	printk("SPI %s: byte transfer timed out (SR1 %08x)\n", dev->name,
	       spi_io_read(cfg, LS2K_SPIIO_SR1));
	return -ETIMEDOUT;
}

static int spi_loongson_io_transceive(const struct device *dev,
				      const struct spi_config *spi_cfg,
				      const struct spi_buf_set *tx_bufs,
				      const struct spi_buf_set *rx_bufs)
{
	struct spi_loongson_io_data *data = dev->data;
	int ret;

	spi_context_lock(&data->ctx, false, NULL, NULL, spi_cfg);

	data->ctx.config = spi_cfg;

	ret = spi_io_configure(dev, spi_cfg);
	if (ret != 0) {
		spi_context_release(&data->ctx, ret);
		return ret;
	}

	spi_context_buffers_setup(&data->ctx, tx_bufs, rx_bufs, 1);

	/*
	 * The block has no chip select lines of its own; the CS comes from the
	 * controller's "cs-gpios" through the context. A config without one
	 * runs the bus without a chip select.
	 */
	spi_context_cs_control(&data->ctx, true);

	while (spi_context_tx_on(&data->ctx) || spi_context_rx_on(&data->ctx)) {
		uint8_t tx_byte = 0U, rx_byte = 0U;

		if (spi_context_tx_buf_on(&data->ctx)) {
			tx_byte = *data->ctx.tx_buf;
		}

		ret = spi_io_xfer_byte(dev, tx_byte, &rx_byte);
		if (ret != 0) {
			break;
		}

		if (spi_context_rx_buf_on(&data->ctx)) {
			*data->ctx.rx_buf = rx_byte;
		}

		spi_context_update_tx(&data->ctx, 1, 1);
		spi_context_update_rx(&data->ctx, 1, 1);
	}

	spi_context_cs_control(&data->ctx, false);

	spi_context_complete(&data->ctx, dev, ret);

	ret = spi_context_wait_for_completion(&data->ctx);

	spi_context_release(&data->ctx, ret);

	return ret;
}

static int spi_loongson_io_release(const struct device *dev, const struct spi_config *spi_cfg)
{
	struct spi_loongson_io_data *data = dev->data;

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

static DEVICE_API(spi, spi_loongson_io_api) = {
	.transceive = spi_loongson_io_transceive,
#ifdef CONFIG_SPI_RTIO
	.iodev_submit = spi_rtio_iodev_default_submit,
#endif
	.release = spi_loongson_io_release,
};

static int spi_loongson_io_init(const struct device *dev)
{
	const struct spi_loongson_io_config *cfg = dev->config;
	struct spi_loongson_io_data *data = dev->data;
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

	/* The vendor driver's controller init, verbatim: master mode with both
	 * data pins enabled, the divider seed at its slowest, 8 bit words, and
	 * the block enabled.
	 */
	spi_io_write(cfg, LS2K_SPIIO_CFG3, LS2K_SPIIO_CFG3_MSTR | LS2K_SPIIO_CFG3_DIE |
					    LS2K_SPIIO_CFG3_DOE);
	spi_io_write(cfg, LS2K_SPIIO_CFG2, LS2K_SPIIO_CFG2_BRINT(0xffu));
	spi_io_write(cfg, LS2K_SPIIO_CFG1, LS2K_SPIIO_CFG1_DSIZE(LS2K_SPIIO_DSIZE));
	spi_io_write(cfg, LS2K_SPIIO_CR1, LS2K_SPIIO_CR1_SPE);

	data->hz = 0U;
	data->mode = 0U;

	ret = spi_context_cs_configure_all(&data->ctx);
	if (ret < 0) {
		return ret;
	}

	spi_context_unlock_unconditionally(&data->ctx);

	return 0;
}

#define SPI_LOONGSON_IO_INIT(n)								\
	PINCTRL_DT_INST_DEFINE(n);							\
											\
	static struct spi_loongson_io_data spi_loongson_io_data_##n = {			\
		SPI_CONTEXT_INIT_LOCK(spi_loongson_io_data_##n, ctx),			\
		SPI_CONTEXT_INIT_SYNC(spi_loongson_io_data_##n, ctx),			\
		SPI_CONTEXT_CS_GPIOS_INITIALIZE(DT_DRV_INST(n), ctx)			\
	};										\
											\
	static const struct spi_loongson_io_config spi_loongson_io_cfg_##n = {		\
		.base = DT_INST_REG_ADDR(n),						\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),			\
		.clock_subsys = (clock_control_subsys_t)DT_INST_PHA(n, clocks, clkid),	\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),				\
	};										\
											\
	SPI_DEVICE_DT_INST_DEFINE(n, spi_loongson_io_init, NULL,			\
				  &spi_loongson_io_data_##n, &spi_loongson_io_cfg_##n,	\
				  POST_KERNEL, CONFIG_SPI_INIT_PRIORITY,		\
				  &spi_loongson_io_api);

DT_INST_FOREACH_STATUS_OKAY(SPI_LOONGSON_IO_INIT)
