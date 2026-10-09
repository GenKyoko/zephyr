/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2S + ES8388 test for the Loongson 2K0300 PAI board.
 *
 * This is the board's audio chain end to end, and the reason it is a board
 * sample rather than a use of the upstream audio samples:
 *
 *   - samples/drivers/audio/codec drives PCM through the *codec* API
 *     (AUDIO_DAI_TYPE_PCM plus audio_codec_write()/done callbacks), which is how
 *     an on-chip codec with its own data path (the sf32lb it is written for)
 *     works. The ES8388 is an I2S slave codec: it has a control path on I2C and
 *     its data path is the SoC's I2S controller, so the codec API only
 *     configures it and the PCM goes through the i2s API.
 *   - samples/drivers/i2s/echo is the upstream sample for that arrangement, but
 *     it passes a uint32_t to i2s_read()'s size_t * argument - harmless on 32 bit
 *     targets, a compile error here - and it configures the codec as the frame
 *     clock controller, while this board wires the SoC as the master and the
 *     codec as the target.
 *
 * What it does:
 *
 *   With -DSAMPLE_CLOCK_ONLY=1 it sends no audio at all: the controller
 *   keeps generating BCLK and LRCLK while the capture phase reports what the
 *   codec's ADC puts on the data line, which tells whether the codec is clocked
 *   at all.

 *      line out) and the controller (same format, controller of both clocks),
 *      and prints the clock the dividers count against,
 *   2. plays a 1 kHz tone for a few seconds, one block at a time, printing the
 *      progress to the console,
 *   3. sweeps the three codec formats (I2S, left and right justified) over the
 *      same tone, because this controller's wire format is fixed in silicon and
 *      the matching codec setting has to be found by ear,
 *   4. plays sample audio - a short melody generated before the build, or a
 *      recording the build was pointed at (-DSAMPLE_AUDIO_INPUT=..., see
 *      tools/make_sample_audio.py) - interpolated from its 16 kHz mono format
 *      to the link's 48 kHz stereo,
 *   5. switches the codec to playback + capture and echoes the input for a few
 *      seconds, printing the peak level of each captured block - which is what
 *      tells whether the ADC and the receive path work.
 *
 * Two build switches help the board bring-up (both off by default):
 * -DSAMPLE_LIVE=1 keeps replaying the melody so that the codec can be poked over
 * I2C from the shell while audio flows, and -DSAMPLE_CODEC_MASTER=1 lets the
 * codec generate BCLK and LRCLK instead of the controller (its own 12.288 MHz
 * oscillator clocks it, and the device tree must then carry "slave-mode").
 *
 * The tone is generated from a table, so the sample itself needs no floating
 * point and no math library. The sample audio is generated before the build by
 * tools/make_sample_audio.py - standard library only, plus ffmpeg when a
 * recording is used as input - and its generated files live in the build
 * directory, not in the tree.
 */

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#define SAMPLE_RATE  48000U
#define TONE_HZ      1000U
#define AMPLITUDE    8000
#define BLOCK_BYTES  320U /* 80 stereo frames = 1.67 ms of audio */
#define BLOCK_COUNT  4U
#define PLAY_BLOCKS  900U   /* 1.5 s of tone */
#define SWEEP_BLOCKS 1500U  /* 2.5 s of tone per format */
#define ECHO_BLOCKS  450U   /* 0.75 s of the echo */

/*
 * One period of a sine in 64 steps, as 16 bit samples. The phase below is
 * accumulated in 16.16 fixed point, so any tone frequency fits.
 */
static const int16_t sine[64] = {
	0,     804,   1608,  2410,  3212,  4011,  4808,  5602,  6393,  7179,  7962,  8739,
	9512,  10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
	18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811,
	25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
	30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
	32609, 32678, 32728, 32757
};

/* Which block of the tone comes next; kept across calls */
static uint32_t phase;

static void fill_tone(uint8_t *buf, size_t bytes)
{
	int16_t *samples = (int16_t *)buf;
	uint32_t step = (uint32_t)(((uint64_t)64U * TONE_HZ << 16) / SAMPLE_RATE);

	for (size_t i = 0U; i < bytes / sizeof(int16_t); i++) {
		samples[i] = (int16_t)((sine[(phase >> 16) & 63U] * AMPLITUDE) / 32767);
		phase += step;
	}
}

#include "sample_audio.h"

