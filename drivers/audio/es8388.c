/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everest ES8388 stereo audio codec driver (I2C control interface).
 *
 * The register model and the sequences are ported from Linux' ES8328 driver
 * (sound/soc/codecs/es8328.c and es8328.h), which is also what the vendor
 * kernel uses for this board's "everest,es8388" node: the ES8388 shares the
 * ES8328 register map for everything this driver touches. Because Linux drives
 * this part through its DAPM widgets, the sequence here has to reproduce what
 * those widgets write when the playback path comes up - the DAC into mixer
 * switches and the output amplifier volumes in particular (see the register
 * notes below); a driver that only enables the DAC and the outputs stays
 * silent.
 *
 * What is implemented: the playback path (DAC -> mixer -> LOUT1/ROUT1), the
 * capture path (ADC), the I2S format with 16/18/20/24/32 bit words, the sample
 * rate tables for a 12.288 MHz and an 11.2896 MHz system clock, master/target
 * role selection, and output/input volume and mute - the output volume scales
 * both the digital (PCM) and the amplifier stage, so it stays audible over its
 * whole range. Codec-specific extras of the part (the bypath/line mixer inputs,
 * the shelving filter, the de-emphasis filter, the on-chip PLL, over-current
 * reporting) are not exposed: the audio codec API has no surface for them, and
 * the board uses the part as a plain stereo DAC/ADC.
 *
 * The control interface is I2C, so the driver is a normal I2C client device;
 * nothing about the I2S data path is programmed from here beyond the format
 * bits inside the codec (see dts/bindings/audio/everest,es8388.yaml).
 */

#define DT_DRV_COMPAT everest_es8388

#include <zephyr/audio/codec.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

/* Register map (ES8328/ES8388), see sound/soc/codecs/es8328.h */
#define ES_CONTROL1    0x00U
#define ES_CONTROL2    0x01U
#define ES_CHIPPOWER   0x02U
#define ES_ADCPOWER    0x03U
#define ES_DACPOWER    0x04U
#define ES_MASTERMODE  0x08U
#define ES_ADCCONTROL4 0x0cU /* ADC format and word length */
#define ES_ADCCONTROL5 0x0dU /* ADC sample rate */
#define ES_ADCCONTROL7 0x0fU /* ADC mute lives here */
#define ES_ADCCONTROL8 0x10U /* left ADC volume */
#define ES_ADCCONTROL9 0x11U /* right ADC volume */
#define ES_DACCONTROL1 0x17U /* DAC format and word length */
#define ES_DACCONTROL2 0x18U /* DAC sample rate, double speed */
#define ES_DACCONTROL3 0x19U /* DAC mute, ramp */
#define ES_DACCONTROL4 0x1aU /* left DAC (PCM) volume */
#define ES_DACCONTROL5 0x1bU /* right DAC (PCM) volume */

/*
 * The two stages between the DACs and the output pins. The Linux driver brings
 * them up through its DAPM widgets rather than in a sequence:
 *
 *   SND_SOC_DAPM_MIXER("Left Mixer",  ES8328_DACCONTROL17, 7, 0, ...)
 *   SND_SOC_DAPM_MIXER("Right Mixer", ES8328_DACCONTROL20, 7, 0, ...)
 *   SOC_DOUBLE_R_TLV("Output 1 Playback Volume", ES8328_LOUT1VOL,
 *                    ES8328_ROUT1VOL, 0, ES8328_OUT1VOL_MAX, 0, play_tlv)
 *
 * i.e. bit 7 of the mixer registers is the "DAC into mixer" switch and the
 * LOUT1/ROUT1 amplifier volumes sit in 0x2e/0x2f (the same registers the header
 * also names DACLVOL/DACRVOL for the ES8388). Without the switches nothing
 * reaches the mixers, and the amplifier volume - "play_tlv", -30 dB at value 0
 * up to +6 dB at 0x24 - is what the output level depends on.
 */
#define ES_DACCONTROL17 0x27U /* left DAC -> left mixer, left mixer volume */
#define ES_DACCONTROL20 0x2aU /* right DAC -> right mixer */
#define ES_LOUT1VOL     0x2eU /* left output amplifier volume, 0..0x24 */
#define ES_ROUT1VOL     0x2fU /* right output amplifier volume, 0..0x24 */
#define ES_LOUT2VOL     0x30U /* second output pair, 0..0x24 */
#define ES_ROUT2VOL     0x31U

