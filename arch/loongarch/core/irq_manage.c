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

/*
 * Secondary interrupt controllers (e.g. the Loongson EIOINTC) own the IRQ
 * numbers above the CPU lines. They register themselves at init so that the
 * generic irq_enable()/irq_disable() APIs are routed to them.
 */
#define LOONGARCH_NUM_SUB_INTC 4

static const struct z_loongarch_sub_intc *sub_intcs[LOONGARCH_NUM_SUB_INTC];

int z_loongarch_sub_intc_register(const struct z_loongarch_sub_intc *intc)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(sub_intcs); i++) {
		if (sub_intcs[i] == NULL) {
			sub_intcs[i] = intc;
			return 0;
		}
	}

	return -ENOMEM;
}

static const struct z_loongarch_sub_intc *sub_intc_lookup(unsigned int irq)
{
	for (unsigned int i = 0; i < ARRAY_SIZE(sub_intcs); i++) {
		const struct z_loongarch_sub_intc *intc = sub_intcs[i];

		if ((intc != NULL) && (irq >= intc->base) &&
		    (irq < (intc->base + intc->count))) {
			return intc;
		}
	}

	return NULL;
}

void arch_irq_enable(unsigned int irq)
{
	unsigned int key = arch_irq_lock();
	const struct z_loongarch_sub_intc *intc = sub_intc_lookup(irq);

	if (intc != NULL) {
		intc->ops->enable(intc->ctx, irq - intc->base);
	} else if (irq < LOONGARCH_CPU_IRQ_NUM) {
		loongarch_csrxchg(LOONGARCH_ECFG_LIE(irq), LOONGARCH_ECFG_LIE(irq),
				  LOONGARCH_CSR_ECFG);
	}

	arch_irq_unlock(key);
}

void arch_irq_disable(unsigned int irq)
{
	unsigned int key = arch_irq_lock();
	const struct z_loongarch_sub_intc *intc = sub_intc_lookup(irq);

	if (intc != NULL) {
		intc->ops->disable(intc->ctx, irq - intc->base);
	} else if (irq < LOONGARCH_CPU_IRQ_NUM) {
		loongarch_csrxchg(0UL, LOONGARCH_ECFG_LIE(irq), LOONGARCH_CSR_ECFG);
	}

	arch_irq_unlock(key);
}

int arch_irq_is_enabled(unsigned int irq)
{
	const struct z_loongarch_sub_intc *intc = sub_intc_lookup(irq);

	if (intc != NULL) {
		return intc->ops->is_enabled(intc->ctx, irq - intc->base);
	}

	if (irq >= LOONGARCH_CPU_IRQ_NUM) {
		return 0;
	}

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
