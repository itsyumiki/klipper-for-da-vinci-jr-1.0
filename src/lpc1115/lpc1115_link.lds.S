// LPC11xx linker script
//
// Based on the generic ARM Cortex-M linker script (generic/armcm_link.lds.S)
// Copyright (C) 2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.
//
// Differences from the generic script:
//  * The word at flash offset 0x2FC is the NXP code read protection (CRP)
//    word.  It is reserved here and set to 0xFFFFFFFF (no protection).
//    Normal code is only placed after it (at 0x300).
//  * The vector table and the early boot code are placed first, and must
//    fit below the CRP word.
//  * Vector table word 7 is the boot ROM checksum.  It can not be known
//    at link time - scripts/lpc11xx_checksum.py fills it in klipper.bin.

#include "autoconf.h" // CONFIG_FLASH_APPLICATION_ADDRESS

OUTPUT_FORMAT("elf32-littlearm", "elf32-littlearm", "elf32-littlearm")
OUTPUT_ARCH(arm)

MEMORY
{
  rom (rx) : ORIGIN = CONFIG_FLASH_APPLICATION_ADDRESS , LENGTH = CONFIG_FLASH_SIZE
  ram (rwx) : ORIGIN = CONFIG_RAM_START , LENGTH = CONFIG_RAM_SIZE
}

SECTIONS
{
    .vectors : {
        . = ALIGN(4);
        _text_vectortable_start = .;
        KEEP(*(.vector_table))
        _text_vectortable_end = .;
        // Early boot code, so it fits below the CRP word
        *(.text.armcm_boot.*)
        // Pad with erased-flash bytes up to the CRP word (this also fails
        // the link if the above content is too large to fit)
        FILL(0xFFFFFFFF)
        . = 0x2FC;
    } > rom

    .crp 0x2FC : {
        LONG(0xFFFFFFFF) // CRP word: no code read protection
    } > rom

    .text 0x300 : {
        . = ALIGN(4);
        *(.text .text.*)
        *(.rodata .rodata*)
    } > rom

    . = ALIGN(4);
    _data_flash = .;

    .data : AT (_data_flash)
    {
        . = ALIGN(4);
        _data_start = .;
        *(.ramfunc .ramfunc.*);
        *(.data .data.*);
        . = ALIGN(4);
        _data_end = .;
    } > ram

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > ram

    _stack_start = CONFIG_RAM_START + CONFIG_RAM_SIZE - CONFIG_STACK_SIZE ;
    .stack _stack_start (NOLOAD) :
    {
        . = . + CONFIG_STACK_SIZE;
        _stack_end = .;
    } > ram

    /DISCARD/ : {
        // The .init/.fini sections are used by __libc_init_array(), but
        // that isn't needed so no need to include them in the binary.
        *(.init)
        *(.fini)
        // Don't include exception tables
        *(.ARM.extab)
        *(.ARM.exidx)
    }
}
