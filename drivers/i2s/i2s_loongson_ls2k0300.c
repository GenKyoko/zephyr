/*
 * Copyright (c) 2026 Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson 2K0300 I2S driver ("IIS", user manual chapter 11).
 *
 * The block is small: a version register, a format register, a control
 * register, one 8 byte FIFO port per direction, and a fractional divider for
 * the codec system clock (MCLK) that the bit clock is derived from:
 *
 *   0x0000 IISVersion  version / address and data width of the IP
 *   0x0004 IISConfig0  LR_LEN[31:24] TX_DEPTH[23:16] BCLK_RATIO[15:8] RX_DEPTH[7:0]
 *   0x0008 IISControl  MCLK_READY[16] MASTER[15] MSB[14] RX_EN[13] TX_EN[12]
 *                      RX_DMA_EN[11] CLK_READY[8] TX_DMA_EN[7] RESETn[4]
 *                      MCLK_EN[3] RX_INT_EN[1] TX_INT_EN[0]
 *   0x000c IISRxData   receive FIFO port
 *   0x0010 IISTxData   transmit FIFO port
 *   0xd014 IISConfig1  MCLK_RATIO[15:0] MCLK_RATIO_FRAC[32:16]
 *
 * Clocking (manual 11.2): MCLK = controller clock / (MCLK_RATIO +
 * FRAC/2^16) and BCLK = MCLK / (2 * (BCLK_RATIO + 1)). The driver therefore
 * picks the smallest BCLK divisor whose resulting MCLK is at least 256 times
 * the sample rate, and programs the fractional divider so that both come out
 * exact. Master mode only generates BCLK and the frame clock; the codec system
 * clock is only driven when the devicetree asks for it ("mclk-output"), because
 * a board may well clock its codec from a fixed oscillator.
 *
 * What this driver is and is not:
 *
 *   - it is polled. The port has no DMA controller driver yet, so instead of
 *     the DMA engine the vendor's Linux driver uses (which feeds the FIFOs in
 *     the background), each 32 bit FIFO word - two frames - is written or read
 *     at its sample instant, paced from the system cycle counter. i2s_write()
 *     therefore blocks for the duration of the buffer rather than queueing it,
 *     and both FIFOs have to be serviced by the same thread: interleave the
 *     two directions, or the 8 byte FIFO of the idle direction overruns.
 *   - it is stereo and I2S format only, like the hardware: the frame always has
 *     two words, the sample depth is the word size (8..32 bit), and the data
 *     order is MSB first.
 *   - its interrupt sources stay disabled. The manual defines them as
 *     "wrote while the FIFO was full" / "read while it was empty" notices, i.e.
 *     an overflow/underflow report, not a pacing interrupt - nothing a polled
 *     driver could use. Underruns and overruns are consequently not detected
 *     and the driver never enters I2S_STATE_ERROR.
 *   - the MCLK output and the MCLK divider are only meaningful when the codec
 *     takes its system clock from this controller; that is what the
 *     "mclk-output" property enables.
 */

#define DT_DRV_COMPAT loongson_ls2k0300_i2s

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define IIS_VERSION 0x0000U
#define IIS_CONFIG  0x0004U
#define IIS_CONTROL 0x0008U
#define IIS_RX_DATA 0x000cU
#define IIS_TX_DATA 0x0010U

/*
 * The manual puts config1 at 0xd014, while both the vendor's 2K0300 driver and
 * mainline's Loongson I2S driver write it at 0x14. The driver writes both
 * offsets: the one the hardware does not implement ignores the store, and at
 * least one of them reaches the divider.
 */
#define IIS_CONFIG1 0xd014U
#define IIS_CONFIG1_VENDOR 0x0014U

#define IIS_CFG_LR_LEN(x)     ((uint32_t)(x) << 24)
#define IIS_CFG_TX_DEPTH(x)   ((uint32_t)(x) << 16)
#define IIS_CFG_BCLK_RATIO(x) ((uint32_t)(x) << 8)
#define IIS_CFG_RX_DEPTH(x)   ((uint32_t)(x) << 0)

