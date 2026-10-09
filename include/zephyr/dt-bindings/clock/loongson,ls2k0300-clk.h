/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clock IDs of the Loongson 2K0300 clock controller
 * (compatible "loongson,ls2k0300-clk").
 *
 * The numbering matches mainline Linux
 * (include/dt-bindings/clock/loongson2k300-clock.h) so that device trees can
 * be shared between the two.
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_CLOCK_LOONGSON_LS2K0300_CLK_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_CLOCK_LOONGSON_LS2K0300_CLK_H_

#define LS2K0300_CLK_REF	0	/* reference oscillator */
#define LS2K0300_CLK_NODE	1	/* NODE PLL output */
#define LS2K0300_CLK_CPU	2
#define LS2K0300_CLK_SCACHE	3
#define LS2K0300_CLK_IODMA	4
#define LS2K0300_CLK_GMAC	5
#define LS2K0300_CLK_I2S	6
#define LS2K0300_CLK_DDR_P	7	/* DDR PLL output */
#define LS2K0300_CLK_DDR	8
#define LS2K0300_CLK_NET	9
#define LS2K0300_CLK_DEVS	10	/* the group the peripherals sit on */
#define LS2K0300_CLK_USB	11
#define LS2K0300_CLK_APB	12	/* the APB bus clock */
#define LS2K0300_CLK_BOOT	13
#define LS2K0300_CLK_SDIO	14
#define LS2K0300_CLK_PIX_P	15	/* PIX PLL output */
#define LS2K0300_CLK_PIX	16
#define LS2K0300_CLK_GMACBP	17

/* Everything on the APB bus is clocked by LS2K0300_CLK_APB */
#define LS2K0300_CLK_UART	LS2K0300_CLK_APB
#define LS2K0300_CLK_CAN	LS2K0300_CLK_APB
#define LS2K0300_CLK_I2C	LS2K0300_CLK_APB
#define LS2K0300_CLK_SPI	LS2K0300_CLK_APB
#define LS2K0300_CLK_AC97	LS2K0300_CLK_APB
#define LS2K0300_CLK_NAND	LS2K0300_CLK_APB
#define LS2K0300_CLK_PWM	LS2K0300_CLK_APB
#define LS2K0300_CLK_HPET	LS2K0300_CLK_APB
#define LS2K0300_CLK_RTC	LS2K0300_CLK_APB

#define LS2K0300_CLK_CNT	18

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_CLOCK_LOONGSON_LS2K0300_CLK_H_ */
