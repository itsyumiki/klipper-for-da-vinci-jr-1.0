// I2C functions on lpc1115
//
// Polled master-mode driver for the single I2C controller (SCL on
// PIO0_4, SDA on PIO0_5).  Address and data NACKs and bus timeouts are
// reported through the I2C_BUS_* return codes.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_CLOCK_FREQ
#include "board/irq.h" // irq_save
#include "board/misc.h" // timer_is_before
#include "command.h" // DECL_COMMAND
#include "gpio.h" // i2c_setup
#include "i2ccmds.h" // I2C_BUS_SUCCESS
#include "internal.h" // gpio_peripheral
#include "sched.h" // sched_shutdown

DECL_ENUMERATION("i2c_bus", "i2c0", 0);
DECL_CONSTANT_STR("BUS_PINS_i2c0", "PIO0_4,PIO0_5");

// CONSET/CONCLR bits
enum {
    IF_AA = 1 << 2, IF_SI = 1 << 3, IF_STO = 1 << 4, IF_STA = 1 << 5,
    IF_ENA = 1 << 6,
};

// I2C status codes (master mode)
enum {
    ST_START = 0x08, ST_RESTART = 0x10,
    ST_SLAW_ACK = 0x18, ST_SLAW_NACK = 0x20,
    ST_DATA_ACK = 0x28, ST_DATA_NACK = 0x30,
    ST_SLAR_ACK = 0x40, ST_SLAR_NACK = 0x48,
    ST_RX_ACK = 0x50, ST_RX_NACK = 0x58,
};

#define I2C_MAX_RATE 400000

struct i2c_config
i2c_setup(uint32_t bus, uint32_t rate, uint8_t addr)
{
    if (bus != 0)
        shutdown("Invalid i2c bus");
    LPC_I2C_TypeDef *i2c = LPC_I2C0;

    static uint8_t have_run_init;
    if (!have_run_init) {
        have_run_init = 1;

        // Init pins (IOCON function 1 is SCL/SDA on these pads)
        gpio_peripheral(GPIO(0, 4), 1, 0);
        gpio_peripheral(GPIO(0, 5), 1, 0);

        // Enable clock and release the peripheral from reset
        LPC_SYSCON_SYSAHBCLKCTRL |= SYSAHBCLK_I2C;
        LPC_SYSCON_PRESETCTRL |= PRESET_I2C;

        // Enable interface
        i2c->CONCLR = IF_AA | IF_SI | IF_STA | IF_ENA;
        i2c->CONSET = IF_ENA;
    }

    // The bus speed is applied per device at the start of each transfer.
    // Above 100KHz use a low:high ratio of 2:1 (fast mode needs a longer
    // low period).
    if (rate < 1000)
        rate = 1000;
    if (rate > I2C_MAX_RATE)
        rate = I2C_MAX_RATE;
    uint32_t total = CONFIG_CLOCK_FREQ / rate;
    uint32_t low = rate > 100000 ? (total * 2 + 2) / 3 : total / 2;
    uint32_t high = total - low;
    if (low < 4)
        low = 4;
    if (high < 4)
        high = 4;
    if (low > 0xffff)
        low = 0xffff;
    if (high > 0xffff)
        high = 0xffff;

    return (struct i2c_config){ .i2c=i2c, .addr=addr<<1,
                                .sclh=high, .scll=low };
}

// Wait for the SI (state change) flag.  Returns the bus status code, or
// -1 on timeout.
static int
i2c_wait(LPC_I2C_TypeDef *i2c, uint32_t timeout)
{
    for (;;) {
        if (i2c->CONSET & IF_SI)
            return i2c->STAT;
        if (!timer_is_before(timer_read_time(), timeout))
            return -1;
    }
}

// Release the bus after an error and put the controller back in idle
static void
i2c_recover(LPC_I2C_TypeDef *i2c)
{
    i2c->CONSET = IF_STO;
    i2c->CONCLR = IF_SI | IF_STA;
    // Give the stop condition a moment to go out
    uint32_t end = timer_read_time() + timer_from_us(200);
    while ((i2c->CONSET & IF_STO) && timer_is_before(timer_read_time(), end))
        ;
    // If the controller is wedged (STO stuck) restart it
    if (i2c->CONSET & IF_STO) {
        i2c->CONCLR = IF_AA | IF_SI | IF_STA | IF_ENA;
        i2c->CONSET = IF_ENA;
    }
}

// Generate a stop condition
static int
i2c_stop(LPC_I2C_TypeDef *i2c, uint32_t timeout)
{
    i2c->CONSET = IF_STO;
    i2c->CONCLR = IF_SI;
    // STO clears itself once the stop condition has been sent
    while (i2c->CONSET & IF_STO) {
        if (!timer_is_before(timer_read_time(), timeout)) {
            i2c_recover(i2c);
            return I2C_BUS_TIMEOUT;
        }
    }
    return I2C_BUS_SUCCESS;
}

