/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch constant frequency timer driver.
 *
 * The timer is a per-core countdown counter (CSR.TCFG/CSR.TVAL) that raises
 * the TI line interrupt (ESTAT.IS[11]) when it reaches zero; CSR.TICLR
 * clears the pending interrupt. A separate stable counter, read with
 * rdtime.d, is used as the free-running cycle source.
 *
 * The unit of TCFG.InitVal is implementation defined - the field starts at bit
 * 2, so the low two bits of the written value and of TVAL are not part of the
 * countdown - and only rdtime is tied to CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC.
 * The ratio between the two is therefore measured once in
 * sys_clock_driver_init() instead of assumed, because getting it wrong is not
 * loud: with a ratio of 4 and a period programmed 4x too short the timer still
 * interrupts, but the handler sees fewer cycles than one tick, computes
 * dticks = 0, and sys_clock_announce(0) never advances the timeout list. Every
 * k_msleep() then hangs while k_uptime_get() keeps working, because that one is
 * driven by rdtime rather than by announced ticks.
 */

#include <limits.h>

#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>
#include <loongarch/csr.h>

/*
 * Debug level at compile time, so the calibration result is available when
 * someone looks for it, but out of the console by default: a driver that
 * announces its clock on every boot is noise. The failure path raises it to a
 * warning, which does show up.
 */
LOG_MODULE_REGISTER(loongarch_constant_timer, LOG_LEVEL_DBG);

#define CYC_PER_TICK ((uint64_t)sys_clock_hw_cycles_per_sec() / \
		      (uint64_t)CONFIG_SYS_CLOCK_TICKS_PER_SEC)

/* Largest countdown ever programmed, in TCFG InitVal units, far below the
 * architectural maximum so that the conversion below cannot overflow.
 */
#define MAX_UNITS (1ULL << 45)

/*
 * The largest value sys_clock_set_timeout() may be handed, and it has to stay
 * a *positive* int32_t.
 *
 * The original definition casted a uint32_t (about 3.5e9) to int32_t, which
 * wraps to a negative number, so CLAMP(ticks - 1, 0, (int32_t)MAX_TICKS) had a
 * negative upper bound and turned *every* timeout - a plain 300 tick sleep
 * included - into garbage. The timer was then armed days out, no tick ever
 * arrived and every sleep in the system hung, while k_uptime_get() kept
 * working because it is derived from rdtime rather than from ticks. It also
 * shows up as a TCFG holding a value near 0xf8ef_ffff_xxxx.
 */
#define MAX_TICKS ((int32_t)MIN(MAX_UNITS / CYC_PER_TICK, (uint64_t)INT32_MAX))
#define MIN_DELAY 1000

#define TICKLESS IS_ENABLED(CONFIG_TICKLESS_KERNEL)

static struct k_spinlock lock;
static uint64_t last_count;

/*
 * TCFG InitVal units per rdtime cycle, as an exact fraction. 1/1, i.e. "one
 * unit is one cycle", until timer_calibrate() has measured it.
 */
static uint32_t units_num = 1U;
static uint32_t units_den = 1U;

/* Program a countdown of 'cycles' rdtime cycles */
static ALWAYS_INLINE void timer_program(uint64_t cycles, bool periodic)
{
	uint64_t units = MIN((cycles * units_num) / units_den, MAX_UNITS);
	unsigned long tcfg;

	if (units < 4ULL) {
		units = 4ULL;
	}

	tcfg = (unsigned long)(units & LOONGARCH_TCFG_INITVAL_MASK) |
	       LOONGARCH_TCFG_EN;

	if (periodic) {
		tcfg |= LOONGARCH_TCFG_PERIODIC;
	}

	/*
	 * Stop the counter before reprogramming it.
	 *
	 * u-boot leaves the constant timer running with En set and a period of
	 * its own, and on the 2K0300 a TCFG write while En is set is ignored:
	 * measured on the board, TVAL kept counting down from 0xff... (tens of
	 * seconds) after this driver had written its 10 ms period, so the first
	 * tick arrived minutes after boot and every timeout in the system hung.
	 * Linux clears En first as well - its clockevents shutdown callback runs
	 * before the periodic one.
	 */
	loongarch_csr_write(loongarch_csr_read(LOONGARCH_CSR_TCFG) & ~LOONGARCH_TCFG_EN,
			    LOONGARCH_CSR_TCFG);

	loongarch_csr_write(tcfg, LOONGARCH_CSR_TCFG);
}

/*
 * Measure how many rdtime cycles one InitVal unit takes.
 *
 * No interrupt can be delivered while this runs: ECFG.LIE[11] is only set
 * after the driver is initialized, so the pending flag stays pending until
 * TICLR is written. The budget is half a second of rdtime, far beyond any
 * plausible countdown for TIMER_CAL_UNITS.
 */
#define TIMER_CAL_UNITS (1ULL << 20)
#define TIMER_CAL_BUDGET ((uint64_t)sys_clock_hw_cycles_per_sec() / 2)

