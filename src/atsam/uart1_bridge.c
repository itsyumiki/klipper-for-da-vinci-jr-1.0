// UART1 side of the USB CDC <-> UART bridge (SAM4E)
//
// Copyright (C) 2026  <author>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_USB_CDC_BRIDGE_BAUD
#include "board/armcm_boot.h" // armcm_enable_irq
#include "board/irq.h" // irq_save
#include "command.h" // DECL_CONSTANT_STR
#include "compiler.h" // ARRAY_SIZE
#include "generic/usb_cdc_bridge.h" // usb_bridge_notify_rx
#include "internal.h" // gpio_peripheral
#include "sched.h" // DECL_INIT

// UART1 on PA5 (URXD1) / PA6 (UTXD1), peripheral C
static Uart * const Port = UART1;
static const uint32_t Pmc_id = ID_UART1;
static const uint32_t rx_pin = GPIO('A', 5), tx_pin = GPIO('A', 6);
DECL_CONSTANT_STR("RESERVE_PINS_uart1_bridge", "PA5,PA6");

// 256-byte rings indexed by uint8_t (wraps naturally). One slot is kept
// empty, so at most 255 bytes are stored. Single producer / single consumer:
// the interrupt and the task each own one index, so no locking is needed.
static uint8_t rx_buf[256], tx_buf[256];
static volatile uint8_t rx_head, rx_tail, tx_head, tx_tail;
static uint32_t rx_overflow, uart_errors;    // diagnostics only

#define TX_WAKE_USED 191    // wake the USB side when >= 64 bytes are free

static inline uint8_t
tx_used(void)
{
    return (uint8_t)(tx_head - tx_tail);
}

void
UART1_Handler(void)
{
    uint32_t sr = Port->UART_SR & Port->UART_IMR;
    if (sr & UART_SR_RXRDY) {
        uint8_t c = Port->UART_RHR;
        uint8_t h = rx_head, nh = h + 1;
        if (nh != rx_tail) {
            rx_buf[h] = c;
            rx_head = nh;
        } else {
            rx_overflow++;       // host not reading - drop
        }
        usb_bridge_notify_rx();
    }
    if (sr & UART_SR_TXRDY) {
        uint8_t t = tx_tail;
        if (t != tx_head) {
            Port->UART_THR = tx_buf[t];
            tx_tail = t + 1;
            if (tx_used() == TX_WAKE_USED)
                usb_bridge_notify_tx_space();
        } else {
            Port->UART_IDR = UART_IDR_TXRDY;
        }
    }
    if (sr & (UART_SR_OVRE | UART_SR_FRAME | UART_SR_PARE)) {
        Port->UART_CR = UART_CR_RSTSTA;
        uart_errors++;
    }
}

uint_fast8_t
uart_bridge_rx_read(uint8_t *buf, uint_fast8_t max_len)
{
    uint8_t t = rx_tail, h = rx_head;
    uint_fast8_t n = 0;
    while (t != h && n < max_len)
        buf[n++] = rx_buf[t++];
    rx_tail = t;
    return n;
}

uint_fast8_t
uart_bridge_tx_free(void)
{
    return 255 - tx_used();
}

// Caller must have checked uart_bridge_tx_free() first
void
uart_bridge_tx_write(const uint8_t *buf, uint_fast8_t len)
{
    uint8_t h = tx_head;
    while (len--)
        tx_buf[h++] = *buf++;
    tx_head = h;
    Port->UART_IER = UART_IER_TXRDY;
}

// Apply baud rate and parity (CDC parity: 0 none, 1 odd, 2 even, 3 mark,
// 4 space). Returns 0 on success, -1 if the UART cannot do it.
int
uart_bridge_set_config(uint32_t baud, uint8_t cdc_parity)
{
    static const uint32_t par_map[] = {
        UART_MR_PAR_NO, UART_MR_PAR_ODD, UART_MR_PAR_EVEN,
        UART_MR_PAR_MARK, UART_MR_PAR_SPACE,
    };
    if (cdc_parity >= ARRAY_SIZE(par_map))
        return -1;
    uint32_t pclk = get_pclock_frequency(Pmc_id);
    if (!baud || baud > pclk / 16)
        return -1;
    uint32_t br16 = baud * 16;
    uint32_t cd = (pclk + br16 / 2) / br16;     // rounded divisor
    if (cd < 1 || cd > 0xffff)
        return -1;
    irqstatus_t flag = irq_save();
    Port->UART_CR = (UART_CR_RSTRX | UART_CR_RSTTX
                     | UART_CR_RXDIS | UART_CR_TXDIS);
    Port->UART_MR = par_map[cdc_parity] | UART_MR_CHMODE_NORMAL;
    Port->UART_BRGR = cd;
    Port->UART_CR = UART_CR_RXEN | UART_CR_TXEN | UART_CR_RSTSTA;
    irq_restore(flag);
    return 0;
}

void
uart1_bridge_init(void)
{
    gpio_peripheral(rx_pin, 'C', 1);
    gpio_peripheral(tx_pin, 'C', 0);
    enable_pclock(Pmc_id);
    Port->UART_PTCR = UART_PTCR_RXTDIS | UART_PTCR_TXTDIS;  // no PDC
    Port->UART_CR = (UART_CR_RSTRX | UART_CR_RSTTX
                     | UART_CR_RXDIS | UART_CR_TXDIS);
    Port->UART_IDR = 0xffffffff;
    uart_bridge_set_config(CONFIG_USB_CDC_BRIDGE_BAUD, 0);
    Port->UART_IER = (UART_IER_RXRDY | UART_IER_OVRE | UART_IER_FRAME
                      | UART_IER_PARE);
    // Priority 1 (same as USB) preempts the SysTick stepper timer (2):
    // keep this handler as short as it is.
    armcm_enable_irq(UART1_Handler, UART1_IRQn, 1);
}
DECL_INIT(uart1_bridge_init);
