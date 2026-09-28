/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch CPU idle support.
 */

#include <zephyr/irq.h>
#include <zephyr/tracing/tracing.h>
#include <loongarch/csr.h>

#ifndef CONFIG_ARCH_HAS_CUSTOM_CPU_IDLE
void arch_cpu_idle(void)
{
#if defined(CONFIG_TRACING)
	sys_trace_idle();
#endif

	/*
	 * The kernel calls arch_cpu_idle() with interrupts masked and expects
	 * them to be unmasked on return (see kernel/idle.c). The IDLE
	 * instruction only halts the core until an interrupt is taken, so
	 * CRMD.IE has to be set before issuing it.
	 */
	loongarch_csrxchg(LOONGARCH_CRMD_IE, LOONGARCH_CRMD_IE, LOONGARCH_CSR_CRMD);
	loongarch_idle();

#if defined(CONFIG_TRACING)
	sys_trace_idle_exit();
#endif
}
#endif /* CONFIG_ARCH_HAS_CUSTOM_CPU_IDLE */

#ifndef CONFIG_ARCH_HAS_CUSTOM_CPU_ATOMIC_IDLE
void arch_cpu_atomic_idle(unsigned int key)
{
#if defined(CONFIG_TRACING)
	sys_trace_idle();
#endif

	/* Enabling interrupts and entering the low power state, then restore
	 * the interrupt lockout state described by 'key'.
	 */
	arch_irq_unlock(key);
	loongarch_idle();

	if (key == 0U) {
		(void)arch_irq_lock();
	}

#if defined(CONFIG_TRACING)
	sys_trace_idle_exit();
#endif
}
#endif /* CONFIG_ARCH_HAS_CUSTOM_CPU_ATOMIC_IDLE */
