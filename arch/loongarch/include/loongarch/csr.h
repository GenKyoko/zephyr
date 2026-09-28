/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch control status register (CSR) definitions.
 *
 * Register numbers and field layout follow the LoongArch Reference Manual,
 * Volume 1 "Basic Architecture", section 7 "Control Status Registers".
 */

#ifndef ZEPHYR_ARCH_LOONGARCH_INCLUDE_LOONGARCH_CSR_H_
#define ZEPHYR_ARCH_LOONGARCH_INCLUDE_LOONGARCH_CSR_H_

/* CSR numbers (csr_num, 14-bit immediate) */
#define LOONGARCH_CSR_CRMD		0x0	/* Current mode info */
#define LOONGARCH_CSR_PRMD		0x1	/* Previous exception mode info */
#define LOONGARCH_CSR_EUEN		0x2	/* Extended unit enable */
#define LOONGARCH_CSR_MISC		0x3	/* Misc config */
#define LOONGARCH_CSR_ECFG		0x4	/* Exception config */
#define LOONGARCH_CSR_ESTAT		0x5	/* Exception status */
#define LOONGARCH_CSR_ERA		0x6	/* Exception return address */
#define LOONGARCH_CSR_BADV		0x7	/* Bad virtual address */
#define LOONGARCH_CSR_BADI		0x8	/* Bad instruction */
#define LOONGARCH_CSR_EENTRY		0xc	/* Exception entry address */
#define LOONGARCH_CSR_CPUID		0x20	/* CPU core id */
#define LOONGARCH_CSR_PRCFG1		0x21	/* Config1 */
#define LOONGARCH_CSR_PRCFG2		0x22	/* Config2 */
#define LOONGARCH_CSR_PRCFG3		0x23	/* Config3 */
/* Scratch registers for system software (at least one is implemented) */
#define LOONGARCH_CSR_SAVE(n)		(0x30 + (n))
#define LOONGARCH_CSR_TID		0x40	/* Timer id */
#define LOONGARCH_CSR_TCFG		0x41	/* Timer config */
#define LOONGARCH_CSR_TVAL		0x42	/* Timer ticks remain */
#define LOONGARCH_CSR_CNTC		0x43	/* Timer compensation */
#define LOONGARCH_CSR_TICLR		0x44	/* Timer interrupt clear */
#define LOONGARCH_CSR_LLBCTL		0x60	/* LLBit control */

/* CPUCFG configuration word indexes */
#define LOONGARCH_CPUCFG0		0x0
#define LOONGARCH_CPUCFG1		0x1
#define LOONGARCH_CPUCFG2		0x2
#define LOONGARCH_CPUCFG4		0x4
#define LOONGARCH_CPUCFG5		0x5
#define LOONGARCH_CPUCFG16		0x10
#define LOONGARCH_CPUCFG17		0x11

/* CPUCFG1 */
#define LOONGARCH_CPUCFG1_ARCH_MASK	0x3
#define LOONGARCH_CPUCFG1_ARCH_LA64	0x2

/* CPUCFG2 */
#define LOONGARCH_CPUCFG2_FP		0x1
#define LOONGARCH_CPUCFG2_FP_SP		0x2
#define LOONGARCH_CPUCFG2_FP_DP		0x4
#define LOONGARCH_CPUCFG2_LSX		0x40
#define LOONGARCH_CPUCFG2_LASX		0x80
#define LOONGARCH_CPUCFG2_LLFTP		0x4000

/* CPUCFG4/CPUCFG5: constant timer frequency settings */
#define LOONGARCH_CPUCFG5_M(x)	((x) & 0xffff)
#define LOONGARCH_CPUCFG5_DIV(x)	(((x) >> 16) & 0xffff)

/* CRMD */
#define LOONGARCH_CRMD_PLV_MASK		0x3
#define LOONGARCH_CRMD_IE		0x4
#define LOONGARCH_CRMD_DA		0x8
#define LOONGARCH_CRMD_PG		0x10
#define LOONGARCH_CRMD_DATF_MASK	0x60
#define LOONGARCH_CRMD_DATF_CC		0x20	/* 0b01: coherent cached */
#define LOONGARCH_CRMD_DATM_MASK	0x180
#define LOONGARCH_CRMD_DATM_CC		0x80	/* 0b01: coherent cached */

/* PRMD: previous PLV/IE, restored into CRMD by ertn */
#define LOONGARCH_PRMD_PPLV_MASK	0x3
#define LOONGARCH_PRMD_PIE		0x4

/* EUEN */
#define LOONGARCH_EUEN_FPE		0x1
#define LOONGARCH_EUEN_SXE		0x2
#define LOONGARCH_EUEN_ASXE		0x4

/* ECFG */
#define LOONGARCH_ECFG_LIE_MASK		0x1fff
#define LOONGARCH_ECFG_LIE(n)		(1 << (n))
#define LOONGARCH_ECFG_VS_SHIFT		16
#define LOONGARCH_ECFG_VS_MASK		0x70000

