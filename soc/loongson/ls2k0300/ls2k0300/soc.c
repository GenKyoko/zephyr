/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Loongson 2K0300 (LS2K300 family) early SoC bring-up.
 *
 *   0x1600_0400 + 0x24  PLL_CLKEN  device clock output enable
 *                               bit0 NODE0, bit1 BOOT1, bit2 USB2,
 *                               bit3 APB3, bit4 SDIO4, bit6 PIX6
 *
 * u-boot leaves 0x5b there, i.e. everything but the USB clock output enabled,
 * so the APB clock output is already on when Zephyr takes over. Adding the
 * gates here is belt and braces; the register is only ever ORed, which keeps
 * the USB clock output under u-boot's control.
 *
 * The module gates in the chip configuration registers (0x1600_0118
 * CHIP_CTRL06 clock gates, 0x1600_011c/0x1600_0120 CHIP_CTRL07/08 resets) are
 * deliberately not written here. Measured on the board they already read back
 * as "clocked, not held in reset", and the reset bits turned out to have the
 * opposite polarity of what the manual's reset column suggests, so writing
 * them blind is a good way to assert reset on the whole chip.
 *
 * Note for whoever finds older notes about this board: the peripherals only
 * behave once reset.S has programmed the direct map windows
 * (CONFIG_LOONGARCH_BOOT_DMW_MODE). With cached DATF/DATM and no window, every
 * MMIO access is shadowed by the D-cache - writes read back but never reach the
 * device and status registers freeze at stale values - which made the console
 * look as if the UART block were gated or held in reset.
 */

#include <loongarch/csr.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

/*
 * Compile the messages in at debug level and let the runtime level filter
 * them: the console guard below must not print by default, but when it repairs
 * something the record has to exist for whoever turns the level up.
 */
LOG_MODULE_REGISTER(ls2k0300_soc, LOG_LEVEL_DBG);

/* Uncached direct-mapped window the DT peripheral addresses live in */
#define LS2K_DIRECT 0x8000000000000000UL

/* Clock controller */
#define LS2K_CLK_BASE  (LS2K_DIRECT + 0x16000400UL)
#define LS2K_CLK_GATE  0x24U

#define LS2K_GATE_NODE BIT(0)
#define LS2K_GATE_BOOT BIT(1)
#define LS2K_GATE_APB  BIT(3)
#define LS2K_GATE_SDIO BIT(4)
#define LS2K_GATE_PIX  BIT(6)

static int ls2k0300_clock_init(void)
{
	mem_addr_t addr = LS2K_CLK_BASE + LS2K_CLK_GATE;
	uint32_t gates = LS2K_GATE_NODE | LS2K_GATE_BOOT | LS2K_GATE_APB |
			 LS2K_GATE_SDIO | LS2K_GATE_PIX;

	/* Only ever add gates: the USB clock output state is u-boot's business */
	sys_write32(sys_read32(addr) | gates, addr);

	/* Make sure the write reaches the block before drivers probe it */
	(void)sys_read32(addr);

	return 0;
}

SYS_INIT(ls2k0300_clock_init, PRE_KERNEL_1, 10);

/*
 * Console UART: one byte receive FIFO trigger level.
 *
 * Zephyr's ns16550 driver programs FCR with the 8 byte trigger level
 * (FCR_FIFO_8) in uart_ns16550_configure(). Above that threshold the receive
 * interrupt fires, below it the UART has to rely on the FIFO character timeout
 * - and that path does not produce interrupts on this UART. The board shows it
 * plainly: type a burst and it gets through, single keystrokes never do, and
 * with a character waiting LSR reports DR set while IIR still reports "no
 * interrupt pending", which is exactly a FIFO waiting for its trigger level.
 *
 * Linux' 8250 driver reaches for UART_FCR_TRIGGER_1 in exactly this situation
 * (serial8250_do_set_termios() does it for interactive, low baud ports) and a
 * console UART is always interactive, so program it here. This runs at
 * PRE_KERNEL_2, i.e. after the driver configured the port at PRE_KERNEL_1 and
 * before anything can receive a character.
 */
#define LS2K_UART_FCR_OFFSET 0x02U
#define LS2K_UART_FCR_1BYTE  0x07U	/* FIFO on, RX trigger 1 byte, clear both */
#define LS2K_UART_MCR_OFFSET 0x04U
#define LS2K_UART_MCR_NORMAL 0x0bU	/* DTR | RTS | OUT2, loopback off */

/* GPIO_CFG2 holds GPIO32..47, two bits per pin, primary function = 3 */
#define LS2K_PINMUX_CFG2      (LS2K_DIRECT + 0x16000498UL)
#define LS2K_PIN40_RX_SHIFT   16U
#define LS2K_PIN41_TX_SHIFT   18U
#define LS2K_PIN_FUNC_MASK    0x3U
#define LS2K_PIN_FUNC_PRIMARY 0x3U

