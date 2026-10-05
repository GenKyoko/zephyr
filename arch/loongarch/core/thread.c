/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch thread creation.
 */

#include <zephyr/kernel.h>
#include <string.h>
#include <loongarch/csr.h>

extern void z_loongarch_thread_start(void);

void z_thread_entry(k_thread_entry_t thread, void *arg1, void *arg2, void *arg3);

void arch_new_thread(struct k_thread *thread, k_thread_stack_t *stack,
		     char *stack_ptr, k_thread_entry_t entry,
		     void *p1, void *p2, void *p3)
{
	struct arch_esf *stack_init;

	/* Initial exception stack frame, sitting at the top of the stack */
	stack_init = (struct arch_esf *)Z_STACK_PTR_ALIGN(
				Z_STACK_PTR_TO_FRAME(struct arch_esf, stack_ptr));

	memset(stack_init, 0, sizeof(struct arch_esf));

	/* Entry point and arguments ($a0-$a3), consumed by z_thread_entry */
	stack_init->regs[4] = (unsigned long)entry;
	stack_init->regs[5] = (unsigned long)p1;
	stack_init->regs[6] = (unsigned long)p2;
	stack_init->regs[7] = (unsigned long)p3;
	stack_init->orig_a0 = (unsigned long)entry;

	/* $sp the thread sees once the initial frame has been popped */
	stack_init->regs[3] = (unsigned long)stack_ptr;

	/*
	 * Like the RISC-V and MIPS ports, a new thread is born through the
	 * exception return path: z_loongarch_thread_start is the shared
	 * exception exit, so ERA selects the entry point and PRMD carries the
	 * interrupt state. With PIE set, ertn enables interrupts before the
	 * thread entry function runs.
	 */
	stack_init->csr_era = (unsigned long)z_thread_entry;
	stack_init->csr_prmd = LOONGARCH_PRMD_PIE;

#if defined(CONFIG_FPU_SHARING)
	/*
	 * A new thread starts with an empty floating point context: all
	 * registers zero, no exception flags and the default rounding mode.
	 * z_loongarch_switch() restores it on the first switch to the thread.
	 */
	memset(&thread->arch.fp_ctx, 0, sizeof(thread->arch.fp_ctx));
#endif /* CONFIG_FPU_SHARING */

	thread->callee_saved.sp = (unsigned long)stack_init;

	/* Where z_loongarch_switch() returns to for a brand new thread */
	thread->callee_saved.ra = (unsigned long)z_loongarch_thread_start;

	/* The switch handle is the thread pointer itself */
	thread->switch_handle = thread;
}

int arch_coprocessors_disable(struct k_thread *thread)
{
	ARG_UNUSED(thread);

	return -ENOTSUP;
}
