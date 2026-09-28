/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch early boot / reset sequence.
 *
 * The CPU may enter this code either from the architectural reset vector or,
 * as it is the case on QEMU's 'virt' machine, from a loader that sets the PC
 * to the ELF entry point. In both cases the reference manual only guarantees
 * the reset values listed in section 6.4, so all state we rely on has to be
 * programmed explicitly here.
 */

#include <zephyr/toolchain.h>
#include <zephyr/linker/sections.h>
#include <loongarch/csr.h>

GTEXT(__start)
GTEXT(z_prep_c)

SECTION_FUNC(TEXT, __start)
	/*
	 * Program the exception entry page. EENTRY only keeps VPN[63:12],
	 * therefore z_loongarch_exception_entry has to be 4 KiB aligned
	 * (guaranteed by core/isr.S and the linker script).
	 */
	la.local	$r12, z_loongarch_exception_entry
	csrwr		$r12, LOONGARCH_CSR_EENTRY

	/* ECFG: VS=0 (single entry), all local interrupts disabled */
	csrwr		$r0, LOONGARCH_CSR_ECFG

	/*
	 * CRMD: PLV=0, IE=0, DA=1, PG=0 and DATF/DATM=0b01 (coherent cached).
	 * The reset value of DATF/DATM is 0 which would make every access
	 * strongly-ordered uncached.
	 */
	li.d		$r12, (LOONGARCH_CRMD_DA | LOONGARCH_CRMD_DATF_CC | \
			       LOONGARCH_CRMD_DATM_CC)
	csrwr		$r12, LOONGARCH_CSR_CRMD

	/* Stop the constant timer and clear a possibly pending TI */
	csrwr		$r0, LOONGARCH_CSR_TCFG
	li.d		$r12, LOONGARCH_TICLR_CLR
	csrwr		$r12, LOONGARCH_CSR_TICLR

#ifdef CONFIG_FPU
	/* EUEN.FPE=1, otherwise FP instructions raise FPD (0xf) */
	li.d		$r12, LOONGARCH_EUEN_FPE
	csrwr		$r12, LOONGARCH_CSR_EUEN
#endif

#ifdef CONFIG_LOONGARCH_LSX
	li.d		$r12, (LOONGARCH_EUEN_FPE | LOONGARCH_EUEN_SXE)
	csrwr		$r12, LOONGARCH_CSR_EUEN
#endif

	/* Initial stack, provided by the linker script */
	la.local	$r3, __stack

	/* Zero BSS, copy data and call z_cstart() */
	bl		z_prep_c

1:
	b		1b