static int ls2k0300_console_fifo_init(void)
{
	mem_addr_t base = (mem_addr_t)DT_REG_ADDR(DT_CHOSEN(zephyr_console));
	uint32_t mux;

	sys_write8(LS2K_UART_FCR_1BYTE, base + LS2K_UART_FCR_OFFSET);

	/*
	 * Put the modem control register back into a state the console can work
	 * from, whatever ran before: OUT2 must be set or the UART's interrupt
	 * output stays deasserted, and MCR.LOOP must be clear or the receiver is
	 * disconnected from the RX pin and the UART only hears itself. The vendor
	 * device tree marks this UART "no-loopback-test"; a bootloader or a
	 * bring-up check that tested loopback and stopped before clearing the bit
	 * leaves a console that looks alive - it prints, and it reads its own
	 * output as typed input - but is deaf to the keyboard.
	 */
	sys_write8(LS2K_UART_MCR_NORMAL, base + LS2K_UART_MCR_OFFSET);

	/*
	 * The console pins, GPIO40 (uart0_rx) and GPIO41 (uart0_tx), on their
	 * primary function. The serial driver does apply the UART's pinctrl state
	 * through the SoC hook in soc/loongson/ls2k0300/pinctrl.c, but that single
	 * path is the whole difference between a shell that can be typed into and
	 * a console that only prints: u-boot configures the pins it drives and on
	 * this board it left uart0_rx in its GPIO setting, which is exactly the
	 * "prints but never receives" console this port kept running into.
	 * Setting the fields here as well makes the port independent of that
	 * path; only the two console pins are touched, with a read-modify-write,
	 * so nothing else in the word moves.
	 */
	mux = sys_read32(LS2K_PINMUX_CFG2);
	mux &= ~((LS2K_PIN_FUNC_MASK << LS2K_PIN40_RX_SHIFT) |
		 (LS2K_PIN_FUNC_MASK << LS2K_PIN41_TX_SHIFT));
	mux |= ((uint32_t)LS2K_PIN_FUNC_PRIMARY << LS2K_PIN40_RX_SHIFT) |
	       ((uint32_t)LS2K_PIN_FUNC_PRIMARY << LS2K_PIN41_TX_SHIFT);
	sys_write32(mux, LS2K_PINMUX_CFG2);

	return 0;
}

SYS_INIT(ls2k0300_console_fifo_init, PRE_KERNEL_2, 0);

/*
 * IS_ENABLED, not #ifdef: a Kconfig symbol is a macro either way, so
 * "#ifdef CONFIG_LS2K0300_IRQ_DIAG" is true even when the option is set to n.
 * The probes below were compiled into every image for exactly that reason.
 */
#if IS_ENABLED(CONFIG_LS2K0300_IRQ_DIAG) || IS_ENABLED(CONFIG_LS2K0300_RX_CHECK)

#include <loongarch/csr.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/irq.h>

/*
 * Console receive chain check, shared by the two bring-up helpers below.
 *
 * Every link is measured with plain MMIO reads (the uncached direct map window
 * is what makes those reads meaningful) and no step needs a terminal: the UART
 * is put into internal loopback and made to receive a byte of its own, which is
 * enough to raise its request. One measurement then decides everything - an
 * empty receive FIFO (LSR.DR == 0) a moment after the byte was fed means the
 * chain delivered it to the driver.
 *
 * That is conclusive only because this check never reads RBR itself, and it has
 * to stay that way: the shell copies RBR into its own ring buffer inside the
 * UART ISR (subsys/shell/backends/shell_uart.c: uart_callback() ->
 * uart_rx_handle() -> uart_fifo_read()) and no thread ever reads RBR, so a byte
 * that leaves the FIFO proves that flag, controller, exception entry and ISR
 * all ran. A thread draining the FIFO by hand - which an earlier version of
 * this check did - can empty it on its own and turn a broken chain into a
 * passing test.
 *
 * When the byte is still there, the samples say which link failed:
 *
 *   IIR = 0x01                          the UART does not raise the request
 *   device status bit 0 clear           the request does not reach the EIOINTC
 *   device status set but the core
 *   status bit 0 clear                  the vector is masked or not routed
 *   core status set but ESTAT bit 3
 *   clear                               the parent line (HWI1) is the break
 *   ESTAT bit 3 set                     dispatch or the child ISR is the break
 *
 * The check holds the scheduler locked: without that the shell's own output is
 * swallowed by the receiver in loopback mode and comes back as typed input, so
 * the shell answers with "command not found" for its own prompt. Interrupts
 * stay enabled, the ISR under test still runs, and the loopback byte is left
 * for the shell to consume, where it shows up as one extra empty line.
 */

/* All of these live in the uncached direct map window */
#define RX_UART0   0x8000000016100000UL
#define RX_EIOINTC 0x8000000016000000UL

/* 8250 register offsets */
#define RX_IER 0x01U
#define RX_IIR 0x02U
#define RX_FCR 0x02U
#define RX_MCR 0x04U
#define RX_LSR 0x05U

#define RX_FCR_TRIG1  0x07U /* FIFO on, receive trigger 1 byte, clear both FIFOs */
#define RX_FCR_16450  0x00U /* no FIFO at all: the request must fire on one byte */
#define RX_MCR_NORMAL 0x0bU /* DTR | RTS | OUT2, what the driver leaves behind */
#define RX_MCR_LOOP   0x10U /* internal loopback */
#define RX_MCR_OUT1   0x04U /* the other modem control output, as a gate */

/*
 * EIOINTC registers, offsets within the system register window (2K0300 user
 * manual, table 3-45 and 3-51). The console UART owns vector 0, and the
 * controller is cascaded on CPU line 3, so its request shows up as ESTAT.IS[3].
 * The enable register is a plain read/write vector mask: it is written back
 * read-modified-write, the way the driver's enable/disable does it too.
 */
