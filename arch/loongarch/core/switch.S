/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch context switch.
 *
 * void z_loongarch_switch(struct k_thread *new, struct k_thread *old)
 *
 * Only the registers the LoongArch ABI defines as callee-saved are stored,
 * mirroring the Linux cpu_context layout: $ra, $sp, $fp and $s0-$s8.
 *
 * With CONFIG_FPU_SHARING the floating point registers, fcsr0 and the
 * condition flags fcc0-fcc7 are preserved per thread as well, so that
 * multiple threads may use the FPU concurrently. The save/restore is done on
 * every switch; a lazy scheme driven by the FPD (EUEN.FPE) exception could
 * be added later to avoid the cost for threads that never touch the FPU.
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

#if defined(CONFIG_FPU_SHARING)
	/*
	 * Floating point context of the outgoing thread. $r12/$r13 are
	 * caller-saved and free to use here.
	 */
	addi.d		$r12, $a1, _thread_offset_to_fp_ctx

	fst.d		$f0, $r12, 0
	fst.d		$f1, $r12, 8
	fst.d		$f2, $r12, 16
	fst.d		$f3, $r12, 24
	fst.d		$f4, $r12, 32
	fst.d		$f5, $r12, 40
	fst.d		$f6, $r12, 48
	fst.d		$f7, $r12, 56
	fst.d		$f8, $r12, 64
	fst.d		$f9, $r12, 72
	fst.d		$f10, $r12, 80
	fst.d		$f11, $r12, 88
	fst.d		$f12, $r12, 96
	fst.d		$f13, $r12, 104
	fst.d		$f14, $r12, 112
	fst.d		$f15, $r12, 120
	fst.d		$f16, $r12, 128
	fst.d		$f17, $r12, 136
	fst.d		$f18, $r12, 144
	fst.d		$f19, $r12, 152
	fst.d		$f20, $r12, 160
	fst.d		$f21, $r12, 168
	fst.d		$f22, $r12, 176
	fst.d		$f23, $r12, 184
	fst.d		$f24, $r12, 192
	fst.d		$f25, $r12, 200
	fst.d		$f26, $r12, 208
	fst.d		$f27, $r12, 216
	fst.d		$f28, $r12, 224
	fst.d		$f29, $r12, 232
	fst.d		$f30, $r12, 240
	fst.d		$f31, $r12, 248

	movcf2gr	$r13, $fcc0
	st.b		$r13, $r12, _fp_ctx_offset_to_fcc + 0
	movcf2gr	$r13, $fcc1
	st.b		$r13, $r12, _fp_ctx_offset_to_fcc + 1
	movcf2gr	$r13, $fcc2
	st.b		$r13, $r12, _fp_ctx_offset_to_fcc + 2
	movcf2gr	$r13, $fcc3
	st.b		$r13, $r12, _fp_ctx_offset_to_fcc + 3
	movcf2gr	$r13, $fcc4
	st.b		$r13, $r12, _fp_ctx_offset_to_fcc + 4
	movcf2gr	$r13, $fcc5
	st.b		$r13, $r12, _fp_ctx_offset_to_fcc + 5
	movcf2gr	$r13, $fcc6
	st.b		$r13, $r12, _fp_ctx_offset_to_fcc + 6
	movcf2gr	$r13, $fcc7
	st.b		$r13, $r12, _fp_ctx_offset_to_fcc + 7

	movfcsr2gr	$r13, $fcsr0
	st.w		$r13, $r12, _fp_ctx_offset_to_fcsr
#endif /* CONFIG_FPU_SHARING */

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

#if defined(CONFIG_FPU_SHARING)
	/* Floating point context of the incoming thread */
	addi.d		$r12, $a0, _thread_offset_to_fp_ctx

	fld.d		$f0, $r12, 0
	fld.d		$f1, $r12, 8
	fld.d		$f2, $r12, 16
	fld.d		$f3, $r12, 24
	fld.d		$f4, $r12, 32
	fld.d		$f5, $r12, 40
	fld.d		$f6, $r12, 48
	fld.d		$f7, $r12, 56
	fld.d		$f8, $r12, 64
	fld.d		$f9, $r12, 72
	fld.d		$f10, $r12, 80
	fld.d		$f11, $r12, 88
	fld.d		$f12, $r12, 96
	fld.d		$f13, $r12, 104
	fld.d		$f14, $r12, 112
	fld.d		$f15, $r12, 120
	fld.d		$f16, $r12, 128
	fld.d		$f17, $r12, 136
	fld.d		$f18, $r12, 144
	fld.d		$f19, $r12, 152
	fld.d		$f20, $r12, 160
	fld.d		$f21, $r12, 168
	fld.d		$f22, $r12, 176
	fld.d		$f23, $r12, 184
	fld.d		$f24, $r12, 192
	fld.d		$f25, $r12, 200
	fld.d		$f26, $r12, 208
	fld.d		$f27, $r12, 216
	fld.d		$f28, $r12, 224
	fld.d		$f29, $r12, 232
	fld.d		$f30, $r12, 240
	fld.d		$f31, $r12, 248

	ld.bu		$r13, $r12, _fp_ctx_offset_to_fcc + 0
	movgr2cf	$fcc0, $r13
	ld.bu		$r13, $r12, _fp_ctx_offset_to_fcc + 1
	movgr2cf	$fcc1, $r13
	ld.bu		$r13, $r12, _fp_ctx_offset_to_fcc + 2
	movgr2cf	$fcc2, $r13
	ld.bu		$r13, $r12, _fp_ctx_offset_to_fcc + 3
	movgr2cf	$fcc3, $r13
	ld.bu		$r13, $r12, _fp_ctx_offset_to_fcc + 4
	movgr2cf	$fcc4, $r13
	ld.bu		$r13, $r12, _fp_ctx_offset_to_fcc + 5
	movgr2cf	$fcc5, $r13
	ld.bu		$r13, $r12, _fp_ctx_offset_to_fcc + 6
	movgr2cf	$fcc6, $r13
	ld.bu		$r13, $r12, _fp_ctx_offset_to_fcc + 7
	movgr2cf	$fcc7, $r13

	ld.w		$r13, $r12, _fp_ctx_offset_to_fcsr
	movgr2fcsr	$fcsr0, $r13
#endif /* CONFIG_FPU_SHARING */

	ld.d		$r1, $a0, _thread_offset_to_ra

	jr		$r1