#define ES_DACCONTROL17_LD2LO BIT(7)
#define ES_DACCONTROL20_RD2RO BIT(7)
#define ES_OUT1VOL_MAX        0x24U

/* CONTROL1: VMID divider, reference, sequencer, shared frame clock */
#define ES_CONTROL1_VMIDSEL_50K  0x01U
#define ES_CONTROL1_VMIDSEL_500K 0x02U
#define ES_CONTROL1_VMIDSEL_MASK 0x03U
#define ES_CONTROL1_ENREF        BIT(2)
#define ES_CONTROL1_SEQEN        BIT(3)
#define ES_CONTROL1_SAMEFS       BIT(4) /* ADC and DAC share LRCLK */
#define ES_CONTROL1_DACMCLK      BIT(5) /* DAC clocked from MCLK */
#define ES_CONTROL1_SCPRESET     BIT(7)

/* CONTROL2: bias and protection */
#define ES_CONTROL2_OVERCURRENT_ON BIT(6)
#define ES_CONTROL2_THERMAL_ON     BIT(7)

/* DACPOWER: which outputs are on; the DAC itself is off when bit 6/7 is set */
#define ES_DACPOWER_LOUT1 BIT(5)
#define ES_DACPOWER_ROUT1 BIT(4)
#define ES_DACPOWER_LOUT2 BIT(3)
#define ES_DACPOWER_ROUT2 BIT(2)
#define ES_DACPOWER_LDAC_OFF BIT(7)
#define ES_DACPOWER_RDAC_OFF BIT(6)

/* ADCPOWER: the ADC blocks are off when their bits are set */
#define ES_ADCPOWER_ALL_OFF 0xc0U

/* MASTERMODE */
#define ES_MASTERMODE_MSC      BIT(7) /* 1: the codec drives BCLK and LRCLK */
#define ES_MASTERMODE_MCLKDIV2 BIT(6)
#define ES_MASTERMODE_BCLK_INV BIT(5)

/* Frame format, in the format field of the ADC/DAC control registers */
#define ES_FMT_I2S    0x00U
#define ES_FMT_LJUST  0x01U
#define ES_FMT_RJUST  0x02U

#define ES_DACCONTROL1_FMT_MASK  0x06U /* bits 2:1 */
#define ES_DACCONTROL1_WL_SHIFT  3U
#define ES_DACCONTROL1_WL_MASK   0x38U /* bits 5:3 */
#define ES_ADCCONTROL4_FMT_MASK  0x03U /* bits 1:0 */
#define ES_ADCCONTROL4_WL_SHIFT  2U
#define ES_ADCCONTROL4_WL_MASK   0x1cU /* bits 4:2 */

#define ES_DACCONTROL3_MUTE      0x04U
#define ES_DACCONTROL3_SOFTRAMP  0x20U
#define ES_DACCONTROL3_ZEROCROSS 0x10U
#define ES_DACCONTROL3_AUTOMUTE  0x04U /* same bit position as the mute bit */

#define ES_ADCCONTROL7_MUTE 0x04U

/*
 * Sample rate tables. The value in the rate register selects one MCLK/LRCLK
 * ratio; Linux carries the same two tables (ratios_12288 / ratios_11289).
 */
struct es_rate {
	uint32_t rate;
	uint8_t code;
};

static const struct es_rate es_rates_12288[] = {
	{ 8000U, 10U },  { 12000U, 7U }, { 16000U, 6U },
	{ 24000U, 4U },  { 32000U, 3U }, { 48000U, 2U },
	{ 96000U, 0U },
};

static const struct es_rate es_rates_11289[] = {
	{ 8018U, 9U }, { 11025U, 7U }, { 22050U, 4U }, { 44100U, 2U }, { 88200U, 0U },
};

struct es8388_config {
	struct i2c_dt_spec bus;
	uint32_t mclk_frequency;
};

struct es8388_data {
	struct k_mutex lock;
	/* The configuration the application asked for */
	uint32_t rate;
	uint8_t word_size;
	audio_route_t route;
	bool codec_is_controller;
	/* Values staged by set_property() until apply_properties() runs */
	uint8_t dac_volume[2];
	uint8_t adc_volume[2];
	bool dac_mute;
	bool adc_mute;
};

