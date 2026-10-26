// Hardware PWM support on lpc1115
//
// The LPC111x counter/timers have a PWM mode in which match register 3
// sets the cycle length and match registers 0-2 set the duty cycle of
// the MAT0-MAT2 outputs.  CT32B1 is used by the Klipper scheduler, so
// PWM is available on CT16B0, CT16B1 and CT32B0.  All pins on one timer
// share the same cycle time.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "command.h" // shutdown
#include "gpio.h" // gpio_pwm_write
#include "internal.h" // GPIO
#include "sched.h" // sched_shutdown

#define MAX_PWM 255
DECL_CONSTANT("PWM_MAX", MAX_PWM);

struct pwm_timer_info {
    LPC_CT_TypeDef *timer;
    uint32_t clkgate;
    uint32_t max_prescale; // largest PR+1 the prescale register can hold
};

static const struct pwm_timer_info pwm_timers[] = {
    { LPC_CT16B0, SYSAHBCLK_CT16B0, 1u << 16 },
    { LPC_CT16B1, SYSAHBCLK_CT16B1, 1u << 16 },
    { LPC_CT32B0, SYSAHBCLK_CT32B0, 0xffffffffu },
};

struct gpio_pwm_info {
    uint8_t pin, timer, match, function;
};

// PIO0_10 is the SWCLK pad and PIO1_6/PIO1_7 are the serial pins, so
// those are deliberately not offered here.
static const struct gpio_pwm_info pwm_pins[] = {
    // CT16B0
    { GPIO(0, 8),  0, 0, 2 }, { GPIO(3, 0), 0, 0, 2 },
    { GPIO(0, 9),  0, 1, 2 }, { GPIO(3, 1), 0, 1, 2 },
    { GPIO(3, 2),  0, 2, 2 },
    // CT16B1
    { GPIO(1, 9),  1, 0, 1 },
    { GPIO(1, 10), 1, 1, 2 }, { GPIO(2, 4), 1, 1, 1 },
    // CT32B0
    { GPIO(2, 5),  2, 0, 1 },
    { GPIO(2, 6),  2, 1, 1 },
    { GPIO(0, 1),  2, 2, 2 }, { GPIO(2, 7), 2, 2, 1 },
};

// PWM mode: MCR bit 10 resets the counter on a match of MR3
#define MCR_MR3_RESET (1u << 10)
#define TCR_ENABLE (1u << 0)
#define TCR_RESET (1u << 1)

struct gpio_pwm
gpio_pwm_setup(uint8_t pin, uint32_t cycle_time, uint8_t val)
{
    // Find pin in pwm_pins table
    const struct gpio_pwm_info *p = pwm_pins;
    for (;; p++) {
        if (p >= &pwm_pins[ARRAY_SIZE(pwm_pins)])
            shutdown("Not a valid PWM pin");
        if (p->pin == pin)
            break;
    }
    const struct pwm_timer_info *ti = &pwm_timers[p->timer];
    LPC_CT_TypeDef *t = ti->timer;

    // Map cycle_time to the timer prescaler.  The timer counts every
    // PR+1 clocks and one cycle is MAX_PWM counts.  Round to the nearest
    // value so the real cycle time is as close as possible to the request.
    uint32_t prescale = (cycle_time + MAX_PWM / 2) / MAX_PWM;
    if (prescale < 1)
        prescale = 1;
    if (prescale > ti->max_prescale)
        shutdown("PWM cycle time too long");

    gpio_peripheral(p->pin, p->function, 0);

    LPC_SYSCON_SYSAHBCLKCTRL |= ti->clkgate;

    if (t->TCR & TCR_ENABLE) {
        if (t->PR != prescale - 1)
            shutdown("PWM already programmed at different speed");
    } else {
        t->TCR = TCR_RESET;
        t->CTCR = 0; // timer mode
        t->PWMC = 0;
        t->PR = prescale - 1;
        // Counts 0..MAX_PWM-1, so a match value of MAX_PWM never occurs
        // and gives a 100% duty cycle; a value of 0 gives 0%.
        t->MR[3] = MAX_PWM - 1;
        t->MCR = MCR_MR3_RESET;
    }

    if (t->PWMC & (1u << p->match))
        shutdown("PWM output already in use");

    // Setup channel
    struct gpio_pwm channel = { .mr = &t->MR[p->match] };
    gpio_pwm_write(channel, val);

    // Enable the PWM output and (re)start the timer
    t->PWMC |= 1u << p->match;
    t->TCR = TCR_ENABLE;

    return channel;
}

void
gpio_pwm_write(struct gpio_pwm g, uint32_t val)
{
    *g.mr = val;
}
