// LPC1115 serial port (UART on PIO1_6 RXD / PIO1_7 TXD)
//
// Register setup follows the UART code of the old RepRapFirmware LPC
// firmware, but is interrupt driven and works with generic/serial_irq.c.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_SERIAL_BAUD
#include "board/armcm_boot.h" // armcm_enable_irq
#include "board/irq.h" // irq_save
#include "board/serial_irq.h" // serial_rx_byte
#include "command.h" // DECL_CONSTANT_STR
#include "internal.h" // LPC_UART0
#include "sched.h" // DECL_INIT

#define UART LPC_UART0
#define UART_IRQn UART0_IRQn

DECL_CONSTANT_STR("RESERVE_PINS_serial", "PIO1_6,PIO1_7");

// The UART is clocked from the main clock divided by UARTCLKDIV (set to 1)
#define UART_PCLK CONFIG_CLOCK_FREQ

#define LSR_RDR (1 << 0) // receive data ready
#define LSR_THRE (1 << 5) // transmit holding register empty
#define IER_RBR 0x01 // receive data available irq
#define IER_RBR_THRE 0x03 // receive and transmit irqs
#define IIR_NOINT (1 << 0) // set when no interrupt is pending
#define IIR_ID_MASK 0x0e
#define IIR_ID_THRE 0x02
#define IIR_ID_RDA 0x04
#define IIR_ID_CTI 0x0c // receive timeout

// Write tx bytes to the serial port
static void
kick_tx(void)
{
    for (;;) {
        if (!(UART->LSR & LSR_THRE)) {
            // Output fifo full - enable tx irq
            UART->IER = IER_RBR_THRE;
            break;
        }
        uint8_t data;
        int ret = serial_get_tx_byte(&data);
        if (ret) {
            // No more data to send - disable tx irq
            UART->IER = IER_RBR;
            break;
        }
        UART->THR = data;
    }
}

void
UART0_IRQHandler(void)
{
    for (;;) {
        uint32_t iir = UART->IIR;
        if (iir & IIR_NOINT)
            break;
        uint32_t id = iir & IIR_ID_MASK;
        if (id == IIR_ID_RDA || id == IIR_ID_CTI) {
            while (UART->LSR & LSR_RDR)
                serial_rx_byte(UART->RBR);
        } else if (id == IIR_ID_THRE) {
            kick_tx();
        } else {
            // Unexpected source (eg, line status) - read LSR to clear it
            (void)UART->LSR;
        }
    }
}

void
serial_enable_tx_irq(void)
{
    if (UART->LSR & LSR_THRE) {
        irqstatus_t flag = irq_save();
        kick_tx();
        irq_restore(flag);
    }
}

// Find divisor settings for the requested baud rate.  The baud is
// pclk / (16 * DL * (1 + DIVADD / MUL)).  See UM10398 "UART baud rate".
static void
calc_baud(uint32_t baud, uint32_t *pdl, uint32_t *pfdr)
{
    uint32_t best_err = 0xffffffff, best_dl = 1, best_fdr = 0x10;
    for (uint32_t mul = 1; mul <= 15; mul++) {
        for (uint32_t add = 0; add < mul; add++) {
            uint32_t scale = 16 * baud * (mul + add);
            uint32_t dl = (UART_PCLK * mul + scale / 2) / scale;
            if (dl < 1 || dl > 0xffff || (add && dl < 3))
                continue;
            uint32_t actual = UART_PCLK * mul / (16 * dl * (mul + add));
            uint32_t err = actual > baud ? actual - baud : baud - actual;
            if (err < best_err) {
                best_err = err;
                best_dl = dl;
                best_fdr = (mul << 4) | add;
            }
        }
    }
    *pdl = best_dl;
    *pfdr = best_fdr;
}

void
serial_init(void)
{
    // Enable clocks for the UART and the pin configuration block
    LPC_SYSCON_SYSAHBCLKCTRL |= SYSAHBCLK_UART | SYSAHBCLK_IOCON;
    LPC_SYSCON_UARTCLKDIV = 1;

    // Setup pins
    LPC_IOCON_PIO1_6 = ((LPC_IOCON_PIO1_6 & ~IOCON_FUNC_MASK)
                        | IOCON_PIO1_6_FUNC_RXD);
    LPC_IOCON_PIO1_7 = ((LPC_IOCON_PIO1_7 & ~IOCON_FUNC_MASK)
                        | IOCON_PIO1_7_FUNC_TXD);

    // Setup baud
    uint32_t dl, fdr;
    calc_baud(CONFIG_SERIAL_BAUD, &dl, &fdr);
    UART->LCR = (1 << 7); // set DLAB bit
    UART->DLL = dl & 0xff;
    UART->DLM = (dl >> 8) & 0xff;
    UART->FDR = fdr;
    UART->LCR = 3; // 8N1 ; clear DLAB bit

    // Enable and clear the fifos, then the transmitter
    UART->FCR = 0x07;
    UART->TER = 0x80;

    // Enable receive irq
    armcm_enable_irq(UART0_IRQHandler, UART_IRQn, 0);
    UART->IER = IER_RBR;
}
DECL_INIT(serial_init);
