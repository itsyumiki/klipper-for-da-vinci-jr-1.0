#ifndef __GENERIC_USB_CDC_BRIDGE_H
#define __GENERIC_USB_CDC_BRIDGE_H

#include <stdint.h> // uint8_t
#include "generic/usbstd_cdc.h" // struct usb_cdc_line_coding

// Provided by generic/usb_cdc_bridge.c
struct usb_cdc_line_coding *usb_bridge_get_line_coding(void);
void usb_bridge_set_line_coding(const struct usb_cdc_line_coding *lc);
void usb_bridge_set_control_state(uint8_t state);
void usb_bridge_notify_rx(void);
void usb_bridge_notify_tx_space(void);

// Provided by the board UART code (atsam/uart1_bridge.c)
uint_fast8_t uart_bridge_rx_read(uint8_t *buf, uint_fast8_t max_len);
uint_fast8_t uart_bridge_tx_free(void);
void uart_bridge_tx_write(const uint8_t *buf, uint_fast8_t len);
int uart_bridge_set_config(uint32_t baud, uint8_t cdc_parity);

#endif // usb_cdc_bridge.h