/*
 * The sample audio: a short melody, or whatever recording the build was pointed
 * at (-DSAMPLE_AUDIO_INPUT=..., see tools/make_sample_audio.py). It is mono at
 * 16 kHz and 16 bit, generated into the build directory, and this is where it
 * meets the link's 48 kHz stereo format.
 */
#define CLIP_RATE 16000U
#define CLIP_STEP (SAMPLE_RATE / CLIP_RATE) /* output frames per input sample */

/* How far into the clip the next output frame is; kept across calls */
static uint32_t clip_frame;

/*
 * Feed the clip to the transmit path: each output frame interpolates between two
 * neighbouring 16 kHz samples (CLIP_STEP frames per input sample), and both
 * channels get the same value. Linear interpolation rather than repeating each
 * sample keeps the melody clean enough to judge by ear.
 */
static __maybe_unused void fill_sample_audio(uint8_t *buf, size_t bytes)
{
	int16_t *samples = (int16_t *)buf;
	uint32_t frames = (uint32_t)sample_audio_16k_samples * CLIP_STEP;

	for (size_t i = 0U; i < bytes / sizeof(int16_t); i += 2U) {
		uint32_t index = clip_frame / CLIP_STEP;
		uint32_t frac = clip_frame % CLIP_STEP;
		int32_t first = sample_audio_16k[index];
		int32_t second = sample_audio_16k[(index + 1U) % sample_audio_16k_samples];
		int16_t value = (int16_t)(first + ((second - first) * (int32_t)frac) /
						  (int32_t)CLIP_STEP);

		samples[i] = value;
		samples[i + 1U] = value;

		if (++clip_frame >= frames) {
			clip_frame = 0U;
		}
	}
}

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

/*
 * What the two drivers programmed, as seen from the outside: the I2S block's
 * three configuration registers and the codec registers that make up the
 * playback path. When the board stays silent this dump says which side to look
 * at - the divider values (config0/config1) show whether the reference clock
 * assumption holds, and the codec registers show whether the DAC, the mixer
 * switches and the output amplifiers are actually on.
 */
/*
 * Are the interface pads actually moving?
 *
 * The GPIO block's input register reads whatever the pads see, whatever the
 * pins are muxed to, so output pins can be watched from software. That is the
 * one piece of the transmit path a register dump cannot show, and it is what
 * separates "the controller drives its pins" from "everything above the pads
 * is right but nothing moves".
 *
 * It has to run while real audio is on the wire: a frame of digital silence is
 * a data pin held low for the whole frame, so a probe taken over silence reads
 * "steady" even on a perfectly working transmit path - which is exactly how an
 * earlier run of this probe (taken right after START, over the prefilled
 * silence) produced a misleading "steady" verdict.
 *
 * The GPIO block has no driver instance here, so its register is addressed
 * directly: the input register sits at offset 0x20, mirrored at 0xa00, one bit
 * per pin - so pins 64..95 are the word at offset 0x08, and within it BCLK
 * (GPIO77) is bit 13, LRCLK (GPIO78) bit 14 and the data out pin (GPIO80)
 * bit 16.
 */
#define GPIO_BASE_ADDR 0x8000000016104000ULL /* GPIO block, non-cached window */
#define GPIO_INPUT_MIRROR 0xa00UL
#define BCLK_PIN_BIT BIT(13) /* GPIO77 within pins 64..95 */
#define LRCLK_PIN_BIT BIT(14) /* GPIO78 */
#define DATA_PIN_BIT BIT(16) /* GPIO80 */

static __maybe_unused void probe_data_pin(void)
{
	mem_addr_t word_addr = (mem_addr_t)(GPIO_BASE_ADDR + GPIO_INPUT_MIRROR + 0x08UL);
	uint32_t first = sys_read32(word_addr);
	uint32_t bclk_moves = 0U, lrclk_moves = 0U, data_moves = 0U;
	uint32_t bclk_levels = first & BCLK_PIN_BIT;
	uint32_t lrclk_levels = first & LRCLK_PIN_BIT;
	uint32_t data_levels = first & DATA_PIN_BIT;

	for (uint32_t i = 1U; i < 64U; i++) {
		uint32_t value = sys_read32(word_addr);

		bclk_moves += (((value ^ first) & BCLK_PIN_BIT) != 0U) ? 1U : 0U;
		lrclk_moves += (((value ^ first) & LRCLK_PIN_BIT) != 0U) ? 1U : 0U;
		data_moves += (((value ^ first) & DATA_PIN_BIT) != 0U) ? 1U : 0U;
		first = value;
		bclk_levels |= value & BCLK_PIN_BIT;
		lrclk_levels |= value & LRCLK_PIN_BIT;
		data_levels |= value & DATA_PIN_BIT;
		k_busy_wait(100);
	}

#define LEVELS(l, bit)                                                                              \
	((l) == 0U) ? "low only" : (((l) & (bit)) != 0U && ((l) & ~(bit)) == 0U) ? "high only"      \
										 : "both levels"
	printk("pads while playing - BCLK: %2u moves (%s), LRCLK: %2u moves (%s), "
	       "data: %2u moves (%s)\n",
	       bclk_moves, LEVELS(bclk_levels, BCLK_PIN_BIT), lrclk_moves,
	       LEVELS(lrclk_levels, LRCLK_PIN_BIT), data_moves,
	       LEVELS(data_levels, DATA_PIN_BIT));
#undef LEVELS
}

