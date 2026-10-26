#ifndef __LPC1115_INTERNAL_H
#define __LPC1115_INTERNAL_H
// Local definitions for lpc1115 code

// CMSIS core configuration for the LPC11xx (Cortex-M0)
#define __CM0_REV                0
#define __MPU_PRESENT            0
#define __NVIC_PRIO_BITS         2
#define __Vendor_SysTickConfig   0

// LPC11xx interrupt numbers (see UM10398 "Nested Vectored Interrupt
// Controller").  Only the numbering is defined here; peripheral
// register definitions will be added as the drivers are written.
typedef enum IRQn {
    NonMaskableInt_IRQn = -14,
    HardFault_IRQn      = -13,
    SVCall_IRQn         = -5,
    PendSV_IRQn         = -2,
    SysTick_IRQn        = -1,
    WAKEUP0_IRQn  = 0,   // WAKEUP1..WAKEUP12 follow as 1..12
    SSP1_IRQn     = 14,
    I2C0_IRQn     = 15,
    CT16B0_IRQn   = 16,
    CT16B1_IRQn   = 17,
    CT32B0_IRQn   = 18,
    CT32B1_IRQn   = 19,
    SSP0_IRQn     = 20,
    UART0_IRQn    = 21,
    ADC_IRQn      = 24,
    WDT_IRQn      = 25,
    BOD_IRQn      = 26,
    PIO3_IRQn     = 28,
    PIO2_IRQn     = 29,
    PIO1_IRQn     = 30,
    PIO0_IRQn     = 31,
} IRQn_Type;

#include "core_cm0.h" // NVIC_EnableIRQ

#include <stddef.h> // offsetof
#include <stdint.h> // uint32_t

// System control (SYSCON) AHB clock enable register, UM10398
#define LPC_SYSCON_SYSAHBCLKCTRL (*(volatile uint32_t *)0x40048080u)
#define SYSAHBCLK_CT16B0 (1u << 7)
#define SYSAHBCLK_CT16B1 (1u << 8)
#define SYSAHBCLK_CT32B0 (1u << 9)
#define SYSAHBCLK_CT32B1 (1u << 10)

// 16/32 bit counter/timer registers (same layout for all four)
typedef struct {
    volatile uint32_t IR;      // 0x00 interrupt register (write 1 to clear)
    volatile uint32_t TCR;     // 0x04 control: bit0 enable, bit1 reset
    volatile uint32_t TC;      // 0x08 timer counter
    volatile uint32_t PR;      // 0x0C prescale
    volatile uint32_t PC;      // 0x10 prescale counter
    volatile uint32_t MCR;     // 0x14 match control
    volatile uint32_t MR[4];   // 0x18 match registers
    volatile uint32_t CCR;     // 0x28 capture control
    volatile uint32_t CR[4];   // 0x2C capture registers
    volatile uint32_t EMR;     // 0x3C external match
    volatile uint32_t RESERVED[12];
    volatile uint32_t CTCR;    // 0x70 count control
    volatile uint32_t PWMC;    // 0x74 PWM control
} LPC_CT_TypeDef;
_Static_assert(offsetof(LPC_CT_TypeDef, MR) == 0x18, "bad CT layout");
_Static_assert(offsetof(LPC_CT_TypeDef, EMR) == 0x3C, "bad CT layout");
_Static_assert(offsetof(LPC_CT_TypeDef, CTCR) == 0x70, "bad CT layout");

#define LPC_CT16B0 ((LPC_CT_TypeDef *)0x4000C000u)
#define LPC_CT16B1 ((LPC_CT_TypeDef *)0x40010000u)
#define LPC_CT32B0 ((LPC_CT_TypeDef *)0x40014000u)
#define LPC_CT32B1 ((LPC_CT_TypeDef *)0x40018000u)

// More SYSCON registers, UM10398
#define LPC_SYSCON_SYSAHBCLKDIV (*(volatile uint32_t *)0x40048078u)
#define LPC_SYSCON_MAINCLKSEL   (*(volatile uint32_t *)0x40048070u)
#define LPC_SYSCON_MAINCLKUEN   (*(volatile uint32_t *)0x40048074u)
#define LPC_SYSCON_UARTCLKDIV   (*(volatile uint32_t *)0x40048098u)
#define SYSAHBCLK_UART  (1u << 12)
#define SYSAHBCLK_IOCON (1u << 16)