#define RX_INTC_CFG0     0x0100U /* chip general configuration register 0 */
#define RX_INTC_CFG0_EN  BIT(19) /* extioi_en: 1 = extended interrupts live */
#define RX_INTC_MAP      0x14c0U /* vector group routing */
#define RX_INTC_IEN      0x1600U /* enable of vectors 0..31 */
#define RX_INTC_POL      0x1640U /* polarity of vectors 0..31 */
#define RX_INTC_ISR      0x1700U /* device status, not gated by the enable mask */
#define RX_INTC_CORE_ISR 0x1800U /* routed to this core, write 1 clears */
#define RX_INTC_VECTOR   BIT(0)
#define RX_INTC_LINE     BIT(3)

struct rx_state {
	uint8_t ier;
	uint8_t iir;
	uint8_t lsr;
	uint8_t mcr;
	uint32_t isr;
	uint32_t core_isr;
	uint32_t ien;
	uint32_t map;
	uint32_t cfg0;
	uint32_t est;
};

static void rx_state_read(struct rx_state *s)
{
	s->ier = sys_read8(RX_UART0 + RX_IER);
	s->iir = sys_read8(RX_UART0 + RX_IIR);
	s->lsr = sys_read8(RX_UART0 + RX_LSR);
	s->mcr = sys_read8(RX_UART0 + RX_MCR);
	s->isr = sys_read32(RX_EIOINTC + RX_INTC_ISR);
	s->core_isr = sys_read32(RX_EIOINTC + RX_INTC_CORE_ISR);
	s->ien = sys_read32(RX_EIOINTC + RX_INTC_IEN);
	s->map = sys_read32(RX_EIOINTC + RX_INTC_MAP);
	s->cfg0 = sys_read32(RX_EIOINTC + RX_INTC_CFG0);
	s->est = (uint32_t)loongarch_csr_read(LOONGARCH_CSR_ESTAT);
}

static void rx_state_print(const char *tag, const struct rx_state *s)
{
	printk("RX %-7s IER=%02x IIR=%02x LSR=%02x MCR=%02x | isr=%08x core=%08x "
	       "ien=%08x map=%08x cfg0=%08x ESTAT=%08x\n",
	       tag, s->ier, s->iir, s->lsr, s->mcr, s->isr, s->core_isr,
	       s->ien, s->map, s->cfg0, s->est);
}

/*
 * Put one byte into the receive FIFO without a terminal, and sample the chain
 * while it is still in there. Nothing may be printed while MCR is in loopback:
 * console output would be swallowed by the receiver and show up in the FIFO as
 * if somebody had typed it.
 */
static void rx_feed(uint8_t fcr, uint8_t mcr, struct rx_state *s)
{
	if (fcr != 0xffU) {
		sys_write8(fcr, RX_UART0 + RX_FCR);
	}

	sys_write8((uint8_t)(RX_MCR_LOOP | mcr), RX_UART0 + RX_MCR);
	sys_write8((uint8_t)'\r', RX_UART0);
	k_busy_wait(2000);
	rx_state_read(s);
	sys_write8(RX_MCR_NORMAL, RX_UART0 + RX_MCR);
}

/*
 * Run the check and, when a link is broken, try the neighbouring
 * configurations that could be the reason for it. Only the console's own vector
 * (bit 0 of the EIOINTC) is touched: it is masked while the register state is
 * sampled, so that a wrong polarity cannot turn into an interrupt storm on the
 * parent handler, and it is put back into service before the check returns.
 */
