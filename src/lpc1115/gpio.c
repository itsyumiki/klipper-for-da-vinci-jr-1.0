// GPIO functions on lpc1115
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "board/irq.h" // irq_save
#include "command.h" // shutdown
#include "gpio.h" // gpio_out_setup
#include "internal.h" // gpio_peripheral
#include "sched.h" // sched_shutdown


/****************************************************************
 * Pin mappings
 ****************************************************************/

// Ports 0-2 have 12 pins, port 3 has 6.  Which pins exist depends on the
// package; the host is responsible for choosing pins that are bonded out.
DECL_ENUMERATION_RANGE("pin", "PIO0_0", GPIO(0, 0), 12);
DECL_ENUMERATION_RANGE("pin", "PIO1_0", GPIO(1, 0), 12);
DECL_ENUMERATION_RANGE("pin", "PIO2_0", GPIO(2, 0), 12);
DECL_ENUMERATION_RANGE("pin", "PIO3_0", GPIO(3, 0), 6);

// IOCON register offset (in 32-bit words) for each PIOx_y pin.  The
// IOCON registers are not laid out in pin order (UM10398, "I/O
// configuration registers").  NO_IOCON marks pins that do not exist.
#define NO_IOCON 0xff
static const uint8_t iocon_word[4][12] = {
    { 0x0c/4, 0x10/4, 0x1c/4, 0x2c/4, 0x30/4, 0x34/4,      // PIO0_0..5
      0x4c/4, 0x50/4, 0x60/4, 0x64/4, 0x68/4, 0x74/4 },    // PIO0_6..11
    { 0x78/4, 0x7c/4, 0x80/4, 0x90/4, 0x94/4, 0xa0/4,      // PIO1_0..5
      0xa4/4, 0xa8/4, 0x14/4, 0x38/4, 0x6c/4, 0x98/4 },    // PIO1_6..11
    { 0x08/4, 0x28/4, 0x5c/4, 0x8c/4, 0x40/4, 0x44/4,      // PIO2_0..5
      0x00/4, 0x20/4, 0x24/4, 0x54/4, 0x58/4, 0x70/4 },    // PIO2_6..11
    { 0x84/4, 0x88/4, 0x9c/4, 0xac/4, 0x3c/4, 0x48/4,      // PIO3_0..5
      NO_IOCON, NO_IOCON, NO_IOCON, NO_IOCON, NO_IOCON, NO_IOCON },
};

static int
pin_valid(uint32_t pin)
{
    uint32_t port = GPIO2PORT(pin), num = GPIO2NUM(pin);
    return port < 4 && num < 12 && iocon_word[port][num] != NO_IOCON;
}

// The plain GPIO function is IOCON function 0 on most pins, but function
// 1 on the pins that power up as reset or debug pins (PIO0_0 reset,
// PIO0_10 SWCLK, PIO0_11 TDI, PIO1_0 TMS, PIO1_1 TDO, PIO1_2 nTRST,
// PIO1_3 SWDIO).
static uint32_t
pio_func(uint32_t pin)
{
    switch (pin) {
    case GPIO(0, 0): case GPIO(0, 10): case GPIO(0, 11):
    case GPIO(1, 0): case GPIO(1, 1): case GPIO(1, 2): case GPIO(1, 3):
        return 1;
    }
    return 0;
}

// Pins that share their pad with an ADC input have an ADMODE bit (bit 7)
// that must be 1 for digital operation.
static int
pin_has_admode(uint32_t pin)
{
    switch (pin) {
    case GPIO(0, 11): case GPIO(1, 0): case GPIO(1, 1): case GPIO(1, 2):
    case GPIO(1, 3): case GPIO(1, 4): case GPIO(1, 10): case GPIO(1, 11):
        return 1;
    }
    return 0;
}

// PIO0_4 and PIO0_5 are the I2C pads: no pull modes, and as GPIO they are
// open drain (they can only drive low).
static int
pin_has_mode(uint32_t pin)
{
    return pin != GPIO(0, 4) && pin != GPIO(0, 5);
}

#define IOCON_MODE_SHIFT 3
#define IOCON_MODE_MASK (0x03u << IOCON_MODE_SHIFT)
#define IOCON_ADMODE_DIGITAL (1u << 7)

// Enable the GPIO and IOCON clocks (idempotent)
static void
gpio_clocks_enable(void)
{
    irqstatus_t flag = irq_save();
    LPC_SYSCON_SYSAHBCLKCTRL |= SYSAHBCLK_GPIO | SYSAHBCLK_IOCON;
    irq_restore(flag);
}