// IOCON registers for the UART pins (bits 2:0 select the function)
#define LPC_IOCON_PIO1_6 (*(volatile uint32_t *)0x400440A4u) // RXD
#define LPC_IOCON_PIO1_7 (*(volatile uint32_t *)0x400440A8u) // TXD
#define IOCON_FUNC_MASK 0x07u
#define IOCON_PIO1_6_FUNC_RXD 1u
#define IOCON_PIO1_7_FUNC_TXD 1u

// UART registers
typedef struct {
    union {
        volatile uint32_t RBR; // 0x00 receive buffer (DLAB=0, read)
        volatile uint32_t THR; // 0x00 transmit holding (DLAB=0, write)
        volatile uint32_t DLL; // 0x00 divisor latch low (DLAB=1)
    };
    union {
        volatile uint32_t IER; // 0x04 interrupt enable (DLAB=0)
        volatile uint32_t DLM; // 0x04 divisor latch high (DLAB=1)
    };
    union {
        volatile uint32_t IIR; // 0x08 interrupt id (read)
        volatile uint32_t FCR; // 0x08 fifo control (write)
    };
    volatile uint32_t LCR;     // 0x0C line control
    volatile uint32_t MCR;     // 0x10 modem control
    volatile uint32_t LSR;     // 0x14 line status
    volatile uint32_t MSR;     // 0x18 modem status
    volatile uint32_t SCR;     // 0x1C scratch
    volatile uint32_t ACR;     // 0x20 auto-baud control
    volatile uint32_t ICR;     // 0x24 irda control
    volatile uint32_t FDR;     // 0x28 fractional divider
    volatile uint32_t OSR;     // 0x2C oversampling (LPC11xx: reserved)
    volatile uint32_t TER;     // 0x30 transmit enable
} LPC_UART_TypeDef;
_Static_assert(offsetof(LPC_UART_TypeDef, LSR) == 0x14, "bad UART layout");
_Static_assert(offsetof(LPC_UART_TypeDef, FDR) == 0x28, "bad UART layout");
_Static_assert(offsetof(LPC_UART_TypeDef, TER) == 0x30, "bad UART layout");

#define LPC_UART0 ((LPC_UART_TypeDef *)0x40008000u)

// GPIO.  Pin ids are GPIO(port, num) with 32 ids reserved per port.  The
// LPC1115 has ports 0-2 with 12 pins (PIOn_0..PIOn_11) and port 3 with 6
// pins (PIO3_0..PIO3_5); which of them are bonded out depends on the
// package.
#define SYSAHBCLK_GPIO (1u << 6)
#define LPC_IOCON_BASE 0x40044000u
#define LPC_GPIO_PORT_BASE(port) (0x50000000u + ((uint32_t)(port) << 16))
#define GPIO(PORT, NUM) (((PORT) << 5) | (NUM))
#define GPIO2PORT(PIN) ((PIN) >> 5)
#define GPIO2NUM(PIN) ((PIN) & 0x1f)
#define GPIO2BIT(PIN) (1u << GPIO2NUM(PIN))

// GPIO port registers (UM10398).  The data register uses masked access:
// a read or write at base + (mask << 2) only touches the bits in mask.
#define LPC_GPIO_DIR_OFFSET 0x8000u
#define LPC_GPIO_DATA(regs, mask) \
    (*(volatile uint32_t *)((uint32_t)(regs) + ((uint32_t)(mask) << 2)))
#define LPC_GPIO_DIR(regs) \
    (*(volatile uint32_t *)((uint32_t)(regs) + LPC_GPIO_DIR_OFFSET))

// Set the IOCON function and pull mode of a pin as a digital pin
// (pull_up: >0 pull-up, <0 pull-down, 0 none).  func is the raw IOCON
// FUNC value, or GPIO_FUNC_PIO for the plain GPIO function (which is
// function 0 on most pins but function 1 on the debug/reset pins).
#define GPIO_FUNC_PIO 0xffu
void gpio_peripheral(uint32_t pin, uint32_t func, int8_t pull_up);

