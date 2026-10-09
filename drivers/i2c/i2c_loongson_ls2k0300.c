/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson 2K0300 I2C controller driver.
 *
 * The block is the "fast I2C" peripheral of the 2K0300 (compatible
 * "loongson,ls2k0300-i2c"; the 6.12 device tree still called it
 * "loongson,lsfs-i2c"). Its register map is the familiar one of the I2C v1
 * peripheral, with 32 bit registers on a 4 byte stride:
 *
 *   0x00 CR1    PE(0) START(8) STOP(9) ACK(10) POS(11) SWRST(15)
 *   0x04 CR2    FREQ(5:0) = input clock in MHz, ITERREN/ITEVTEN/ITBUFEN
 *   0x08 OAR    own slave address (unused here)
 *   0x10 DR     data
 *   0x14 SR1    SB(0) ADDR(1) BTF(2) RXNE(6) TXE(7) BERR(8) ARLO(9) AF(10)
 *   0x18 SR2    MSL(0) BUSY(1) TRA(2)
 *   0x1c CCR    CCR(11:0) DUTY(14) FS(15)
 *   0x20 TRISE  TRISE(5:0)
 *   0x24 FLTR   (left alone, as in both reference drivers)
 *
 * Everything is polled: the status bits are plain level flags, so the driver
 * needs neither an interrupt line nor any interrupt controller support. The
 * sequence follows the two references for this SoC, u-boot's
 * drivers/i2c/ls2k300_i2c.c and mainline's drivers/i2c/busses/i2c-ls2x-v2.c,
 * including the three receive end games of the peripheral (one byte, two bytes,
 * and reads long enough to drop the acknowledge three bytes before the end).
 *
 * Note for the record: the older "loongson,ls2k-i2c" block that
 * drivers/i2c/busses/i2c-ls2x.c drives (PCF8584 style, byte registers at
 * 0x00..0x04, completion flag in SR bit 0) is a different controller and does
 * not exist on this SoC - driving these addresses with that register map is
 * what a previous version of this file did, and it cannot work.
 */
#define DT_DRV_COMPAT loongson_ls2k0300_i2c

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

/*
 * No LOG_* in this driver. Two multi-argument LOG_ERR calls were garbled on the
 * board - the values printed shifted (a register holding the CCR value, another
 * a RAM pointer) - and one of them faulted inside the formatter with a TLB
 * exception, which is fatal. printk's varargs are the form the SoC console
 * guard uses and are known to print correctly; the messages here only appear
 * when something actually fails.
 */

/* Registers */
#define I2C_CR1   0x00U
#define I2C_CR2   0x04U
#define I2C_DR    0x10U
#define I2C_SR1   0x14U
#define I2C_SR2   0x18U
#define I2C_CCR   0x1cU
#define I2C_TRISE 0x20U

/* CR1 */
#define I2C_CR1_PE    BIT(0)
#define I2C_CR1_START BIT(8)
#define I2C_CR1_STOP  BIT(9)
#define I2C_CR1_ACK   BIT(10)
#define I2C_CR1_POS   BIT(11)
#define I2C_CR1_SWRST BIT(15)

/* CR2 */
#define I2C_CR2_FREQ GENMASK(5, 0) /* input clock in MHz, six bits */
#define I2C_CR2_FREQ_MAX 0x3fU

/*
 * Interrupt class enables: error, event, buffer. The reference driver for this
 * peripheral sets all three, and the event flags a polled driver reads are
 * generated together with them. The controller's interrupt output is not
 * connected in this port (no IRQ is wired for the I2C), so setting them changes
 * no interrupt behaviour - it only brings the flag logic up as the vendor
 * driver has it.
 */
#define I2C_CR2_IT_MASK (BIT(8) | BIT(9) | BIT(10))

/* SR1 */
#define I2C_SR1_SB   BIT(0)
#define I2C_SR1_ADDR BIT(1)
#define I2C_SR1_BTF  BIT(2)
#define I2C_SR1_RXNE BIT(6)
#define I2C_SR1_TXE  BIT(7)
#define I2C_SR1_BERR BIT(8)
#define I2C_SR1_ARLO BIT(9)
#define I2C_SR1_AF   BIT(10)
#define I2C_SR1_ERR  (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_AF)

