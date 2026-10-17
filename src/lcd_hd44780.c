// Commands for sending messages to a 4-bit hd44780 lcd driver
//
// Copyright (C) 2018  Kevin O'Connor <kevin@koconnor.net>
// Copyright (C) 2025  Mark (rewritten with correct HD44780 timings)
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_MACH_AVR
#include "basecmd.h" // oid_alloc
#include "board/gpio.h" // gpio_out_write
#include "board/irq.h" // irq_disable
#include "board/misc.h" // timer_from_us
#include "command.h" // DECL_COMMAND
#include "sched.h" // DECL_SHUTDOWN

struct hd44780 {
    // ready_time: earliest timer value at which the chip can accept the next
    // byte/nibble. The wait lengths below are enforced here on the mcu, as
    // the host's minclock only gates when a message is *sent*, not when the
    // mcu executes it (messages are batched, so gaps under a few ms are lost).
    uint32_t ready_time, cmd_wait_ticks, long_wait_ticks, init_wait_ticks;
    uint32_t max_wait_ticks;
    struct gpio_out rs, e, d4, d5, d6, d7;
};


/****************************************************************
 * Timing functions
 ****************************************************************/

// Convert microseconds to timer ticks
static inline uint32_t
usecs_to_ticks(uint32_t us)
{
    return timer_from_us(us);
}

// Blocking delay for specified number of ticks
static void
delay_ticks(uint32_t ticks)
{
    uint32_t end = timer_read_time() + ticks;
    while (timer_is_before(timer_read_time(), end))
        irq_poll();
}


/****************************************************************
 * Low-level HD44780 communication
 ****************************************************************/

// Write a single nibble (4 bits) to the display
// This pulses the Enable pin high, sets data, then low
static void
hd44780_write_nibble(struct hd44780 *h, uint8_t nibble)
{
    // Set data pins (directly, no toggle optimization)
    gpio_out_write(h->d4, (nibble >> 0) & 1);
    gpio_out_write(h->d5, (nibble >> 1) & 1);
    gpio_out_write(h->d6, (nibble >> 2) & 1);
    gpio_out_write(h->d7, (nibble >> 3) & 1);

    // Data setup time: minimum 60ns (we use ~1µs for safety)
    delay_ticks(usecs_to_ticks(1));

    // Pulse Enable high
    gpio_out_write(h->e, 1);

    // Enable pulse width: minimum 450ns (we use 1µs for safety)
    delay_ticks(usecs_to_ticks(1));

    // Pulse Enable low
    gpio_out_write(h->e, 0);

    // Enable cycle time: minimum 1µs total
    // Data hold time after E falls: minimum 10ns
    delay_ticks(usecs_to_ticks(1));
}

// Write a full byte (8 bits) as two nibbles, high nibble first
static void
hd44780_write_byte(struct hd44780 *h, uint8_t data)
{
    // Send high nibble first
    hd44780_write_nibble(h, data >> 4);

    // Inter-nibble delay (not specified in datasheet, but some displays need it)
    delay_ticks(usecs_to_ticks(1));

    // Send low nibble
    hd44780_write_nibble(h, data & 0x0F);
}

// Wait until the chip has finished its previous operation.
// ready_time - now is evaluated as unsigned: any result larger than the
// longest possible wait means ready_time is already in the past. (A signed
// timer_is_before() check would hang for ~18s on a 120MHz mcu if the display
// was idle for more than 2^31 ticks.)
static void
hd44780_wait_ready(struct hd44780 *h)
{
    uint32_t remaining = h->ready_time - timer_read_time();
    if (remaining && remaining <= h->max_wait_ticks)
        delay_ticks(remaining);
}

// Transmit a series of bytes to the chip.
// Each byte waits for the previous one to finish, and sets how long the
// chip needs to process it: Clear/Home take ~1.5ms, everything else ~40us.
static void
hd44780_xmit(struct hd44780 *h, uint8_t rs, uint8_t len, uint8_t *data)
{
    while (len--) {
        uint8_t b = *data++;
        hd44780_wait_ready(h);
        hd44780_write_byte(h, b);
        uint32_t wait = h->cmd_wait_ticks;
        if (!rs && (b == 0x01 || (b & 0xFE) == 0x02))
            wait = h->long_wait_ticks;
        h->ready_time = timer_read_time() + wait;
    }
}