static void rx_chain_check(const char *tag)
{
	struct rx_state s;
	const char *why;
	bool taken;

	k_sched_lock();

	/* 1. Delivery in the production configuration: one byte into the FIFO,
	 *    the ISR free to take it.
	 */
	rx_feed(RX_FCR_TRIG1, 0U, &s);
	rx_state_print(tag, &s);
	taken = (s.lsr & 0x1U) == 0U;

	if (!taken) {
		uint32_t ien = sys_read32(RX_EIOINTC + RX_INTC_IEN);

		/* The byte is still in the FIFO, so the chain broke before the
		 * ISR. Mask the console's own vector - and only that one - so
		 * the request cannot be handled while the register state is
		 * sampled. The device status answers anyway, it is not gated by
		 * the enable mask; the routed status is, which is exactly what
		 * separates the two samples.
		 */
		sys_write32(ien & ~RX_INTC_VECTOR, RX_EIOINTC + RX_INTC_IEN);
		rx_feed(RX_FCR_TRIG1, 0U, &s);
		rx_state_print("masked", &s);

		if ((s.isr & RX_INTC_VECTOR) == 0U) {
			/* Nothing reaches the controller. UART side suspects:
			 * the FIFO trigger logic, and the OUT1 gate.
			 */
			rx_feed(RX_FCR_16450, 0U, &s);
			rx_state_print("16450", &s);
			rx_feed(RX_FCR_TRIG1, RX_MCR_OUT1, &s);
			rx_state_print("out1", &s);
		}

		if (((s.isr & RX_INTC_VECTOR) != 0U) &&
		    ((s.core_isr & RX_INTC_VECTOR) == 0U)) {
			/* The controller sees the request but nothing reaches
			 * the core: the other input polarity is the one thing
			 * left to try on a level source.
			 */
			uint32_t pol = sys_read32(RX_EIOINTC + RX_INTC_POL);

			sys_write32(pol | RX_INTC_VECTOR, RX_EIOINTC + RX_INTC_POL);
			k_busy_wait(200);
			rx_state_read(&s);
			rx_state_print("pol=1", &s);
			sys_write32(pol, RX_EIOINTC + RX_INTC_POL);
		}

		/* Vector back into service: a request that is still pending now
		 * has to reach the ISR on its own.
		 */
		sys_write32(ien | RX_INTC_VECTOR, RX_EIOINTC + RX_INTC_IEN);
		k_busy_wait(2000);
		rx_state_read(&s);
		rx_state_print("unmask", &s);
		taken = (s.lsr & 0x1U) == 0U;
	}

	/* Leave the UART as the driver expects it. The loopback byte is left
	 * for the shell on purpose: see the note above.
	 */
	sys_write8(RX_FCR_TRIG1, RX_UART0 + RX_FCR);

	if (taken) {
		why = "chain works, the ISR took the loopback byte";
	} else if ((s.iir & 0x0fU) == 0x01U) {
		why = "the UART never raises the request (IIR=01)";
	} else if ((s.cfg0 & RX_INTC_CFG0_EN) == 0U) {
		why = "the extended interrupt path is off (cfg0 bit 19)";
	} else if ((s.isr & RX_INTC_VECTOR) == 0U) {
		why = "the request does not reach the EIOINTC (device status stays 0)";
	} else if ((s.core_isr & RX_INTC_VECTOR) == 0U) {
		why = "the vector is masked or not routed (core status stays 0)";
	} else if ((loongarch_csr_read(LOONGARCH_CSR_ESTAT) & RX_INTC_LINE) == 0U) {
		why = "the parent line HWI1 never asserts (ESTAT bit 3)";
	} else {
		why = "dispatch or the ISR is the break";
	}

	/* Print before unlocking so the shell cannot interleave itself */
	printk("RX %s verdict: %s (LSR.DR=%u IIR=%02x isr=%u core=%u ESTAT=%08x)\n",
	       tag, why, s.lsr & 0x1U, s.iir, (uint32_t)(s.isr & RX_INTC_VECTOR),
	       (uint32_t)(s.core_isr & RX_INTC_VECTOR),
	       (uint32_t)loongarch_csr_read(LOONGARCH_CSR_ESTAT));

	k_sched_unlock();
}

#endif /* CONFIG_LS2K0300_IRQ_DIAG || CONFIG_LS2K0300_RX_CHECK */

#if IS_ENABLED(CONFIG_LS2K0300_IRQ_DIAG)
#include <loongarch/csr.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/irq.h>

/*
 * Bring-up diagnostic (CONFIG_LS2K0300_IRQ_DIAG).
 *
 * This is the self test that drove the bring-up of the two interrupt driven
 * peripherals on this SoC, kept as a regression check. Both of the problems it
 * was written for are fixed and measured:
 *
 *   - The constant timer produced no working tick at all. u-boot leaves the
 *     counter running, and a TCFG write while En is set is ignored, so the
 *     first tick of the kernel arrived tens of seconds late; on top of that the
 *     unit of TCFG.InitVal was assumed to be one cycle instead of measured.
 *     The driver now stops the counter before programming it and measures how
 *     many rdtime cycles one InitVal unit is worth during its init. The
 *     "Timer:" line in the log is that measurement; on the 2K0300 it reads
 *     1 unit = 524293/524288 cycles, i.e. the two clocks are the same.
 *   - The console receive chain works, and the dead console was the timer
 *     above: input ends up in a shell thread, which needs a working tick to be
 *     scheduled at all. rx_chain_check() proves the chain below without a
 *     terminal and without ticks, which is what separates the two.
 *
 * Everything here is measured from the CPU (the JTAG port does not answer for
 * the peripheral windows and the gdb stub exposes no CSR registers):
 *
 *   1. dumps the CSRs and the timer state as full 64 bit values,
 *   2. reads the EIOINTC state - read only: an earlier version wrote into the
 *      enable register, and a level triggered vector that nothing handles then
 *      stormed the parent handler until the console died,
 *   3. watches the tick health with sys_clock_elapsed(), the number of ticks
 *      that have passed but not been announced yet,
 *   4. runs the loopback receive chain check,
 *   5. measures the rdtime frequency against the console's baud rate, the only
 *      external clock the board offers: 16 bytes at 115200 baud take 1388 us
 *      on the wire, which is enough to tell whether
 *      CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC matches the hardware.
 *
 * Runs at APPLICATION level, i.e. after the drivers are up and before the
 * application's main(). It never sleeps and never polls for seconds, so it
 * cannot starve the shell.
 */
#define LS2K_DIAG_UART0   0x8000000016100000UL
#define LS2K_DIAG_PINMUX  0x8000000016000490UL
#define LS2K_DIAG_EIOINTC 0x8000000016000000UL
#define LS2K_DIAG_GPIO    0x8000000016104000UL

/* GPIO byte regions, one byte per absolute pin (gpio-loongson-64bit layout) */
#define LS2K_GPIO_CONF 0x800U
#define LS2K_GPIO_OUT  0x900U
#define LS2K_GPIO_IN   0xa00U