/* SR2 */
#define I2C_SR2_BUSY BIT(1)

/* CCR */
#define I2C_CCR_CCR  GENMASK(11, 0)
#define I2C_CCR_FS   BIT(15)

/* TRISE */
#define I2C_TRISE_SCL GENMASK(5, 0)

/* One flag wait: a byte on a 100 kHz bus takes 90 us, so this is generous */
#define I2C_FLAG_TIMEOUT_US 20000U

struct ls2k0300_i2c_config {
	mem_addr_t base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	uint32_t bus_hz; /* initial bus speed, from the device tree */
	const struct pinctrl_dev_config *pcfg;
};

struct ls2k0300_i2c_data {
	struct k_mutex lock;
	uint32_t clock_hz;
	uint32_t bus_hz;
};

static inline uint32_t i2c_rd(const struct device *dev, uint32_t reg)
{
	const struct ls2k0300_i2c_config *cfg = dev->config;

	return sys_read32(cfg->base + reg);
}

static inline void i2c_wr(const struct device *dev, uint32_t reg, uint32_t val)
{
	const struct ls2k0300_i2c_config *cfg = dev->config;

	sys_write32(val, cfg->base + reg);
}

/*
 * Error flags are cleared by writing a zero to them. Returns the flags that
 * were set, so the caller can tell a NACK (AF) from a lost arbitration.
 */
static uint32_t i2c_clear_errors(const struct device *dev)
{
	uint32_t sr1 = i2c_rd(dev, I2C_SR1);
	uint32_t err = sr1 & I2C_SR1_ERR;

	if (err != 0U) {
		i2c_wr(dev, I2C_SR1, sr1 & ~I2C_SR1_ERR);
	}

	return err;
}

/* Wait for a status bit to become set (set == true) or clear */
static int i2c_wait_bit(const struct device *dev, uint32_t reg, uint32_t mask, bool set)
{
	uint32_t start = k_cycle_get_32();

	for (;;) {
		uint32_t val = i2c_rd(dev, reg);

		if (((val & mask) != 0U) == set) {
			return 0;
		}

		if (k_cyc_to_us_floor32(k_cycle_get_32() - start) >= I2C_FLAG_TIMEOUT_US) {
			printk("i2c: wait on reg 0x%x mask 0x%x set=%u timed out: "
			       "SR1=%08x SR2=%08x CR1=%08x CR2=%08x CCR=%08x\n",
			       (unsigned int)reg, (unsigned int)mask, (unsigned int)set,
			       i2c_rd(dev, I2C_SR1), i2c_rd(dev, I2C_SR2),
			       i2c_rd(dev, I2C_CR1), i2c_rd(dev, I2C_CR2), i2c_rd(dev, I2C_CCR));
			return -ETIMEDOUT;
		}
	}
}

/*
 * Software reset, the reference recovery path: pulse SWRST, drop the latched
 * error flags, then bring the peripheral back with the same setup.
 */
static void i2c_program_speed(const struct device *dev);

static void i2c_reset(const struct device *dev)
{
	uint32_t cr1 = i2c_rd(dev, I2C_CR1);

	i2c_wr(dev, I2C_CR1, (cr1 & ~I2C_CR1_PE) | I2C_CR1_SWRST);
	i2c_wr(dev, I2C_CR1, cr1 & ~I2C_CR1_PE);
	(void)i2c_clear_errors(dev);

	i2c_program_speed(dev);
}

/*
 * Input clock in CR2.FREQ (MHz), the clock control register CCR in both bus
 * modes, and TRISE full scale - exactly what the reference drivers write.
 */