#if defined(SAMPLE_CLOCK_ONLY)
/*
 * Can the pad drive at all?
 *
 * The data pin reads steady low while the controller is streaming, so this
 * asks the pad directly: drive it as a plain GPIO and read the input register
 * back. If the level follows, the pad and its direction control are fine and
 * it is the I2S function that does not drive the pin; if it does not follow,
 * the pad cannot be driven at all - and knowing that points at the direction
 * control rather than at the audio path.
 *
 * Nothing here disturbs the test: this mode transmits nothing, so the pin is
 * free for the experiment.
 */
static void probe_gpio_drive(void)
{
	const struct device *gpio = DEVICE_DT_GET(DT_NODELABEL(gpio2));
	mem_addr_t input_reg = (mem_addr_t)(GPIO_BASE_ADDR + GPIO_INPUT_MIRROR + 0x08UL);
	mem_addr_t mux_reg = 0x80000000160004a4ULL; /* pin mux word, G80 = bits[1:0] */
	const uint8_t pin = 16U; /* GPIO80 within the 64..95 bank */
	uint32_t mux = sys_read32(mux_reg);
	uint32_t driven_low;
	uint32_t driven_high;

	/*
	 * The pad is muxed to the I2S function here, so first hand it to the
	 * GPIO block (mux field 0), then drive it both ways and read the input
	 * register back: this calibrates the observation channel the pad probe
	 * below relies on. If a GPIO-driven level does not show up in the input
	 * register, no reading taken through it - including "the pads never
	 * move" - says anything about the pads.
	 */
	sys_write32(mux & ~0x3U, mux_reg);

	(void)gpio_pin_configure(gpio, pin, GPIO_OUTPUT_INACTIVE);
	k_busy_wait(100);
	driven_low = sys_read32(input_reg);

	(void)gpio_pin_configure(gpio, pin, GPIO_OUTPUT_ACTIVE);
	k_busy_wait(100);
	driven_high = sys_read32(input_reg);

	/* Back to the I2S function and left as an input, like the probe reads it */
	sys_write32(mux, mux_reg);
	(void)gpio_pin_configure(gpio, pin, GPIO_INPUT);

	printk("G80 as a gpio: driven low -> pad %u, driven high -> pad %u (%s)\n",
	       (driven_low & DATA_PIN_BIT) ? 1U : 0U, (driven_high & DATA_PIN_BIT) ? 1U : 0U,
	       ((driven_low & DATA_PIN_BIT) == 0U && (driven_high & DATA_PIN_BIT) != 0U)
		       ? "the pad follows, so the input register is a valid window"
		       : "the pad does not follow, the input register shows nothing here");
}
#endif

static __maybe_unused void dump_state(void)
{
	struct i2c_dt_spec bus = I2C_DT_SPEC_GET(DT_NODELABEL(es8388));
	mem_addr_t i2s = DT_REG_ADDR(DT_NODELABEL(i2s_rxtx));

	/*
	 * config1 lives at 0x14 in the vendor's driver and at 0xd014 in the
	 * manual, so both are printed; the version register identifies the IP.
	 */
	printk("I2S    version=%08x config0=%08x control=%08x\n", sys_read32(i2s + 0x00),
	       sys_read32(i2s + 0x04), sys_read32(i2s + 0x08));
	printk("I2S    config1@0x14=%08x config1@0xd014=%08x\n", sys_read32(i2s + 0x14),
	       sys_read32(i2s + 0xd014));

	/*
	 * Every codec register, 8 per row - the parts that shape the DAC path
	 * beyond the handful the driver writes (mixer gains, the filter
	 * registers 0x1c..0x26, the second output pair 0x30/0x31) are exactly
	 * where a difference against the working vendor state can hide.
	 */
	for (uint8_t base = 0U; base < 0x54U; base += 8U) {
		uint8_t v[8];

		for (size_t i = 0U; i < 8U; i++) {
			if (i2c_reg_read_byte_dt(&bus, base + i, &v[i]) != 0) {
				v[i] = 0xffU;
			}
		}

		printk("i2c %02x: %02x %02x %02x %02x %02x %02x %02x %02x\n", base, v[0], v[1],
		       v[2], v[3], v[4], v[5], v[6], v[7]);
	}
}

