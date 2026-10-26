// SPI support on lpc1115
//
// Uses the two SSP controllers in SPI master mode with 8-bit frames.
// The chip select is always a plain GPIO handled by the host.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_CLOCK_FREQ
#include "command.h" // shutdown
#include "gpio.h" // spi_setup
#include "internal.h" // gpio_peripheral
#include "sched.h" // sched_shutdown

// Pin sets: SSP0 can place SCK on one of three pads (selected by
// SCK0_LOC), while MISO0/MOSI0 are fixed on PIO0_8/PIO0_9.  "ssp0" uses
// PIO0_6 so that it does not collide with the SWCLK pad.  The two
// bus names that use the same SSP block cannot be used together.
struct spi_info {
    LPC_SSP_TypeDef *spi;
    uint8_t ssp;
    uint8_t miso_pin, mosi_pin, sck_pin;
    uint8_t miso_func, mosi_func, sck_func;
    uint8_t sck_loc; // SCK0_LOC value (SSP0 only)
};

DECL_ENUMERATION("spi_bus", "ssp0", 0);
DECL_CONSTANT_STR("BUS_PINS_ssp0", "PIO0_8,PIO0_9,PIO0_6");
DECL_ENUMERATION("spi_bus", "ssp0a", 1);
DECL_CONSTANT_STR("BUS_PINS_ssp0a", "PIO0_8,PIO0_9,PIO2_11");
DECL_ENUMERATION("spi_bus", "ssp0b", 2);
DECL_CONSTANT_STR("BUS_PINS_ssp0b", "PIO0_8,PIO0_9,PIO0_10");
DECL_ENUMERATION("spi_bus", "ssp1", 3);
DECL_CONSTANT_STR("BUS_PINS_ssp1", "PIO2_2,PIO2_3,PIO2_1");

static const struct spi_info spi_bus[] = {
    { LPC_SSP0, 0, GPIO(0, 8), GPIO(0, 9), GPIO(0, 6),
      1, 1, 2, IOCON_SCK0_LOC_PIO0_6 },
    { LPC_SSP0, 0, GPIO(0, 8), GPIO(0, 9), GPIO(2, 11),
      1, 1, 1, IOCON_SCK0_LOC_PIO2_11 },
    { LPC_SSP0, 0, GPIO(0, 8), GPIO(0, 9), GPIO(0, 10),
      1, 1, 2, IOCON_SCK0_LOC_PIO0_10 },
    { LPC_SSP1, 1, GPIO(2, 2), GPIO(2, 3), GPIO(2, 1),
      2, 2, 2, 0 },
};

#define SSP_CLK_DIV 1 // SSP clock = CLOCK_FREQ / 1
#define SSP_PCLK (CONFIG_CLOCK_FREQ / SSP_CLK_DIV)

enum {
    CR0_DSS_8BIT = 0x07,
    CR0_CPOL = 1 << 6, CR0_CPHA = 1 << 7,
    CR1_SSE = 1 << 1,
    SR_RNE = 1 << 2,
};

static void
spi_init(uint32_t bus)
{
    static int8_t ssp_bus_in_use[2] = { -1, -1 };
    const struct spi_info *info = &spi_bus[bus];
    int8_t used = ssp_bus_in_use[info->ssp];
    if (used == (int8_t)bus)
        return;
    if (used >= 0)
        shutdown("SSP already in use with different pins");
    ssp_bus_in_use[info->ssp] = bus;

    // Configure the MISO, MOSI and SCK pins
    gpio_peripheral(info->miso_pin, info->miso_func, 0);
    gpio_peripheral(info->mosi_pin, info->mosi_func, 0);
    if (info->ssp == 0)
        LPC_IOCON_SCK0_LOC = info->sck_loc;
    gpio_peripheral(info->sck_pin, info->sck_func, 0);

    // Enable clock and release the peripheral from reset
    if (info->ssp == 0) {
        LPC_SYSCON_SSP0CLKDIV = SSP_CLK_DIV;
        LPC_SYSCON_SYSAHBCLKCTRL |= SYSAHBCLK_SSP0;
        LPC_SYSCON_PRESETCTRL |= PRESET_SSP0;
    } else {
        LPC_SYSCON_SSP1CLKDIV = SSP_CLK_DIV;
        LPC_SYSCON_SYSAHBCLKCTRL |= SYSAHBCLK_SSP1;
        LPC_SYSCON_PRESETCTRL |= PRESET_SSP1;
    }

    // Set initial registers (master mode, 8 bit frames)
    LPC_SSP_TypeDef *spi = info->spi;
    spi->CR0 = CR0_DSS_8BIT;
    spi->CPSR = 254;
    spi->CR1 = CR1_SSE;
}

struct spi_config
spi_setup(uint32_t bus, uint8_t mode, uint32_t rate)
{
    if (bus >= ARRAY_SIZE(spi_bus) || mode > 3)
        shutdown("Invalid spi_setup parameters");

    // Make sure bus is enabled
    spi_init(bus);

    // Bit rate is SSP_PCLK / (CPSR * (SCR + 1)) with CPSR even (2..254)
    // and SCR 0..255.  Round the divider up so the rate is never exceeded.
    uint32_t div = DIV_ROUND_UP(SSP_PCLK, rate ? rate : 1);
    uint32_t cpsr = (DIV_ROUND_UP(div, 256) + 1) & ~1u;
    if (cpsr < 2)
        cpsr = 2;
    if (cpsr > 254)
        cpsr = 254;
    uint32_t scr = DIV_ROUND_UP(div, cpsr) - 1;
    if (scr > 255)
        scr = 255;

    struct spi_config res = { spi_bus[bus].spi, 0, 0 };
    res.cpsr = cpsr;
    res.cr0 = CR0_DSS_8BIT | ((mode & 2) ? CR0_CPOL : 0)
        | ((mode & 1) ? CR0_CPHA : 0) | (scr << 8);
    return res;
}

void
spi_prepare(struct spi_config config)
{
    LPC_SSP_TypeDef *spi = config.spi;
    spi->CR0 = config.cr0;
    spi->CPSR = config.cpsr;
}

void
spi_transfer(struct spi_config config, uint8_t receive_data
             , uint8_t len, uint8_t *data)
{
    LPC_SSP_TypeDef *spi = config.spi;
    if (receive_data) {
        while (len--) {
            spi->DR = *data;
            // wait for read data to be ready
            while (!(spi->SR & SR_RNE))
                ;
            // get data
            *data++ = spi->DR;
        }
    } else {
        while (len--) {
            spi->DR = *data++;
            // wait for read data to be ready
            while (!(spi->SR & SR_RNE))
                ;
            // read data (to clear receive fifo)
            (void)spi->DR;
        }
    }
}
