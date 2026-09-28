/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch IRQ offload.
 *
 * A software interrupt is used to run a routine in interrupt context. The
 * pending state of ESTAT.IS[1:0] is owned by software: it is raised by
 * writing a 1 and acknowledged by writing a 0 (LoongArch reference manual,
 * sections 6.1.2 and 7.4.6), so z_loongarch_enter_irq() clears the line
 * before dispatching the handler and no explicit end-of-interrupt is needed
 * here.
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <kernel_internal.h>
#include <zephyr/irq.h>
#include <zephyr/irq_offload.h>
#include <loongarch/csr.h>

static volatile irq_offload_routine_t offload_routine;
static volatile const void *offload_param;

/*
 * Called from z_loongarch_enter_irq().
 *
 * The offload routine pointer is cleared before it is invoked so that a
 * fault inside the offloaded code does not cause it to be re-run by the
 * fault handler.
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

void arch_irq_offload(irq_offload_routine_t routine, const void *parameter)
{
	unsigned int key = arch_irq_lock();

	offload_routine = routine;
	offload_param = parameter;

	/* Raise the software interrupt; it is taken as soon as interrupts are
	 * enabled again below.
	 */
	loongarch_csrxchg(LOONGARCH_ESTAT_IS(CONFIG_LOONGARCH_IRQ_OFFLOAD_IRQ),
			  LOONGARCH_ESTAT_IS(CONFIG_LOONGARCH_IRQ_OFFLOAD_IRQ),
			  LOONGARCH_CSR_ESTAT);

	arch_irq_unlock(key);
}

static void irq_offload_isr(const void *arg)
{
	ARG_UNUSED(arg);

	z_irq_do_offload();
}

void arch_irq_offload_init(void)
{
	IRQ_CONNECT(CONFIG_LOONGARCH_IRQ_OFFLOAD_IRQ, 0, irq_offload_isr, NULL, 0);
	irq_enable(CONFIG_LOONGARCH_IRQ_OFFLOAD_IRQ);
}