/* ESTAT */
#define LOONGARCH_ESTAT_IS_MASK		0x1fff
#define LOONGARCH_ESTAT_IS(n)		(1 << (n))
#define LOONGARCH_ESTAT_ECODE_SHIFT	16
#define LOONGARCH_ESTAT_ECODE_MASK	0x3f0000
#define LOONGARCH_ESTAT_ESUBCODE_SHIFT	22
#define LOONGARCH_ESTAT_ESUBCODE_MASK	0x7fc00000

/* Exception codes (Ecode), manual table 7-8 */
#define LOONGARCH_ECODE_INT		0x0
#define LOONGARCH_ECODE_PIL		0x1
#define LOONGARCH_ECODE_PIS		0x2
#define LOONGARCH_ECODE_PIF		0x3
#define LOONGARCH_ECODE_PME		0x4
#define LOONGARCH_ECODE_PNR		0x5
#define LOONGARCH_ECODE_PNX		0x6
#define LOONGARCH_ECODE_PPI		0x7
#define LOONGARCH_ECODE_ADE		0x8
#define LOONGARCH_ECODE_ALE		0x9
#define LOONGARCH_ECODE_BCE		0xa
#define LOONGARCH_ECODE_SYS		0xb
#define LOONGARCH_ECODE_BRK		0xc
#define LOONGARCH_ECODE_INE		0xd
#define LOONGARCH_ECODE_IPE		0xe
#define LOONGARCH_ECODE_FPD		0xf
#define LOONGARCH_ECODE_SXD		0x10
#define LOONGARCH_ECODE_ASXD		0x11
#define LOONGARCH_ECODE_FPE		0x12

/* TCFG: the InitVal field starts at bit 2, hardware appends two zero bits */
#define LOONGARCH_TCFG_EN		0x1
#define LOONGARCH_TCFG_PERIODIC		0x2
#define LOONGARCH_TCFG_INITVAL_MASK	(~0x3)

/* TICLR: write 1 to clear the timer interrupt */
#define LOONGARCH_TICLR_CLR		0x1

/* Interrupt numbers (also ESTAT.IS bit positions) */
#define LOONGARCH_IRQ_SWI0		0
#define LOONGARCH_IRQ_SWI1		1
#define LOONGARCH_IRQ_HWI0		2
#define LOONGARCH_IRQ_HWI1		3
#define LOONGARCH_IRQ_HWI2		4
#define LOONGARCH_IRQ_HWI3		5
#define LOONGARCH_IRQ_HWI4		6
#define LOONGARCH_IRQ_HWI5		7
#define LOONGARCH_IRQ_HWI6		8
#define LOONGARCH_IRQ_HWI7		9
#define LOONGARCH_IRQ_PMI		10
#define LOONGARCH_IRQ_TIMER		11
#define LOONGARCH_IRQ_IPI		12
#define LOONGARCH_NUM_IRQS		13

#ifndef _ASMLANGUAGE

#include <zephyr/types.h>
#include <zephyr/toolchain.h>

static ALWAYS_INLINE unsigned long loongarch_csr_read(unsigned int reg)
{
	unsigned long val;

	__asm__ volatile("csrrd %0, %1" : "=r"(val) : "i"(reg));

	return val;
}

/* csrwr returns the previous CSR value, hence the read-write operand */
static ALWAYS_INLINE unsigned long loongarch_csr_write(unsigned long val, unsigned int reg)
{
	__asm__ volatile("csrwr %0, %1" : "+r"(val) : "i"(reg));

	return val;
}

static ALWAYS_INLINE unsigned long loongarch_csrxchg(unsigned long val, unsigned long mask,
						     unsigned int reg)
{
	__asm__ volatile("csrxchg %0, %1, %2" : "+r"(val) : "r"(mask), "i"(reg));

	return val;
}

static ALWAYS_INLINE unsigned int loongarch_cpucfg(unsigned int idx)
{
	unsigned int val;

	__asm__ volatile("cpucfg %0, %1" : "=r"(val) : "r"(idx));

	return val;
}

/* rdtime.d returns the stable counter in rd and the CounterID in rj */
static ALWAYS_INLINE uint64_t loongarch_rdtime(void)
{
	uint64_t counter;
	unsigned long id;

	__asm__ volatile("rdtime.d %0, %1" : "=r"(counter), "=r"(id));
	(void)id;

	return counter;
}

static ALWAYS_INLINE void loongarch_idle(void)
{
	__asm__ volatile("idle 0");
}

static ALWAYS_INLINE void loongarch_dbar(void)
{
	__asm__ volatile("dbar 0" ::: "memory");
}

static ALWAYS_INLINE void loongarch_ibar(void)
{
	__asm__ volatile("ibar 0" ::: "memory");
}

#endif /* !_ASMLANGUAGE */

#endif /* ZEPHYR_ARCH_LOONGARCH_INCLUDE_LOONGARCH_CSR_H_ */
