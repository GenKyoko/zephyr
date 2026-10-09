/*
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
 *   - the data path is driven by the LSIA DMA controller (drivers/dma/
 *     dma_loongson_lsia.c): each direction runs one circular channel over a
 *     two block ring, paced by the controller's own data requests, the way
 *     the vendor's Linux driver feeds the FIFOs. The half and wrap boundaries
 *     arrive as interrupts that release a ring half to i2s_write() /
 *     i2s_read(), so a write blocks only until the engine is done with the
 *     half it refills - not for the whole buffer - and the two directions can
 *     run from independent threads.
 *   - its data format is fixed in hardware (there is no format register - the
 *     vendor driver's set_dai_fmt() writes nothing), so the driver accepts
 *     whatever format the caller asks for and it is the codec that has to
 *     match; refusing would not change the wire and would only make the pair
 *     harder to find. What it does check is that the bit pattern cannot come out
 *     wrong: it is stereo only, the frame always has
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
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/gpio.h>
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
 * The manual puts config1 at 0xd014, but both the vendor's 2K0300 driver and
 * mainline's Loongson I2S driver write it at 0x14 - and on this silicon the
 * value demonstrably lands at 0x14 while 0xd014 always reads 0, so the manual's
 * offset is not implemented. Only 0x14 is written.
 */
#define IIS_CONFIG1 0x0014U

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

/* Both FIFOs hold 8 bytes, i.e. two 32 bit words */
#define IIS_FIFO_WORDS 2U

/* Sample rates that fit the divider of a 4 byte frame */
#define IIS_MAX_DIVIDER 0xffU

/*
 * The DMA rings are handed to the engine by the generic driver, which does
 * the virtual to physical translation itself (the low 32 bits of any of the
 * port's addresses are the physical one). The rings are ordinary memory and
 * the CPU touches them directly; that only works because the SoC start-up
 * leaves the data cache off (see the cache note in the SoC file) - with a
 * caching CPU the bytes written here could still be dirty in cache while the
 * engine reads the memory behind its back. Once this port has cache
 * maintenance, this is where a flush before the transfer and an invalidate
 * after it belong.
 */
static inline void *iis_ring(const void *addr)
{
	return (void *)addr;
}

/* Bytes per half of the DMA ring; the API's block size has to fit in one */
#define IIS_RING_BLOCK_MAX 1024U

/* One ring is two halves, which is also what the boundary semaphore counts */
#define IIS_RING_HALVES 2U

struct i2s_loongson_config {
	mem_addr_t base;
	const struct device *clock_dev;
	clock_control_subsys_t clock_subsys;
	const struct pinctrl_dev_config *pcfg;
	bool slave_mode;
	bool mclk_output;
	/* The bit clock divider counts the reference clock instead of the system clock */
	bool bclk_from_reference;
	/*
	 * The system clock (MCLK) as a multiple of the frame rate - the "xfs"
	 * of the vendor driver, which defaults it to 128 and is what its working
	 * configuration programs on this SoC (its board trees set nothing, so
	 * 128 it is). 128 * 48 kHz = 6.144 MHz at the codec.
	 */
	uint32_t mclk_xfs;
	/* The codec's data out, which has to be an input pad */
	struct gpio_dt_spec data_in;
	/* The DMA controller and the channels the data requests are wired to */
	const struct device *dma_dev;
	uint32_t dma_tx_ch;
	uint32_t dma_rx_ch;
};