static void i2c_program_speed(const struct device *dev)
{
	struct ls2k0300_i2c_data *data = dev->data;
	uint32_t freq_mhz = DIV_ROUND_UP(data->clock_hz, 1000000U);
	uint32_t field;
	uint32_t ccr;

	freq_mhz = CLAMP(freq_mhz, 1U, I2C_CR2_FREQ_MAX);

	if (data->bus_hz > 100000U) {
		/* Fast mode, 1:2 duty: CCR = fclk / (3 * bus) */
		field = DIV_ROUND_UP(data->clock_hz, data->bus_hz * 3U);
		ccr = I2C_CCR_FS;
	} else {
		/* Standard mode: CCR = fclk / (2 * bus) */
		field = DIV_ROUND_UP(data->clock_hz, data->bus_hz * 2U);
		ccr = 0U;
	}

	/* Clamp the 12 bit field only: clamping the whole register would throw
	 * the mode bit away, and a fast-mode request would come out as a
	 * standard-mode bus at the slowest possible clock.
	 */
	ccr |= MIN(field, (uint32_t)I2C_CCR_CCR);

	i2c_wr(dev, I2C_CCR, ccr);
	i2c_wr(dev, I2C_TRISE, I2C_TRISE_SCL);
	i2c_wr(dev, I2C_CR2,
	       (i2c_rd(dev, I2C_CR2) & ~I2C_CR2_FREQ) | freq_mhz | I2C_CR2_IT_MASK);
	i2c_wr(dev, I2C_CR1, i2c_rd(dev, I2C_CR1) | I2C_CR1_PE);
}

/*
 * START and the address phase, issued once per direction.
 *
 * The reference driver generates START once for the whole transfer and then
 * just keeps feeding bytes, with STOP at the end: two messages of the same
 * direction - a control byte and its data, the shape the SSD1306 driver
 * produces - are one bus transaction without a repeated start in the middle.
 * A direction change is the one case that needs a new START (a register read:
 * write the register address, then read), so the caller issues this again
 * exactly then.
 */
static int i2c_addr_phase(const struct device *dev, uint16_t addr, bool read)
{
	uint32_t start = k_cycle_get_32();
	int ret;

	if (read) {
		/* Acknowledge received bytes; the end game drops it again */
		i2c_wr(dev, I2C_CR1, i2c_rd(dev, I2C_CR1) | I2C_CR1_ACK);
	}

	i2c_wr(dev, I2C_CR1, i2c_rd(dev, I2C_CR1) | I2C_CR1_START);

	ret = i2c_wait_bit(dev, I2C_SR1, I2C_SR1_SB, true);
	if (ret != 0) {
		return ret;
	}

	i2c_wr(dev, I2C_DR, ((uint32_t)addr << 1) | (read ? 1U : 0U));

	/*
	 * The address phase ends with ADDR set, or with a failure flag when
	 * nobody answers. Both have to be watched for in the same loop: waiting
	 * for ADDR alone would turn an absent target into a timeout per address
	 * instead of a fast NACK.
	 */
	for (;;) {
		uint32_t sr1 = i2c_rd(dev, I2C_SR1);

		if ((sr1 & I2C_SR1_ADDR) != 0U) {
			/* Reading SR2 releases ADDR and hands out the direction */
			(void)i2c_rd(dev, I2C_SR2);
			return 0;
		}

		if ((sr1 & I2C_SR1_ERR) != 0U) {
			(void)i2c_clear_errors(dev);
			return ((sr1 & I2C_SR1_AF) != 0U) ? -ENXIO : -EIO;
		}

		if (k_cyc_to_us_floor32(k_cycle_get_32() - start) >= I2C_FLAG_TIMEOUT_US) {
			printk("i2c: address phase timed out: CR1=%08x SR1=%08x SR2=%08x\n",
			       i2c_rd(dev, I2C_CR1), sr1, i2c_rd(dev, I2C_SR2));
			return -ETIMEDOUT;
		}
	}
}