int main(void)
{
	const struct device *i2s = DEVICE_DT_GET(DT_NODELABEL(i2s_rxtx));
	const struct device *codec = DEVICE_DT_GET(DT_NODELABEL(es8388));
	struct i2s_config i2s_cfg = {
		.word_size = 16U,
		.channels = 2U,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		/*
		 * Who generates BCLK and LRCLK? By default the SoC does, which is
		 * how the vendor machine driver configures the link (CBS_CFS).
		 * With SAMPLE_CODEC_MASTER it is the codec instead - it has its own
		 * 12.288 MHz oscillator - and the board device tree must then carry
		 * "slave-mode" on this controller.
		 */
#if defined(SAMPLE_CODEC_MASTER)
		.options = I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET,
#else
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
#endif
		.frame_clk_freq = SAMPLE_RATE,
		.mem_slab = NULL, /* the slab is created below */
		.block_size = BLOCK_BYTES,
		.timeout = 2000,
	};
	struct audio_codec_cfg codec_cfg = {
		.dai_type = AUDIO_DAI_TYPE_I2S,
		.dai_route = AUDIO_ROUTE_PLAYBACK,
		.dai_cfg.i2s = {
			.word_size = 16U,
			.channels = 2U,
			.format = I2S_FMT_DATA_FORMAT_I2S,
			/* The mirror image of the choice made for the controller */
#if defined(SAMPLE_CODEC_MASTER)
			.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
#else
			.options = I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET,
#endif
			.frame_clk_freq = SAMPLE_RATE,
			.mem_slab = NULL,
			.block_size = BLOCK_BYTES,
			.timeout = 2000,
		},
	};
	/* Louder is larger on this codec (0..0xc0); 0x90 keeps the digital
	 * stage about 24 dB down from clipping, which the board's output
	 * amplifier turns into a comfortable default level.
	 */
	audio_property_value_t volume = { .vol = 0x90 };
	const struct device *clk = DEVICE_DT_GET(DT_CLOCKS_CTLR(DT_NODELABEL(i2s_rxtx)));
	uint32_t ref_hz = 0U;
	void *block;
	int ret;

	printk("I2S + ES8388 board test\n");

	/* The rate the two dividers count against - see the device tree note */
	(void)clock_control_get_rate(clk,
				     (clock_control_subsys_t)DT_PHA(DT_NODELABEL(i2s_rxtx),
								    clocks, clkid),
				     &ref_hz);
	printk("I2S divider reference clock: %u Hz\n", ref_hz);

	if (!device_is_ready(i2s)) {
		printk("I2S device not ready\n");
		return 0;
	}

	if (!device_is_ready(codec)) {
		printk("codec device not ready\n");
		return 0;
	}

	K_MEM_SLAB_DEFINE_STATIC(slab, BLOCK_BYTES, BLOCK_COUNT, 4);

	i2s_cfg.mem_slab = &slab;
	codec_cfg.dai_cfg.i2s.mem_slab = &slab;

	/* 1. The codec first: it has to be listening before the clocks start */
	ret = audio_codec_configure(codec, &codec_cfg);
	if (ret != 0) {
		printk("codec configure failed: %d\n", ret);
		return 0;
	}

	(void)audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL,
				       volume);
	(void)audio_codec_apply_properties(codec);
	audio_codec_start(codec, AUDIO_DAI_DIR_TX);

	/* 2. Then the controller */
	ret = i2s_configure(i2s, I2S_DIR_TX, &i2s_cfg);
	if (ret != 0) {
		printk("i2s configure failed: %d\n", ret);
		return 0;
	}

