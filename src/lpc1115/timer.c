// LPC1115 timer interrupt scheduling
//
// Klipper's scheduler clock is the free-running 32bit counter CT32B1,
// clocked at the core clock.  Match register 0 raises the interrupt for
// the next software timer.  (CT32B0 and the 16bit timers are left free
// for PWM.)
//
// The Cortex-M0 has no DWT cycle counter, so the generic armcm_timer.c
// can not be used here.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "board/armcm_boot.h" // armcm_enable_irq
#include "board/irq.h" // irq_disable
#include "board/misc.h" // timer_read_time
#include "board/timer_irq.h" // timer_dispatch_many
#include "internal.h" // LPC_CT32B1
#include "sched.h" // DECL_INIT

#define TMR LPC_CT32B1
#define TMR_IRQn CT32B1_IRQn
#define TMR_MATCH_MR0 (1 << 0) // IR/MCR bit for match register 0

// Set the next irq time
static void
timer_set(uint32_t value)
{
    // The counter only raises the irq when TC equals MR0, so value must
    // be a little in the future (timer_dispatch_many() ensures that).
    TMR->MR[0] = value;
    TMR->IR = TMR_MATCH_MR0;
}

// Return the current time (in absolute clock ticks).
uint32_t
timer_read_time(void)
{
    return TMR->TC;
}

// Activate timer dispatch as soon as possible
void
timer_kick(void)
{
    // Pend the interrupt directly.  Programming MR0 to "now + a bit"
    // could miss the match if the write took too long at 12MHz.
    NVIC_SetPendingIRQ(TMR_IRQn);
}

// IRQ handler
void __aligned(16)
CT32B1_Handler(void)
{
    irq_disable();
    uint32_t next = timer_dispatch_many();
    timer_set(next);
    irq_enable();
}

void
timer_init(void)
{
    // Supply clock to the timer
    LPC_SYSCON_SYSAHBCLKCTRL |= SYSAHBCLK_CT32B1;

    irqstatus_t flag = irq_save();
    TMR->TCR = 2; // hold in reset
    TMR->CTCR = 0; // timer mode (count PCLK cycles)
    TMR->PR = 0; // no prescaler: one tick per core clock
    TMR->MCR = TMR_MATCH_MR0; // irq on MR0 match; do not reset or stop
    TMR->IR = 0x1f; // clear any stale flags
    armcm_enable_irq(CT32B1_Handler, TMR_IRQn, 2);
    timer_kick();
    TMR->TCR = 1; // release reset and start counting
    irq_restore(flag);
}
DECL_INIT(timer_init);
