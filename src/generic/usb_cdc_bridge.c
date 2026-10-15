// USB CDC-ACM second port: transparent byte bridge to a board UART
//
// Copyright (C) 2026  <author>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_USB_CDC_BRIDGE_BAUD
#include "board/usb_cdc_ep.h" // USB_CDC2_EP_BULK_IN_SIZE
#include "sched.h" // DECL_TASK
#include "usb_cdc.h" // usb_read_bulk_out2
#include "usb_cdc_bridge.h" // uart_bridge_rx_read


/****************************************************************
 * Line coding / control line state (EP0 requests, see usb_cdc.c)
 ****************************************************************/

static struct usb_cdc_line_coding bridge_coding = {
    .dwDTERate = CONFIG_USB_CDC_BRIDGE_BAUD,
    .bCharFormat = 0, .bParityType = 0, .bDataBits = 8,
};
static uint8_t bridge_control_state;

struct usb_cdc_line_coding *
usb_bridge_get_line_coding(void)
{
    return &bridge_coding;
}

void
usb_bridge_set_control_state(uint8_t state)
{
    // bit0 = DTR, bit1 = RTS. Stored only (phase 2 may map these to pins).
    bridge_control_state = state;
}

// Called after the SET_LINE_CODING data and status stages completed. The
// request was already acknowledged, so unsupported settings are ignored and
// GET_LINE_CODING keeps reporting the coding that is really in effect.
void
usb_bridge_set_line_coding(const struct usb_cdc_line_coding *lc)
{
    if (lc->bCharFormat != 0 || lc->bDataBits != 8 || lc->bParityType > 4)
        return;
    if (uart_bridge_set_config(lc->dwDTERate, lc->bParityType))
        return;
    bridge_coding = *lc;
}


/****************************************************************
 * UART -> USB (bulk IN)
 ****************************************************************/

static struct task_wake bridge_in_wake;
static uint8_t in_pkt[USB_CDC2_EP_BULK_IN_SIZE], in_len;

void
usb_notify_bulk_in2(void)
{
    sched_wake_task(&bridge_in_wake);
}

// Called from the UART RX interrupt
void
usb_bridge_notify_rx(void)
{
    sched_wake_task(&bridge_in_wake);
}

void
usb_bridge_in_task(void)
{
    if (!sched_check_wake(&bridge_in_wake))
        return;
    if (!in_len)
        in_len = uart_bridge_rx_read(in_pkt, sizeof(in_pkt));
    if (!in_len)
        return;
    int_fast8_t ret = usb_send_bulk_in2(in_pkt, in_len);
    if (ret <= 0)
        // Endpoint busy - the UDP interrupt armed by usb_send_bulk_in2
        // wakes this task again
        return;
    in_len = 0;
    usb_notify_bulk_in2();    // more data may already be waiting
}
DECL_TASK(usb_bridge_in_task);


/****************************************************************
 * USB (bulk OUT) -> UART
 ****************************************************************/

static struct task_wake bridge_out_wake;

void
usb_notify_bulk_out2(void)
{
    sched_wake_task(&bridge_out_wake);
}

// Called from the UART TX interrupt when the TX ring has room again
void
usb_bridge_notify_tx_space(void)
{
    sched_wake_task(&bridge_out_wake);
}

void
usb_bridge_out_task(void)
{
    if (!sched_check_wake(&bridge_out_wake))
        return;
    if (uart_bridge_tx_free() < USB_CDC2_EP_BULK_OUT_SIZE)
        // No room for a full packet. Leaving the packet in the endpoint
        // makes the host wait (NAK); the TX interrupt wakes us later.
        return;
    uint8_t pkt[USB_CDC2_EP_BULK_OUT_SIZE];
    int_fast8_t ret = usb_read_bulk_out2(pkt, sizeof(pkt));
    if (ret < 0)
        return;               // nothing received; interrupt armed
    if (ret > 0)
        uart_bridge_tx_write(pkt, ret);
    usb_notify_bulk_out2();   // the other bank may hold another packet
}
DECL_TASK(usb_bridge_out_task);