#define IIS_CTRL_MCLK_READY BIT(16)
#define IIS_CTRL_MASTER     BIT(15)
#define IIS_CTRL_MSB        BIT(14)
#define IIS_CTRL_RX_EN      BIT(13)
#define IIS_CTRL_TX_EN      BIT(12)
#define IIS_CTRL_RX_DMA_EN  BIT(11)
#define IIS_CTRL_CLK_READY  BIT(8)
#define IIS_CTRL_TX_DMA_EN  BIT(7)
#define IIS_CTRL_RESETN     BIT(4)
#define IIS_CTRL_MCLK_EN    BIT(3)
#define IIS_CTRL_RX_INT_EN  BIT(1)
#define IIS_CTRL_TX_INT_EN  BIT(0)

/* Both FIFOs hold 8 bytes, i.e. two 32 bit words */
#define IIS_FIFO_WORDS 2U

/* Sample rates that fit the divider of a 4 byte frame */
#define IIS_MAX_DIVIDER 0xffU

struct i2s_loongson_config {
	mem_addr_t base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pcfg;
	bool slave_mode;
	bool mclk_output;
};

struct i2s_loongson_data {
	/* Serializes configure/trigger and the two buffer paths */
	struct k_mutex lock;
	struct i2s_config cfg[2]; /* [0] = TX, [1] = RX */
	enum i2s_state state[2];
	uint32_t clock_hz;
	uint32_t cycles_per_word;
};

static inline int i2s_loongson_dir_idx(enum i2s_dir dir)
{
	return (dir == I2S_DIR_TX) ? 0 : 1;
}

static inline uint32_t i2s_loongson_reg_read(const struct i2s_loongson_config *cfg, uint32_t reg)
{
	return sys_read32(cfg->base + reg);
}

static inline void i2s_loongson_reg_write(const struct i2s_loongson_config *cfg, uint32_t reg,
				      uint32_t value)
{
	sys_write32(value, cfg->base + reg);
}

static inline void i2s_loongson_reg_update(const struct i2s_loongson_config *cfg, uint32_t reg,
				       uint32_t mask, uint32_t value)
{
	i2s_loongson_reg_write(cfg, reg, (i2s_loongson_reg_read(cfg, reg) & ~mask) | (value & mask));
}

/*
 * The control bits that describe the link itself, as opposed to the per
 * direction enables: out of soft reset, MSB first, master or slave, and the
 * MCLK output.
 */
static uint32_t i2s_loongson_control_base(const struct i2s_loongson_config *cfg)
{
	uint32_t val = IIS_CTRL_RESETN | IIS_CTRL_MSB;

	if (!cfg->slave_mode) {
		val |= IIS_CTRL_MASTER;
	}

	if (cfg->mclk_output) {
		val |= IIS_CTRL_MCLK_EN;
	}

	return val;
}

/* Wait until the cycle counter reaches @p due (used to pace the FIFO) */
static void i2s_loongson_wait_until(uint64_t due)
{
	while ((int64_t)(k_cycle_get_64() - due) < 0) {
		/* spin: the FIFO only holds two words, so there is nothing else
		 * to do in this window
		 */
	}
}

