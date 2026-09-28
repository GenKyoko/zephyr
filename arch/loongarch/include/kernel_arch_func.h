/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Private kernel function definitions for LoongArch.
 */

#ifndef ZEPHYR_ARCH_LOONGARCH_INCLUDE_KERNEL_ARCH_FUNC_H_
#define ZEPHYR_ARCH_LOONGARCH_INCLUDE_KERNEL_ARCH_FUNC_H_

#include <kernel_arch_data.h>

#include <zephyr/platform/hooks.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _ASMLANGUAGE

static ALWAYS_INLINE void arch_kernel_init(void)
{
	soc_per_core_init_hook();
}

#ifdef CONFIG_USE_SWITCH
static ALWAYS_INLINE void arch_switch(void *switch_to, void **switched_from)
{
	extern void z_loongarch_switch(struct k_thread *new, struct k_thread *old);
	struct k_thread *new = switch_to;
	struct k_thread *old = CONTAINER_OF(switched_from, struct k_thread,
					    switch_handle);

	z_loongarch_switch(new, old);
}
#endif /* CONFIG_USE_SWITCH */

FUNC_NORETURN void z_loongarch_fatal_error(unsigned int reason,
					   const struct arch_esf *esf);
FUNC_NORETURN void z_loongarch_fault(struct arch_esf *esf);

static inline bool arch_is_in_isr(void)
{
	return _current_cpu->nested != 0U;
}

#endif /* _ASMLANGUAGE */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_ARCH_LOONGARCH_INCLUDE_KERNEL_ARCH_FUNC_H_ */
