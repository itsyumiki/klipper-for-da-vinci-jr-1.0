// Analog to digital support on lpc1115
//
// The LPC111x ADC is a single 10-bit successive approximation converter
// with 8 multiplexed channels.  Conversions are software started one
// channel at a time and polled via the DONE flags, so no interrupt is
// needed.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_CLOCK_FREQ
#include "board/irq.h" // irq_save
#include "board/misc.h" // timer_from_us
#include "command.h" // shutdown
#include "gpio.h" // gpio_adc_setup
#include "internal.h" // gpio_analog
#include "sched.h" // sched_shutdown

// Channel n is on pin adc_pins[n].  The AD function is IOCON function 2
// on the five R/SWD pads and function 1 on PIO1_4, PIO1_10 and PIO1_11.
struct adc_pin_info {
    uint8_t pin, func;
};
static const struct adc_pin_info adc_pins[] = {
    { GPIO(0, 11), 2 }, // AD0
    { GPIO(1, 0), 2 },  // AD1
    { GPIO(1, 1), 2 },  // AD2
    { GPIO(1, 2), 2 },  // AD3
    { GPIO(1, 3), 2 },  // AD4 (shares the SWDIO pad)
    { GPIO(1, 4), 1 },  // AD5
    { GPIO(1, 10), 1 }, // AD6
    { GPIO(1, 11), 1 }, // AD7
};

DECL_CONSTANT("ADC_MAX", 1023);

// The ADC clock must not exceed 4.5MHz.  The ADC runs from the AHB
// clock (CLOCK_FREQ) divided by (CLKDIV + 1).
#define ADC_FREQ_MAX 4500000
#define ADC_CLKDIV DIV_ROUND_UP(CONFIG_CLOCK_FREQ, ADC_FREQ_MAX)
// A 10-bit conversion takes 11 ADC clocks
#define ADC_CONV_TICKS (11 * ADC_CLKDIV)

enum {
    CR_SEL_MASK = 0xff,
    CR_CLKDIV_SHIFT = 8,
    CR_BURST = 1 << 16,
    CR_PDN = 1 << 21,
    CR_START_NOW = 1 << 24,
    DR_DONE = 1u << 31,
};

#define ADC_IDLE 0xff
static uint8_t adc_chan = ADC_IDLE;
static uint32_t adc_cr; // control register with no channel and no start

struct gpio_adc
gpio_adc_setup(uint8_t pin)
{
    // Find pin in adc_pins table
    uint32_t chan;
    for (chan=0; ; chan++) {
        if (chan >= ARRAY_SIZE(adc_pins))
            shutdown("Not a valid ADC pin");
        if (adc_pins[chan].pin == pin)
            break;
    }

    if (!adc_cr) {
        // Power up the ADC and enable its clock
        irqstatus_t flag = irq_save();
        LPC_SYSCON_PDRUNCFG &= ~PDRUNCFG_ADC;
        LPC_SYSCON_SYSAHBCLKCTRL |= SYSAHBCLK_ADC;
        irq_restore(flag);
        adc_cr = ((ADC_CLKDIV - 1) << CR_CLKDIV_SHIFT) | CR_PDN;
        LPC_ADC->CR = adc_cr;
        LPC_ADC->INTEN = 0; // polled operation, no adc interrupts
    }

    gpio_analog(pin, adc_pins[chan].func);

    return (struct gpio_adc){ .chan = chan };
}

// Try to sample a value. Returns zero if sample ready, otherwise
// returns the number of clock ticks the caller should wait before
// retrying this function.
uint32_t
gpio_adc_sample(struct gpio_adc g)
{
    uint32_t chan = adc_chan;
    if (chan == g.chan) {
        // Sample already underway - check if it is ready
        if (LPC_ADC->STAT & (1 << g.chan))
            return 0;
        goto need_delay;
    }
    if (chan != ADC_IDLE)
        // ADC busy on some other channel
        goto need_delay;

    // Start new sample
    (void)LPC_ADC->DR[g.chan]; // clear any stale DONE flag
    adc_chan = g.chan;
    LPC_ADC->CR = adc_cr | (1 << g.chan) | CR_START_NOW;

need_delay:
    return ADC_CONV_TICKS + timer_from_us(2);
}

// Read a value; use only after gpio_adc_sample() returns zero
uint16_t
gpio_adc_read(struct gpio_adc g)
{
    uint32_t result = LPC_ADC->DR[g.chan]; // also clears the DONE flag
    LPC_ADC->CR = adc_cr; // deselect channel and clear the start bits
    adc_chan = ADC_IDLE;
    return (result >> 6) & 0x3ff;
}

// Cancel a sample that was started with gpio_adc_sample()
void
gpio_adc_cancel_sample(struct gpio_adc g)
{
    irqstatus_t flag = irq_save();
    if (adc_chan == g.chan) {
        LPC_ADC->CR = adc_cr;
        (void)LPC_ADC->DR[g.chan]; // clear the DONE flag
        adc_chan = ADC_IDLE;
    }
    irq_restore(flag);
}
