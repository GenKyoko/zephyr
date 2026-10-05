/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch exception / interrupt entry and exit.
 *
 * ECFG.VS is kept at 0 so that all ordinary exceptions share a single entry
 * point; the exception type is decoded from ESTAT.Ecode (0 means interrupt,
 * see the LoongArch reference manual sections 6.1.4 and 6.3.1).
 *
 * The entry code borrows CSR.SAVE0/SAVE1 to stash $r12/$r13 before a stack
 * frame exists, exactly like the Linux kernel does with KS0/KS1.
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>
#include <offsets_short.h>
#include <loongarch/csr.h>

GTEXT(z_loongarch_exception_entry)
GTEXT(z_loongarch_thread_start)
GTEXT(z_loongarch_enter_irq)
#if defined(CONFIG_IRQ_OFFLOAD)
GTEXT(z_loongarch_enter_offload)
#endif
GTEXT(z_loongarch_fault)
GTEXT(z_get_next_switch_handle)
GTEXT(z_loongarch_switch)

	.section .exception.entry, "ax"
	.balign 4096

GTEXT(z_loongarch_exception_entry)
z_loongarch_exception_entry:
	/* Stash $r12/$r13 (they are clobbered by csrwr) in CSR.SAVE0/SAVE1 */
	csrwr		$r12, LOONGARCH_CSR_SAVE(0)
	csrwr		$r13, LOONGARCH_CSR_SAVE(1)

	/* Build struct arch_esf on the current stack */
	addi.d		$sp, $sp, -__struct_arch_esf_SIZEOF

	st.d		$r0, $sp, _esf_reg_offset(0)
	st.d		$r1, $sp, _esf_reg_offset(1)
	st.d		$r2, $sp, _esf_reg_offset(2)
	st.d		$r4, $sp, _esf_reg_offset(4)
	st.d		$r5, $sp, _esf_reg_offset(5)
	st.d		$r6, $sp, _esf_reg_offset(6)
	st.d		$r7, $sp, _esf_reg_offset(7)
	st.d		$r8, $sp, _esf_reg_offset(8)
	st.d		$r9, $sp, _esf_reg_offset(9)
	st.d		$r10, $sp, _esf_reg_offset(10)
	st.d		$r11, $sp, _esf_reg_offset(11)
	st.d		$r14, $sp, _esf_reg_offset(14)
	st.d		$r15, $sp, _esf_reg_offset(15)
	st.d		$r16, $sp, _esf_reg_offset(16)
	st.d		$r17, $sp, _esf_reg_offset(17)
	st.d		$r18, $sp, _esf_reg_offset(18)
	st.d		$r19, $sp, _esf_reg_offset(19)
	st.d		$r20, $sp, _esf_reg_offset(20)
	st.d		$r21, $sp, _esf_reg_offset(21)
	st.d		$r22, $sp, _esf_reg_offset(22)
	st.d		$r23, $sp, _esf_reg_offset(23)
	st.d		$r24, $sp, _esf_reg_offset(24)
	st.d		$r25, $sp, _esf_reg_offset(25)
	st.d		$r26, $sp, _esf_reg_offset(26)
	st.d		$r27, $sp, _esf_reg_offset(27)
	st.d		$r28, $sp, _esf_reg_offset(28)
	st.d		$r29, $sp, _esf_reg_offset(29)
	st.d		$r30, $sp, _esf_reg_offset(30)
	st.d		$r31, $sp, _esf_reg_offset(31)

	/* Restore $r12/$r13 with their original values and save them */
	csrrd		$r12, LOONGARCH_CSR_SAVE(0)
	st.d		$r12, $sp, _esf_reg_offset(12)
	csrrd		$r13, LOONGARCH_CSR_SAVE(1)
	st.d		$r13, $sp, _esf_reg_offset(13)

	/* Save the interrupted $sp ($r3) */
	addi.d		$r12, $sp, __struct_arch_esf_SIZEOF
	st.d		$r12, $sp, _esf_reg_offset(3)

	/* Keep a copy of $a0 for potential syscall handling */
	st.d		$r4, $sp, __struct_arch_esf_orig_a0_OFFSET

	/* Save the CSR state */
	csrrd		$r12, LOONGARCH_CSR_ERA
	st.d		$r12, $sp, __struct_arch_esf_csr_era_OFFSET
	csrrd		$r12, LOONGARCH_CSR_BADV
	st.d		$r12, $sp, __struct_arch_esf_csr_badv_OFFSET
	csrrd		$r12, LOONGARCH_CSR_PRMD
	st.d		$r12, $sp, __struct_arch_esf_csr_prmd_OFFSET
	csrrd		$r12, LOONGARCH_CSR_ESTAT
	st.d		$r12, $sp, __struct_arch_esf_csr_estat_OFFSET

	/* Ecode == 0 means interrupt (ECFG.VS == 0) */
	bstrpick.d	$r13, $r12, 21, 16
	beqz		$r13, .Linterrupt

#if defined(CONFIG_IRQ_OFFLOAD)
	/* irq_offload() traps through BREAK to run in exception context */
	li.w		$r14, LOONGARCH_ECODE_BRK
	beq		$r13, $r14, .Loffload
