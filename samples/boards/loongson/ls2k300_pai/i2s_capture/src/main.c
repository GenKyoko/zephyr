/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Capture-only test for the Loongson 2K0300 PAI board: the ES8388's ADC and
 * the I2S receive DMA path, for as long as it takes to poke at the microphone
 * while the console keeps score.
 *
 * No transmit data goes out - the controller still generates the clocks (the
 * codec needs them to sample), but the echo/tone stages of the board sample
 * are left out so that everything on the console is about the receive side.
 *
 * Every half second the sample prints one line:
 *
 *   peak     the largest absolute sample of the last 300 blocks - the level
 *            the ADC delivered (0 = a flat line, moving = sound arrives)
 *   cndtr    the receive channel's count register: in circular mode it walks
 *            down from 160 and wraps; a frozen value means the engine never
 *            saw a request, i.e. the FIFO never filled
 *   isr      the DMA status nibbles: channel 0 is bits 3..0, half transfer in
 *            bit 2 and complete in bit 1 (the driver clears them in its ISR,
 *            so a running channel usually reads 0 here)
 *   ctrl     the I2S control register: RX_EN (bit 13) and RX_DMA_EN (bit 11)
 *            have to be on for anything to move
 */

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#define SAMPLE_RATE  48000U
#define BLOCK_BYTES  320U     /* 80 stereo frames = 1.67 ms of audio */
#define BLOCK_COUNT  8U
#define PHASE_BLOCKS 4800U    /* 8 s per input selection */
#define PRINT_EVERY  300U     /* one console line per half second */
#define PHASES       4U       /* total 32 s */

#define IIS_CONTROL 0x08U

/* ES8388 registers this sample tunes directly (the codec driver leaves
 * them at their reset values): the microphone PGA gain, left channel in
 * bits[7:4] and right in bits[3:0] (4 bit each, 0..8, about 3 dB a step),
 * and the PGA input mux, left in bits[7:6] and right in bits[5:4]:
 * 0 = L/RINPUT1, 1 = INPUT2, 2 = INPUT3, 3 = the differential pair (its
 * line pick goes to ADCCONTROL3 bit 7).
 */
#define ES_ADCCONTROL1 0x09U
#define ES_ADCCONTROL2 0x0aU
#define PGA_MAX        0x77U

static int16_t peak_of(const uint8_t *buf, size_t bytes)
{
	const int16_t *samples = (const int16_t *)buf;
	int16_t peak = 0;

	for (size_t i = 0U; i < bytes / sizeof(int16_t); i++) {
		int16_t value = samples[i] < 0 ? -samples[i] : samples[i];

		if (value > peak) {
			peak = value;
		}
	}

	return peak;
}

static void dump_codec_adc(void)
{
	struct i2c_dt_spec bus = I2C_DT_SPEC_GET(DT_NODELABEL(es8388));
	/* The ADC side of the codec: power, input selects, gains, volumes -
	 * and 0x0f, the ADC digital mute, which the driver lifts in start();
	 * it read 0x24 (mute set) through every capture run before, and a
	 * muted ADC flattens the line no matter which PGA input is selected.
	 */
	static const uint8_t regs[] = { 0x00U, 0x02U, 0x03U, 0x04U, 0x05U,
					0x06U, 0x07U, 0x08U, 0x09U, 0x0aU,
					0x0bU, 0x0fU, 0x10U, 0x11U, 0x12U };

	for (size_t i = 0U; i < ARRAY_SIZE(regs); i++) {
		uint8_t v;

		if (i2c_reg_read_byte_dt(&bus, regs[i], &v) == 0) {
			printk("codec reg %02x = %02x\n", regs[i], v);
		}
	}
}