// Send a start (or repeated start) and the address byte.  Returns the
// status after the address byte, or -1 on a timeout or bus error.
static int
i2c_start_addr(LPC_I2C_TypeDef *i2c, uint8_t addr, uint32_t timeout)
{
    i2c->CONSET = IF_STA;
    i2c->CONCLR = IF_SI;
    int st = i2c_wait(i2c, timeout);
    if (st != ST_START && st != ST_RESTART)
        return -1;
    i2c->DAT = addr;
    i2c->CONCLR = IF_STA | IF_SI;
    return i2c_wait(i2c, timeout);
}

// Send one data byte and return the resulting status (-1 on timeout)
static int
i2c_send_byte(LPC_I2C_TypeDef *i2c, uint8_t b, uint32_t timeout)
{
    i2c->DAT = b;
    i2c->CONCLR = IF_SI;
    return i2c_wait(i2c, timeout);
}

// Fail an in-progress transfer with the given return code
static int
i2c_fail(LPC_I2C_TypeDef *i2c, int ret, uint32_t timeout)
{
    if (ret == I2C_BUS_TIMEOUT)
        i2c_recover(i2c);
    else
        i2c_stop(i2c, timeout);
    return ret;
}

static void
i2c_set_speed(struct i2c_config config)
{
    LPC_I2C_TypeDef *i2c = config.i2c;
    i2c->SCLH = config.sclh;
    i2c->SCLL = config.scll;
}

// Send the address byte and the data bytes for a write transfer.
// Returns I2C_BUS_SUCCESS or an error code (the bus is released on error).
static int
i2c_write_bytes(struct i2c_config config, uint8_t len, uint8_t *data
                , uint32_t timeout)
{
    LPC_I2C_TypeDef *i2c = config.i2c;
    int st = i2c_start_addr(i2c, config.addr, timeout);
    if (st == ST_SLAW_NACK)
        return i2c_fail(i2c, I2C_BUS_START_NACK, timeout);
    if (st != ST_SLAW_ACK)
        return i2c_fail(i2c, I2C_BUS_TIMEOUT, timeout);
    while (len--) {
        st = i2c_send_byte(i2c, *data++, timeout);
        if (st == ST_DATA_NACK)
            return i2c_fail(i2c, I2C_BUS_NACK, timeout);
        if (st != ST_DATA_ACK)
            return i2c_fail(i2c, I2C_BUS_TIMEOUT, timeout);
    }
    return I2C_BUS_SUCCESS;
}

int
i2c_write(struct i2c_config config, uint8_t write_len, uint8_t *write)
{
    LPC_I2C_TypeDef *i2c = config.i2c;
    uint32_t timeout = timer_read_time()
        + timer_from_us(2000 + 200 * (uint32_t)(write_len + 1));
    i2c_set_speed(config);

    int ret = i2c_write_bytes(config, write_len, write, timeout);
    if (ret != I2C_BUS_SUCCESS)
        return ret;
    return i2c_stop(i2c, timeout);
}

int
i2c_read(struct i2c_config config, uint8_t reg_len, uint8_t *reg
         , uint8_t read_len, uint8_t *read)
{
    LPC_I2C_TypeDef *i2c = config.i2c;
    uint32_t timeout = timer_read_time()
        + timer_from_us(2000 + 200 * (uint32_t)(reg_len + read_len + 2));
    i2c_set_speed(config);

    if (reg_len) {
        // Write the register bytes, then continue with a repeated start
        int ret = i2c_write_bytes(config, reg_len, reg, timeout);
        if (ret != I2C_BUS_SUCCESS)
            return ret;
    }

    // Start (or repeated start) and address the device for reading
    int st = i2c_start_addr(i2c, config.addr | 0x01, timeout);
    if (st == ST_SLAR_NACK)
        return i2c_fail(i2c, I2C_BUS_START_READ_NACK, timeout);
    if (st != ST_SLAR_ACK)
        return i2c_fail(i2c, I2C_BUS_TIMEOUT, timeout);

    // Read the data, NACKing the final byte
    while (read_len) {
        if (read_len == 1) {
            i2c->CONCLR = IF_AA | IF_SI;
        } else {
            i2c->CONSET = IF_AA;
            i2c->CONCLR = IF_SI;
        }
        st = i2c_wait(i2c, timeout);
        if (st != ST_RX_ACK && st != ST_RX_NACK)
            return i2c_fail(i2c, I2C_BUS_TIMEOUT, timeout);
        *read++ = i2c->DAT;
        read_len--;
    }

    return i2c_stop(i2c, timeout);
}
