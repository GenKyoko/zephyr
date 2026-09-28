/*
 * Copyright (c) 2026 Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch constant frequency timer driver.
 *
 * The timer is a per-core countdown counter (CSR.TCFG/CSR.TVAL) that raises
 * the TI line interrupt (ESTAT.IS[11]) when it reaches zero; CSR.TICLR
 * clears the pending interrupt. A separate stable counter, read with
 * rdtime.d, is used as the free-running cycle source.
 */

#include <limits.h>

#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/util.h>
#include <loongarch/csr.h>

#define CYC_PER_TICK ((uint64_t)sys_clock_hw_cycles_per_sec() / \
		      (uint64_t)CONFIG_SYS_CLOCK_TICKS_PER_SEC)

/*
 * TCFG.InitVal occupies bits [n-1:2], keep well below the architectural
 * maximum so that the conversion below never overflows.
 */
#define MAX_CYC (1ULL << 45)
#define MAX_TICKS ((uint32_t)((MAX_CYC - CYC_PER_TICK) / CYC_PER_TICK))
#define MIN_DELAY 1000

#define TICKLESS IS_ENABLED(CONFIG_TICKLESS_KERNEL)

static struct k_spinlock lock;
static uint64_t last_count;

/* Program a countdown of 'delta' timer cycles ('delta' must be >= 4) */
static ALWAYS_INLINE void timer_program(uint64_t delta, bool periodic)
{
	unsigned long tcfg = (unsigned long)(delta & LOONGARCH_TCFG_INITVAL_MASK) |
			     LOONGARCH_TCFG_EN;

	if (periodic) {
		tcfg |= LOONGARCH_TCFG_PERIODIC;
	}

	loongarch_csr_write(tcfg, LOONGARCH_CSR_TCFG);
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

	loongarch_csr_write(LOONGARCH_TICLR_CLR, LOONGARCH_CSR_TICLR);

	last_count = loongarch_rdtime();

	/* Periodic mode is enough when the kernel is not tickless */
	timer_program(CYC_PER_TICK, !TICKLESS);

	irq_enable(LOONGARCH_IRQ_TIMER);

	return 0;
}

SYS_INIT(sys_clock_driver_init, PRE_KERNEL_2, CONFIG_SYSTEM_CLOCK_INIT_PRIORITY);