// Select an analog function on a pad: sets the IOCON function, clears the
// pull mode and puts the pad in analog mode (ADMODE=0).
void gpio_analog(uint32_t pin, uint32_t func);

// More SYSCON registers and clock gates used by the peripheral drivers
#define LPC_SYSCON_PRESETCTRL   (*(volatile uint32_t *)0x40048004u)
#define LPC_SYSCON_SSP0CLKDIV   (*(volatile uint32_t *)0x40048094u)
#define LPC_SYSCON_SSP1CLKDIV   (*(volatile uint32_t *)0x4004809cu)
#define LPC_SYSCON_PDRUNCFG     (*(volatile uint32_t *)0x40048238u)
// PRESETCTRL: a 0 bit holds the peripheral in reset (the power-up state)
#define PRESET_SSP0 (1u << 0)
#define PRESET_I2C  (1u << 1)
#define PRESET_SSP1 (1u << 2)
#define PDRUNCFG_ADC (1u << 4)
#define SYSAHBCLK_I2C  (1u << 5)
#define SYSAHBCLK_SSP0 (1u << 11)
#define SYSAHBCLK_ADC  (1u << 13)
#define SYSAHBCLK_SSP1 (1u << 18)

// IOCON pin location select registers for the SSP pins (UM10398)
#define LPC_IOCON_SCK0_LOC (*(volatile uint32_t *)0x400440b0u)
#define IOCON_SCK0_LOC_PIO0_10 0u
#define IOCON_SCK0_LOC_PIO2_11 1u
#define IOCON_SCK0_LOC_PIO0_6  2u

// ADC registers (UM10398 "LPC111x ADC")
typedef struct {
    volatile uint32_t CR;      // 0x00 control
    volatile uint32_t GDR;     // 0x04 global data
    volatile uint32_t RESERVED0;
    volatile uint32_t INTEN;   // 0x0C interrupt enable
    volatile uint32_t DR[8];   // 0x10 channel data registers
    volatile uint32_t STAT;    // 0x30 status
} LPC_ADC_TypeDef;
_Static_assert(offsetof(LPC_ADC_TypeDef, INTEN) == 0x0c, "bad ADC layout");
_Static_assert(offsetof(LPC_ADC_TypeDef, STAT) == 0x30, "bad ADC layout");
#define LPC_ADC ((LPC_ADC_TypeDef *)0x4001c000u)

// SSP (SPI) registers
typedef struct {
    volatile uint32_t CR0;     // 0x00 control 0
    volatile uint32_t CR1;     // 0x04 control 1
    volatile uint32_t DR;      // 0x08 data
    volatile uint32_t SR;      // 0x0C status
    volatile uint32_t CPSR;    // 0x10 clock prescale
    volatile uint32_t IMSC;    // 0x14 interrupt mask
    volatile uint32_t RIS;     // 0x18 raw interrupt status
    volatile uint32_t MIS;     // 0x1C masked interrupt status
    volatile uint32_t ICR;     // 0x20 interrupt clear
} LPC_SSP_TypeDef;
_Static_assert(offsetof(LPC_SSP_TypeDef, CPSR) == 0x10, "bad SSP layout");
#define LPC_SSP0 ((LPC_SSP_TypeDef *)0x40040000u)
#define LPC_SSP1 ((LPC_SSP_TypeDef *)0x40058000u)

// I2C registers
typedef struct {
    volatile uint32_t CONSET;  // 0x00 control set
    volatile uint32_t STAT;    // 0x04 status
    volatile uint32_t DAT;     // 0x08 data
    volatile uint32_t ADR0;    // 0x0C slave address 0
    volatile uint32_t SCLH;    // 0x10 SCL high duty
    volatile uint32_t SCLL;    // 0x14 SCL low duty
    volatile uint32_t CONCLR;  // 0x18 control clear
} LPC_I2C_TypeDef;
_Static_assert(offsetof(LPC_I2C_TypeDef, SCLL) == 0x14, "bad I2C layout");
_Static_assert(offsetof(LPC_I2C_TypeDef, CONCLR) == 0x18, "bad I2C layout");
#define LPC_I2C0 ((LPC_I2C_TypeDef *)0x40000000u)

#endif // internal.h
