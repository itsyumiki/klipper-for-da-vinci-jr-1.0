// Main starting point for LPC1115 boards.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "board/armcm_boot.h" // armcm_main
#include "internal.h" // PIO0_IRQn
#include "sched.h" // sched_main

void DefaultHandler(void);

// Size the vector table to cover every LPC11xx interrupt (the last one
// is PIO0).  Unused entries point to DefaultHandler.
DECL_ARMCM_IRQ(DefaultHandler, PIO0_IRQn);

// Run from the 12MHz internal RC oscillator (CONFIG_CLOCK_FREQ), with
// no AHB divider.  This is also the reset default, but set it
// explicitly in case a bootloader changed it.
static void
clock_setup(void)
{
    LPC_SYSCON_MAINCLKSEL = 0; // IRC oscillator
    LPC_SYSCON_MAINCLKUEN = 0; // toggle to latch the selection
    LPC_SYSCON_MAINCLKUEN = 1;
    LPC_SYSCON_SYSAHBCLKDIV = 1;
}

// Main entry point - called from armcm_boot.c
void
armcm_main(void)
{
    clock_setup();
    sched_main();
}