static int i2s_loongson_configure(const struct device *dev, enum i2s_dir dir,
				  const struct i2s_config *cfg)
{
	const struct i2s_loongson_config *dcfg = dev->config;
	struct i2s_loongson_data *data = dev->data;
	uint32_t word = cfg->word_size;
	uint32_t bclk_ratio, mclk_hz, mclk_int, mclk_frac, val;
	bool master;
	int idx;

	if ((dir != I2S_DIR_TX) && (dir != I2S_DIR_RX)) {
		return -EINVAL;
	}

	idx = i2s_loongson_dir_idx(dir);

	if (cfg->frame_clk_freq == 0U) {
		/* Release the configuration, the way the API defines it */
		data->state[idx] = I2S_STATE_NOT_READY;
		return 0;
	}

	if ((data->state[idx] != I2S_STATE_NOT_READY) &&
	    (data->state[idx] != I2S_STATE_READY)) {
		return -EIO;
	}

	/*
	 * The hardware is stereo only and always in I2S format: two words per
	 * frame, MSB first. Everything else is refused here rather than
	 * producing a wrong bit pattern on the wire.
	 */
	if ((cfg->channels != 2U) || (word < 8U) || (word > 32U) ||
	    ((cfg->format & I2S_FMT_DATA_FORMAT_MASK) != I2S_FMT_DATA_FORMAT_I2S) ||
	    ((cfg->format & I2S_FMT_DATA_ORDER_LSB) != 0U) ||
	    ((cfg->format & I2S_FMT_CLK_FORMAT_MASK) != I2S_FMT_CLK_NF_NB) ||
	    ((cfg->options & I2S_OPT_LOOPBACK) != 0U)) {
		return -EINVAL;
	}

	/*
	 * The two directions share the format and the bit clock registers, so
	 * both have to be configured with the same rate and word size.
	 */
	if (data->state[1 - idx] == I2S_STATE_READY) {
		const struct i2s_config *other = &data->cfg[1 - idx];

		if ((other->frame_clk_freq != cfg->frame_clk_freq) ||
		    (other->word_size != cfg->word_size)) {
			return -EINVAL;
		}
	}

	/*
	 * Who generates the clocks has to match the wiring. The caller
	 * describes its own role through the options: the controller (the
	 * default) or the target of an external bit/frame clock.
	 */
	master = (cfg->options & (I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET)) == 0U;
	if (master == dcfg->slave_mode) {
		return -EINVAL;
	}

	/*
	 * One FIFO word carries two frames, so its period is 2 / frame_clk_freq
	 * seconds; the pacing below uses the 64 bit cycle counter, which is why
	 * the rate has to fit.
	 */
	if ((cfg->frame_clk_freq > (sys_clock_hw_cycles_per_sec() / 2U)) ||
	    ((cfg->block_size == 0U) || ((cfg->block_size % sizeof(uint32_t)) != 0U))) {
		return -EINVAL;
	}

	/*
	 * Pick the smallest bit clock divisor whose MCLK is at least 256 times
	 * the sample rate, keeping BCLK = MCLK / (2 * (ratio + 1)) exact:
	 *
	 *   MCLK = 4 * (ratio + 1) * word * frame_clk_freq
	 */
	bclk_ratio = DIV_ROUND_UP(64U, word) - 1U;
	if (bclk_ratio > IIS_MAX_DIVIDER) {
		return -EINVAL;
	}

	mclk_hz = 4U * (bclk_ratio + 1U) * word * cfg->frame_clk_freq;

	/* MCLK = controller clock / (integer + fraction / 2^16) */
	mclk_int = data->clock_hz / mclk_hz;
	mclk_frac = (uint32_t)(((uint64_t)(data->clock_hz - (mclk_int * mclk_hz)) << 16) /
			       mclk_hz);

	if ((mclk_int == 0U) || (mclk_int > 0xffffU)) {
		return -EINVAL;
	}

	val = IIS_CFG_LR_LEN(word) | IIS_CFG_TX_DEPTH(word) | IIS_CFG_BCLK_RATIO(bclk_ratio) |
	      IIS_CFG_RX_DEPTH(word);

	k_mutex_lock(&data->lock, K_FOREVER);

	i2s_loongson_reg_write(dcfg, IIS_CONFIG, val);
	i2s_loongson_reg_write(dcfg, IIS_CONFIG1, (mclk_frac << 16) | mclk_int);
	i2s_loongson_reg_write(dcfg, IIS_CONFIG1_VENDOR, (mclk_frac << 16) | mclk_int);

	/*
	 * Program the link itself and make sure neither the DMA enables (the
	 * FIFOs are fed by the CPU here) nor the interrupt sources are set.
	 */
	i2s_loongson_reg_update(dcfg, IIS_CONTROL,
			    IIS_CTRL_MASTER | IIS_CTRL_MSB | IIS_CTRL_RESETN | IIS_CTRL_MCLK_EN |
				    IIS_CTRL_TX_DMA_EN | IIS_CTRL_RX_DMA_EN |
				    IIS_CTRL_TX_INT_EN | IIS_CTRL_RX_INT_EN,
			    i2s_loongson_control_base(dcfg));

	data->cfg[idx] = *cfg;
	data->cycles_per_word = (uint32_t)((2ULL * sys_clock_hw_cycles_per_sec()) /
					   cfg->frame_clk_freq);
	data->state[idx] = I2S_STATE_READY;

	k_mutex_unlock(&data->lock);

	return 0;
}

static const struct i2s_config *i2s_loongson_config_get(const struct device *dev,
						       enum i2s_dir dir)
{
	struct i2s_loongson_data *data = dev->data;
	int idx;

	if ((dir != I2S_DIR_TX) && (dir != I2S_DIR_RX)) {
		return NULL;
	}

	idx = i2s_loongson_dir_idx(dir);

	return (data->state[idx] == I2S_STATE_READY) ? &data->cfg[idx] : NULL;
}