struct i2s_loongson_data {
	/* Serializes configure/trigger and the two buffer paths */
	struct k_mutex lock;
	struct i2s_config cfg[2]; /* [0] = TX, [1] = RX */
	enum i2s_state state[2];
	uint32_t clock_hz;
	/*
	 * The data path is a DMA ring of two blocks per direction: the engine
	 * moves the words, paced by the controller's own requests, and the CPU
	 * only waits for room. The rings are ordinary memory; that the CPU can
	 * hand them to the engine without cache maintenance is the iis_ring()
	 * note.
	 */
	uint8_t tx_ring[2 * IIS_RING_BLOCK_MAX];
	uint8_t rx_ring[2 * IIS_RING_BLOCK_MAX];
	uint8_t *tx_ring_unc;
	uint8_t *rx_ring_unc;
	/*
	 * One count per ring half boundary: the engine raises the half flag
	 * going into the second half and the complete flag on the wrap, and
	 * either boundary means "one more half dealt with" - which is what a
	 * write waits for before refilling and a read before handing out. The
	 * count tops at the ring's two halves, the way the status flags the
	 * polled driver read were sticky.
	 */
	struct k_sem half_done[2];
	uint32_t block_size; /* bytes per ring half, from the configuration */
	uint8_t tx_half;     /* the half i2s_write() fills next */
	uint8_t rx_half;     /* the half i2s_read() hands out next */
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
 * Bring the clock tree up in one write, out of soft reset.
 *
 * The vendor's PCM probe looks like a staged bring-up (writel(0x8), wait for
 * MCLK_READY, writel(0x8008), wait for CLK_READY), but on this silicon both
 * ready bits read 1 even with the clock outputs off - so on the vendor kernel
 * those waits fall through immediately and the two stores are effectively
 * back to back. Holding the block in soft reset the way a literal reading of
 * that sequence suggests (this port did: RESETn stayed 0 from init until the
 * first trigger) is NOT what the vendor does, and the board punishes it: with
 * the dividers otherwise programmed exactly like the vendor's, the bit clock,
 * frame clock and data pads never drive again (the receive DMA keeps counting
 * on the internal clock, while the codec - which only sees the pads - goes
 * silent and its ADC flatlines).
 */
static void i2s_loongson_clock_start(const struct i2s_loongson_config *cfg)
{
	uint32_t val = IIS_CTRL_RESETN | IIS_CTRL_MSB;

	if (!cfg->slave_mode) {
		val |= IIS_CTRL_MASTER;
	}

	if (cfg->mclk_output) {
		val |= IIS_CTRL_MCLK_EN;
	}

	i2s_loongson_reg_write(cfg, IIS_CONTROL, val);
}

/*
 * The DMA callback of both directions. The engine crosses a ring half
 * boundary when the half flag stands - it left half 0 - or when it wrapped,
 * and either boundary is "one more half dealt with", which is exactly what
 * the producer and the consumer wait for. A transfer error takes the stream
 * down; the API's PREPARE trigger is how it comes back.
 */
static void i2s_loongson_dma_cb(const struct device *dma_dev, void *user_data, uint32_t channel,
				int status)
{
	const struct device *dev = user_data;
	const struct i2s_loongson_config *cfg = dev->config;
	struct i2s_loongson_data *data = dev->data;
	int idx = (channel == cfg->dma_tx_ch) ? 0 : 1;

	if (status < 0) {
		printk("I2S: dma channel %u error %d - stream down\n", channel, status);
		data->state[idx] = I2S_STATE_ERROR;
		return;
	}

	k_sem_give(&data->half_done[idx]);
}

/*
 * Point one direction's channel at its ring and set it running: circular,
 * memory increments, four bytes on both sides (a stereo frame is what one 32
 * bit word carries, and the vendor's PCM driver programs the same widths),
 * half and wrap boundaries armed. The count register takes beats, so the
 * ring length in words.
 */
static int iis_dma_setup(const struct device *dev, enum i2s_dir dir, uint8_t *ring)
{
	const struct i2s_loongson_config *cfg = dev->config;
	struct i2s_loongson_data *data = dev->data;
	bool to_periph = (dir == I2S_DIR_TX);
	uint32_t ch = to_periph ? cfg->dma_tx_ch : cfg->dma_rx_ch;
	struct dma_config config = {0};
	struct dma_block_config block = {0};
	int ret;

	block.block_size = 2U * data->block_size;
	if (to_periph) {
		block.source_address = (uint32_t)(uintptr_t)ring;
		block.dest_address = (uint32_t)(uintptr_t)(cfg->base + IIS_TX_DATA);
		block.source_addr_adj = DMA_ADDR_ADJ_INCREMENT;
		block.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	} else {
		block.source_address = (uint32_t)(uintptr_t)(cfg->base + IIS_RX_DATA);
		block.dest_address = (uint32_t)(uintptr_t)ring;
		block.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
		block.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	}

	config.channel_direction = to_periph ? MEMORY_TO_PERIPHERAL : PERIPHERAL_TO_MEMORY;
	config.source_data_size = sizeof(uint32_t);
	config.dest_data_size = sizeof(uint32_t);
	config.channel_priority = 2U;
	config.head_block = &block;
	config.block_count = 1U;
	config.cyclic = true;
	config.half_complete_callback_en = 1U;
	config.dma_callback = i2s_loongson_dma_cb;
	config.user_data = (void *)dev;

	ret = dma_config(cfg->dma_dev, ch, &config);
	if (ret != 0) {
		return ret;
	}

	return dma_start(cfg->dma_dev, ch);
}