int main(void)
{
	const struct device *i2s = DEVICE_DT_GET(DT_NODELABEL(i2s_rxtx));
	const struct device *codec = DEVICE_DT_GET(DT_NODELABEL(es8388));
	mem_addr_t iis = DT_REG_ADDR(DT_NODELABEL(i2s_rxtx));
	mem_addr_t dma = DT_REG_ADDR(DT_PHANDLE(DT_NODELABEL(i2s_rxtx), loongson_dmas));
	struct i2s_config i2s_cfg = {
		.word_size = 16U,
		.channels = 2U,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = SAMPLE_RATE,
		.mem_slab = NULL,
		.block_size = BLOCK_BYTES,
		.timeout = 2000,
	};
	struct audio_codec_cfg codec_cfg = {
		.dai_type = AUDIO_DAI_TYPE_I2S,
		.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE,
		.dai_cfg.i2s = {
			.word_size = 16U,
			.channels = 2U,
			.format = I2S_FMT_DATA_FORMAT_I2S,
			.options = I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET,
			.frame_clk_freq = SAMPLE_RATE,
			.mem_slab = NULL,
			.block_size = BLOCK_BYTES,
			.timeout = 2000,
		},
	};
	/* The ADC input is what this test listens to: microphone PGA wide
	 * open as well (louder is larger on this codec, 0..0xc0, 0.5 dB a
	 * step).
	 */
	audio_property_value_t adc_volume = { .vol = 0xc0 };
	struct i2c_dt_spec bus = I2C_DT_SPEC_GET(DT_NODELABEL(es8388));
	/* The four PGA input selections, scanned with the gain wide open:
	 * whichever phase makes the peak move is where the board's input
	 * is wired.
	 */
	static const struct {
		const char *name;
		uint8_t inssel;
	} phases[PHASES] = {
		{ "L/R = INPUT1",       0x00U },
		{ "L/R = INPUT2",       0x50U },
		{ "L/R = INPUT3",       0xa0U },
		{ "L/R = differential", 0xf0U },
	};
	int ret;

	printk("I2S capture test: %u s, %u Hz\n", (unsigned)(PHASES * PHASE_BLOCKS / 600U),
	       SAMPLE_RATE);

	if (!device_is_ready(i2s) || !device_is_ready(codec)) {
		printk("device not ready\n");
		return 0;
	}

	K_MEM_SLAB_DEFINE_STATIC(slab, BLOCK_BYTES, BLOCK_COUNT, 4);

	i2s_cfg.mem_slab = &slab;
	codec_cfg.dai_cfg.i2s.mem_slab = &slab;

	if (((ret = audio_codec_configure(codec, &codec_cfg)) != 0) ||
	    ((ret = audio_codec_set_property(codec, AUDIO_PROPERTY_INPUT_VOLUME,
					     AUDIO_CHANNEL_ALL, adc_volume)) != 0) ||
	    ((ret = audio_codec_apply_properties(codec)) != 0) ||
	    ((ret = audio_codec_start(codec, AUDIO_DAI_DIR_TXRX)) != 0)) {
		printk("codec setup failed: %d\n", ret);
		return 0;
	}

	if (((ret = i2s_configure(i2s, I2S_DIR_RX, &i2s_cfg)) != 0) ||
	    ((ret = i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_START)) != 0)) {
		printk("i2s rx setup failed: %d\n", ret);
		return 0;
	}

	printk("capture running - make some noise at the microphone\n");
	dump_codec_adc();

	for (uint32_t p = 0U; p < PHASES; p++) {
		int16_t phase_max = 0;

		/* Microphone gain wide open, then the input the PGA listens to */
		(void)i2c_reg_write_byte_dt(&bus, ES_ADCCONTROL1, PGA_MAX);
		(void)i2c_reg_write_byte_dt(&bus, ES_ADCCONTROL2, phases[p].inssel);
		printk("== %s, PGA gain max ==\n", phases[p].name);

		for (uint32_t i = 0U; i < PHASE_BLOCKS; i++) {
			void *rx_block;
			size_t rx_size;
			int16_t peak;

			ret = i2s_read(i2s, &rx_block, &rx_size);
			if (ret != 0) {
				printk("i2s read failed at block %u: %d\n", i, ret);
				goto done;
			}

			peak = peak_of(rx_block, rx_size);
			k_mem_slab_free(&slab, rx_block);

			if (peak > phase_max) {
				phase_max = peak;
			}

			if ((i % PRINT_EVERY) == (PRINT_EVERY - 1U)) {
				printk("t=%2lus peak %6d | cndtr %5u isr %x ctrl %08x\n",
				       (unsigned long)(((p * PHASE_BLOCKS) + i + 1U) / 600U), peak,
				       sys_read32(dma + 0x0cU), sys_read32(dma + 0x00U),
				       sys_read32(iis + IIS_CONTROL));
			}
		}

		printk("== %s: max peak %d ==\n", phases[p].name, phase_max);
	}

done:
	(void)i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_STOP);
	printk("capture done\n");
	return 0;
}
