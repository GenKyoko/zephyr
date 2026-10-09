/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson 2K0300 real time clock driver.
 *
 * The block holds two counters: a free running one clocked by the 32.768 kHz
 * crystal, and the TOY ("time of year") counter, which is the wall clock used
 * here. The register map is the one of chapter 25 of the 2K0300 user manual and
 * is identical to mainline Linux' rtc-loongson.c:
 *
 *   0x20  sys_toytrim   must be written 0 by software
 *   0x24  sys_toywrite0 TOY value (write only)
 *   0x28  sys_toywrite1 TOY year (write only)
 *   0x2c  sys_toyread0  TOY value (read only)
 *   0x30  sys_toyread1  TOY year (read only)
 *   0x40  sys_rtcctrl   REN bit 13 (RTC counter enable), TEN bit 11 (TOY
 *                       enable), EO bit 8 (crystal enable); the upper bits and
 *                       the low bits are read only write-status flags
 *   0x60  sys_rtctrim   must be written 0 by software
 *
 * TOY value layout (manual table 25-2/-4): MON[31:26] DAY[25:21] HOUR[20:16]
 * MIN[15:10] SEC[9:4] and a tenths-of-a-second field in [3:0]; the year lives
 * in its own register and spans 0..16383.
 *
 * The 2K0300 variant has no working TOY match alarm - mainline Linux keeps it
 * under LOONGSON_RTC_ALARM_WORKAROUND ("Loongson-1C/2K0300 RTC does not support
 * alarm") and clears RTC_FEATURE_ALARM - so this driver implements the wall
 * clock only and leaves the match registers, the RTC counter and the RTC
 * interrupt sources alone.
 *
 * No logging: multi-argument LOG_* calls are still broken on this port (see
 * boards/loongson/ls2k300_pai/PORT_STATUS.md 4.1) and this driver has nothing
 * to say beyond its return codes.
 */

#define DT_DRV_COMPAT loongson_ls2k0300_rtc

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/init.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "rtc_utils.h"

#define TOY_TRIM_REG   0x20U
#define TOY_WRITE0_REG 0x24U
#define TOY_WRITE1_REG 0x28U
#define TOY_READ0_REG  0x2cU
#define TOY_READ1_REG  0x30U
#define RTC_CTRL_REG   0x40U
#define RTC_TRIM_REG   0x60U

#define TOY_MON    GENMASK(31, 26)
#define TOY_DAY    GENMASK(25, 21)
#define TOY_HOUR   GENMASK(20, 16)
#define TOY_MIN    GENMASK(15, 10)
#define TOY_SEC    GENMASK(9, 4)
#define TOY_TENTHS GENMASK(3, 0)

#define RTC_CTRL_TEN BIT(11) /* TOY counter enable */
#define RTC_CTRL_EO  BIT(8)  /* 32.768 kHz crystal enable */

/* The manual: the TOY year register spans 0..16383 */
#define TOY_YEAR_MAX 16383

/* Fields of struct rtc_time the hardware actually stores */
#define RTC_TIME_MASK                                                          \
	(RTC_ALARM_TIME_MASK_SECOND | RTC_ALARM_TIME_MASK_MINUTE |             \
	 RTC_ALARM_TIME_MASK_HOUR | RTC_ALARM_TIME_MASK_MONTHDAY |             \
	 RTC_ALARM_TIME_MASK_MONTH | RTC_ALARM_TIME_MASK_YEAR)

struct rtc_loongson_config {
	mem_addr_t base;
};

static int rtc_loongson_set_time(const struct device *dev, const struct rtc_time *tm)
{
	const struct rtc_loongson_config *cfg = dev->config;
	uint32_t low;

	if ((rtc_utils_validate_rtc_time(tm, RTC_TIME_MASK) == false) ||
	    (tm->tm_year > TOY_YEAR_MAX)) {
		return -EINVAL;
	}

	/*
	 * The tenths field takes 0..9 (manual table 25-2). tm_nsec is 0 when the
	 * caller has no sub-second information, and is clamped rather than
	 * rejected so a caller that passes the "unknown" -1 still gets its wall
	 * clock programmed.
	 */
	low = FIELD_PREP(TOY_TENTHS, CLAMP(tm->tm_nsec / 100000000, 0, 9)) |
	      FIELD_PREP(TOY_SEC, tm->tm_sec) | FIELD_PREP(TOY_MIN, tm->tm_min) |
	      FIELD_PREP(TOY_HOUR, tm->tm_hour) | FIELD_PREP(TOY_DAY, tm->tm_mday) |
	      FIELD_PREP(TOY_MON, tm->tm_mon + 1);

	sys_write32(low, cfg->base + TOY_WRITE0_REG);
	sys_write32((uint32_t)tm->tm_year, cfg->base + TOY_WRITE1_REG);

	/*
	 * A written TOY value only advances while the TOY counter and the
	 * crystal are enabled, which is what mainline does after every set_time
	 * as well (loongson_rtc_set_enabled()).
	 */
	sys_write32(sys_read32(cfg->base + RTC_CTRL_REG) | RTC_CTRL_TEN | RTC_CTRL_EO,
		    cfg->base + RTC_CTRL_REG);

	return 0;
}

static int rtc_loongson_get_time(const struct device *dev, struct rtc_time *tm)
{
	const struct rtc_loongson_config *cfg = dev->config;
	uint32_t low = sys_read32(cfg->base + TOY_READ0_REG);
	uint32_t year = sys_read32(cfg->base + TOY_READ1_REG);

	tm->tm_sec = FIELD_GET(TOY_SEC, low);
	tm->tm_min = FIELD_GET(TOY_MIN, low);
	tm->tm_hour = FIELD_GET(TOY_HOUR, low);
	tm->tm_mday = FIELD_GET(TOY_DAY, low);
	tm->tm_mon = FIELD_GET(TOY_MON, low) - 1;
	tm->tm_year = (int)year;
	tm->tm_nsec = FIELD_GET(TOY_TENTHS, low) * 100000000;
	tm->tm_yday = -1;
	tm->tm_isdst = -1;

	return 0;
}

static DEVICE_API(rtc, rtc_loongson_api) = {
	.set_time = rtc_loongson_set_time,
	.get_time = rtc_loongson_get_time,
};

static int rtc_loongson_init(const struct device *dev)
{
	const struct rtc_loongson_config *cfg = dev->config;

	/*
	 * Both trim registers are documented as "software must write 0" and the
	 * value in them survives a reboot on the battery, so write them on every
	 * boot rather than once.
	 */
	sys_write32(0U, cfg->base + TOY_TRIM_REG);
	sys_write32(0U, cfg->base + RTC_TRIM_REG);

	/* Crystal and TOY counter on: without them the wall clock stands still */
	sys_write32(sys_read32(cfg->base + RTC_CTRL_REG) | RTC_CTRL_TEN | RTC_CTRL_EO,
		    cfg->base + RTC_CTRL_REG);

	return 0;
}

#define RTC_LOONGSON_INIT(n)                                                          \
	static const struct rtc_loongson_config rtc_loongson_cfg_##n = {              \
		.base = DT_INST_REG_ADDR(n),                                          \
	};                                                                            \
                                                                                      \
	DEVICE_DT_INST_DEFINE(n, rtc_loongson_init, NULL, NULL, &rtc_loongson_cfg_##n, \
			      POST_KERNEL, CONFIG_RTC_INIT_PRIORITY, &rtc_loongson_api);

DT_INST_FOREACH_STATUS_OKAY(RTC_LOONGSON_INIT)