#if !defined(SAMPLE_CLOCK_ONLY)
	ret = i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret != 0) {
		printk("i2s start failed: %d\n", ret);
		return 0;
	}

	printk("playback: %u blocks of %u bytes, %u Hz tone\n", PLAY_BLOCKS, BLOCK_BYTES,
	       TONE_HZ);
	dump_state();

	for (uint32_t i = 0U; i < PLAY_BLOCKS; i++) {
		ret = k_mem_slab_alloc(&slab, &block, K_FOREVER);
		if (ret != 0) {
			break;
		}

		fill_tone(block, BLOCK_BYTES);

		/* Blocks until the data has been pushed into the FIFOs */
		ret = i2s_write(i2s, block, BLOCK_BYTES);
		k_mem_slab_free(&slab, block);

		if (ret != 0) {
			printk("i2s write failed at block %u: %d\n", i, ret);
			break;
		}

		/*
		 * The pad probe belongs here, over real audio: half the tone in,
		 * the ring carries loud samples, so a data pin that holds level
		 * now really is a pin that is not driven (over silence an all-zero
		 * frame reads the same as a dead pad).
		 */
		if (i == (PLAY_BLOCKS / 2U)) {
			probe_data_pin();
		}

		if ((i % 100U) == 0U) {
			printk("  playback block %u\n", i);
		}
	}

		/*
	 * 3. Format sweep: which format does this controller actually speak? The
	 * block has no format register - the vendor driver's set_dai_fmt() writes
	 * nothing - so its wire format is fixed in silicon and it is the codec
	 * that has to be told the matching one. The same tone is played with the
	 * codec in each of the three formats; the one that matches comes out as a
	 * clean tone, the others as noise or distortion.
	 */
	static const struct {
		audio_dai_type_t type;
		const char *name;
	} sweep[] = {
		{ AUDIO_DAI_TYPE_I2S, "I2S" },
		{ AUDIO_DAI_TYPE_LEFT_JUSTIFIED, "left justified" },
		{ AUDIO_DAI_TYPE_RIGHT_JUSTIFIED, "right justified" },
	};

	printk("format sweep, %u blocks each (%u formats): ", SWEEP_BLOCKS,
	       (uint32_t)ARRAY_SIZE(sweep));

	for (size_t f = 0U; f < ARRAY_SIZE(sweep); f++) {
		codec_cfg.dai_type = sweep[f].type;
		codec_cfg.dai_route = AUDIO_ROUTE_PLAYBACK;

		if (audio_codec_configure(codec, &codec_cfg) != 0) {
			printk("(%s: configure failed) ", sweep[f].name);
			continue;
		}

		(void)audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME,
					       AUDIO_CHANNEL_ALL, volume);
		(void)audio_codec_apply_properties(codec);
		audio_codec_start(codec, AUDIO_DAI_DIR_TX);

		for (uint32_t b = 0U; b < SWEEP_BLOCKS; b++) {
			ret = k_mem_slab_alloc(&slab, &block, K_FOREVER);
			if (ret != 0) {
				break;
			}

			fill_tone(block, BLOCK_BYTES);
			ret = i2s_write(i2s, block, BLOCK_BYTES);
			k_mem_slab_free(&slab, block);

			if (ret != 0) {
				break;
			}
		}

		printk("%s, ", sweep[f].name);
	}

	printk("done\n");

	/* Back to the format the rest of the test assumes */
	codec_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
	codec_cfg.dai_route = AUDIO_ROUTE_PLAYBACK;
	(void)audio_codec_configure(codec, &codec_cfg);
	audio_codec_start(codec, AUDIO_DAI_DIR_TX);

	/*
	 * 4. The sample audio: the melody the build generated (or the recording it
	 * was pointed at). It plays once; the pitch and the tempo of the notes are
	 * what tells whether the two clocks are right.
	 */
	printk("sample audio: %u samples at %u Hz = %u ms\n",
	       (uint32_t)sample_audio_16k_samples, CLIP_RATE,
	       (uint32_t)((uint64_t)sample_audio_16k_samples * 1000U / CLIP_RATE));

	uint32_t clip_frames = (uint32_t)sample_audio_16k_samples * CLIP_STEP;
	uint32_t clip_blocks = (clip_frames * 4U + BLOCK_BYTES - 1U) / BLOCK_BYTES;

	for (uint32_t b = 0U; b < clip_blocks; b++) {
		ret = k_mem_slab_alloc(&slab, &block, K_FOREVER);
		if (ret != 0) {
			break;
		}

		fill_sample_audio(block, BLOCK_BYTES);

		ret = i2s_write(i2s, block, BLOCK_BYTES);
		k_mem_slab_free(&slab, block);

		if (ret != 0) {
			printk("i2s write failed during the clip: %d\n", ret);
			break;
		}

		if ((b % 40U) == 0U) {
			printk("  clip block %u/%u\n", b, clip_blocks);
		}
	}

