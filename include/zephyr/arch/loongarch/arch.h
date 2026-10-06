/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoongArch specific kernel interface header.
 */

#ifndef ZEPHYR_INCLUDE_ARCH_LOONGARCH_ARCH_H_
#define ZEPHYR_INCLUDE_ARCH_LOONGARCH_ARCH_H_

#include <zephyr/arch/loongarch/thread.h>
#include <zephyr/arch/exception.h>
#include <zephyr/arch/common/sys_bitops.h>
#include <zephyr/arch/common/sys_io.h>
#include <zephyr/arch/common/ffs.h>
#include <zephyr/irq.h>
#include <zephyr/sw_isr_table.h>
#include <zephyr/devicetree.h>
#include <loongarch/csr.h>

/* LoongArch ABI requires 16-byte stack alignment */
#define ARCH_STACK_PTR_ALIGN 16

#define STACK_ROUND_UP(x) ROUND_UP(x, ARCH_STACK_PTR_ALIGN)

#ifndef _ASMLANGUAGE
#include <zephyr/sys/util.h>

#ifdef __cplusplus
extern "C" {
#endif

void arch_irq_enable(unsigned int irq);
void arch_irq_disable(unsigned int irq);
int arch_irq_is_enabled(unsigned int irq);

/* Number of CPU interrupt lines delivered through ESTAT.IS / ECFG.LIE */
#define LOONGARCH_CPU_IRQ_NUM 16

/*
 * Secondary (aggregated) interrupt controllers.
 *
 * SoCs such as the Loongson 2K0300 route most peripheral interrupts through a
 * secondary controller (EIOINTC / LIOINTC) that is itself wired to a CPU line.
 * The children of such a controller occupy the flat IRQ space above the CPU
 * lines and register themselves here so that irq_enable()/irq_disable() reach
 * the secondary controller instead of ECFG.LIE.
 */
struct z_loongarch_sub_intc_ops {
	void (*enable)(const void *ctx, unsigned int bit);
	void (*disable)(const void *ctx, unsigned int bit);
	int (*is_enabled)(const void *ctx, unsigned int bit);
};

struct z_loongarch_sub_intc {
	/** First flat IRQ number handled by this controller */
	unsigned int base;
	/** Number of IRQ lines of this controller */
	unsigned int count;
	const struct z_loongarch_sub_intc_ops *ops;
	/** Opaque pointer passed back to the ops */
	const void *ctx;
};

/**
 * @brief Register a secondary interrupt controller
 *
 * @param intc Controller description, must have static storage duration
 *
 * @return 0 on success, negative errno otherwise
 */
int z_loongarch_sub_intc_register(const struct z_loongarch_sub_intc *intc);

/**
 * Configure a static interrupt.
 *
 * All arguments must be computable by the compiler at build time.
 */
#define ARCH_IRQ_CONNECT(irq_p, priority_p, isr_p, isr_param_p, flags_p) \
	{ Z_ISR_DECLARE(irq_p, 0, isr_p, isr_param_p); }

static ALWAYS_INLINE unsigned int arch_irq_lock(void)
{
	unsigned long crmd = loongarch_csr_read(LOONGARCH_CSR_CRMD);
	unsigned long key = crmd & LOONGARCH_CRMD_IE;

	if (key != 0UL) {
		loongarch_csrxchg(0UL, LOONGARCH_CRMD_IE, LOONGARCH_CSR_CRMD);
	}

	/* Non-zero means interrupts were enabled before locking */
	return (unsigned int)key;
}

static ALWAYS_INLINE void arch_irq_unlock(unsigned int key)
{
	loongarch_csrxchg(key != 0U ? LOONGARCH_CRMD_IE : 0UL, LOONGARCH_CRMD_IE,
			  LOONGARCH_CSR_CRMD);
}

static ALWAYS_INLINE bool arch_irq_unlocked(unsigned int key)
{
	return key != 0U;
}

static ALWAYS_INLINE void arch_nop(void)
{
	__asm__ volatile("nop");
}

static ALWAYS_INLINE uint32_t arch_proc_id(void)
{
	return (uint32_t)loongarch_csr_read(LOONGARCH_CSR_CPUID);
}

extern uint32_t sys_clock_cycle_get_32(void);
extern uint64_t sys_clock_cycle_get_64(void);

static inline uint32_t arch_k_cycle_get_32(void)
{
	return sys_clock_cycle_get_32();
}

static inline uint64_t arch_k_cycle_get_64(void)
{
	return sys_clock_cycle_get_64();
}

#ifdef __cplusplus
}
#endif
#endif /* _ASMLANGUAGE */

#endif /* ZEPHYR_INCLUDE_ARCH_LOONGARCH_ARCH_H_ */