static int es_write(const struct device *dev, uint8_t reg, uint8_t value)
{
	const struct es8388_config *cfg = dev->config;

	return i2c_reg_write_byte_dt(&cfg->bus, reg, value);
}

static int es_update(const struct device *dev, uint8_t reg, uint8_t mask, uint8_t value)
{
	const struct es8388_config *cfg = dev->config;

	return i2c_reg_update_byte_dt(&cfg->bus, reg, mask, value);
}

static int es_read(const struct device *dev, uint8_t reg, uint8_t *value)
{
	const struct es8388_config *cfg = dev->config;

	return i2c_reg_read_byte_dt(&cfg->bus, reg, value);
}

static const struct es_rate *es_rate_lookup(const struct es_rate *table, size_t count,
					    uint32_t rate)
{
	for (size_t i = 0U; i < count; i++) {
		if (table[i].rate == rate) {
			return &table[i];
		}
	}

	return NULL;
}

/* Word length codes as the ES8328 uses them (see Linux' es8328_hw_params) */

/*
 * Bring the part up in the order the ES8328 driver uses: power the blocks up
 * (CHIPPOWER = 0), start the reference and the VMID divider, wait for the
 * capacitors, then settle the divider at its low power setting and switch the
 * over-current and thermal protection on.
 */
static int es_power_up(const struct device *dev)
{
	int ret = 0;

	ret = es_write(dev, ES_CHIPPOWER, 0x00U);
	if (ret != 0) {
		return ret;
	}

	ret = es_write(dev, ES_CONTROL1,
		       ES_CONTROL1_VMIDSEL_50K | ES_CONTROL1_ENREF | ES_CONTROL1_SEQEN |
			       ES_CONTROL1_SAMEFS | ES_CONTROL1_DACMCLK);
	if (ret != 0) {
		return ret;
	}

	k_msleep(100);

	ret = es_write(dev, ES_CONTROL2, ES_CONTROL2_OVERCURRENT_ON | ES_CONTROL2_THERMAL_ON);
	if (ret != 0) {
		return ret;
	}

	return es_update(dev, ES_CONTROL1,
			 ES_CONTROL1_VMIDSEL_MASK | ES_CONTROL1_ENREF,
			 ES_CONTROL1_VMIDSEL_500K | ES_CONTROL1_ENREF);
}

