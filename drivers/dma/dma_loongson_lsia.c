/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson LSIA DMA controller ("dma@1612c000" in the vendor device tree,
 * user manual chapter 12; the vendor kernel drives the same block through
 * drivers/dma/lsia_dma.c, this port through this file).
 *
 * A small STM32-F1 shaped engine: eight channels, each with four registers
 * 0x14 apart, and a four bit status nibble per channel in the two shared
 * status registers:
 *
 *   0x0000 ISR    status, one nibble per channel, write 1 to IFCR to clear
 *   0x0004 IFCR   interrupt flag clear
 *   0x0008 CCR    config, at 0x08 + 0x14 * channel
 *   0x000c CNDTR  transfer count, in beats
 *   0x0010 CPAR   peripheral address
 *   0x0014 CMAR   memory address
 *
 *   CCR: M2M[14] PL[13:12] MSIZE[11:10] PSIZE[9:8] MINC[7] PINC[6]
 *        CIRC[5] DIR[4] TEIE[3] HTIE[2] TCIE[1] EN[0]
 *   ISR nibble: TEI[3] HTI[2] TCI[1]
 *
 * What there is no such thing as here:
 *
 *   - a request select. Which peripheral drives which channel is fixed in
 *     silicon (the I2S transmit request on channel 1, its receive on
 *     channel 0), so a consumer names its channel and nothing else; the
 *     API's "slot" carries no information and is ignored.
 *   - bursts, FIFO thresholds, flow control. One beat is one request: a
 *     beat reads MSIZE bytes and writes PSIZE bytes, and CNDTR counts
 *     beats. The burst fields of the API are therefore ignored as well.
 *   - a multi-block engine. One config programs one block; the API's
 *     block_count has to be 1 (a scatter/gather list would need the
 *     interrupt-driven CMAR reloading the vendor driver performs, which is
 *     not reproduced here).
 *   - a current address register. dma_get_status() can say how many beats
 *     are left (CNDTR counts down) but not where the engine is pointing.
 *
 * Directions: DIR selects which side the peripheral is on (0 = source, the
 * peripheral to memory of channel 0; 1 = destination), M2M turns both sides
 * into memory. The API's address adjustment is honoured per side, so a
 * peripheral FIFO has to be described with DMA_ADDR_ADJ_NO_CHANGE on its
 * side - the zero-initialized default (increment, increment) is only right
 * for memory to memory.
 *
 * The count register takes beats, so a block size has to be a multiple of
 * the beat width, and no more than 65535 beats fit. The beat that paces the
 * transfer is the peripheral side's width (the memory side adjusts to it),
 * except in memory to memory, where the source width does.
 *
 * Caching: the DMA master works in physical addresses. The low 32 bits of
 * any of the port's virtual addresses are the physical one, whether they
 * come from the I/O window (the port's nodes put 0x8000_0000 on the front)
 * or from the DDR map, so the translation is dropping the window bit. That
 * the CPU can hand the engine ordinary memory without cache maintenance is
 * a property of this port - the SoC start-up leaves the data cache off (see
 * the cache note in the SoC file). On a caching CPU a flush before the
 * transfer and an invalidate after it would belong in config()/get_status().
 */

#define DT_DRV_COMPAT loongson_lsia_dma

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define LSIA_ISR       0x0000U
#define LSIA_IFCR      0x0004U

#define LSIA_CCR(ch)   (0x0008U + 0x14U * (ch))
#define LSIA_CNDTR(ch) (0x000cU + 0x14U * (ch))
#define LSIA_CPAR(ch)  (0x0010U + 0x14U * (ch))
#define LSIA_CMAR(ch)  (0x0014U + 0x14U * (ch))

#define LSIA_CCR_M2M     BIT(14)
#define LSIA_CCR_PL(n)   ((uint32_t)(n) << 12)
#define LSIA_CCR_MSIZE(n) ((uint32_t)(n) << 10)
#define LSIA_CCR_PSIZE(n) ((uint32_t)(n) << 8)
#define LSIA_CCR_MINC    BIT(7)
#define LSIA_CCR_PINC    BIT(6)
#define LSIA_CCR_CIRC    BIT(5)
#define LSIA_CCR_DIR     BIT(4)
#define LSIA_CCR_TEIE    BIT(3)
#define LSIA_CCR_HTIE    BIT(2)
#define LSIA_CCR_TCIE    BIT(1)
#define LSIA_CCR_EN      BIT(0)

/* The interrupt enables sit in the same bit positions as the status they arm */
#define LSIA_CCR_IRQ (LSIA_CCR_TEIE | LSIA_CCR_HTIE | LSIA_CCR_TCIE)

/* The status nibble: transfer complete, half transfer, transfer error */
#define LSIA_ISR_TCI  BIT(1)
#define LSIA_ISR_HTI  BIT(2)
#define LSIA_ISR_TEI  BIT(3)
#define LSIA_ISR_MASK (LSIA_ISR_TCI | LSIA_ISR_HTI | LSIA_ISR_TEI)

/* The silicon: eight channels, and the count register is 16 beats wide.
 * Plain literals: the channel count goes into LISTIFY, which needs it free
 * of a type suffix.
 */
#define LSIA_MAX_CHANNELS 8
#define LSIA_MAX_BEATS    0xffffU

struct lsia_dma_channel {
	/* Snapshot of the running configuration, for the ISR and status */
	dma_callback_t cb;
	void *user_data;
	uint8_t direction; /* enum dma_channel_direction of the last config */
	uint8_t unit;      /* bytes per beat: the pending_length factor */
	bool cyclic;
};

struct lsia_dma_data {
	/* Has to stay the first member, the core reads it through dev->data */
	struct dma_context ctx;
	struct lsia_dma_channel chan[LSIA_MAX_CHANNELS];
};

struct lsia_dma_config {
	mem_addr_t base;
	uint32_t channels;
	void (*irq_config)(const struct device *dev);
};

static inline uint32_t lsia_dma_read(const struct lsia_dma_config *cfg, uint32_t reg)
{
	return sys_read32(cfg->base + reg);
}

static inline void lsia_dma_write(const struct lsia_dma_config *cfg, uint32_t reg, uint32_t value)
{
	sys_write32(value, cfg->base + reg);
}

/*
 * Virtual to physical for what the engine is handed: peripheral ports (the
 * port's devicetree addresses carry the 0x8000_0000 I/O window bit) and DDR
 * (the port's DDR map keeps the physical address in the low 32 bits). Both
 * reduce to dropping the window bit, see the note in the header.
 */
static uint32_t lsia_dma_phys(uintptr_t addr)
{
	return (uint32_t)(addr & ~BIT64(63));
}

/* Transfer width in bytes to the two bit MSIZE/PSIZE encoding */
static int lsia_dma_width_enc(uint32_t bytes, uint32_t *enc)
{
	switch (bytes) {
	case 1U:
		*enc = 0U;
		return 0;
	case 2U:
		*enc = 1U;
		return 0;
	case 4U:
		*enc = 2U;
		return 0;
	default:
		return -EINVAL;
	}
}

static void lsia_dma_irq_clear(const struct lsia_dma_config *cfg, uint32_t ch, uint32_t flags)
{
	lsia_dma_write(cfg, LSIA_IFCR, (flags & LSIA_ISR_MASK) << (4U * ch));
}

static int lsia_dma_config(const struct device *dev, uint32_t ch, struct dma_config *conf)
{
	const struct lsia_dma_config *cfg = dev->config;
	struct lsia_dma_data *data = dev->data;
	struct lsia_dma_channel *chan = &data->chan[ch];
	const struct dma_block_config *blk;
	uint32_t ccr = 0U, src_enc, dst_enc, unit, beats;
	uint32_t periph_addr, mem_addr;
	unsigned int key;
	int ret;

	if (ch >= cfg->channels) {
		return -EINVAL;
	}
	if (conf->block_count != 1U) {
		return -ENOTSUP;
	}
	blk = conf->head_block;
	if (blk == NULL) {
		return -EINVAL;
	}
	/* One block, forward walking, no gather/scatter and no reload tricks */
	if (blk->source_gather_en || blk->dest_scatter_en || blk->source_reload_en ||
	    blk->dest_reload_en || blk->source_addr_adj == DMA_ADDR_ADJ_DECREMENT ||
	    blk->dest_addr_adj == DMA_ADDR_ADJ_DECREMENT || conf->channel_priority > 3U) {
		return -ENOTSUP;
	}

	ret = lsia_dma_width_enc(conf->source_data_size, &src_enc);
	if (ret != 0) {
		return ret;
	}
	ret = lsia_dma_width_enc(conf->dest_data_size, &dst_enc);
	if (ret != 0) {
		return ret;
	}

	ccr |= LSIA_CCR_PL(conf->channel_priority);

	switch (conf->channel_direction) {
	case MEMORY_TO_PERIPHERAL:
		ccr |= LSIA_CCR_DIR | LSIA_CCR_MSIZE(src_enc) | LSIA_CCR_PSIZE(dst_enc);
		ccr |= (blk->source_addr_adj == DMA_ADDR_ADJ_INCREMENT) ? LSIA_CCR_MINC : 0U;
		ccr |= (blk->dest_addr_adj == DMA_ADDR_ADJ_INCREMENT) ? LSIA_CCR_PINC : 0U;
		mem_addr = blk->source_address;
		periph_addr = blk->dest_address;
		unit = conf->dest_data_size;
		break;
	case PERIPHERAL_TO_MEMORY:
		ccr |= LSIA_CCR_MSIZE(dst_enc) | LSIA_CCR_PSIZE(src_enc);
		ccr |= (blk->dest_addr_adj == DMA_ADDR_ADJ_INCREMENT) ? LSIA_CCR_MINC : 0U;
		ccr |= (blk->source_addr_adj == DMA_ADDR_ADJ_INCREMENT) ? LSIA_CCR_PINC : 0U;
		periph_addr = blk->source_address;
		mem_addr = blk->dest_address;
		unit = conf->source_data_size;
		break;
	case MEMORY_TO_MEMORY:
		if (conf->cyclic) {
			return -ENOTSUP;
		}
		ccr |= LSIA_CCR_M2M | LSIA_CCR_MSIZE(src_enc) | LSIA_CCR_PSIZE(dst_enc);
		ccr |= (blk->source_addr_adj == DMA_ADDR_ADJ_INCREMENT) ? LSIA_CCR_MINC : 0U;
		ccr |= (blk->dest_addr_adj == DMA_ADDR_ADJ_INCREMENT) ? LSIA_CCR_PINC : 0U;
		periph_addr = blk->source_address;
		mem_addr = blk->dest_address;
		unit = conf->source_data_size;
		break;
	default:
		/* The fixed request wiring decides what a channel can do, and
		 * peripheral to peripheral describes none of it.
		 */
		return -ENOTSUP;
	}

	if ((blk->block_size % unit) != 0U) {
		return -EINVAL;
	}
	beats = blk->block_size / unit;
	if ((beats == 0U) || (beats > LSIA_MAX_BEATS)) {
		return -EINVAL;
	}

	/* Interrupts only exist to run the callback */
	if (conf->dma_callback != NULL) {
		ccr |= LSIA_CCR_TCIE;
		if (conf->half_complete_callback_en) {
			ccr |= LSIA_CCR_HTIE;
		}
		if (!conf->error_callback_dis) {
			ccr |= LSIA_CCR_TEIE;
		}
	}
	if (conf->cyclic) {
		ccr |= LSIA_CCR_CIRC;
	}

	key = irq_lock();
	if ((lsia_dma_read(cfg, LSIA_CCR(ch)) & LSIA_CCR_EN) != 0U) {
		irq_unlock(key);
		return -EBUSY;
	}

	chan->cb = conf->dma_callback;
	chan->user_data = conf->user_data;
	chan->direction = (uint8_t)conf->channel_direction;
	chan->unit = (uint8_t)unit;
	chan->cyclic = conf->cyclic;

	/* Left over flags of an earlier run would be taken for progress */
	lsia_dma_irq_clear(cfg, ch, LSIA_ISR_MASK);
	lsia_dma_write(cfg, LSIA_CCR(ch), ccr);
	lsia_dma_write(cfg, LSIA_CNDTR(ch), beats);
	lsia_dma_write(cfg, LSIA_CPAR(ch), lsia_dma_phys(periph_addr));
	lsia_dma_write(cfg, LSIA_CMAR(ch), lsia_dma_phys(mem_addr));
	irq_unlock(key);

	return 0;
}

static int lsia_dma_start(const struct device *dev, uint32_t ch)
{
	const struct lsia_dma_config *cfg = dev->config;
	unsigned int key;

	if (ch >= cfg->channels) {
		return -EINVAL;
	}

	key = irq_lock();
	lsia_dma_write(cfg, LSIA_CCR(ch), lsia_dma_read(cfg, LSIA_CCR(ch)) | LSIA_CCR_EN);
	irq_unlock(key);

	return 0;
}

static int lsia_dma_stop(const struct device *dev, uint32_t ch)
{
	const struct lsia_dma_config *cfg = dev->config;
	unsigned int key;

	if (ch >= cfg->channels) {
		return -EINVAL;
	}

	key = irq_lock();
	/* The interrupt enables too: a disabled channel has nothing to say */
	lsia_dma_write(cfg, LSIA_CCR(ch),
		       lsia_dma_read(cfg, LSIA_CCR(ch)) & ~(LSIA_CCR_IRQ | LSIA_CCR_EN));
	irq_unlock(key);
	lsia_dma_irq_clear(cfg, ch, LSIA_ISR_MASK);

	return 0;
}

/*
 * Pause without losing the configuration: the count register holds the beats
 * that were left when the engine was stopped, and setting EN runs on from
 * there. (That resumability is a property of the STM32 shaped engine family;
 * on this silicon it is untested - the only user this port has, the audio
 * path, has no use for a pause.)
 */
static int lsia_dma_suspend(const struct device *dev, uint32_t ch)
{
	const struct lsia_dma_config *cfg = dev->config;
	uint32_t ccr;
	unsigned int key;
	int ret = 0;

	if (ch >= cfg->channels) {
		return -EINVAL;
	}

	key = irq_lock();
	ccr = lsia_dma_read(cfg, LSIA_CCR(ch));
	if ((ccr & LSIA_CCR_EN) == 0U) {
		ret = -EINVAL;
	} else {
		lsia_dma_write(cfg, LSIA_CCR(ch), ccr & ~LSIA_CCR_EN);
	}
	irq_unlock(key);

	return ret;
}

static int lsia_dma_resume(const struct device *dev, uint32_t ch)
{
	const struct lsia_dma_config *cfg = dev->config;
	unsigned int key;
	int ret = 0;

	if (ch >= cfg->channels) {
		return -EINVAL;
	}

	key = irq_lock();
	if ((lsia_dma_read(cfg, LSIA_CCR(ch)) & LSIA_CCR_EN) != 0U) {
		ret = -EINVAL;
	} else {
		lsia_dma_write(cfg, LSIA_CCR(ch), lsia_dma_read(cfg, LSIA_CCR(ch)) | LSIA_CCR_EN);
	}
	irq_unlock(key);

	return ret;
}

/*
 * Refresh a channel's addresses and count while it is stopped - the count
 * register only takes writes with EN clear, so a running channel refuses.
 * The direction decides which argument is the peripheral: like config(),
 * src is the peripheral side of a peripheral to memory transfer and dst the
 * peripheral side of the other direction.
 */
#ifdef CONFIG_DMA_64BIT
static int lsia_dma_reload(const struct device *dev, uint32_t ch, uint64_t src, uint64_t dst,
			   size_t size)
#else
static int lsia_dma_reload(const struct device *dev, uint32_t ch, uint32_t src, uint32_t dst,
			   size_t size)
#endif
{
	const struct lsia_dma_config *cfg = dev->config;
	struct lsia_dma_data *data = dev->data;
	uint32_t periph_addr, mem_addr, ccr, unit;
	unsigned int key;

	if (ch >= cfg->channels) {
		return -EINVAL;
	}

	unit = data->chan[ch].unit;
	if ((unit == 0U) || ((size % unit) != 0U)) {
		return -EINVAL;
	}

	key = irq_lock();
	ccr = lsia_dma_read(cfg, LSIA_CCR(ch));
	if ((ccr & LSIA_CCR_EN) != 0U) {
		irq_unlock(key);
		return -EBUSY;
	}

	if ((ccr & LSIA_CCR_DIR) != 0U) {
		mem_addr = (uint32_t)src;
		periph_addr = (uint32_t)dst;
	} else {
		periph_addr = (uint32_t)src;
		mem_addr = (uint32_t)dst;
	}

	lsia_dma_write(cfg, LSIA_CNDTR(ch), (uint32_t)(size / unit));
	lsia_dma_write(cfg, LSIA_CPAR(ch), lsia_dma_phys(periph_addr));
	lsia_dma_write(cfg, LSIA_CMAR(ch), lsia_dma_phys(mem_addr));
	irq_unlock(key);

	return 0;
}

static int lsia_dma_get_status(const struct device *dev, uint32_t ch, struct dma_status *stat)
{
	const struct lsia_dma_config *cfg = dev->config;
	struct lsia_dma_data *data = dev->data;
	uint32_t ccr, flags, beats;

	if (ch >= cfg->channels) {
		return -EINVAL;
	}

	ccr = lsia_dma_read(cfg, LSIA_CCR(ch));
	flags = (lsia_dma_read(cfg, LSIA_ISR) >> (4U * ch)) & LSIA_ISR_MASK;

	/* A one shot transfer is over when the complete flag stands, whether
	 * or not the engine has dropped EN itself yet.
	 */
	stat->busy = (((ccr & LSIA_CCR_EN) != 0U) && ((flags & LSIA_ISR_TCI) == 0U));

	beats = lsia_dma_read(cfg, LSIA_CNDTR(ch));
	stat->pending_length = beats * data->chan[ch].unit;
	stat->dir = (enum dma_channel_direction)data->chan[ch].direction;
	/* The engine exposes no current address register */
	stat->write_position = 0U;
	stat->read_position = 0U;

	return 0;
}

static int lsia_dma_get_attribute(const struct device *dev, uint32_t type, uint32_t *value)
{
	switch (type) {
	case DMA_ATTR_BUFFER_ADDRESS_ALIGNMENT:
		/* Aligned to the beat, at most four bytes wide */
		*value = 4U;
		return 0;
	case DMA_ATTR_BUFFER_SIZE_ALIGNMENT:
	case DMA_ATTR_COPY_ALIGNMENT:
		*value = 1U;
		return 0;
	case DMA_ATTR_MAX_BLOCK_COUNT:
		*value = 1U;
		return 0;
	default:
		return -EINVAL;
	}
}

static void lsia_dma_isr(const struct device *dev, uint32_t ch)
{
	const struct lsia_dma_config *cfg = dev->config;
	struct lsia_dma_data *data = dev->data;
	struct lsia_dma_channel *chan = &data->chan[ch];
	uint32_t flags, ccr;
	dma_callback_t cb;
	void *user_data;
	int status;

	ccr = lsia_dma_read(cfg, LSIA_CCR(ch));
	flags = (lsia_dma_read(cfg, LSIA_ISR) >> (4U * ch)) & LSIA_ISR_MASK;
	/* Only what was armed is news; the rest is somebody else's flag */
	flags &= ccr;
	if (flags == 0U) {
		return;
	}
	lsia_dma_irq_clear(cfg, ch, flags);

	cb = chan->cb;
	user_data = chan->user_data;

	if ((flags & LSIA_ISR_TCI) != 0U) {
		/* The end of the buffer in both readings of the API: the last
		 * beat of a one shot transfer, or the wrap of a circular one.
		 */
		status = DMA_STATUS_COMPLETE;
	} else if ((flags & LSIA_ISR_HTI) != 0U) {
		/* The water mark: the engine went past the middle of the ring */
		status = DMA_STATUS_BLOCK;
	} else {
		status = -EIO;
		/* An errored engine has nothing left to say either */
		lsia_dma_write(cfg, LSIA_CCR(ch), ccr & ~(LSIA_CCR_IRQ | LSIA_CCR_EN));
		printk("dma lsia: channel %u transfer error\n", ch);
	}

	if (cb != NULL) {
		cb(dev, user_data, ch, status);
	}
}

static DEVICE_API(dma, lsia_dma_driver_api) = {
	.config = lsia_dma_config,
	.start = lsia_dma_start,
	.stop = lsia_dma_stop,
	.suspend = lsia_dma_suspend,
	.resume = lsia_dma_resume,
	.reload = lsia_dma_reload,
	.get_status = lsia_dma_get_status,
	.get_attribute = lsia_dma_get_attribute,
};

static int lsia_dma_init(const struct device *dev)
{
	const struct lsia_dma_config *cfg = dev->config;

	cfg->irq_config(dev);

	/* Nothing of ours should survive the boot loader: a channel left
	 * enabled there would be racing for the memory of whoever configures
	 * it next.
	 */
	for (uint32_t ch = 0U; ch < cfg->channels; ch++) {
		lsia_dma_write(cfg, LSIA_CCR(ch),
			       lsia_dma_read(cfg, LSIA_CCR(ch)) & ~(LSIA_CCR_IRQ | LSIA_CCR_EN));
		lsia_dma_irq_clear(cfg, ch, LSIA_ISR_MASK);
	}

	return 0;
}

/* One ISR per channel line, all funneling into the shared handler */
#define LSIA_DMA_ISR(ch, _)                                                                       \
	static void lsia_dma_isr_##ch(const struct device *dev)                                   \
	{                                                                                         \
		lsia_dma_isr(dev, ch);                                                            \
	}

LISTIFY(LSIA_MAX_CHANNELS, LSIA_DMA_ISR, ())

/* LISTIFY hands its loop index first, so the channel comes before the
 * instance the same way the generated ISR wrappers are named.
 */
#define LSIA_DMA_IRQ_CONNECT(ch, n)                                                               \
	IRQ_CONNECT(DT_INST_IRQ_BY_IDX(n, ch, irq), DT_INST_IRQ_BY_IDX(n, ch, priority),          \
		    lsia_dma_isr_##ch, DEVICE_DT_INST_GET(n), 0);                                 \
	irq_enable(DT_INST_IRQ_BY_IDX(n, ch, irq));

#define LSIA_DMA_INIT(n)                                                                          \
	BUILD_ASSERT(DT_INST_PROP(n, dma_channels) <= LSIA_MAX_CHANNELS);                         \
	BUILD_ASSERT(DT_NUM_IRQS(DT_DRV_INST(n)) == DT_INST_PROP(n, dma_channels));               \
	static void lsia_dma##n##_irq_config(const struct device *dev)                            \
	{                                                                                         \
		LISTIFY(DT_NUM_IRQS(DT_DRV_INST(n)), LSIA_DMA_IRQ_CONNECT, (), n);                \
	}                                                                                         \
	ATOMIC_DEFINE(lsia_dma_atomic_##n, DT_INST_PROP(n, dma_channels));                        \
	static struct lsia_dma_data lsia_dma_data_##n = {                                         \
		.ctx =                                                                            \
			{                                                                         \
				.magic = DMA_MAGIC,                                               \
				.dma_channels = DT_INST_PROP(n, dma_channels),                    \
				.atomic = lsia_dma_atomic_##n,                                    \
			},                                                                        \
	};                                                                                        \
	static const struct lsia_dma_config lsia_dma_config_##n = {                               \
		.base = DT_INST_REG_ADDR(n),                                                      \
		.channels = DT_INST_PROP(n, dma_channels),                                        \
		.irq_config = lsia_dma##n##_irq_config,                                           \
	};                                                                                        \
	DEVICE_DT_INST_DEFINE(n, lsia_dma_init, NULL, &lsia_dma_data_##n, &lsia_dma_config_##n,   \
			      PRE_KERNEL_1, CONFIG_DMA_LOONGSON_LSIA_INIT_PRIORITY,                \
			      &lsia_dma_driver_api);

DT_INST_FOREACH_STATUS_OKAY(LSIA_DMA_INIT)