/****************************************************************
 * Command interface
 ****************************************************************/

void
command_config_hd44780(uint32_t *args)
{
    struct hd44780 *h = oid_alloc(args[0], command_config_hd44780, sizeof(*h));

    // Initialize all pins as outputs, active low
    h->rs = gpio_out_setup(args[1], 0);
    h->e = gpio_out_setup(args[2], 0);
    h->d4 = gpio_out_setup(args[3], 0);
    h->d5 = gpio_out_setup(args[4], 0);
    h->d6 = gpio_out_setup(args[5], 0);
    h->d7 = gpio_out_setup(args[6], 0);

    // Store delay ticks for command execution time
    h->cmd_wait_ticks = args[7];
    h->long_wait_ticks = usecs_to_ticks(2000);  // Clear/Home: 1.52ms
    h->init_wait_ticks = usecs_to_ticks(5000);  // wake-up nibbles: 4.1ms
    h->max_wait_ticks = usecs_to_ticks(10000);
    h->ready_time = timer_read_time();
}
DECL_COMMAND(command_config_hd44780,
             "config_hd44780 oid=%c rs_pin=%u e_pin=%u"
             " d4_pin=%u d5_pin=%u d6_pin=%u d7_pin=%u delay_ticks=%u");

// Send initialization nibbles (single 4-bit values)
// Used during the 8-bit to 4-bit mode switch sequence
void
command_hd44780_send_init(uint32_t *args)
{
    struct hd44780 *h = oid_lookup(args[0], command_config_hd44780);
    uint8_t len = args[1];
    uint8_t *nibbles = command_decode_ptr(args[2]);

    // RS = 0 for command mode
    gpio_out_write(h->rs, 0);

    // Send each nibble individually (not as byte pairs). The chip needs
    // more than 4.1ms after the first wake-up nibble and 100us after the
    // others; use the longer wait for all of them, enforced here.
    while (len--) {
        hd44780_wait_ready(h);
        hd44780_write_nibble(h, *nibbles++);
        h->ready_time = timer_read_time() + h->init_wait_ticks;
    }
}
DECL_COMMAND(command_hd44780_send_init, "hd44780_send_init oid=%c nibbles=%*s");

// Send command bytes (RS = 0)
void
command_hd44780_send_cmds(uint32_t *args)
{
    struct hd44780 *h = oid_lookup(args[0], command_config_hd44780);

    // RS = 0 for command mode
    gpio_out_write(h->rs, 0);

    uint8_t len = args[1];
    uint8_t *cmds = command_decode_ptr(args[2]);
    hd44780_xmit(h, 0, len, cmds);
}
DECL_COMMAND(command_hd44780_send_cmds, "hd44780_send_cmds oid=%c cmds=%*s");

// Send data bytes (RS = 1)
void
command_hd44780_send_data(uint32_t *args)
{
    struct hd44780 *h = oid_lookup(args[0], command_config_hd44780);

    // RS = 1 for data mode
    gpio_out_write(h->rs, 1);

    uint8_t len = args[1];
    uint8_t *data = command_decode_ptr(args[2]);
    hd44780_xmit(h, 1, len, data);
}
DECL_COMMAND(command_hd44780_send_data, "hd44780_send_data oid=%c data=%*s");

void
hd44780_shutdown(void)
{
    uint8_t i;
    struct hd44780 *h;
    foreach_oid(i, h, command_config_hd44780) {
        gpio_out_write(h->rs, 0);
        gpio_out_write(h->e, 0);
        gpio_out_write(h->d4, 0);
        gpio_out_write(h->d5, 0);
        gpio_out_write(h->d6, 0);
        gpio_out_write(h->d7, 0);
    }
}
DECL_SHUTDOWN(hd44780_shutdown);