/*
 * Wait until the engine has crossed the next ring half boundary. The API's
 * negative timeout means "wait forever", and so does zero in this port - the
 * reading the polled driver gave it, kept so behaviour does not shift; a
 * positive value bounds the wait. What the engine was doing matters when the
 * wait fails: a channel that is not counting down never saw a request, and
 * one that is part way through is running late. Both look like this from
 * here, and the difference decides where to look.
 */
static int iis_dma_wait_half(const struct i2s_loongson_config *cfg, struct i2s_loongson_data *data,
			     int idx, int32_t timeout)
{
	k_timeout_t wait = (timeout <= 0) ? K_FOREVER : K_MSEC((uint32_t)timeout);
	uint32_t ch = (idx == 0) ? cfg->dma_tx_ch : cfg->dma_rx_ch;
	struct dma_status stat;
	int ret;

	ret = k_sem_take(&data->half_done[idx], wait);
	if (ret != 0) {
		stat.pending_length = 0U;
		stat.busy = false;
		(void)dma_get_status(cfg->dma_dev, ch, &stat);
		printk("I2S: dma channel %u did not cross a ring boundary in %d ms "
		       "(pending %u bytes, %s)\n",
		       ch, timeout, stat.pending_length, stat.busy ? "busy" : "idle");
	}

	return ret;
}

