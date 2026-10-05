/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Per-arch thread definitions for LoongArch.
 */

#ifndef ZEPHYR_INCLUDE_ARCH_LOONGARCH_THREAD_H_
#define ZEPHYR_INCLUDE_ARCH_LOONGARCH_THREAD_H_

#ifndef _ASMLANGUAGE
#include <zephyr/types.h>

/*
 * Registers which are preserved across a function call according to the
 * LoongArch ABI: $ra, $sp, $fp and $s0-$s8.
 */
struct _callee_saved {
	unsigned long sp;	/* $r3  */
	unsigned long ra;	/* $r1  */
	unsigned long fp;	/* $r22 */
	unsigned long s0;	/* $r23 */
	unsigned long s1;	/* $r24 */
	unsigned long s2;	/* $r25 */
	unsigned long s3;	/* $r26 */
	unsigned long s4;	/* $r27 */
	unsigned long s5;	/* $r28 */
	unsigned long s6;	/* $r29 */
	unsigned long s7;	/* $r30 */
	unsigned long s8;	/* $r31 */
};
typedef struct _callee_saved _callee_saved_t;

#if defined(CONFIG_FPU_SHARING)
/*
 * Floating point context, saved and restored by z_loongarch_switch() when
 * CONFIG_FPU_SHARING is enabled: the 32 floating point registers, the
 * control/status register fcsr0 (rounding mode, exception enables and
 * accumulated flags) and the eight condition flag registers fcc0-fcc7.
 *
 * The condition flags have to be part of the context as well, since a
 * floating point comparison and the conditional branch consuming its result
 * may be separated by a context switch.
 */
struct z_loongarch_fp_context {
	unsigned long f[32];	/* $f0   - $f31 */
	unsigned char fcc[8];	/* $fcc0 - $fcc7, one byte each */
	unsigned int fcsr;	/* $fcsr0 */
};

struct _thread_arch {
	struct z_loongarch_fp_context fp_ctx;
};
#else
struct _thread_arch {
	/* Keep the structure non-empty for C/C++ size compatibility. */
	uint8_t unused;
};
#endif /* CONFIG_FPU_SHARING */

typedef struct _thread_arch _thread_arch_t;

#endif /* _ASMLANGUAGE */

#endif /* ZEPHYR_INCLUDE_ARCH_LOONGARCH_THREAD_H_ */