// Set the function and pull mode of a pin (digital operation).  Note that
// selecting the GPIO function on PIO0_10 or PIO1_3 disables SWD.
void
gpio_peripheral(uint32_t pin, uint32_t func, int8_t pull_up)
{
    volatile uint32_t *reg = (volatile uint32_t *)
        (LPC_IOCON_BASE + ((uint32_t)iocon_word[GPIO2PORT(pin)][GPIO2NUM(pin)]
                           << 2));
    uint32_t mask = IOCON_FUNC_MASK;
    uint32_t val = (func == GPIO_FUNC_PIO ? pio_func(pin) : func)
        & IOCON_FUNC_MASK;
    if (pin_has_mode(pin)) {
        uint32_t mode = pull_up > 0 ? 2 : (pull_up < 0 ? 1 : 0);
        mask |= IOCON_MODE_MASK;
        val |= mode << IOCON_MODE_SHIFT;
    }
    if (pin_has_admode(pin)) {
        mask |= IOCON_ADMODE_DIGITAL;
        val |= IOCON_ADMODE_DIGITAL;
    }
    gpio_clocks_enable();
    irqstatus_t flag = irq_save();
    *reg = (*reg & ~mask) | val;
    irq_restore(flag);
}

// Select an analog function on a pad.  Only the pads that share an ADC
// input have an ADMODE bit; it is cleared here to enable analog mode.
void
gpio_analog(uint32_t pin, uint32_t func)
{
    volatile uint32_t *reg = (volatile uint32_t *)
        (LPC_IOCON_BASE + ((uint32_t)iocon_word[GPIO2PORT(pin)][GPIO2NUM(pin)]
                           << 2));
    gpio_clocks_enable();
    irqstatus_t flag = irq_save();
    *reg = (*reg & ~(IOCON_FUNC_MASK | IOCON_MODE_MASK | IOCON_ADMODE_DIGITAL))
        | (func & IOCON_FUNC_MASK);
    irq_restore(flag);
}

// Convert a register base and bit mask back to an integer pin identifier
static uint32_t
regs_to_pin(void *regs, uint32_t bit)
{
    uint32_t port = ((uint32_t)regs - LPC_GPIO_PORT_BASE(0)) >> 16;
    return GPIO(port, __builtin_ctz(bit));
}


/****************************************************************
 * General Purpose Input Output (GPIO) pins
 ****************************************************************/

struct gpio_out
gpio_out_setup(uint8_t pin, uint8_t val)
{
    if (!pin_valid(pin))
        goto fail;
    struct gpio_out g = { .regs=(void*)LPC_GPIO_PORT_BASE(GPIO2PORT(pin)),
                          .bit=GPIO2BIT(pin) };
    gpio_out_reset(g, val);
    return g;
fail:
    shutdown("Not an output pin");
}

void
gpio_out_reset(struct gpio_out g, uint8_t val)
{
    uint32_t pin = regs_to_pin(g.regs, g.bit);
    irqstatus_t flag = irq_save();
    gpio_clocks_enable();
    // Latch the level and direction first, then connect the pad, so
    // that the pin comes up at the requested level
    LPC_GPIO_DATA(g.regs, g.bit) = val ? g.bit : 0;
    LPC_GPIO_DIR(g.regs) |= g.bit;
    gpio_peripheral(pin, GPIO_FUNC_PIO, 0);
    irq_restore(flag);
}

// Note: the LPC11xx has no separate output latch readback; the masked
// data register reads the pad level.
void
gpio_out_toggle_noirq(struct gpio_out g)
{
    volatile uint32_t *data = &LPC_GPIO_DATA(g.regs, g.bit);
    *data = *data ^ g.bit;
}

void
gpio_out_toggle(struct gpio_out g)
{
    irqstatus_t flag = irq_save();
    gpio_out_toggle_noirq(g);
    irq_restore(flag);
}

void
gpio_out_write(struct gpio_out g, uint8_t val)
{
    LPC_GPIO_DATA(g.regs, g.bit) = val ? g.bit : 0;
}


struct gpio_in
gpio_in_setup(uint8_t pin, int8_t pull_up)
{
    if (!pin_valid(pin))
        goto fail;
    struct gpio_in g = { .regs=(void*)LPC_GPIO_PORT_BASE(GPIO2PORT(pin)),
                         .bit=GPIO2BIT(pin) };
    gpio_in_reset(g, pull_up);
    return g;
fail:
    shutdown("Not an input pin");
}

void
gpio_in_reset(struct gpio_in g, int8_t pull_up)
{
    uint32_t pin = regs_to_pin(g.regs, g.bit);
    irqstatus_t flag = irq_save();
    gpio_clocks_enable();
    LPC_GPIO_DIR(g.regs) &= ~g.bit;
    gpio_peripheral(pin, GPIO_FUNC_PIO, pull_up);
    irq_restore(flag);
}

uint8_t
gpio_in_read(struct gpio_in g)
{
    return !!LPC_GPIO_DATA(g.regs, g.bit);
}