static int i2s_loongson_configure(const struct device *dev, enum i2s_dir dir,
				  const struct i2s_config *cfg)
{
	const struct i2s_loongson_config *dcfg = dev->config;
	struct i2s_loongson_data *data = dev->data;
	uint32_t word = cfg->word_size;
	uint32_t fmt;
	uint32_t frame_clk_hz;
	uint32_t bclk_ratio, mclk_hz, mclk_int, mclk_frac, divider, val;
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
	 * The hardware is stereo only: two words per frame, MSB first. What is
	 * refused here is what would put a wrong bit pattern on the wire.
	 *
	 * The data format (I2S against left or right justified) is deliberately
	 * not checked: this block has no format register - the vendor driver's
	 * set_dai_fmt() writes nothing - so its wire format is fixed in silicon
	 * and the codec has to be told the matching one. Accepting the format the
	 * caller passes lets a codec be configured to either and the pair be
	 * found by listening.
	 */
	fmt = cfg->format & I2S_FMT_DATA_FORMAT_MASK;

	if ((cfg->channels != 2U) || (word < 8U) || (word > 32U) ||
	    ((fmt != I2S_FMT_DATA_FORMAT_I2S) &&
	     (fmt != I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED) &&
	     (fmt != I2S_FMT_DATA_FORMAT_RIGHT_JUSTIFIED)) ||
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
	 * Pick the bit clock division, and the system clock that goes with it.
	 *
	 * The default is the vendor's revision-1 clocking, the one its working
	 * driver programs on this SoC: the system clock is "xfs" times the frame
	 * rate (xfs = 128, the value its device trees never override), and the
	 * BCLK_RATIO field divides that system clock by 2*(ratio+1):
	 *
	 *   MCLK = xfs * frame rate            128 * 48 kHz = 6.144 MHz
	 *   BCLK = word * 2 * frame rate       1.536 MHz at 16 bit
	 *   BCLK_RATIO = MCLK/(2*BCLK) - 1     = 1
	 *
	 * (Earlier this port picked the smallest ratio whose MCLK reached
	 * 256 times the frame rate - 12.288 MHz and ratio 3, both self consistent
	 * but never what the working reference programs. Reproducing the vendor's
	 * numbers bit for bit is the point now.)
	 *
	 * Two models exist for what BCLK_RATIO divides, and the vendor driver
	 * uses a different one depending on the IP revision it detects: the
	 * system clock (its revision 1, this default), or straight from the
	 * block's reference clock (its revision 0, see "loongson,bclk-from-
	 * reference"), which cannot hit the frame rate exactly - in that second
	 * case the rate the divider really produces is taken, and the system
	 * clock is derived from it as 256 times the frame rate so that the ratio
	 * the codec's rate register describes still holds.
	 */
	if (dcfg->bclk_from_reference) {
		uint64_t ratio = DIV_ROUND_CLOSEST((uint64_t)data->clock_hz,
						   2ULL * 2ULL * word * cfg->frame_clk_freq);
		uint64_t lrclk;

		bclk_ratio = (ratio > 0U) ? (uint32_t)(ratio - 1U) : 0U;
		if (bclk_ratio > IIS_MAX_DIVIDER) {
			return -EINVAL;
		}

		lrclk = (uint64_t)data->clock_hz /
			(4ULL * (bclk_ratio + 1U) * word);
		mclk_hz = (uint32_t)(256ULL * lrclk);

		/* The wire runs at that rate, not at the one that was asked for */
		frame_clk_hz = (uint32_t)lrclk;
	} else {
		uint32_t bclk_hz = 2U * word * cfg->frame_clk_freq;

		mclk_hz = dcfg->mclk_xfs * cfg->frame_clk_freq;
		if (mclk_hz < 2U * bclk_hz) {
			return -EINVAL;
		}

		/* The vendor's DIV_ROUND_CLOSEST, which lands on 1 at 48 kHz */
		bclk_ratio = DIV_ROUND_CLOSEST(mclk_hz, 2U * bclk_hz) - 1U;
		if (bclk_ratio > IIS_MAX_DIVIDER) {
			return -EINVAL;
		}

		frame_clk_hz = cfg->frame_clk_freq;
	}

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

	divider = (mclk_frac << 16) | mclk_int;

	/*
	 * Only the two format registers go here; the control register keeps the
	 * state the vendor's probe left it in (clock tree up, serial side in
	 * soft reset) until a stream is triggered. The vendor writes exactly
	 * these two registers in its hw_params as well - at 0x14, which is where
	 * the divider of this silicon demonstrably lands (the manual's 0xd014
	 * is not implemented and always reads 0).
	 */
	i2s_loongson_reg_write(dcfg, IIS_CONFIG, val);
	i2s_loongson_reg_write(dcfg, IIS_CONFIG1, divider);

	printk("I2S: reference %u Hz -> MCLK %u Hz (divider %u + %u/65536), BCLK %u Hz "
	       "(%s model), frames %u Hz\n",
	       data->clock_hz, mclk_hz, mclk_int, mclk_frac,
	       dcfg->bclk_from_reference
		       ? data->clock_hz / (2U * (bclk_ratio + 1U))
		       : mclk_hz / (2U * (bclk_ratio + 1U)),
	       dcfg->bclk_from_reference ? "reference" : "system clock", frame_clk_hz);

	if ((cfg->block_size == 0U) || ((cfg->block_size % sizeof(uint32_t)) != 0U) ||
	    (cfg->block_size > IIS_RING_BLOCK_MAX)) {
		k_mutex_unlock(&data->lock);
		return -EINVAL;
	}

	data->block_size = cfg->block_size;

	/*
	 * Say when the divider did not take: without it the bit clock comes out
	 * of an unprogrammed (much faster) system clock, which is heard as noise
	 * instead of audio.
	 */
	if (i2s_loongson_reg_read(dcfg, IIS_CONFIG1) != divider) {
		printk("I2S: system clock divider did not stick - wrote %08x to 0x%04x, "
		       "read %08x\n",
		       divider, (uint32_t)IIS_CONFIG1,
		       i2s_loongson_reg_read(dcfg, IIS_CONFIG1));
	}

	data->cfg[idx] = *cfg;
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
	struct i2s_loongson_data *data = dev->data;
	uint32_t half = data->tx_half;
	int32_t timeout_ms;
	int ret;

	if ((data->state[0] != I2S_STATE_RUNNING)) {
		return -EIO;
	}

	if ((size == 0U) || (size > data->block_size) || ((size % sizeof(uint32_t)) != 0U)) {
		return -EINVAL;
	}

	timeout_ms = data->cfg[0].timeout;

	k_mutex_lock(&data->lock, K_FOREVER);

	/*
	 * The engine moves the data, paced by the controller's own requests, so
	 * nothing here has to reproduce the sample rate: what the caller waits
	 * for is room in the ring, i.e. the previous half having been played.
	 * That is the blocking the CPU-fed FIFO path could not offer - it wrote
	 * into a two word FIFO on a schedule of its own.
	 */
	ret = iis_dma_wait_half(dev->config, data, 0, timeout_ms);
	if (ret == 0) {
		uint8_t *dst = data->tx_ring_unc + (half * data->block_size);

		memcpy(dst, mem_block, size);
		memset(&dst[size], 0, data->block_size - size);
		data->tx_half = half ^ 1U;
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int i2s_loongson_read(const struct device *dev, void **mem_block, size_t *size)
{
	const struct i2s_loongson_config *cfg = dev->config;
	struct i2s_loongson_data *data = dev->data;
	uint32_t half = data->rx_half;
	int32_t timeout_ms;
	k_timeout_t timeout;
	void *words;
	int ret;

	if (data->state[1] != I2S_STATE_RUNNING) {
		return -EIO;
	}

	timeout_ms = data->cfg[1].timeout;

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Room on the other side: the engine has filled the half we hand out */
	ret = iis_dma_wait_half(cfg, data, 1, timeout_ms);

	k_mutex_unlock(&data->lock);

	if (ret != 0) {
		return ret;
	}

	/* The API gives the read timeout in milliseconds; the slab takes a
	 * k_timeout_t, and a negative value means "wait forever".
	 */
	timeout = (data->cfg[1].timeout == 0)  ? K_NO_WAIT
		  : (data->cfg[1].timeout < 0) ? K_FOREVER
						: K_MSEC(data->cfg[1].timeout);

	ret = k_mem_slab_alloc(data->cfg[1].mem_slab, &words, timeout);
	if (ret != 0) {
		return -EAGAIN;
	}

	memcpy(words, data->rx_ring_unc + (half * data->block_size), data->block_size);

	k_mutex_lock(&data->lock, K_FOREVER);
	data->rx_half = half ^ 1U;
	k_mutex_unlock(&data->lock);

	*mem_block = words;
	*size = data->block_size;

	return 0;
}

static int i2s_loongson_trigger(const struct device *dev, enum i2s_dir dir,
				enum i2s_trigger_cmd cmd)
{
	const struct i2s_loongson_config *cfg = dev->config;
	struct i2s_loongson_data *data = dev->data;
	uint32_t dir_en, link;
	int ret = 0;

	if ((dir != I2S_DIR_TX) && (dir != I2S_DIR_RX)) {
		return -EINVAL;
	}

	/*
	 * The vendor trigger brings a stream up as one read-modify-write of
	 * 0xc010 | the direction's two bits - the link bits (soft reset off,
	 * MSB first, master) and the direction enable together with the
	 * direction's DMA enable, which is what lets the controller's data
	 * requests out to the engine. STOP only clears the direction bits and
	 * leaves the link up, the way the vendor does; the clock tree itself
	 * came up in i2s_loongson_init() and is never touched here.
	 */
	dir_en = (dir == I2S_DIR_TX) ? (IIS_CTRL_TX_EN | IIS_CTRL_TX_DMA_EN)
				     : (IIS_CTRL_RX_EN | IIS_CTRL_RX_DMA_EN);
	link = IIS_CTRL_RESETN | IIS_CTRL_MSB |
	       (cfg->slave_mode ? 0U : IIS_CTRL_MASTER);

	k_mutex_lock(&data->lock, K_FOREVER);

	switch (cmd) {
	case I2S_TRIGGER_START:
		if (data->state[i2s_loongson_dir_idx(dir)] != I2S_STATE_READY) {
			ret = -EIO;
			break;
		}

		/*
		 * Both halves start out as silence, so the engine has valid data
		 * to move before the first block arrives, and then it runs on its
		 * own requests until stopped. The boundary count starts empty:
		 * flags left over from an earlier run would be taken for
		 * progress.
		 */
		if (dir == I2S_DIR_TX) {
			memset(data->tx_ring_unc, 0, 2U * data->block_size);
			data->tx_half = 0U;
			k_sem_init(&data->half_done[0], 0, IIS_RING_HALVES);
			ret = iis_dma_setup(dev, I2S_DIR_TX, data->tx_ring_unc);
		} else {
			memset(data->rx_ring_unc, 0, 2U * data->block_size);
			data->rx_half = 0U;
			k_sem_init(&data->half_done[1], 0, IIS_RING_HALVES);
			ret = iis_dma_setup(dev, I2S_DIR_RX, data->rx_ring_unc);
		}
		if (ret != 0) {
			break;
		}

		i2s_loongson_reg_update(cfg, IIS_CONTROL, dir_en | link, dir_en | link);
		data->state[i2s_loongson_dir_idx(dir)] = I2S_STATE_RUNNING;
		break;

	case I2S_TRIGGER_STOP:
		if (data->state[i2s_loongson_dir_idx(dir)] != I2S_STATE_RUNNING) {
			ret = -EIO;
			break;
		}

		dma_stop(cfg->dma_dev, (dir == I2S_DIR_TX) ? cfg->dma_tx_ch : cfg->dma_rx_ch);
		i2s_loongson_reg_update(cfg, IIS_CONTROL, dir_en, 0U);
		data->state[i2s_loongson_dir_idx(dir)] = I2S_STATE_READY;
		break;

	case I2S_TRIGGER_DRAIN:
		if (data->state[i2s_loongson_dir_idx(dir)] != I2S_STATE_RUNNING) {
			ret = -EIO;
			break;
		}

		/*
		 * Up to two halves can be queued, so draining means letting the
		 * engine come round once more; the wait is bounded by the
		 * configured timeout and does not fail the drain if it expires.
		 */
		if (dir == I2S_DIR_TX) {
			int32_t timeout_ms =
				(data->cfg[0].timeout < 0) ? 1000 : data->cfg[0].timeout;

			(void)iis_dma_wait_half(cfg, data, 0, timeout_ms);
		}

		dma_stop(cfg->dma_dev, (dir == I2S_DIR_TX) ? cfg->dma_tx_ch : cfg->dma_rx_ch);
		i2s_loongson_reg_update(cfg, IIS_CONTROL, dir_en, 0U);
		data->state[i2s_loongson_dir_idx(dir)] = I2S_STATE_READY;
		break;

	case I2S_TRIGGER_DROP:
		dma_stop(cfg->dma_dev, cfg->dma_tx_ch);
		dma_stop(cfg->dma_dev, cfg->dma_rx_ch);
		i2s_loongson_reg_update(cfg, IIS_CONTROL, dir_en, 0U);

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
	 * The data input - the codec's data out - is an input to this
	 * controller, and the pin state above only chose its function. The pad
	 * direction is a separate register (GPIO_OEN, active low) that the boot
	 * loader leaves alone, so on this board every one of the five interface
	 * pins comes up as an output. Left that way the pad drives the codec's
	 * data line against it and the receive path reads nothing.
	 */
	if (cfg->data_in.port != NULL) {
		ret = gpio_pin_configure(cfg->data_in.port, cfg->data_in.pin, GPIO_INPUT);
		if (ret < 0) {
			return ret;
		}
	}

	/*
	 * Bring the clock tree up in one write, out of soft reset (see
	 * i2s_loongson_clock_start for why the literal vendor probe sequence
	 * must not be reproduced here). The direction enables still only go on
	 * when a stream is triggered, the way the vendor trigger does it.
	 */
	i2s_loongson_clock_start(cfg);

	/*
	 * The rings are ordinary memory handed to the engine through the DMA
	 * driver, which does the address translation (see iis_ring() for the
	 * cache part of the story).
	 */
	data->tx_ring_unc = iis_ring(data->tx_ring);
	data->rx_ring_unc = iis_ring(data->rx_ring);
	k_sem_init(&data->half_done[0], 0, IIS_RING_HALVES);
	k_sem_init(&data->half_done[1], 0, IIS_RING_HALVES);

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
		.base = DT_INST_REG_ADDR(n),	\
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(n)),	\
		.clock_subsys = (clock_control_subsys_t)DT_INST_PHA(n, clocks, clkid),	\
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(n),	\
		.slave_mode = DT_INST_PROP(n, slave_mode),	\
		.mclk_output = DT_INST_PROP(n, mclk_output),	\
		.bclk_from_reference = DT_INST_PROP_OR(n, loongson_bclk_from_reference, 0),	\
		.mclk_xfs = DT_INST_PROP_OR(n, loongson_mclk_xfs, 128),				\
		.data_in = {								\
			.port = DEVICE_DT_GET(DT_INST_PHANDLE(n, loongson_data_in_gpios)),	\
			.pin = DT_INST_PHA_BY_IDX(n, loongson_data_in_gpios, 0, pin),			\
			.dt_flags = DT_INST_PHA_BY_IDX(n, loongson_data_in_gpios, 0, flags),	\
		},									\
		.dma_dev = DEVICE_DT_GET(DT_INST_PHANDLE(n, loongson_dmas)),	\
		.dma_tx_ch = DT_INST_PHA(n, loongson_dmas, tx_channel),	\
		.dma_rx_ch = DT_INST_PHA(n, loongson_dmas, rx_channel),	\
	};										\
											\
	DEVICE_DT_INST_DEFINE(n, i2s_loongson_init, NULL, &i2s_loongson_data_##n,	\
			      &i2s_loongson_cfg_##n, POST_KERNEL,					\
			      CONFIG_I2S_INIT_PRIORITY, &i2s_loongson_api);

DT_INST_FOREACH_STATUS_OKAY(I2S_LOONGSON_INIT)
