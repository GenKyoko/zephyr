/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch floating point support.
 *
 * Floating point is optional and split over two options:
 *
 *   CONFIG_FPU          - the hardware floating point unit is enabled in
 *                         EUEN.FPE by the reset code. Nothing is preserved
 *                         across context switches, so only a single thread
 *                         may use the FPU (unshared FP registers mode).
 *
 *   CONFIG_FPU_SHARING  - the floating point context (f0-f31, fcsr0 and
 *                         fcc0-fcc7) is additionally saved and restored per
 *                         thread by z_loongarch_switch(), so multiple threads
 *                         may use the FPU concurrently.
 *
 * In both modes interrupt service routines must not use floating point: the
 * exception entry does not save the floating point context.
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <kernel_internal.h>

#if defined(CONFIG_FPU) && defined(CONFIG_FPU_SHARING)

/*
 * This port always preserves the floating point context of every thread, so
 * the FP capability flag can only be used to track whether a thread declares
 * itself as a floating point user. Both calls are restricted to the current
 * thread and to thread context, like the ARM implementations.
 */
int arch_float_disable(struct k_thread *thread)
{
	unsigned int key;

	if ((thread != _current) || arch_is_in_isr()) {
		return -EINVAL;
	}

	key = arch_irq_lock();
	thread->base.user_options &= ~K_FP_REGS;
	arch_irq_unlock(key);

	return 0;
}

int arch_float_enable(struct k_thread *thread, unsigned int options)
{
	unsigned int key;

	ARG_UNUSED(options);

	if ((thread != _current) || arch_is_in_isr()) {
		return -EINVAL;
	}

	key = arch_irq_lock();
	thread->base.user_options |= K_FP_REGS;
	arch_irq_unlock(key);

	return 0;
}

#endif /* CONFIG_FPU && CONFIG_FPU_SHARING */
