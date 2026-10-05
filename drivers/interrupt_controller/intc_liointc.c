/*
 * Copyright (c) 2026 Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson LIOINTC (Local I/O interrupt controller) v2.0 driver.
 *
 * The controller aggregates 32 peripheral interrupt lines and forwards them
 * to one LoongArch CPU interrupt line (IP2..IP7). The children live in the
 * SoC wide flat IRQ space above the 16 CPU lines, see the "zephyr,irq-base"
 * property and arch/loongarch/core/irq_manage.c.
 *
 * Register layout (offsets relative to the "main" region):
 *
 *   0x00 + i     routing byte of child i:
 *                  bits 7:4  parent line (1 << parent index)
 *                  bits 3:0  target core mask
 *   0x24         enable status (read only)
 *   0x28         enable  (write 1 to unmask)
 *   0x2c         disable (write 1 to mask)
 *   0x30         polarity (1 = active low / falling edge)
 *   0x34         trigger  (1 = edge, 0 = level)
 *
 * The pending status of each child is read from the separate "isr0" region.
 */

#define DT_DRV_COMPAT loongson_liointc_2_0

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sw_isr_table.h>

#define LIOINTC_NUM_IRQS     32U
#define LIOINTC_NUM_PARENTS  4U

#define LIOINTC_REG_ROUTE(i) (0x00U + (i))
#define LIOINTC_REG_EN_STATUS 0x24U
#define LIOINTC_REG_ENABLE    0x28U
#define LIOINTC_REG_DISABLE   0x2cU
#define LIOINTC_REG_POL       0x30U
#define LIOINTC_REG_EDGE      0x34U

#define LIOINTC_SHIFT_PARENT  4U

struct liointc_config {
	mem_addr_t base;
	mem_addr_t isr;
	unsigned int irq_base;
	unsigned int parent_irq;
	struct z_loongarch_sub_intc sub;
};

static void liointc_irq_enable(const void *ctx, unsigned int bit)
{
	const struct liointc_config *cfg = ctx;

	sys_write32(BIT(bit % 32U), cfg->base + LIOINTC_REG_ENABLE);
}

static void liointc_irq_disable(const void *ctx, unsigned int bit)
{
	const struct liointc_config *cfg = ctx;

	sys_write32(BIT(bit % 32U), cfg->base + LIOINTC_REG_DISABLE);
}

static int liointc_irq_is_enabled(const void *ctx, unsigned int bit)
{
	const struct liointc_config *cfg = ctx;

	return (int)((sys_read32(cfg->base + LIOINTC_REG_EN_STATUS) >> (bit % 32U)) & 1U);
}

static const struct z_loongarch_sub_intc_ops liointc_ops = {
	.enable = liointc_irq_enable,
	.disable = liointc_irq_disable,
	.is_enabled = liointc_irq_is_enabled,
};

/*
 * Dispatch the pending children. Child handlers are installed in the static
 * software ISR table by the device drivers through IRQ_CONNECT().
 */
static void liointc_dispatch(const struct liointc_config *cfg)
{
	uint32_t pending = sys_read32(cfg->isr);

	while (pending != 0U) {
		unsigned int bit = (unsigned int)find_lsb_set(pending) - 1U;
		unsigned int irq = cfg->irq_base + bit;

		if (irq < (unsigned int)CONFIG_NUM_IRQS) {
			const struct _isr_table_entry *entry = &_sw_isr_table[irq];

			if ((entry->isr != NULL) && (entry->isr != z_irq_spurious)) {
				entry->isr(entry->arg);
			}
		}

		pending &= ~BIT(bit);
	}
}

/*
 * Only level triggered, active high children are used on the 2K0300, so the
 * routing of every child is programmed to the parent line it is aggregated
 * to (from "loongson,parent-int-map") targeting core 0.
 */
static void liointc_set_route(const struct liointc_config *cfg,
			      const uint32_t *parent_map)
{
	for (unsigned int i = 0; i < LIOINTC_NUM_IRQS; i++) {
		uint32_t parent = 0U;

		for (unsigned int p = 0; p < LIOINTC_NUM_PARENTS; p++) {
			if ((parent_map[p] & BIT(i)) != 0U) {
				parent = p;
				break;
			}
		}

		sys_write8((uint8_t)(BIT(parent) << LIOINTC_SHIFT_PARENT) | 0x1U,
			   cfg->base + LIOINTC_REG_ROUTE(i));
	}
}

#define LIOINTC_INIT(n)								\
	static void liointc_isr_##n(const void *arg)				\
	{									\
		liointc_dispatch((const struct liointc_config *)arg);		\
	}									\
										\
	static const struct liointc_config liointc_cfg_##n = {			\
		.base = DT_INST_REG_ADDR_BY_NAME(n, main),			\
		.isr = DT_INST_REG_ADDR_BY_NAME(n, isr0),			\
		.irq_base = DT_INST_PROP(n, zephyr_irq_base),			\
		.parent_irq = DT_INST_IRQN(n),					\
		.sub = {							\
			.base = DT_INST_PROP(n, zephyr_irq_base),		\
			.count = LIOINTC_NUM_IRQS,				\
			.ops = &liointc_ops,					\
			.ctx = &liointc_cfg_##n,				\
		},								\
	};									\
										\
	static const uint32_t liointc_parent_map_##n[] =			\
		DT_INST_PROP(n, loongson_parent_int_bitmap);			\
										\
	static int liointc_init_##n(void)					\
	{									\
		const struct liointc_config *cfg = &liointc_cfg_##n;		\
										\
		/* Disable every child and default to level, active high */	\
		sys_write32(0xffffffffU, cfg->base + LIOINTC_REG_DISABLE);	\
		sys_write32(0U, cfg->base + LIOINTC_REG_EDGE);			\
		sys_write32(0U, cfg->base + LIOINTC_REG_POL);			\
		liointc_set_route(cfg, liointc_parent_map_##n);			\
										\
		z_loongarch_sub_intc_register(&cfg->sub);			\
										\
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority),		\
			    liointc_isr_##n, &liointc_cfg_##n, 0);		\
										\
		/* Unmask the parent CPU line */				\
		irq_enable(cfg->parent_irq);					\
										\
		return 0;							\
	}									\
										\
	SYS_INIT(liointc_init_##n, PRE_KERNEL_1, CONFIG_INTC_INIT_PRIORITY);

DT_INST_FOREACH_STATUS_OKAY(LIOINTC_INIT)