#endif

	/*
	 * Line interrupts can also be delivered as direct (vector) interrupts,
	 * where the exception code is 0x40 + line number instead of 0 (INT0 ..
	 * INT12, Ecode 0x40..0x4C, see the exception code table). Those are
	 * ordinary line interrupts and must be dispatched by line, not sent to
	 * the fault handler. ESTAT.IS is not necessarily set in that case, so
	 * build the bit mask from the exception code.
	 */
	li.w		$r14, 0x40
	bltu		$r13, $r14, .Lexception
	li.w		$r14, 0x4c
	bltu		$r14, $r13, .Lexception

	addi.d		$a0, $r13, -0x40
	li.w		$r12, 1
	sll.d		$a0, $r12, $a0
	bl		z_loongarch_enter_irq
	bnez		$a0, .Lcheck_reschedule
	b		.Lno_reschedule

.Lexception:
	move		$a0, $sp
	bl		z_loongarch_fault
	/* z_loongarch_fault() does not return */
	b		.Lno_reschedule

#if defined(CONFIG_IRQ_OFFLOAD)
.Loffload:
	move		$a0, $sp
	bl		z_loongarch_enter_offload
	bnez		$a0, .Lcheck_reschedule
	b		.Lno_reschedule
#endif

.Linterrupt:
	/* int_vec = ESTAT.IS & ECFG.LIE */
	csrrd		$r12, LOONGARCH_CSR_ESTAT
	csrrd		$r13, LOONGARCH_CSR_ECFG
	and		$r12, $r12, $r13
	bstrpick.d	$a0, $r12, 12, 0
	bl		z_loongarch_enter_irq

	/* Non-zero: the outermost interrupt handler returned and a
	 * rescheduling decision has to be taken.
	 */
	bnez		$a0, .Lcheck_reschedule
	b		.Lno_reschedule

.Lcheck_reschedule:
#if defined(CONFIG_MULTITHREADING)
	/* old = &_kernel.cpus[0].current */
	la.local	$r12, _kernel
	ld.d		$a1, $r12, _kernel_offset_to_current

	addi.d		$sp, $sp, -16
	st.d		$a1, $sp, 0
	move		$a0, $r0
	bl		z_get_next_switch_handle
	ld.d		$a1, $sp, 0
	addi.d		$sp, $sp, 16

	beqz		$a0, .Lno_reschedule

	/* a0 = new thread, a1 = old thread */
	bl		z_loongarch_switch
#endif /* CONFIG_MULTITHREADING */

.Lno_reschedule:
	/*
	 * A brand new thread starts here as well: arch_new_thread() points
	 * callee_saved.ra at this label and callee_saved.sp at the initial
	 * struct arch_esf, so the thread "returns" from an exception whose ERA
	 * is z_thread_entry and whose PRMD has PIE set, which makes ertn
	 * enable interrupts before the entry function runs.
	 */
GTEXT(z_loongarch_thread_start)
z_loongarch_thread_start:
	/* Restore ERA/PRMD (needed when interrupts were nested) */
	ld.d		$r12, $sp, __struct_arch_esf_csr_era_OFFSET
	csrwr		$r12, LOONGARCH_CSR_ERA
	ld.d		$r12, $sp, __struct_arch_esf_csr_prmd_OFFSET
	csrwr		$r12, LOONGARCH_CSR_PRMD

	/* Restore the general purpose registers, $sp last */
	ld.d		$r1, $sp, _esf_reg_offset(1)
	ld.d		$r2, $sp, _esf_reg_offset(2)
	ld.d		$r4, $sp, _esf_reg_offset(4)
	ld.d		$r5, $sp, _esf_reg_offset(5)
	ld.d		$r6, $sp, _esf_reg_offset(6)
	ld.d		$r7, $sp, _esf_reg_offset(7)
	ld.d		$r8, $sp, _esf_reg_offset(8)
	ld.d		$r9, $sp, _esf_reg_offset(9)
	ld.d		$r10, $sp, _esf_reg_offset(10)
	ld.d		$r11, $sp, _esf_reg_offset(11)
	ld.d		$r12, $sp, _esf_reg_offset(12)
	ld.d		$r13, $sp, _esf_reg_offset(13)
	ld.d		$r14, $sp, _esf_reg_offset(14)
	ld.d		$r15, $sp, _esf_reg_offset(15)
	ld.d		$r16, $sp, _esf_reg_offset(16)
	ld.d		$r17, $sp, _esf_reg_offset(17)
	ld.d		$r18, $sp, _esf_reg_offset(18)
	ld.d		$r19, $sp, _esf_reg_offset(19)
	ld.d		$r20, $sp, _esf_reg_offset(20)
	ld.d		$r21, $sp, _esf_reg_offset(21)
	ld.d		$r22, $sp, _esf_reg_offset(22)
	ld.d		$r23, $sp, _esf_reg_offset(23)
	ld.d		$r24, $sp, _esf_reg_offset(24)
	ld.d		$r25, $sp, _esf_reg_offset(25)
	ld.d		$r26, $sp, _esf_reg_offset(26)
	ld.d		$r27, $sp, _esf_reg_offset(27)
	ld.d		$r28, $sp, _esf_reg_offset(28)
	ld.d		$r29, $sp, _esf_reg_offset(29)
	ld.d		$r30, $sp, _esf_reg_offset(30)
	ld.d		$r31, $sp, _esf_reg_offset(31)
	ld.d		$r3, $sp, _esf_reg_offset(3)

	ertn