static int es8388_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	struct es8388_data *data = dev->data;
	const struct es8388_config *dcfg = dev->config;
	const struct es_rate *rate_entry;
	const struct es_rate *table;
	size_t table_len;
	uint8_t fmt;
	uint8_t wl;
	bool master;
	int ret = 0;

	if ((cfg->dai_type != AUDIO_DAI_TYPE_I2S) &&
	    (cfg->dai_type != AUDIO_DAI_TYPE_LEFT_JUSTIFIED) &&
	    (cfg->dai_type != AUDIO_DAI_TYPE_RIGHT_JUSTIFIED)) {
		return -ENOTSUP;
	}

	if (cfg->dai_route == AUDIO_ROUTE_BYPASS) {
		return -ENOTSUP;
	}

	/* Which table applies is decided by the system clock the board runs */
	switch (dcfg->mclk_frequency) {
	case 12288000U:
		table = es_rates_12288;
		table_len = ARRAY_SIZE(es_rates_12288);
		break;
	case 11289600U:
		table = es_rates_11289;
		table_len = ARRAY_SIZE(es_rates_11289);
		break;
	default:
		return -EINVAL;
	}

	/*
	 * The application tells us whether it wants the codec to be the target of
	 * the bit and frame clocks (the SoC drives them, as on this board) or the
	 * controller that generates them.
	 */
	master = (cfg->dai_cfg.i2s.options &
		  (I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET)) == 0U;

	rate_entry = es_rate_lookup(table, table_len, cfg->dai_cfg.i2s.frame_clk_freq);
	if (rate_entry == NULL) {
		return -EINVAL;
	}

	if (ret != 0) {
		return ret;
	}

	switch (cfg->dai_type) {
	case AUDIO_DAI_TYPE_LEFT_JUSTIFIED:
		fmt = ES_FMT_LJUST;
		break;
	case AUDIO_DAI_TYPE_RIGHT_JUSTIFIED:
		fmt = ES_FMT_RJUST;
		break;
	default:
		fmt = ES_FMT_I2S;
		break;
	}

	k_mutex_lock(&data->lock, K_FOREVER);

	ret = es_power_up(dev);
	if (ret != 0) {
		goto out;
	}

	/* Master/target role; the dividers stay at their reset (divide by 1) */
	ret = es_update(dev, ES_MASTERMODE, ES_MASTERMODE_MSC | ES_MASTERMODE_MCLKDIV2 |
						 ES_MASTERMODE_BCLK_INV,
			master ? ES_MASTERMODE_MSC : 0U);
	if (ret != 0) {
		goto out;
	}

	/*
	 * Frame format AND word length, playback and capture side alike - the
	 * two fields the Linux driver's hw_params() writes on both registers
	 * (16 bit -> 3; 18 -> 2; 20 -> 1; 24 -> 0; 32 -> 4). A word length that
	 * differs from the frames the controller puts on the wire misframes the
	 * codec's parser - on the capture side this showed as a flat, near-zero
	 * echo while the part sat at its reset word length.
	 */
	switch (cfg->dai_cfg.i2s.word_size) {
	case 16U:
		wl = 3U;
		break;
	case 18U:
		wl = 2U;
		break;
	case 20U:
		wl = 1U;
		break;
	case 24U:
		wl = 0U;
		break;
	case 32U:
		wl = 4U;
		break;
	default:
		k_mutex_unlock(&data->lock);
		return -EINVAL;
	}

	ret = es_update(dev, ES_DACCONTROL1, ES_DACCONTROL1_FMT_MASK | (0x7U << 3),
			(fmt << 1) | (wl << 3));
	if (ret != 0) {
		goto out;
	}

	ret = es_update(dev, ES_ADCCONTROL4, ES_ADCCONTROL4_FMT_MASK | (0x7U << 2),
			fmt | (wl << 2));
	if (ret != 0) {
		goto out;
	}

	/*
	 * Sample rate for both directions. When the codec is the clock target -
	 * as on this board - the Linux driver always writes 0 here: its
	 * hw_params only computes a ratio when the codec is the clock provider,
	 * and the working configuration of this board is a target with code 0
	 * (128 fs), fed 6.144 MHz (= 128 * 48 kHz) by the SoC. Only a provider
	 * gets the rate the table says.
	 */
	if (!master) {
		ret = es_update(dev, ES_DACCONTROL2, 0x1fU, 0U);
		if (ret != 0) {
			goto out;
		}

		ret = es_update(dev, ES_ADCCONTROL5, 0x1fU, 0U);
	} else {
		ret = es_update(dev, ES_DACCONTROL2, 0x1fU, rate_entry->code);
		if (ret != 0) {
			goto out;
		}

		ret = es_update(dev, ES_ADCCONTROL5, 0x1fU, rate_entry->code);
	}

	if (ret != 0) {
		goto out;
	}

	/* Playback path: line out on, DAC on, soft ramp, no mute yet */
	if ((cfg->dai_route == AUDIO_ROUTE_PLAYBACK) ||
	    (cfg->dai_route == AUDIO_ROUTE_PLAYBACK_CAPTURE)) {
		/*
		 * Both output pairs are enabled, because which one a board brings
		 * out is its own business: OUT1 is the headphone pair in the
		 * usual application, OUT2 the speaker pair. Enabling an unconnected
		 * pair costs nothing, while leaving the connected one off means
		 * perfect registers and complete silence.
		 */
		ret = es_update(dev, ES_DACPOWER,
				ES_DACPOWER_LOUT1 | ES_DACPOWER_ROUT1 | ES_DACPOWER_LOUT2 |
					ES_DACPOWER_ROUT2 | ES_DACPOWER_LDAC_OFF |
					ES_DACPOWER_RDAC_OFF,
				ES_DACPOWER_LOUT1 | ES_DACPOWER_ROUT1 | ES_DACPOWER_LOUT2 |
					ES_DACPOWER_ROUT2);
		if (ret != 0) {
			goto out;
		}

		ret = es_update(dev, ES_DACCONTROL3,
				ES_DACCONTROL3_MUTE | ES_DACCONTROL3_SOFTRAMP |
					ES_DACCONTROL3_ZEROCROSS,
				ES_DACCONTROL3_MUTE | ES_DACCONTROL3_SOFTRAMP |
					ES_DACCONTROL3_ZEROCROSS);
		if (ret != 0) {
			goto out;
		}

		/*
		 * Connect each DAC into its output mixer and open the output
		 * amplifiers. These are the two stages the Linux driver writes
		 * from its DAPM widgets, and without them the part stays silent
		 * no matter what the DAC does - which is exactly what happened
		 * before they were added here.
		 */
		ret = es_update(dev, ES_DACCONTROL17, ES_DACCONTROL17_LD2LO,
				ES_DACCONTROL17_LD2LO);
		if (ret != 0) {
			goto out;
		}

		ret = es_update(dev, ES_DACCONTROL20, ES_DACCONTROL20_RD2RO,
				ES_DACCONTROL20_RD2RO);
		if (ret != 0) {
			goto out;
		}

		ret = es_write(dev, ES_LOUT1VOL, ES_OUT1VOL_MAX);
		if (ret != 0) {
			goto out;
		}

		ret = es_write(dev, ES_ROUT1VOL, ES_OUT1VOL_MAX);
		if (ret != 0) {
			goto out;
		}
	}

	/* Capture path: ADC blocks on */
	if ((cfg->dai_route == AUDIO_ROUTE_CAPTURE) ||
	    (cfg->dai_route == AUDIO_ROUTE_PLAYBACK_CAPTURE)) {
		ret = es_write(dev, ES_ADCPOWER, 0x00U);
	} else {
		ret = es_write(dev, ES_ADCPOWER, ES_ADCPOWER_ALL_OFF);
	}

	if (ret != 0) {
		goto out;
	}

	data->rate = cfg->dai_cfg.i2s.frame_clk_freq;
	data->word_size = cfg->dai_cfg.i2s.word_size;
	data->route = cfg->dai_route;
	data->codec_is_controller = master;

