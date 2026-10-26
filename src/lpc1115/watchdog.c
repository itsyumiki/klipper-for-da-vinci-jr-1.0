// Watchdog support on LPC1115
//
// The windowless watchdog timer is clocked from the 12MHz IRC (via the
// WDTCLKSEL mux) and then divided by 4 inside the watchdog block.  Once
// enabled it cannot be stopped until the next reset.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "board/irq.h" // irq_save
#include "board/misc.h" // timer_read_time
#include "internal.h" // LPC_SYSCON_SYSAHBCLKCTRL
#include "sched.h" // DECL_TASK

#define LPC_WDT_BASE 0x40004000u
#define WDT_MOD  (*(volatile uint32_t *)(LPC_WDT_BASE + 0x00))
#define WDT_TC   (*(volatile uint32_t *)(LPC_WDT_BASE + 0x04))
#define WDT_FEED (*(volatile uint32_t *)(LPC_WDT_BASE + 0x08))

#define WDMOD_WDEN    (1u << 0)
#define WDMOD_WDRESET (1u << 1)

#define SYSAHBCLK_WDT (1u << 15)
#define LPC_SYSCON_WDTCLKSEL (*(volatile uint32_t *)0x400480d0u)
#define LPC_SYSCON_WDTCLKUEN (*(volatile uint32_t *)0x400480d4u)
#define LPC_SYSCON_WDTCLKDIV (*(volatile uint32_t *)0x400480d8u)

// The IRC is always 12MHz, independent of the main clock setting.  The
// watchdog counts at that clock / 4, so 0.5s is 1500000 counts.
#define WDT_IRC_HZ 12000000u
#define WDT_TIMEOUT_TICKS (WDT_IRC_HZ / 4 / 2)

// The feed sequence must not be interrupted between the two writes
static void
watchdog_feed(void)
{
    irqstatus_t flag = irq_save();
    WDT_FEED = 0xAA;
    WDT_FEED = 0x55;
    irq_restore(flag);
}

void
watchdog_task(void)
{
    static uint32_t last_feed;
    uint32_t now = timer_read_time();
    // Feeding is cheap, but there is no need to do it on every pass
    if (timer_is_before(now, last_feed + timer_from_us(50000)))
        return;
    last_feed = now;
    watchdog_feed();
}
DECL_TASK(watchdog_task);

void
watchdog_init(void)
{
    LPC_SYSCON_SYSAHBCLKCTRL |= SYSAHBCLK_WDT;
    LPC_SYSCON_WDTCLKSEL = 0; // IRC oscillator
    LPC_SYSCON_WDTCLKUEN = 0; // toggle to latch the selection
    LPC_SYSCON_WDTCLKUEN = 1;
    LPC_SYSCON_WDTCLKDIV = 1;
    WDT_TC = WDT_TIMEOUT_TICKS;
    WDT_MOD = WDMOD_WDEN | WDMOD_WDRESET;
    watchdog_feed();
}
DECL_INIT(watchdog_init);
