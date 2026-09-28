/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch interrupt management.
 *
 * LoongArch has 13 line interrupts per core and ECFG.LIE[n] maps 1:1 to
 * ESTAT.IS[n], so the Zephyr IRQ number is simply the IS bit index.
 */

#include <zephyr/kernel.h>
#include <kernel_internal.h>
#include <kswap.h>
#include <zephyr/logging/log.h>
#include <zephyr/tracing/tracing.h>
#include <loongarch/csr.h>

LOG_MODULE_DECLARE(os, CONFIG_KERNEL_LOG_LEVEL);

/*
 * Called from the exception entry with int_vec = ESTAT.IS & ECFG.LIE.
 *
 * Returns non-zero when the outermost handler is leaving, i.e. when the
 * caller has to run the rescheduling check.
 */
int z_loongarch_enter_irq(unsigned int int_vec)
{
	_current_cpu->nested++;

	while (int_vec != 0U) {
		unsigned int irq = (unsigned int)find_lsb_set(int_vec) - 1U;
		const struct _isr_table_entry *entry = &_sw_isr_table[irq];

		/*
		 * ESTAT.IS[1:0] are software interrupts: unlike the line
		 * interrupts they are not cleared by the hardware, they have
		 * to be acknowledged by writing a 0. Do it before dispatching
		 * the handler so that a re-trigger inside the handler is not
		 * lost.
		 */
		if (irq <= LOONGARCH_IRQ_SWI1) {
			loongarch_csrxchg(0UL, LOONGARCH_ESTAT_IS(irq), LOONGARCH_CSR_ESTAT);
		}

		if (IS_ENABLED(CONFIG_TRACING_ISR)) {
			sys_trace_isr_enter();
		}

		entry->isr(entry->arg);

		if (IS_ENABLED(CONFIG_TRACING_ISR)) {
			sys_trace_isr_exit();
		}

		int_vec &= ~BIT(irq);
	}

	_current_cpu->nested--;

	if (IS_ENABLED(CONFIG_STACK_SENTINEL)) {
		z_check_stack_sentinel();
	}

	return _current_cpu->nested == 0U;
}

void arch_irq_enable(unsigned int irq)
{
	unsigned int key = arch_irq_lock();

	loongarch_csrxchg(LOONGARCH_ECFG_LIE(irq), LOONGARCH_ECFG_LIE(irq),
			  LOONGARCH_CSR_ECFG);
	arch_irq_unlock(key);
}

void arch_irq_disable(unsigned int irq)
{
	unsigned int key = arch_irq_lock();

	loongarch_csrxchg(0UL, LOONGARCH_ECFG_LIE(irq), LOONGARCH_CSR_ECFG);
	arch_irq_unlock(key);
}

int arch_irq_is_enabled(unsigned int irq)
{
	return (loongarch_csr_read(LOONGARCH_CSR_ECFG) & LOONGARCH_ECFG_LIE(irq)) != 0U;
}

FUNC_NORETURN void z_irq_spurious(const void *unused)
{
	ARG_UNUSED(unused);

	LOG_ERR("Spurious interrupt detected");

	z_loongarch_fatal_error(K_ERR_SPURIOUS_IRQ, NULL);
}

#ifdef CONFIG_DYNAMIC_INTERRUPTS
int arch_irq_connect_dynamic(unsigned int irq, unsigned int priority,
			     void (*routine)(const void *parameter),
			     const void *parameter, uint32_t flags)
{
	ARG_UNUSED(priority);
	ARG_UNUSED(flags);

	z_isr_install(irq, routine, parameter);

	return (int)irq;
}
#endif /* CONFIG_DYNAMIC_INTERRUPTS */