static int i2c_write_msg(const struct device *dev, struct i2c_msg *msg, bool stop)
{
	int ret;
	uint32_t err;

	if (msg->len == 0U) {
		goto stop;
	}

	ret = i2c_wait_bit(dev, I2C_SR1, I2C_SR1_TXE, true);
	if (ret != 0) {
		return ret;
	}

	for (uint16_t i = 0U; i < msg->len; i++) {
		i2c_wr(dev, I2C_DR, msg->buf[i]);

		/*
		 * TXE (data register empty) for every byte but the last, BTF
		 * (data and shift register both empty) for the last one: the
		 * canonical flow of this peripheral family and what the Linux
		 * driver for the SoC does. Waiting for BTF after *every* byte was
		 * measured on the board to time out, so BTF cannot serve as the
		 * per-byte handshake here.
		 */
		ret = i2c_wait_bit(dev, I2C_SR1,
				   ((i + 1U) < msg->len) ? I2C_SR1_TXE : I2C_SR1_BTF, true);
		if (ret != 0) {
			return ret;
		}

		err = i2c_clear_errors(dev);
		if (err != 0U) {
			return ((err & I2C_SR1_AF) != 0U) ? -ENXIO : -EIO;
		}
	}

stop:
	if (stop) {
		i2c_wr(dev, I2C_CR1, i2c_rd(dev, I2C_CR1) | I2C_CR1_STOP);

		return i2c_wait_bit(dev, I2C_SR2, I2C_SR2_BUSY, false);
	}

	return 0;
}

/* Drop ACK/POS and optionally generate STOP: both receive end games need it */
static void i2c_read_end_game(const struct device *dev, bool stop)
{
	uint32_t cr1 = i2c_rd(dev, I2C_CR1) & ~(I2C_CR1_ACK | I2C_CR1_POS);

	if (stop) {
		cr1 |= I2C_CR1_STOP;
	}

	i2c_wr(dev, I2C_CR1, cr1);
}

static int i2c_read_msg(const struct device *dev, struct i2c_msg *msg, bool stop)
{
	uint32_t err;
	int ret;

	if (msg->len == 0U) {
		if (stop) {
			i2c_read_end_game(dev, true);

			return i2c_wait_bit(dev, I2C_SR2, I2C_SR2_BUSY, false);
		}

		return 0;
	}

	if (msg->len == 1U) {
		i2c_read_end_game(dev, stop);

		ret = i2c_wait_bit(dev, I2C_SR1, I2C_SR1_RXNE, true);
		if (ret != 0) {
			return ret;
		}

		msg->buf[0] = (uint8_t)i2c_rd(dev, I2C_DR);
	} else if (msg->len == 2U) {
		ret = i2c_wait_bit(dev, I2C_SR1, I2C_SR1_RXNE, true);
		if (ret != 0) {
			return ret;
		}

		msg->buf[0] = (uint8_t)i2c_rd(dev, I2C_DR);
		i2c_read_end_game(dev, stop);

		ret = i2c_wait_bit(dev, I2C_SR1, I2C_SR1_RXNE, true);
		if (ret != 0) {
			return ret;
		}

		msg->buf[1] = (uint8_t)i2c_rd(dev, I2C_DR);
	} else {
		uint16_t i = 0U;
		uint16_t remain = msg->len;

		while (remain > 3U) {
			ret = i2c_wait_bit(dev, I2C_SR1, I2C_SR1_RXNE, true);
			if (ret != 0) {
				return ret;
			}

			msg->buf[i++] = (uint8_t)i2c_rd(dev, I2C_DR);
			remain--;
		}

		ret = i2c_wait_bit(dev, I2C_SR1, I2C_SR1_BTF, true);
		if (ret != 0) {
			return ret;
		}

		msg->buf[i++] = (uint8_t)i2c_rd(dev, I2C_DR);

		i2c_read_end_game(dev, stop);

		ret = i2c_wait_bit(dev, I2C_SR1, I2C_SR1_BTF, true);
		if (ret != 0) {
			return ret;
		}

		msg->buf[i++] = (uint8_t)i2c_rd(dev, I2C_DR);

		ret = i2c_wait_bit(dev, I2C_SR1, I2C_SR1_RXNE, true);
		if (ret != 0) {
			return ret;
		}

		msg->buf[i++] = (uint8_t)i2c_rd(dev, I2C_DR);
	}

	err = i2c_clear_errors(dev);
	if (err != 0U) {
		return -EIO;
	}

	/*
	 * The end game generated STOP when this message ends the transfer; wait
	 * for the bus to be free so the next one can start from a clean state.
	 */
	if (stop) {
		return i2c_wait_bit(dev, I2C_SR2, I2C_SR2_BUSY, false);
	}

	return 0;
}