/*
 * EIOINTC registers, offsets within the system register window (2K0300 user
 * manual, table 3-45 and 3-51). Vector 0 belongs to the console UART and the
 * controller is cascaded on CPU line 3.
 */
#define LS2K_INTC_CFG0     0x0100U
#define LS2K_INTC_CFG0_EN  BIT(19)
#define LS2K_INTC_MAP      0x14c0U
#define LS2K_INTC_IEN      0x1600U
#define LS2K_INTC_ISR      0x1700U
#define LS2K_INTC_CORE_ISR 0x1800U

#define LS2K_CRMD_PLV(crmd)  ((crmd) & 0x3U)
#define LS2K_CRMD_IE(crmd)   (((crmd) >> 2) & 0x1U)
#define LS2K_CRMD_DA(crmd)   (((crmd) >> 3) & 0x1U)
#define LS2K_CRMD_PG(crmd)   (((crmd) >> 4) & 0x1U)
#define LS2K_CRMD_DATF(crmd) (((crmd) >> 5) & 0x3U)
#define LS2K_CRMD_DATM(crmd) (((crmd) >> 7) & 0x3U)

static void ls2k0300_dump_csr(const char *tag)
{
	uint32_t crmd = (uint32_t)loongarch_csr_read(LOONGARCH_CSR_CRMD);
	uint32_t ecfg = (uint32_t)loongarch_csr_read(LOONGARCH_CSR_ECFG);
	uint32_t estat = (uint32_t)loongarch_csr_read(LOONGARCH_CSR_ESTAT);

	printk("DIAG %s: CRMD=%08x [PLV=%u IE=%u DA=%u PG=%u DATF=%u DATM=%u]\n", tag, crmd,
	       LS2K_CRMD_PLV(crmd), LS2K_CRMD_IE(crmd), LS2K_CRMD_DA(crmd), LS2K_CRMD_PG(crmd),
	       LS2K_CRMD_DATF(crmd), LS2K_CRMD_DATM(crmd));
	printk("DIAG %s: ECFG=%08x [LIE=%03x] ESTAT=%08x [IS=%03x Ecode=%u]\n", tag, ecfg,
	       ecfg & 0x1fffU, estat, estat & 0x1fffU, (estat >> 16) & 0x3fU);
	printk("DIAG %s: TCFG=%016llx TVAL=%016llx\n", tag,
	       (unsigned long long)loongarch_csr_read(LOONGARCH_CSR_TCFG),
	       (unsigned long long)loongarch_csr_read(LOONGARCH_CSR_TVAL));
}

/*
 * Tick health: sys_clock_elapsed() is the number of ticks that have passed but
 * not been announced yet, so it stays near zero when the timer interrupts and
 * sys_clock_announce() work, and grows with rdtime when they do not. The UART
 * is timed as well: 64 characters at 115200 baud take 64 * 10 / 115200 s, which
 * turns the terminal's baud rate into an external reference for the otherwise
 * unmeasurable rdtime frequency.
 */
static void ls2k0300_dump_tick(const char *tag)
{
	printk("DIAG tick %s: elapsed=%u rdtime=%016llx TCFG=%016llx TVAL=%016llx\n", tag,
	       (uint32_t)sys_clock_elapsed(),
	       (unsigned long long)loongarch_rdtime(),
	       (unsigned long long)loongarch_csr_read(LOONGARCH_CSR_TCFG),
	       (unsigned long long)loongarch_csr_read(LOONGARCH_CSR_TVAL));
}

/*
 * rdtime frequency, measured against the only external clock the board offers.
 *
 * The console runs at 115200 baud, i.e. 86.8 us per 10 bit character, and
 * CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC claims how many rdtime cycles that is. The
 * scheduler is locked and no other thread writes, so the polled THRE waits
 * measure the transmit FIFO draining and nothing else. (Measured on the board:
 * without the lock the same probe read 157029 cycles for what should have been
 * ten byte times, because the shell's own output was already in the FIFO.)
 */
#define LS2K_CONSOLE_BYTES         16U
#define LS2K_CONSOLE_BITS_PER_BYTE 10U
#define LS2K_CONSOLE_BAUD          115200UL

static void ls2k0300_time_probe(void)
{
	uint64_t t0;
	uint64_t delta;

	k_sched_lock();

	t0 = loongarch_rdtime();

	for (unsigned int i = 0U; i < LS2K_CONSOLE_BYTES; i++) {
		while ((sys_read8(LS2K_DIAG_UART0 + 5U) & 0x20U) == 0U) {
		}
		sys_write8('.', LS2K_DIAG_UART0);
	}

	delta = loongarch_rdtime() - t0;

	k_sched_unlock();

	printk(" <- %u bytes = %u us on the wire => rdtime %llu Hz (config %u Hz)\n",
	       (uint32_t)LS2K_CONSOLE_BYTES,
	       (uint32_t)((uint64_t)LS2K_CONSOLE_BYTES * LS2K_CONSOLE_BITS_PER_BYTE *
			  1000000ULL / LS2K_CONSOLE_BAUD),
	       (unsigned long long)(delta * LS2K_CONSOLE_BAUD /
				    ((uint64_t)LS2K_CONSOLE_BYTES *
				     LS2K_CONSOLE_BITS_PER_BYTE)),
	       (uint32_t)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);
}