static void timer_calibrate(void)
{
	uint64_t start;
	uint64_t delta;
	uint64_t a;
	uint64_t b;
	unsigned int key = arch_irq_lock();

	/* Stop the counter and drop whatever u-boot left pending */
	loongarch_csr_write(loongarch_csr_read(LOONGARCH_CSR_TCFG) & ~LOONGARCH_TCFG_EN,
			    LOONGARCH_CSR_TCFG);
	loongarch_csr_write(LOONGARCH_TICLR_CLR, LOONGARCH_CSR_TICLR);

	start = loongarch_rdtime();
	loongarch_csr_write((unsigned long)(TIMER_CAL_UNITS & LOONGARCH_TCFG_INITVAL_MASK) |
				    LOONGARCH_TCFG_EN,
			    LOONGARCH_CSR_TCFG);

	while ((loongarch_csr_read(LOONGARCH_CSR_ESTAT) &
		LOONGARCH_ESTAT_IS(LOONGARCH_IRQ_TIMER)) == 0U) {
		if ((loongarch_rdtime() - start) > TIMER_CAL_BUDGET) {
			LOG_WRN("the pending flag never came, keeping 1 unit = 1 cycle");
			loongarch_csr_write(LOONGARCH_TICLR_CLR, LOONGARCH_CSR_TICLR);
			arch_irq_unlock(key);
			return;
		}
	}

	delta = loongarch_rdtime() - start;
	loongarch_csr_write(LOONGARCH_TICLR_CLR, LOONGARCH_CSR_TICLR);

	if (delta != 0U) {
		/* Reduce TIMER_CAL_UNITS / delta so the conversion stays small */
		a = TIMER_CAL_UNITS;
		b = delta;

		while (b != 0U) {
			uint64_t t = a % b;

			a = b;
			b = t;
		}

		units_num = (uint32_t)(TIMER_CAL_UNITS / a);
		units_den = (uint32_t)(delta / a);
	}

	LOG_DBG("%llu units = %llu rdtime cycles (1 unit = %u/%u cycles)",
		(unsigned long long)TIMER_CAL_UNITS, (unsigned long long)delta,
		units_den, units_num);

	arch_irq_unlock(key);
}

static void timer_isr(const void *arg)
{
	ARG_UNUSED(arg);

	k_spinlock_key_t key = k_spin_lock(&lock);
	uint64_t now = loongarch_rdtime();
	uint32_t dticks = (uint32_t)((now - last_count) / CYC_PER_TICK);

	/* Clear the timer interrupt first */
	loongarch_csr_write(LOONGARCH_TICLR_CLR, LOONGARCH_CSR_TICLR);

	last_count += (uint64_t)dticks * CYC_PER_TICK;

	k_spin_unlock(&lock, key);

	sys_clock_announce(TICKLESS ? (int32_t)dticks : 1);
}

void sys_clock_set_timeout(int32_t ticks, bool idle)
{
	ARG_UNUSED(idle);

	if (!TICKLESS) {
		return;
	}

	ticks = (ticks == K_TICKS_FOREVER) ? (int32_t)MAX_TICKS : ticks;
	ticks = CLAMP(ticks - 1, 0, (int32_t)MAX_TICKS);

	k_spinlock_key_t key = k_spin_lock(&lock);
	uint64_t now = loongarch_rdtime();
	uint64_t elapsed = now - last_count;
	uint64_t delay = (uint64_t)ticks * CYC_PER_TICK + elapsed + (CYC_PER_TICK - 1);

	/* Round up to the next tick boundary */
	delay = (delay / CYC_PER_TICK) * CYC_PER_TICK;
	delay -= elapsed;

	if (delay < MIN_DELAY) {
		delay += CYC_PER_TICK;
	}

	timer_program(delay, false);
	k_spin_unlock(&lock, key);
}

uint32_t sys_clock_elapsed(void)
{
	if (!TICKLESS) {
		return 0;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t ticks = (uint32_t)((loongarch_rdtime() - last_count) / CYC_PER_TICK);

	k_spin_unlock(&lock, key);

	return ticks;
}

uint64_t sys_clock_cycle_get_64(void)
{
	return loongarch_rdtime();
}

uint32_t sys_clock_cycle_get_32(void)
{
	return (uint32_t)loongarch_rdtime();
}

static int sys_clock_driver_init(void)
{
	IRQ_CONNECT(LOONGARCH_IRQ_TIMER, 0, timer_isr, NULL, 0);

	/* Learn what one InitVal unit is worth, then arm the first tick */
	timer_calibrate();

	loongarch_csr_write(LOONGARCH_TICLR_CLR, LOONGARCH_CSR_TICLR);

	last_count = loongarch_rdtime();

	/* Periodic mode is enough when the kernel is not tickless */
	timer_program(CYC_PER_TICK, !TICKLESS);

	irq_enable(LOONGARCH_IRQ_TIMER);

	return 0;
}

SYS_INIT(sys_clock_driver_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