static int ls2k0300_i2c_transfer(const struct device *dev, struct i2c_msg *msgs,
				 uint8_t num_msgs, uint16_t addr)
{
	struct ls2k0300_i2c_data *data = dev->data;
	bool started = false;  /* START and the address phase are on the bus */
	bool cur_read = false; /* direction that address phase was for */
	int ret = 0;

	if (num_msgs == 0U) {
		return -EINVAL;
	}

	if ((addr > 0x7fU) || ((msgs[0].flags & I2C_MSG_ADDR_10_BITS) != 0U)) {
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	for (uint8_t i = 0U; i < num_msgs; i++) {
		struct i2c_msg *msg = &msgs[i];
		bool read = (msg->flags & I2C_MSG_READ) != 0U;
		/* The last message always ends the transfer with a STOP, unlike
		 * u-boot the caller's flags alone cannot be trusted for it.
		 */
		bool stop = ((msg->flags & I2C_MSG_STOP) != 0U) || ((i + 1U) == num_msgs);

		/*
		 * START and the address phase, once per direction. Messages that
		 * continue in the same direction are one bus transaction and are
		 * streamed back to back - that is how a control byte and its
		 * data reach the SSD1306 (i2c_burst_write sends them as two
		 * messages), and issuing a repeated start in between was the one
		 * structural difference between that path and the single-message
		 * writes that are known to work on this board. A direction change
		 * still gets a new START, which is what a register read needs.
		 */
		if (!started || (read != cur_read)) {
			ret = i2c_addr_phase(dev, addr, read);
			if (ret != 0) {
				break;
			}

			started = true;
			cur_read = read;
		}

		if (read) {
			ret = i2c_read_msg(dev, msg, stop);
		} else {
			ret = i2c_write_msg(dev, msg, stop);
		}

		if (ret == 0 && stop) {
			started = false;
		}

		if (ret != 0) {
			/*
			 * A NACK is a normal bus answer: a scan probes addresses
			 * nobody owns. It is neither worth a message nor allowed to
			 * touch the peripheral - resetting on it would abort a
			 * transfer that another thread is in the middle of, which is
			 * exactly what happens when a scan runs next to a display
			 * refresh.
			 */
			if (ret == -ENXIO) {
				/* An address that nobody owns is the normal answer
				 * while scanning, so it is not worth a message. It
				 * still has to be followed by a STOP so the bus is
				 * released for the next probe.
				 */
				i2c_wr(dev, I2C_CR1, i2c_rd(dev, I2C_CR1) | I2C_CR1_STOP);
				(void)i2c_wait_bit(dev, I2C_SR2, I2C_SR2_BUSY, false);
			} else {
				printk("i2c: transfer failed msg %u/%u len=%u flags=%02x "
				       "addr=0x%02x ret=%d SR1=%08x SR2=%08x\n",
				       i, num_msgs, msgs[i].len, msgs[i].flags, addr, ret,
				       i2c_rd(dev, I2C_SR1), i2c_rd(dev, I2C_SR2));
				i2c_reset(dev);
			}
			break;
		}
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int ls2k0300_i2c_configure(const struct device *dev, uint32_t dev_config)
{
	struct ls2k0300_i2c_data *data = dev->data;
	uint32_t bus_hz;

	/*
	 * Only the bus speed is taken from dev_config. The mode bit is not
	 * required: plenty of callers pass nothing but I2C_SPEED_SET(speed),
	 * and this controller cannot act as a target anyway.
	 */
	/*
	 * I2C_SPEED_GET(), not the raw field: in this API the speed constants
	 * (I2C_SPEED_STANDARD = 1, I2C_SPEED_FAST = 2, ...) are the unshifted
	 * codes and I2C_SPEED_SET()/GET() do the shifting. Comparing the
	 * shifted field against the constants matches the wrong branch - "speed
	 * 1" lands in the fast case and silently programmes 400 kHz - and
	 * rejects every other speed outright.
	 */
	switch (I2C_SPEED_GET(dev_config)) {
	case I2C_SPEED_STANDARD:
		bus_hz = 100000U;
		break;
	case I2C_SPEED_FAST:
		bus_hz = 400000U;
		break;
	default:
		return -ENOTSUP;
	}

	k_mutex_lock(&data->lock, K_FOREVER);
	data->bus_hz = bus_hz;
	i2c_program_speed(dev);
	k_mutex_unlock(&data->lock);

	return 0;
}

static int ls2k0300_i2c_get_config(const struct device *dev, uint32_t *dev_config)
{
	struct ls2k0300_i2c_data *data = dev->data;
	uint32_t speed = (data->bus_hz > 100000U) ? (uint32_t)I2C_SPEED_FAST
						  : (uint32_t)I2C_SPEED_STANDARD;

	*dev_config = I2C_SPEED_SET(speed) | I2C_MODE_CONTROLLER;

	return 0;
}

/*
 * Best effort: the peripheral can generate a STOP and be reset, which releases
 * a bus it is holding, but it cannot clock SCL by hand to free a target that is
 * stuck mid-byte - that would need the pins as GPIO.
 */
static int ls2k0300_i2c_recover_bus(const struct device *dev)
{
	struct ls2k0300_i2c_data *data = dev->data;

	k_mutex_lock(&data->lock, K_FOREVER);
	i2c_wr(dev, I2C_CR1, i2c_rd(dev, I2C_CR1) | I2C_CR1_STOP);
	(void)i2c_wait_bit(dev, I2C_SR2, I2C_SR2_BUSY, false);
	i2c_reset(dev);
	k_mutex_unlock(&data->lock);

	return 0;
}

static const struct i2c_driver_api ls2k0300_i2c_api = {
	.configure = ls2k0300_i2c_configure,
	.get_config = ls2k0300_i2c_get_config,
	.transfer = ls2k0300_i2c_transfer,
	.recover_bus = ls2k0300_i2c_recover_bus,
};

static int ls2k0300_i2c_init(const struct device *dev)
{
	const struct ls2k0300_i2c_config *cfg = dev->config;
	struct ls2k0300_i2c_data *data = dev->data;
	int ret;

	k_mutex_init(&data->lock);

	if (!device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_get_rate(cfg->clock_dev, cfg->clock_subsys, &data->clock_hz);
	if (ret != 0) {
		return ret;
	}

	if (IS_ENABLED(CONFIG_PINCTRL)) {
		ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			return ret;
		}
	}

	data->bus_hz = cfg->bus_hz;
	i2c_program_speed(dev);

	/*
	 * The peripheral has to hold what was written to it. A module whose clock
	 * gate is closed or that is held in reset reads back zeros and would then
	 * time out on every transfer. Only report it - the transfer path says
	 * more about the state than a refused init.
	 */
	if ((i2c_rd(dev, I2C_CR1) & I2C_CR1_PE) == 0U) {
		printk("i2c: CR1.PE does not stick: CR1=%08x CR2=%08x SR1=%08x SR2=%08x\n",
		       i2c_rd(dev, I2C_CR1), i2c_rd(dev, I2C_CR2), i2c_rd(dev, I2C_SR1),
		       i2c_rd(dev, I2C_SR2));
	}

	return 0;
}

#define LS2K0300_I2C_INIT(n)                                                      \
	IF_ENABLED(CONFIG_PINCTRL, (PINCTRL_DT_INST_DEFINE(n)));                  \
										  \
	static const struct ls2k0300_i2c_config ls2k0300_i2c_config_##n = {       \
		.base = DT_INST_REG_ADDR(n),                                      \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),               \
		.clock_subsys = (clock_control_subsys_t)DT_INST_PHA(n, clocks,    \
								    clkid), \
		.bus_hz = DT_INST_PROP_OR(n, clock_frequency, 100000),            \
		IF_ENABLED(CONFIG_PINCTRL,                                        \
			(.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),))             \
	};                                                                        \
										  \
	static struct ls2k0300_i2c_data ls2k0300_i2c_data_##n;                    \
										  \
	DEVICE_DT_INST_DEFINE(n, ls2k0300_i2c_init, NULL, &ls2k0300_i2c_data_##n, \
			      &ls2k0300_i2c_config_##n, POST_KERNEL,              \
			      CONFIG_I2C_INIT_PRIORITY, &ls2k0300_i2c_api);

DT_INST_FOREACH_STATUS_OKAY(LS2K0300_I2C_INIT)