static int i2s_loongson_write(const struct device *dev, void *mem_block, size_t size)
{
	const struct i2s_loongson_config *cfg = dev->config;
	struct i2s_loongson_data *data = dev->data;
	const uint32_t *words = mem_block;
	size_t count = size / sizeof(uint32_t);
	uint64_t due = k_cycle_get_64();
	int ret = 0;

	if ((data->state[0] != I2S_STATE_RUNNING)) {
		return -EIO;
	}

	if ((size == 0U) || ((size % sizeof(uint32_t)) != 0U)) {
		return -EINVAL;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	/*
	 * Feed the FIFO one 32 bit word (two frames) per sample period. The
	 * pacing starts at "now": the caller is expected to interleave this
	 * with i2s_read() of the other direction, which keeps both FIFOs at
	 * one or two words.
	 */
	for (size_t i = 0U; i < count; i++) {
		if (i > 0U) {
			due += data->cycles_per_word;
			i2s_loongson_wait_until(due);
		}

		i2s_loongson_reg_write(cfg, IIS_TX_DATA, words[i]);
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int i2s_loongson_read(const struct device *dev, void **mem_block, size_t *size)
{
	const struct i2s_loongson_config *cfg = dev->config;
	struct i2s_loongson_data *data = dev->data;
	uint32_t *words;
	size_t count;
	uint64_t due;
	k_timeout_t timeout;
	int ret;

	if (data->state[1] != I2S_STATE_RUNNING) {
		return -EIO;
	}

	/* The API gives the read timeout in milliseconds; the slab takes a
	 * k_timeout_t, and a negative value means "wait forever".
	 */
	timeout = (data->cfg[1].timeout == 0)     ? K_NO_WAIT
		  : (data->cfg[1].timeout < 0)    ? K_FOREVER
						  : K_MSEC(data->cfg[1].timeout);

	ret = k_mem_slab_alloc(data->cfg[1].mem_slab, (void **)&words, timeout);
	if (ret != 0) {
		return -EAGAIN;
	}

	count = data->cfg[1].block_size / sizeof(uint32_t);
	due = k_cycle_get_64();

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Same pacing as the transmit path, but the other way around */
	for (size_t i = 0U; i < count; i++) {
		if (i > 0U) {
			due += data->cycles_per_word;
			i2s_loongson_wait_until(due);
		}

		words[i] = i2s_loongson_reg_read(cfg, IIS_RX_DATA);
	}

	k_mutex_unlock(&data->lock);

	*mem_block = words;
	*size = data->cfg[1].block_size;

	return 0;
}

static int i2s_loongson_trigger(const struct device *dev, enum i2s_dir dir,
				enum i2s_trigger_cmd cmd)
{
	const struct i2s_loongson_config *cfg = dev->config;
	struct i2s_loongson_data *data = dev->data;
	uint32_t enable;
	int ret = 0;

	if ((dir != I2S_DIR_TX) && (dir != I2S_DIR_RX)) {
		return -EINVAL;
	}

	enable = (dir == I2S_DIR_TX) ? IIS_CTRL_TX_EN : IIS_CTRL_RX_EN;

	k_mutex_lock(&data->lock, K_FOREVER);

	switch (cmd) {
	case I2S_TRIGGER_START:
		if (data->state[i2s_loongson_dir_idx(dir)] != I2S_STATE_READY) {
			ret = -EIO;
			break;
		}

		i2s_loongson_reg_update(cfg, IIS_CONTROL, enable, enable);
		data->state[i2s_loongson_dir_idx(dir)] = I2S_STATE_RUNNING;
		break;

	case I2S_TRIGGER_STOP:
		if (data->state[i2s_loongson_dir_idx(dir)] != I2S_STATE_RUNNING) {
			ret = -EIO;
			break;
		}

		i2s_loongson_reg_update(cfg, IIS_CONTROL, enable, 0U);
		data->state[i2s_loongson_dir_idx(dir)] = I2S_STATE_READY;
		break;

	case I2S_TRIGGER_DRAIN:
		if (data->state[i2s_loongson_dir_idx(dir)] != I2S_STATE_RUNNING) {
			ret = -EIO;
			break;
		}

		/*
		 * Nothing is queued in software: i2s_write() has already pushed
		 * its data into the FIFO, so draining means waiting for the two
		 * words that are in there to be shifted out.
		 */
		if (dir == I2S_DIR_TX) {
			uint64_t due = k_cycle_get_64() +
				       (3ULL * data->cycles_per_word);

			i2s_loongson_wait_until(due);
		}

		i2s_loongson_reg_update(cfg, IIS_CONTROL, enable, 0U);
		data->state[i2s_loongson_dir_idx(dir)] = I2S_STATE_READY;
		break;

	case I2S_TRIGGER_DROP:
		i2s_loongson_reg_update(cfg, IIS_CONTROL, enable, 0U);

		/* The soft reset is the only way to empty a FIFO */
		i2s_loongson_reg_update(cfg, IIS_CONTROL, IIS_CTRL_RESETN, 0U);
		i2s_loongson_reg_update(cfg, IIS_CONTROL, IIS_CTRL_RESETN, IIS_CTRL_RESETN);

		data->state[0] = I2S_STATE_READY;
		data->state[1] = I2S_STATE_READY;
		break;

	case I2S_TRIGGER_PREPARE:
		/* Underruns are not detected, so nothing is ever in ERROR */
		if (data->state[i2s_loongson_dir_idx(dir)] != I2S_STATE_ERROR) {
			ret = -EIO;
			break;
		}

		data->state[i2s_loongson_dir_idx(dir)] = I2S_STATE_READY;
		break;

	default:
		ret = -EINVAL;
		break;
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static DEVICE_API(i2s, i2s_loongson_api) = {
	.configure = i2s_loongson_configure,
	.config_get = i2s_loongson_config_get,
	.read = i2s_loongson_read,
	.write = i2s_loongson_write,
	.trigger = i2s_loongson_trigger,
};

static int i2s_loongson_init(const struct device *dev)
{
	const struct i2s_loongson_config *cfg = dev->config;
	struct i2s_loongson_data *data = dev->data;
	int ret;

	k_mutex_init(&data->lock);

	if (!device_is_ready(cfg->clock_dev)) {
		return -ENODEV;
	}

	ret = clock_control_get_rate(cfg->clock_dev, cfg->clock_subsys, &data->clock_hz);
	if ((ret != 0) || (data->clock_hz == 0U)) {
		return -EINVAL;
	}

	if (IS_ENABLED(CONFIG_PINCTRL)) {
		ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			return ret;
		}
	}

	/*
	 * Put the controller into a known state: both directions disabled, no
	 * DMA, no interrupts, out of soft reset with MSB first. The reset pulse
	 * empties whatever the previous stage left in the FIFOs.
	 */
	i2s_loongson_reg_update(cfg, IIS_CONTROL,
			    IIS_CTRL_MASTER | IIS_CTRL_MSB | IIS_CTRL_RESETN | IIS_CTRL_MCLK_EN |
				    IIS_CTRL_TX_EN | IIS_CTRL_RX_EN | IIS_CTRL_TX_DMA_EN |
				    IIS_CTRL_RX_DMA_EN | IIS_CTRL_TX_INT_EN | IIS_CTRL_RX_INT_EN,
			    0U);
	i2s_loongson_reg_update(cfg, IIS_CONTROL, IIS_CTRL_RESETN, IIS_CTRL_RESETN);

	data->state[0] = I2S_STATE_NOT_READY;
	data->state[1] = I2S_STATE_NOT_READY;

	return 0;
}

#define I2S_LOONGSON_INIT(n)								\
	PINCTRL_DT_INST_DEFINE(n);							\
											\
	static struct i2s_loongson_data i2s_loongson_data_##n;				\
											\
	static const struct i2s_loongson_config i2s_loongson_cfg_##n = {		\
		.base = DT_INST_REG_ADDR(n),						\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),			\
		.clock_subsys =								\
			(clock_control_subsys_t)DT_INST_PHA(n, clocks, clkid),		\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),				\
		.slave_mode = DT_INST_PROP(n, slave_mode),				\
		.mclk_output = DT_INST_PROP(n, mclk_output),				\
	};										\
											\
	DEVICE_DT_INST_DEFINE(n, i2s_loongson_init, NULL, &i2s_loongson_data_##n,	\
			      &i2s_loongson_cfg_##n, POST_KERNEL,			\
			      CONFIG_I2S_INIT_PRIORITY, &i2s_loongson_api);

DT_INST_FOREACH_STATUS_OKAY(I2S_LOONGSON_INIT)