out:
	k_mutex_unlock(&data->lock);

	return ret;
}

static int es8388_start(const struct device *dev, audio_dai_dir_t dir)
{
	struct es8388_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	/* TX is the direction the host transmits in, i.e. the DAC path */
	if ((dir & AUDIO_DAI_DIR_TX) != 0U) {
		/* Soft ramp off the mute, exactly like Linux' unmute path */
		ret = es_update(dev, ES_DACCONTROL3,
				ES_DACCONTROL3_MUTE | ES_DACCONTROL3_SOFTRAMP,
				ES_DACCONTROL3_SOFTRAMP);
		data->dac_mute = false;
	}

	/*
	 * RX is the capture path, and its digital mute lives in ADCCONTROL7 -
	 * it has to be lifted here. The part comes up muted (es8388_init), the
	 * mute is written by apply_properties(), and this is the only place
	 * that could ever clear it: Linux' mute_stream() touches just the DAC,
	 * so its capture path simply never writes that register and runs on
	 * the reset value. Leaving the bit set made the ADC deliver a flat
	 * line whatever the PGA input mux was set to, which is what kept the
	 * capture tests reading a peak of 1 through a whole input scan.
	 */
	if ((dir & AUDIO_DAI_DIR_RX) != 0U) {
		ret = es_update(dev, ES_ADCCONTROL7, ES_ADCCONTROL7_MUTE, 0U);
		data->adc_mute = false;
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int es8388_stop(const struct device *dev, audio_dai_dir_t dir)
{
	struct es8388_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	/* TX: the DAC path; RX: the ADC path (see es8388_start) */
	if ((dir & AUDIO_DAI_DIR_TX) != 0U) {
		ret = es_update(dev, ES_DACCONTROL3, ES_DACCONTROL3_MUTE,
				ES_DACCONTROL3_MUTE);
		data->dac_mute = true;
	}

	if ((dir & AUDIO_DAI_DIR_RX) != 0U) {
		/* Mute first, then power the input stages down */
		ret = es_update(dev, ES_ADCCONTROL7, ES_ADCCONTROL7_MUTE,
				ES_ADCCONTROL7_MUTE);
		data->adc_mute = true;

		ret = es_write(dev, ES_ADCPOWER, ES_ADCPOWER_ALL_OFF);
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

static int es8388_set_property(const struct device *dev, audio_property_t property,
			       audio_channel_t channel, audio_property_value_t val)
{
	struct es8388_data *data = dev->data;
	int ret = 0;

	k_mutex_lock(&data->lock, K_FOREVER);

	switch (property) {
	case AUDIO_PROPERTY_OUTPUT_VOLUME:
		/*
		 * The API leaves the volume scale to the codec; this one takes
		 * the codec's own 0..0xc0 range, louder = larger. Mind the
		 * hardware direction: the PCM volume register holds a NEGATIVE
		 * dB scale (Linux: dac_adc_tlv with invert=1), 0 = 0 dB and
		 * 0xc0 = -96 dB, so the loudest request maps to the smallest
		 * register value (es8388_apply_properties does the inversion).
		 */
		if ((val.vol < 0) || (val.vol > 0xc0)) {
			ret = -EINVAL;
			break;
		}

		if ((channel == AUDIO_CHANNEL_FRONT_LEFT) || (channel == AUDIO_CHANNEL_ALL)) {
			data->dac_volume[0] = (uint8_t)val.vol;
		}

		if ((channel == AUDIO_CHANNEL_FRONT_RIGHT) || (channel == AUDIO_CHANNEL_ALL)) {
			data->dac_volume[1] = (uint8_t)val.vol;
		}
		break;

	case AUDIO_PROPERTY_OUTPUT_MUTE:
		data->dac_mute = val.mute;
		break;

	case AUDIO_PROPERTY_INPUT_VOLUME:
		if ((val.vol < 0) || (val.vol > 0xc0)) {
			ret = -EINVAL;
			break;
		}

		if ((channel == AUDIO_CHANNEL_FRONT_LEFT) || (channel == AUDIO_CHANNEL_ALL)) {
			data->adc_volume[0] = (uint8_t)val.vol;
		}

		if ((channel == AUDIO_CHANNEL_FRONT_RIGHT) || (channel == AUDIO_CHANNEL_ALL)) {
			data->adc_volume[1] = (uint8_t)val.vol;
		}
		break;

	case AUDIO_PROPERTY_INPUT_MUTE:
		data->adc_mute = val.mute;
		break;

	default:
		ret = -ENOTSUP;
		break;
	}

	k_mutex_unlock(&data->lock);

	return ret;
}

/* Map the 0..0xc0 digital volume onto the 0..0x24 amplifier range */
static uint8_t es_output_volume(uint8_t digital)
{
	return (uint8_t)(((uint32_t)digital * ES_OUT1VOL_MAX + (0xc0U / 2U)) / 0xc0U);
}

static int es8388_apply_properties(const struct device *dev)
{
	struct es8388_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);

	/*
	 * DACCONTROL4/5 hold the digital (PCM) volume, DACCONTROL3 the mute, and
	 * the amplifier registers 0x2e/0x2f the output level. The API's volume is
	 * applied to both, scaled from its 0..0xc0 range onto the amplifiers'
	 * 0..0x24 (which is -30 dB .. +6 dB, so the top of the range is also the
	 * loudest setting).
	 *
	 * The digital stage is inverted in hardware (0 = 0 dB, 0xc0 = -96 dB -
	 * writing the loudness directly here is what silenced the board for
	 * twenty-five rounds), so the API's "louder is larger" volume goes in
	 * as 0xc0 - volume.
	 */
	ret = es_write(dev, ES_DACCONTROL4, 0xc0U - data->dac_volume[0]);
	if (ret != 0) {
		goto out;
	}

	ret = es_write(dev, ES_DACCONTROL5, 0xc0U - data->dac_volume[1]);
	if (ret != 0) {
		goto out;
	}

	ret = es_write(dev, ES_LOUT1VOL,
		       es_output_volume(data->dac_volume[0]));
	if (ret != 0) {
		goto out;
	}

	ret = es_write(dev, ES_ROUT1VOL,
		       es_output_volume(data->dac_volume[1]));
	if (ret != 0) {
		goto out;
	}

	/* The second output pair, for a board that listens there instead */
	ret = es_write(dev, ES_LOUT2VOL,
		       es_output_volume(data->dac_volume[0]));
	if (ret != 0) {
		goto out;
	}

	ret = es_write(dev, ES_ROUT2VOL,
		       es_output_volume(data->dac_volume[1]));
	if (ret != 0) {
		goto out;
	}

	ret = es_update(dev, ES_DACCONTROL3, ES_DACCONTROL3_MUTE,
			data->dac_mute ? ES_DACCONTROL3_MUTE : 0U);
	if (ret != 0) {
		goto out;
	}

	/* Same inverted negative-dB scale as the DAC volume (0 = 0 dB) */
	ret = es_write(dev, ES_ADCCONTROL8, 0xc0U - data->adc_volume[0]);
	if (ret != 0) {
		goto out;
	}

	ret = es_write(dev, ES_ADCCONTROL9, 0xc0U - data->adc_volume[1]);
	if (ret != 0) {
		goto out;
	}

	ret = es_update(dev, ES_ADCCONTROL7, ES_ADCCONTROL7_MUTE,
			data->adc_mute ? ES_ADCCONTROL7_MUTE : 0U);

out:
	k_mutex_unlock(&data->lock);

	return ret;
}

static int es8388_route_output(const struct device *dev, audio_channel_t channel, uint32_t output)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	ARG_UNUSED(output);

	/* The default DAC -> line out path is the only one wired on this board */
	return -ENOTSUP;
}

/*
 * The audio codec class has both a direction-explicit pair (start/stop, for
 * parts that also capture) and a playback-only pair (start_output/stop_output)
 * that the simple codec users call; both are provided and do the same for the
 * DAC path.
 */
static void es8388_start_output(const struct device *dev)
{
	(void)es8388_start(dev, AUDIO_DAI_DIR_TX);
}

static void es8388_stop_output(const struct device *dev)
{
	(void)es8388_stop(dev, AUDIO_DAI_DIR_TX);
}

static const struct audio_codec_api es8388_api = {
	.configure = es8388_configure,
	.start = es8388_start,
	.stop = es8388_stop,
	.start_output = es8388_start_output,
	.stop_output = es8388_stop_output,
	.set_property = es8388_set_property,
	.apply_properties = es8388_apply_properties,
	.route_output = es8388_route_output,
};

static int es8388_init(const struct device *dev)
{
	struct es8388_data *data = dev->data;
	const struct es8388_config *cfg = dev->config;
	uint8_t id;

	k_mutex_init(&data->lock);

	if (!i2c_is_ready_dt(&cfg->bus)) {
		return -ENODEV;
	}

	/*
	 * Probe the part the way Linux' es8328 does not have to: reading back a
	 * register that has a known non-zero reset value tells us that the codec
	 * answers at all (CHIPLOPOW1 resets to 0x02 on this family, hmm - use the
	 * volume registers instead, they reset to 0xc0 in mute position).
	 */
	if (es_read(dev, ES_DACCONTROL4, &id) != 0) {
		return -EIO;
	}

	/*
	 * Start muted (the mute flags above do that) and at a comfortable
	 * volume: louder is larger on this codec, and 0x90 sits some 24 dB
	 * below the digital full scale - 0xc0 here would be "as loud as it
	 * goes", not the mute position the hardware's reset value suggests.
	 */
	data->dac_volume[0] = 0x90U;
	data->dac_volume[1] = 0x90U;
	data->adc_volume[0] = 0x90U;
	data->adc_volume[1] = 0x90U;
	data->dac_mute = true;
	data->adc_mute = true;
	data->route = AUDIO_ROUTE_PLAYBACK;

	return 0;
}

#define ES8388_INIT(n)								\
	static struct es8388_data es8388_data_##n;				\
										\
	static const struct es8388_config es8388_config_##n = {			\
		.bus = I2C_DT_SPEC_INST_GET(n),					\
		.mclk_frequency = DT_INST_PROP_OR(n, mclk_frequency, 12288000),	\
	};									\
										\
	DEVICE_DT_INST_DEFINE(n, es8388_init, NULL, &es8388_data_##n,		\
			      &es8388_config_##n, POST_KERNEL,			\
			      CONFIG_AUDIO_CODEC_INIT_PRIORITY, &es8388_api);

DT_INST_FOREACH_STATUS_OKAY(ES8388_INIT)
