# lpc1115 build rules

# Setup the toolchain
CROSS_PREFIX=arm-none-eabi-

dirs-y += src/lpc1115 src/generic
CFLAGS += -mthumb -mcpu=cortex-m0 -Ilib/cmsis-core

# The LPC11xx needs its own linker script (CRP word and boot checksum)
CFLAGS_klipper.elf += -nostdlib -lgcc -lc_nano
CFLAGS_klipper.elf += -T $(OUT)src/lpc1115/lpc1115_link.ld
$(OUT)klipper.elf: $(OUT)src/lpc1115/lpc1115_link.ld

# Add source files
src-y += lpc1115/main.c lpc1115/timer.c lpc1115/gpio.c lpc1115/watchdog.c
src-y += generic/armcm_boot.c generic/armcm_irq.c generic/timer_irq.c
src-y += generic/armcm_reset.c generic/crc16_ccitt.c
src-$(CONFIG_WANT_ADC) += lpc1115/adc.c
src-$(CONFIG_WANT_SPI) += lpc1115/spi.c
src-$(CONFIG_WANT_I2C) += lpc1115/i2c.c
src-$(CONFIG_WANT_HARD_PWM) += lpc1115/hard_pwm.c
src-$(CONFIG_SERIAL) += lpc1115/serial.c generic/serial_irq.c

# Build the additional bin output file
target-y += $(OUT)klipper.bin

$(OUT)klipper.bin: $(OUT)klipper.elf
	@echo "  Creating bin file $@"
	$(Q)$(OBJCOPY) -O binary $< $@
	$(Q)$(PYTHON) scripts/lpc11xx_checksum.py $@
