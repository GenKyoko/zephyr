/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch context switch.
 *
 * void z_loongarch_switch(struct k_thread *new, struct k_thread *old)
 *
 * Only the registers the LoongArch ABI defines as callee-saved are stored,
 * mirroring the Linux cpu_context layout: $ra, $sp, $fp and $s0-$s8.
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>
#include <offsets_short.h>

GTEXT(z_loongarch_switch)

SECTION_FUNC(TEXT, z_loongarch_switch)
	/* Save the outgoing thread's callee-saved registers ($a1 = old) */
	st.d		$r1, $a1, _thread_offset_to_ra
	st.d		$r22, $a1, _thread_offset_to_fp
	st.d		$r23, $a1, _thread_offset_to_s0
	st.d		$r24, $a1, _thread_offset_to_s1
	st.d		$r25, $a1, _thread_offset_to_s2
	st.d		$r26, $a1, _thread_offset_to_s3
	st.d		$r27, $a1, _thread_offset_to_s4
	st.d		$r28, $a1, _thread_offset_to_s5
	st.d		$r29, $a1, _thread_offset_to_s6
	st.d		$r30, $a1, _thread_offset_to_s7
	st.d		$r31, $a1, _thread_offset_to_s8
	st.d		$r3, $a1, _thread_offset_to_sp

	/* Mark the switch as complete: old->switch_handle = old */
	st.d		$a1, $a1, _thread_offset_to_switch_handle

	/* Restore the incoming thread ($a0 = new), $ra last */
	ld.d		$r22, $a0, _thread_offset_to_fp
	ld.d		$r23, $a0, _thread_offset_to_s0
	ld.d		$r24, $a0, _thread_offset_to_s1
	ld.d		$r25, $a0, _thread_offset_to_s2
	ld.d		$r26, $a0, _thread_offset_to_s3
	ld.d		$r27, $a0, _thread_offset_to_s4
	ld.d		$r28, $a0, _thread_offset_to_s5
	ld.d		$r29, $a0, _thread_offset_to_s6
	ld.d		$r30, $a0, _thread_offset_to_s7
	ld.d		$r31, $a0, _thread_offset_to_s8
	ld.d		$r3, $a0, _thread_offset_to_sp
	ld.d		$r1, $a0, _thread_offset_to_ra

	jr		$r1
