/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson EIOINTC (extended I/O interrupt controller), "EXTIOI" in the 2K0300
 * user manual. It is the successor of the LIOINTC: instead of aggregating the
 * peripheral lines onto a handful of LIOINTC lines, it gives every peripheral
 * its own interrupt vector (128 of them on the 2K0300, table 3-48 of the user
 * manual) and forwards the whole vector set to one LoArch CPU interrupt line
 * (INT0..INT3, i.e. a LoongArch IP line driven through ESTAT.IS/ECFG.LIE).
 *
 * The vectors live in the SoC wide flat IRQ space above the 16 CPU lines, see
 * the "zephyr,irq-base" property and arch/loongarch/core/irq_manage.c. The
 * device tree carries that flat number, so vector N of this controller is
 * zephyr,irq-base + N.
 *
 * Register layout (offsets relative to the system register window the node
 * describes):
 *
 *   0x0100   chip general configuration register 0
 *              bit 19 extioi_en: 1 = extended interrupts are live as well
 *   0x14c0   EXTIOI_MAP: routing of the four groups of 32 vectors, one 4 bit
 *              one hot field per group at bits [3:0], [11:8], [19:16], [27:24]
 *              (0001 = INT0 ... 1000 = INT3)
 *   0x1600   EXTIOI_IEN0..3: enable of vectors 0..127, bit per vector
 *   0x1640   EXTIOI_POL0..3: level polarity (1 = inverted, "active low")
 *   0x1700   EXTIOI_ISR0..3: raw device status, not gated by the enable mask
 *   0x1800   EXTIOI_CORE_ISR0..3: status of the vectors routed to this core,
 *              i.e. the ones that may be dispatched, cleared by writing 1s
 *              (CORE_EXTICLR shares the same addresses)
 *
 * The controller has no per-vector edge/level selector: the trigger type is a
 * property of the source, and the only thing software configures besides the
 * enable mask is the polarity of level sources.
 */

#define DT_DRV_COMPAT loongson_ls2k0300_eiointc

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/sw_isr_table.h>

#define EIOINTC_VEC_COUNT 128U
#define EIOINTC_WORDS     (EIOINTC_VEC_COUNT / 32U)

#define EIOINTC_REG_CFG0       0x0100U
#define EIOINTC_REG_MAP        0x14c0U
#define EIOINTC_REG_ENABLE     0x1600U
#define EIOINTC_REG_POL        0x1640U
#define EIOINTC_REG_ISR        0x1700U
#define EIOINTC_REG_CORE_ISR   0x1800U

/* INT0 is delivered on CPU line 2 (ESTAT.IS[2]), INT1 on line 3 and so on */
#define EIOINTC_INT_LINE_BASE  2U
#define EIOINTC_INT_LINE_COUNT 4U

/* One hot map field per group of 32 vectors, all groups to the same line */
#define EIOINTC_MAP_WORD(line) ((uint32_t)BIT((line) - EIOINTC_INT_LINE_BASE) * 0x01010101U)

struct eiointc_config {
	mem_addr_t base;
	unsigned int irq_base;
	unsigned int parent_irq;
	unsigned int enable_bit;
	uint32_t map;
	struct z_loongarch_sub_intc sub;
};

static mem_addr_t eiointc_word(const struct eiointc_config *cfg, uint32_t reg,
			       unsigned int vector)
{
	return cfg->base + reg + ((mem_addr_t)(vector / 32U) << 2);
}

static void eiointc_irq_enable(const void *ctx, unsigned int bit)
{
	const struct eiointc_config *cfg = ctx;
	mem_addr_t reg = eiointc_word(cfg, EIOINTC_REG_ENABLE, bit);

	sys_write32(sys_read32(reg) | BIT(bit % 32U), reg);
}

static void eiointc_irq_disable(const void *ctx, unsigned int bit)
{
	const struct eiointc_config *cfg = ctx;
	mem_addr_t reg = eiointc_word(cfg, EIOINTC_REG_ENABLE, bit);

	sys_write32(sys_read32(reg) & ~BIT(bit % 32U), reg);
}

static int eiointc_irq_is_enabled(const void *ctx, unsigned int bit)
{
	const struct eiointc_config *cfg = ctx;

	return (int)((sys_read32(eiointc_word(cfg, EIOINTC_REG_ENABLE, bit)) >>
		      (bit % 32U)) & 1U);
}

static const struct z_loongarch_sub_intc_ops eiointc_ops = {
	.enable = eiointc_irq_enable,
	.disable = eiointc_irq_disable,
	.is_enabled = eiointc_irq_is_enabled,
};