static int ls2k0300_irq_diag(void)
{
	ls2k0300_dump_csr("app-init");
	printk("DIAG config: hw_cycles_per_sec=%u ticks_per_sec=%u tickless=%u\n",
	       (uint32_t)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
	       (uint32_t)CONFIG_SYS_CLOCK_TICKS_PER_SEC,
	       (uint32_t)IS_ENABLED(CONFIG_TICKLESS_KERNEL));

	/* 1. EIOINTC: is the extended path on, does the enable that irq_enable()
	 *    - which the ns16550 driver does call - land here, and does vector 0
	 *    reach the core on the CPU line the node cascades on?
	 */
	printk("DIAG eiointc: cfg0=%08x[extioi_en=%u] map=%08x ien=%08x core=%08x isr=%08x\n",
	       sys_read32(LS2K_DIAG_EIOINTC + LS2K_INTC_CFG0),
	       (sys_read32(LS2K_DIAG_EIOINTC + LS2K_INTC_CFG0) & LS2K_INTC_CFG0_EN) ? 1U : 0U,
	       sys_read32(LS2K_DIAG_EIOINTC + LS2K_INTC_MAP),
	       sys_read32(LS2K_DIAG_EIOINTC + LS2K_INTC_IEN),
	       sys_read32(LS2K_DIAG_EIOINTC + LS2K_INTC_CORE_ISR),
	       sys_read32(LS2K_DIAG_EIOINTC + LS2K_INTC_ISR));

	/*
	 * Read only, deliberately.
	 *
	 * An earlier version of this diagnostic also unmasked a second vector to
	 * see whether the register was writable and whether irq_enable() reached
	 * the driver. That vector was handled by nothing: a level triggered
	 * source that stays asserted re-enters the parent handler forever, so
	 * the interrupt storm starved the shell and input died right after the
	 * diagnostic had printed. Never write to an interrupt controller's
	 * enable register from a diagnostic - even the console's own vector
	 * belongs to the driver that owns the line.
	 */

	/* 2. Tick health: elapsed is the number of ticks not announced yet, so it
	 *    stays near zero when the timer and sys_clock_announce() work.
	 */
	ls2k0300_dump_tick("start");
	{
		volatile uint32_t acc = 0U;

		for (uint32_t i = 0U; i < 3000000U; i++) {
			acc += i;
		}
		acc = acc;
	}
	ls2k0300_dump_tick("after busy loop");

	/*
	 * Deliberately no writes to other peripherals from here.
	 *
	 * Two earlier versions did more than read, and both broke the console:
	 *
	 *  - a "watch one character" step polled LSR/RBR by hand, which steals a
	 *    byte the shell would have received, and
	 *  - an LED hunt drove six candidate pins for ~13 s, which also left the
	 *    CPU spinning in k_busy_wait() long enough to starve the shell.
	 *
	 * The GPIO register layout itself was checked against the vendor kernel
	 * (gpio-loongson-irq.c: loongson,ls2k0300-gpio uses generic_reg_table
	 * with dir 0x800, out 0x900, in 0xa00), so the offsets in
	 * drivers/gpio/gpio_loongson_ls2k0300.c are right. Any pin hunting is
	 * better done from the shell, which is interactive and can be stopped.
	 */

	/* 3. The console receive chain, without a terminal and without ticks:
	 *    rx_chain_check() feeds the UART a byte of its own and reports which
	 *    link breaks. See its comment above for how to read the samples.
	 */
	rx_chain_check("boot");

	/* 4. rdtime frequency, measured against the console's baud rate */
	ls2k0300_time_probe();

	/* 5. No sleep here on purpose: with a broken timer k_msleep() never
	 *    returns and the console would be gone for good. The tick health is
	 *    reported by the work item in the CONFIG_LS2K0300_RX_CHECK block
	 *    instead, which cannot run at all while ticks are broken.
	 */
	printk("DIAG boot check done, the shell should take input now (try help)\n");

	return 0;
}

SYS_INIT(ls2k0300_irq_diag, APPLICATION, 0);
#endif /* CONFIG_LS2K0300_IRQ_DIAG */

#if IS_ENABLED(CONFIG_LS2K0300_RX_CHECK)
#include <zephyr/drivers/uart.h>
#include <zephyr/irq.h>
#include <zephyr/sys/sys_io.h>

/*
 * Console receive interrupt self check (CONFIG_LS2K0300_RX_CHECK).
 *
 * The same check again three seconds after the application starts, this time
 * from a work queue item. That timing is a test of its own: a work queue item
 * cannot run at all while the timer does not announce ticks, so seeing the
 * "RXCHK uptime" line is the proof that the timer keeps time and that
 * sys_clock_announce() reaches the timeout list. The verdict that follows is
 * taken with the shell fully up and through the driver's own device.
 *
 * Checked in this build: the console (CONFIG_UART_CONSOLE) installs a receive
 * path of its own only when CONFIG_CONSOLE_HANDLER is selected - it is not -
 * so the shell's callback is the only consumer of the UART's receive interrupt.
 * Two consumers on one UART would otherwise fight over the single callback slot
 * the driver has, and whichever registers last silently starves the other.
 */
