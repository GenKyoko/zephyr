/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch fault handling.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <loongarch/csr.h>

LOG_MODULE_DECLARE(os, CONFIG_KERNEL_LOG_LEVEL);

static const char *const ecode_str[] = {
	[LOONGARCH_ECODE_INT] = "interrupt",
	[LOONGARCH_ECODE_PIL] = "page invalid on load",
	[LOONGARCH_ECODE_PIS] = "page invalid on store",
	[LOONGARCH_ECODE_PIF] = "page invalid on fetch",
	[LOONGARCH_ECODE_PME] = "page modify",
	[LOONGARCH_ECODE_PNR] = "page not readable",
	[LOONGARCH_ECODE_PNX] = "page not executable",
	[LOONGARCH_ECODE_PPI] = "privilege level violation",
	[LOONGARCH_ECODE_ADE] = "address error",
	[LOONGARCH_ECODE_ALE] = "address alignment error",
	[LOONGARCH_ECODE_BCE] = "bound check error",
	[LOONGARCH_ECODE_SYS] = "system call",
	[LOONGARCH_ECODE_BRK] = "breakpoint",
	[LOONGARCH_ECODE_INE] = "instruction not exist",
	[LOONGARCH_ECODE_IPE] = "instruction privilege error",
	[LOONGARCH_ECODE_FPD] = "FPU disabled",
	[LOONGARCH_ECODE_SXD] = "LSX disabled",
	[LOONGARCH_ECODE_ASXD] = "LASX disabled",
	[LOONGARCH_ECODE_FPE] = "floating point error",
};

FUNC_NORETURN void z_loongarch_fatal_error(unsigned int reason,
					   const struct arch_esf *esf)
{
#ifdef CONFIG_EXCEPTION_DEBUG
	if (esf != NULL) {
		EXCEPTION_DUMP(" r0: %016lx  r1: %016lx  r2: %016lx  r3: %016lx\n",
			       esf->regs[0], esf->regs[1], esf->regs[2], esf->regs[3]);
		EXCEPTION_DUMP(" r4: %016lx  r5: %016lx  r6: %016lx  r7: %016lx\n",
			       esf->regs[4], esf->regs[5], esf->regs[6], esf->regs[7]);
		EXCEPTION_DUMP(" r8: %016lx  r9: %016lx r10: %016lx r11: %016lx\n",
			       esf->regs[8], esf->regs[9], esf->regs[10], esf->regs[11]);
		EXCEPTION_DUMP("r12: %016lx r13: %016lx r14: %016lx r15: %016lx\n",
			       esf->regs[12], esf->regs[13], esf->regs[14], esf->regs[15]);
		EXCEPTION_DUMP("r16: %016lx r17: %016lx r18: %016lx r19: %016lx\n",
			       esf->regs[16], esf->regs[17], esf->regs[18], esf->regs[19]);
		EXCEPTION_DUMP("r20: %016lx r21: %016lx r22: %016lx r23: %016lx\n",
			       esf->regs[20], esf->regs[21], esf->regs[22], esf->regs[23]);
		EXCEPTION_DUMP("r24: %016lx r25: %016lx r26: %016lx r27: %016lx\n",
			       esf->regs[24], esf->regs[25], esf->regs[26], esf->regs[27]);
		EXCEPTION_DUMP("r28: %016lx r29: %016lx r30: %016lx r31: %016lx\n",
			       esf->regs[28], esf->regs[29], esf->regs[30], esf->regs[31]);
		EXCEPTION_DUMP("ERA: %016lx BADV: %016lx\n",
			       esf->csr_era, esf->csr_badv);
		EXCEPTION_DUMP("ESTAT: %016lx PRMD: %016lx\n",
			       esf->csr_estat, esf->csr_prmd);
	}
#endif /* CONFIG_EXCEPTION_DEBUG */

	z_fatal_error(reason, esf);
	CODE_UNREACHABLE;
}

FUNC_NORETURN void z_loongarch_fault(struct arch_esf *esf)
{
	unsigned int ecode = (unsigned int)((esf->csr_estat & LOONGARCH_ESTAT_ECODE_MASK) >>
					    LOONGARCH_ESTAT_ECODE_SHIFT);
	const char *name = (ecode < ARRAY_SIZE(ecode_str)) ? ecode_str[ecode] : NULL;

	if (name != NULL) {
		EXCEPTION_DUMP("Ecode: %u (%s)\n", ecode, name);
	} else {
		EXCEPTION_DUMP("Ecode: %u\n", ecode);
	}

	z_loongarch_fatal_error(K_ERR_CPU_EXCEPTION, esf);
}