#if defined(SAMPLE_LIVE)
	/*
	 * Live mode: the melody repeats for good, so that the codec can be poked
	 * over I2C from the shell while audio keeps flowing. Every register write
	 * is heard immediately, which is how the register experiments documented
	 * in the port notes are meant to be run; nothing here touches the codec.
	 */
	printk("live: the melody repeats - poke the codec on i2c2 (0x11) and listen\n");

	for (uint32_t round = 1U;; round++) {
		for (uint32_t b = 0U; b < clip_blocks; b++) {
			ret = k_mem_slab_alloc(&slab, &block, K_FOREVER);
			if (ret != 0) {
				break;
			}

			fill_sample_audio(block, BLOCK_BYTES);
			ret = i2s_write(i2s, block, BLOCK_BYTES);
			k_mem_slab_free(&slab, block);

			if (ret != 0) {
				break;
			}
		}

		printk("  round %u\n", round);
	}
#endif

	(void)i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
#else
	/*
	 * Clocks only (-DSAMPLE_CLOCK_ONLY=1): the controller keeps generating
	 * BCLK and LRCLK as the clock master, but nothing is transmitted, so the
	 * data line stays idle and what the capture phase prints is the codec's
	 * own ADC output. A codec that has a system clock digitises at least its
	 * noise floor, so a non-zero peak means the clocks arrive and the
	 * codec's data path works - and everything reaching the headphone jack
	 * is then a matter of the analog side.
	 */
	printk("clock only: no transmit, capture follows\n");
#if defined(SAMPLE_CLOCK_ONLY)
	probe_gpio_drive();
#endif
#endif

	/* 5. Full duplex: echo what the ADC captures back to the DAC */
	codec_cfg.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE;
	ret = audio_codec_configure(codec, &codec_cfg);
	if (ret != 0) {
		printk("codec reconfigure failed: %d\n", ret);
		return 0;
	}

	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	ret = i2s_configure(i2s, I2S_DIR_RX, &i2s_cfg);
	if (ret != 0) {
		printk("i2s RX configure failed: %d\n", ret);
		return 0;
	}

	/* Reconfiguring muted the DAC again, so unmute it before the echo */
	audio_codec_start(codec, AUDIO_DAI_DIR_TXRX);

#if !defined(SAMPLE_CLOCK_ONLY)
	(void)i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_START);
#endif
	(void)i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_START);

	printk("loopback: %u blocks, peak per block\n", ECHO_BLOCKS);

	uint32_t write_errors = 0U;

	for (uint32_t i = 0U; i < ECHO_BLOCKS; i++) {
		void *rx_block;
		size_t rx_size;

		/*
		 * Receive first. When the codec is the clock master the transmit
		 * side has no bit clock to shift out on and its writes stall, so
		 * reading first is what keeps the capture verdict reachable; a
		 * failed write is counted and reported rather than ending the loop.
		 */
		ret = i2s_read(i2s, &rx_block, &rx_size);
		if (ret != 0) {
			printk("i2s read failed at block %u: %d\n", i, ret);
			break;
		}

		if ((i % 50U) == 0U) {
			printk("  echo block %u: peak %d\n", i, peak_of(rx_block, rx_size));
		}

		k_mem_slab_free(&slab, rx_block);

		if (k_mem_slab_alloc(&slab, &block, K_FOREVER) != 0) {
			break;
		}

		fill_tone(block, BLOCK_BYTES);
		if (i2s_write(i2s, block, BLOCK_BYTES) != 0) {
			if (write_errors == 0U) {
				printk("  (transmit side is not running - expected with "
				       "the codec as clock master; capture continues)\n");
			}

			write_errors++;
		}

		k_mem_slab_free(&slab, block);
	}

	if (write_errors > 0U) {
		printk("  %u of %u transmit writes failed\n", write_errors, ECHO_BLOCKS);
	}

	(void)i2s_trigger(i2s, I2S_DIR_TX, I2S_TRIGGER_STOP);
	(void)i2s_trigger(i2s, I2S_DIR_RX, I2S_TRIGGER_STOP);
	audio_codec_stop(codec, AUDIO_DAI_DIR_TXRX);

	printk("done\n");

	return 0;
}