#define LS2K_RXCHK_IRQ 16U

static const struct device *const rxchk_uart =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_shell_uart));

static void ls2k0300_rx_check_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	printk("RXCHK uptime=%lld ms, unannounced ticks=%u, irq_en=%d, rx_ready=%d\n",
	       k_uptime_get(), (uint32_t)sys_clock_elapsed(), irq_is_enabled(LS2K_RXCHK_IRQ),
	       uart_irq_rx_ready(rxchk_uart));

	rx_chain_check("after3s");
}

static struct k_work_delayable ls2k0300_rx_check_work;

static int ls2k0300_rx_check_init(void)
{
	k_work_init_delayable(&ls2k0300_rx_check_work, ls2k0300_rx_check_handler);
	k_work_schedule(&ls2k0300_rx_check_work, K_SECONDS(3));

	return 0;
}

SYS_INIT(ls2k0300_rx_check_init, APPLICATION, 0);
#endif /* CONFIG_LS2K0300_RX_CHECK */

#if IS_ENABLED(CONFIG_LS2K0300_CONSOLE_GUARD)

#include <loongarch/csr.h>

/*
 * Console guard (CONFIG_LS2K0300_CONSOLE_GUARD).
 *
 * The console UART has states that all present as "the shell went deaf while
 * the rest of the system is fine", and the console cannot report them while it
 * is in one of them. The guard repairs them and stays quiet:
 *
 *   1. The receive pin no longer on function 3, i.e. written out of uart0_rx by
 *      something touching the pin mux field. The SoC comes up with the pin in
 *      its GPIO setting and it is the pin controller that moves it, so any
 *      later write of that field - a GPIO driver probing the pad - takes the
 *      receiver off the pin. The console keeps printing, because the
 *      transmitter's field is in the same word and still right, and nothing at
 *      all arrives.
 *   2. The UART left in internal loopback: MCR.LOOP set and OUT2 cleared with
 *      it. The receiver is off the RX pin, the shell's own output is fed
 *      straight back into it, and OUT2 = 0 keeps the interrupt output
 *      deasserted as well. The vendor device tree marks this UART
 *      "no-loopback-test" for good reason.
 *   3. A received byte that nobody serves for two ticks: the UART asked for an
 *      interrupt once and the request did not reach the driver. IER is toggled
 *      so the UART raises it again. CONFIG_UART_NS16550_WA_ISR_REENABLE_
 *      INTERRUPT now covers the normal case; this is the belt to its braces.
 *   4. Shell output queued (IER.TBE set) while the transmitter is idle (THRE
 *      set) for two ticks: the empty-transmitter request never came. IER.TBE
 *      is toggled so it does.
 *   5. A transmitter that stays busy for three ticks: both FIFOs are cleared
 *      and the modem lines are written back, the only thing left to try when
 *      the shift register is stuck.
 *
 * Everything else is read only, and nothing is reported: a console error cannot
 * be announced over the console itself - the shell writes through the blocking
 * uart_poll_out(), which spins on LSR.THRE inside a spinlock with interrupts
 * off, so a print into a stalled transmitter hangs the caller - and a repaired
 * console needs no announcement. Each repair is recorded with LOG_DBG, which
 * the runtime log level keeps out of the console by default.
 */
#define GRD_UART0           0x8000000016100000UL
#define GRD_PINMUX_UART0_RX 0x8000000016000498UL

#define GRD_REG_IER 0x01U
#define GRD_REG_FCR 0x02U
#define GRD_REG_MCR 0x04U
#define GRD_REG_LSR 0x05U

#define GRD_MCR_GOOD  0x0bU /* DTR | RTS | OUT2, internal loopback off */
#define GRD_FCR_TRIG1 0x07U /* FIFO on, RX trigger 1 byte, clear both */
#define GRD_IER_RDA   0x01U
#define GRD_IER_TBE   0x02U
#define GRD_LSR_DR    BIT(0)
#define GRD_LSR_THRE  BIT(5)

/*
 * The two console pins share one pin mux word (GPIO32..47, two bits per pin,
 * function 3 = uart0), which is why a pin that leaves function 3 takes the
 * receiver with it while the transmitter keeps working.
 */
#define GRD_PIN40_SHIFT   16U
#define GRD_PIN_FUNC_MASK 0x3U
#define GRD_PIN_FUNC_UART 0x3U

#define GRD_PERIOD     K_SECONDS(1)
#define GRD_DR_TICKS   2U /* unserved byte for this many ticks: re-arm */
#define GRD_THRE_TICKS 3U /* stalled transmitter for this many ticks: kick */

static struct k_work_delayable console_guard_work;
static uint16_t guard_dr_ticks;
static uint16_t guard_thre_ticks;
static uint16_t guard_tbe_ticks;

