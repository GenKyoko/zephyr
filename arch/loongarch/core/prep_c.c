/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Full C support initialization for LoongArch: zero .bss, copy initialized
 * data and hand over to the kernel.
 */

#include <stddef.h>
#include <zephyr/toolchain.h>
#include <zephyr/kernel_structs.h>
#include <zephyr/platform/hooks.h>
#include <zephyr/arch/common/xip.h>
#include <zephyr/arch/common/init.h>

FUNC_NORETURN void z_prep_c(void)
{
	soc_prep_hook();

	arch_bss_zero();
	arch_data_copy();

	z_cstart();
	CODE_UNREACHABLE;
}
