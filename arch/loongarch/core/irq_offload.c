/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch IRQ offload.
 *
 * The offloaded routine has to run synchronously, in exception context. That
 * is done by trapping through BREAK, which the CPU takes immediately: the
 * software interrupts (ESTAT.IS[1:0]) are only reported to the interrupt
 * logic and - at least under emulation - are not guaranteed to be taken as
 * soon as CRMD.IE is set. The MIPS port relies on the same idea using the
 * syscall instruction.
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <kernel_internal.h>
#include <kswap.h>
#include <zephyr/arch/exception.h>
#include <zephyr/irq_offload.h>

static volatile irq_offload_routine_t offload_routine;
static volatile const void *offload_param;

/*
 * Called from the BREAK exception entry, see core/isr.S.
 *
 * The routine pointer is cleared before it is invoked so that a fault inside
 * the offloaded code does not cause it to be re-run by the fault handler.
 */
void z_irq_do_offload(void)
{
	irq_offload_routine_t tmp;

	if (offload_routine == NULL) {
		return;
	}

	tmp = offload_routine;
	offload_routine = NULL;

	tmp((const void *)offload_param);
}

/*
 * Exception entry helper, returns non-zero when the outermost exception
 * handler returned, i.e. when the caller has to run the rescheduling check.
 */
int z_loongarch_enter_offload(struct arch_esf *esf)
{
	/* ERA holds the address of the BREAK instruction itself (manual
	 * section 6.3.3): skip it so that ertn resumes right after the trap.
	 */
	esf->csr_era += 4;

	/* The routine runs in interrupt context: keep the nesting counter in
	 * sync so that arch_is_in_isr() reports it.
	 */
	_current_cpu->nested++;

	z_irq_do_offload();

	_current_cpu->nested--;

	if (IS_ENABLED(CONFIG_STACK_SENTINEL)) {
		z_check_stack_sentinel();
	}

	return _current_cpu->nested == 0U;
}

void arch_irq_offload(irq_offload_routine_t routine, const void *parameter)
{
	unsigned int key = arch_irq_lock();

	offload_routine = routine;
	offload_param = parameter;

	/* Synchronously trap into exception context */
	__asm__ volatile("break 0");

	arch_irq_unlock(key);
}

void arch_irq_offload_init(void)
{
	/* Nothing to set up: BREAK needs no interrupt line or handler table
	 * entry, the exception entry dispatches it directly.
	 */
}
