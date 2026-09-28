/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch public exception handling.
 */

#ifndef ZEPHYR_INCLUDE_ARCH_LOONGARCH_EXCEPTION_H_
#define ZEPHYR_INCLUDE_ARCH_LOONGARCH_EXCEPTION_H_

#ifndef _ASMLANGUAGE
#include <zephyr/types.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Exception stack frame. The general purpose registers are stored at their
 * architectural index (regs[$rN]) so that assembly can address them with a
 * simple offset, see the ESF_* defines used by core/isr.S.
 */
struct arch_esf {
	unsigned long regs[32];		/* $r0 - $r31 */

	unsigned long orig_a0;		/* Original $r4 (reserved for syscalls) */

	unsigned long csr_era;
	unsigned long csr_badv;
	unsigned long csr_prmd;
	unsigned long csr_estat;
	unsigned long csr_crmd;
	unsigned long csr_ecfg;
	unsigned long csr_euen;
} __aligned(16);

#ifdef __cplusplus
}
#endif

#endif /* _ASMLANGUAGE */

#endif /* ZEPHYR_INCLUDE_ARCH_LOONGARCH_EXCEPTION_H_ */