/*
 * Dispatch the vectors that reached this core. The handlers are installed in
 * the static software ISR table by the device drivers through IRQ_CONNECT().
 *
 * The routed status is cleared before the child runs: a source that is still
 * asserting re-latches its bit right away, and one that was a pulse has been
 * latched by the controller until now, so no request is lost either way.
 */
static void eiointc_dispatch(const struct eiointc_config *cfg)
{
	for (unsigned int w = 0; w < EIOINTC_WORDS; w++) {
		mem_addr_t reg = cfg->base + EIOINTC_REG_CORE_ISR + (w * 4U);
		uint32_t pending = sys_read32(reg);

		if (pending == 0U) {
			continue;
		}

		sys_write32(pending, reg);

		while (pending != 0U) {
			unsigned int bit = (unsigned int)find_lsb_set(pending) - 1U;
			unsigned int irq = cfg->irq_base + (w * 32U) + bit;

			if (irq < (unsigned int)CONFIG_NUM_IRQS) {
				const struct _isr_table_entry *entry = &_sw_isr_table[irq];

				if ((entry->isr != NULL) && (entry->isr != z_irq_spurious)) {
					entry->isr(entry->arg);
				}
			}

			pending &= ~BIT(bit);
		}
	}
}

/*
 * At init the extended interrupt path is turned on, every vector is masked and
 * reset to level, active high, the stale routed status is dropped, all four
 * vector groups are sent to the CPU line the node cascades on, and finally the
 * controller is registered and that CPU line unmasked.
 *
 * Turning the path on ("extioi_en" of the chip general configuration register)
 * is left to this driver because neither u-boot nor a Linux boot leaves it
 * enabled on this SoC. It does not disturb the legacy path: the manual states
 * that both stay live at the same time, so a 2K0300 port could put a device
 * back on a LIOINTC line - this port does not, it models the EIOINTC only.
 */
#define EIOINTC_INIT(n)								\
	BUILD_ASSERT(DT_INST_IRQN(n) >= EIOINTC_INT_LINE_BASE &&		\
			     DT_INST_IRQN(n) < EIOINTC_INT_LINE_BASE +		\
						       EIOINTC_INT_LINE_COUNT,	\
		     "EIOINTC can only be cascaded on INT0..INT3 (CPU lines 2..5)"); \
										\
	static void eiointc_isr_##n(const void *arg)				\
	{									\
		eiointc_dispatch((const struct eiointc_config *)arg);		\
	}									\
										\
	static const struct eiointc_config eiointc_cfg_##n = {			\
		.base = DT_INST_REG_ADDR(n),					\
		.irq_base = DT_INST_PROP(n, zephyr_irq_base),			\
		.parent_irq = DT_INST_IRQN(n),					\
		.enable_bit = DT_INST_PROP(n, loongson_extioi_enable_bit),	\
		.map = EIOINTC_MAP_WORD(DT_INST_IRQN(n)),			\
		.sub = {							\
			.base = DT_INST_PROP(n, zephyr_irq_base),		\
			.count = EIOINTC_VEC_COUNT,				\
			.ops = &eiointc_ops,					\
			.ctx = &eiointc_cfg_##n,				\
		},								\
	};									\
										\
	static int eiointc_init_##n(void)					\
	{									\
		const struct eiointc_config *cfg = &eiointc_cfg_##n;		\
		mem_addr_t cfg0 = cfg->base + EIOINTC_REG_CFG0;			\
										\
		sys_write32(sys_read32(cfg0) | BIT(cfg->enable_bit), cfg0);	\
										\
		for (unsigned int w = 0; w < EIOINTC_WORDS; w++) {		\
			mem_addr_t core_isr = cfg->base + EIOINTC_REG_CORE_ISR;	\
										\
			sys_write32(0U, cfg->base + EIOINTC_REG_ENABLE + (w * 4U)); \
			sys_write32(0U, cfg->base + EIOINTC_REG_POL + (w * 4U)); \
			sys_write32(0xffffffffU, core_isr + (w * 4U));		\
		}								\
										\
		sys_write32(cfg->map, cfg->base + EIOINTC_REG_MAP);		\
										\
		z_loongarch_sub_intc_register(&cfg->sub);			\
										\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),		\
			    eiointc_isr_##n, &eiointc_cfg_##n, 0);		\
										\
		/* Unmask the parent CPU line */				\
		irq_enable(cfg->parent_irq);					\
										\
		return 0;							\
	}									\
										\
	SYS_INIT(eiointc_init_##n, PRE_KERNEL_1, CONFIG_INTC_INIT_PRIORITY);

DT_INST_FOREACH_STATUS_OKAY(EIOINTC_INIT)