static void console_guard_handler(struct k_work *work)
{
	uint8_t lsr = sys_read8(GRD_UART0 + GRD_REG_LSR);
	uint8_t ier = sys_read8(GRD_UART0 + GRD_REG_IER);
	uint32_t pinmux = sys_read32(GRD_PINMUX_UART0_RX);
	uint32_t rx_func = (pinmux >> GRD_PIN40_SHIFT) & GRD_PIN_FUNC_MASK;

	ARG_UNUSED(work);

	/* 1. The receive pin, see the comment above */
	if (rx_func != GRD_PIN_FUNC_UART) {
		sys_write32((pinmux & ~(GRD_PIN_FUNC_MASK << GRD_PIN40_SHIFT)) |
				    ((uint32_t)GRD_PIN_FUNC_UART << GRD_PIN40_SHIFT),
			    GRD_PINMUX_UART0_RX);
		LOG_DBG("console guard: put uart0_rx back on function 3 (pin mux was %08x)",
			pinmux);
	}

	/* 2. A UART left in loopback hears nobody and answers itself */
	if (sys_read8(GRD_UART0 + GRD_REG_MCR) != GRD_MCR_GOOD) {
		sys_write8(GRD_MCR_GOOD, GRD_UART0 + GRD_REG_MCR);
		LOG_DBG("console guard: uart0 was left in loopback, MCR written back");
	}

	/* 3. The receive request, then the transmit request: one IER
	 *    read-modify-write per tick, or the second would undo the first.
	 */
	if ((lsr & GRD_LSR_DR) != 0U) {
		if (++guard_dr_ticks >= GRD_DR_TICKS) {
			sys_write8(0U, GRD_UART0 + GRD_REG_IER);
			sys_write8((uint8_t)(ier | GRD_IER_RDA), GRD_UART0 + GRD_REG_IER);
			guard_dr_ticks = 0U;
			LOG_DBG("console guard: re-armed the lost receive request");
		}
	} else if (((ier & GRD_IER_TBE) != 0U) && ((lsr & GRD_LSR_THRE) != 0U)) {
		if (++guard_tbe_ticks >= GRD_DR_TICKS) {
			sys_write8((uint8_t)(ier & ~GRD_IER_TBE), GRD_UART0 + GRD_REG_IER);
			sys_write8(ier, GRD_UART0 + GRD_REG_IER);
			guard_tbe_ticks = 0U;
			LOG_DBG("console guard: re-armed the lost transmit request");
		}
	} else {
		guard_dr_ticks = 0U;
		guard_tbe_ticks = 0U;
	}

	/* 4. A transmitter that stays busy: clear the FIFOs and rewrite MCR,
	 *    the only thing left to try when the shift register is stuck.
	 */
	if ((lsr & GRD_LSR_THRE) == 0U) {
		if (++guard_thre_ticks >= GRD_THRE_TICKS) {
			sys_write8(GRD_FCR_TRIG1, GRD_UART0 + GRD_REG_FCR);
			sys_write8(GRD_MCR_GOOD, GRD_UART0 + GRD_REG_MCR);
			guard_thre_ticks = 0U;
			LOG_DBG("console guard: uart0 transmitter stuck, FIFOs cleared");
		}
	} else {
		guard_thre_ticks = 0U;
	}

	k_work_reschedule(&console_guard_work, GRD_PERIOD);
}

static int ls2k0300_console_guard_init(void)
{
	k_work_init_delayable(&console_guard_work, console_guard_handler);
	k_work_schedule(&console_guard_work, GRD_PERIOD);

	return 0;
}

SYS_INIT(ls2k0300_console_guard_init, APPLICATION, 0);
#endif /* CONFIG_LS2K0300_CONSOLE_GUARD */

/*
 * The data cache is deliberately left off.
 *
 * This port has no cache maintenance at all - no CACOP sequences, no cache
 * Kconfig, no sys_cache_data_flush_range() - while the board has DMA engines
 * that reach memory over the bus without passing through the CPU's cache. The
 * LSIA controller behind the I2S data path is one of them: with the cache on,
 * bytes the CPU wrote into a buffer it prepared for a transfer can still be
 * dirty in cache while the engine reads the memory behind its back, and the
 * transfer sees stale data (measured: the audio ring read back as all zeroes,
 * so the DMA moved silence for ever). The same happens in reverse on the
 * receive side.
 *
 * Linux turns the caches off the same way before it brings them up with its own
 * maintenance (CPUCFG2.DIE / DPE / DPDE, the LoongArch control register 0x302).
 * Until this port grows cache maintenance, leaving them off is what makes DMA
 * buffers work, and it costs memory bandwidth only. The value is printed before
 * and after, so what the hardware actually had enabled is visible on the
 * console.
 */
#define LS2K_CPUCFG2_DIE  (1UL << 21) /* data cache invalidate enable */
#define LS2K_CPUCFG2_DPE  (1UL << 22) /* data cache prefetch enable */
#define LS2K_CPUCFG2_DPDE (1UL << 23) /* data cache prefetch enable */

static int ls2k0300_cache_init(void)
{
	unsigned long before, after;
	unsigned long wanted;

	before = loongarch_csr_read(LOONGARCH_CPUCFG2);

	wanted = before & ~(LS2K_CPUCFG2_DIE | LS2K_CPUCFG2_DPE | LS2K_CPUCFG2_DPDE);
	(void)loongarch_csr_write(wanted, LOONGARCH_CPUCFG2);

	after = loongarch_csr_read(LOONGARCH_CPUCFG2);

	LOG_INF("CPUCFG2 0x%08lx -> 0x%08lx (data cache %s)", before, after,
		((before & LS2K_CPUCFG2_DIE) != 0UL) ? "was on, now off" : "already off");

	return 0;
}

SYS_INIT(ls2k0300_cache_init, PRE_KERNEL_1, 0);
