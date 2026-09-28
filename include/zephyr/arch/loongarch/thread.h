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

struct _thread_arch {
	/* Keep the structure non-empty for C/C++ size compatibility. */
	uint8_t unused;
};
typedef struct _thread_arch _thread_arch_t;

#endif /* _ASMLANGUAGE */

#endif /* ZEPHYR_INCLUDE_ARCH_LOONGARCH_THREAD_H_ */
