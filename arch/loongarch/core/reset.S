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
	 * Address translation. CRMD ends up with PLV=0, IE=0.
	 *
	 * LOONGARCH_BOOT_DMW_MODE programs the direct map windows the way the
	 * Linux kernel does on this SoC and only then enables paging:
	 *
	 *   0x8000_0000_0000_0000  MMIO, strongly ordered uncached  (DMW0)
	 *   0x9000_0000_0000_0000  RAM,  coherent cached            (DMW1)
	 *
	 * The 2K0300 u-boot hands over with CRMD=0, i.e. PG=0 and no usable
	 * window: in that state every access, the instruction fetch included,
	 * is resolved through the TLB, so the kernel quietly runs on the boot
	 * loader's page tables. The first page u-boot never mapped - the GPIO
	 * block shares no page with the console UART, for instance - then
	 * raises a TLB refill exception that lands in u-boot's handler and the
	 * board dies without a single line on the console.
	 *
	 * Programming the windows first makes the whole image, its stacks and
	 * every device tree address reachable without a single TLB entry. It
	 * is also what keeps MMIO uncached: with DATF/DATM alone a peripheral
	 * write sits in a cache line (it reads back, but the device never sees
	 * it) and status registers freeze at stale values.
	 *
	 * Otherwise enable direct address translation with coherent cached
	 * DATF/DATM, which is what the QEMU 'virt' machine is happy with.
	 */
#ifdef CONFIG_LOONGARCH_BOOT_DMW_MODE
	li.d		$r12, LOONGARCH_DMW0_INIT
	csrwr		$r12, LOONGARCH_CSR_DMW0
	li.d		$r12, LOONGARCH_DMW1_INIT
	csrwr		$r12, LOONGARCH_CSR_DMW1
	li.d		$r12, (LOONGARCH_CRMD_PG | LOONGARCH_CRMD_DATF_CC | \
			       LOONGARCH_CRMD_DATM_CC)
	csrwr		$r12, LOONGARCH_CSR_CRMD
#else
	li.d		$r12, (LOONGARCH_CRMD_DA | LOONGARCH_CRMD_DATF_CC | \
			       LOONGARCH_CRMD_DATM_CC)
	csrwr		$r12, LOONGARCH_CSR_CRMD
#endif

	/* Make the CSR programming above visible before the next fetch */
	dbar		0
	ibar		0

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
