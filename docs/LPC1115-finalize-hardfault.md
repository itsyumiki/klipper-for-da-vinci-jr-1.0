# LPC1115: `finalize_config` HardFault (stack overflow into the move queue)

Investigation report, 2026-10-25/26. Status: **root cause identified from a
SWD halt; fix applied in the source tree; fix confirmed on hardware with one
console run (2026-10-26, see "Hardware verification"); broader testing still
to do.**

## Summary

With the LPC1115 port, any config that contains an object with a move queue
(`config_digital_out`, i.e. `output_pin`, and by extension PWM and similar)
followed by `finalize_config` hung the CPU. With the watchdog enabled the MCU
rebooted about 0.5 s later (`starting`); without it the MCU went silent
forever.

Cause: `CONFIG_STACK_SIZE` was 256 bytes, but the firmware's real call depth
on the `finalize_config` path is about 536 bytes. The stack silently overflowed
below `_stack_start` into free RAM, which is harmless until `finalize_config`
allocates the move queue over that RAM and `memset()`s it. The memset zeroed the
live stack frames, including `alloc_chunk()`'s own saved return address.
`alloc_chunk` then executed `pop {r4, pc}` and popped `pc = 0` (Thumb bit
clear), which is an immediate HardFault.

Fix: `STACK_SIZE` default 256 -> 1024 in `src/lpc1115/Kconfig`.

## Symptoms (what the owner saw)

* `FIRMWARE_RESTART` / config with a digital output: MCU reboots or hangs; the
  host appears hung.
* Console test, digital out on `PIO0_9`: `starting` ~0.5 s after
  `finalize_config`.
* Same with `PIO2_10`: so it is **not pin-specific** (the early suspicion about
  `PIO0_9`, and about the hotend/heater, was wrong; the heater was not even
  connected during the tests).
* `finalize_config` with **no objects**: survives.
* ADC-only configs (hotend NTC reading 23-24 degC) worked: ADC objects have no
  move queue.

## Why only some configs crashed

`finalize_config` (`move_finalize()` in `src/basecmd.c`) allocates up to 1024
move-queue items of the largest registered item size, limited by free RAM, via
`alloc_chunks()` -> `alloc_chunk()`. `alloc_chunk()` ends with
`memset(data, 0, size)` over the whole allocation.

| Config | Item size | Allocation | Zeroed range ends at | Result |
|---|---|---|---|---|
| no objects / ADC only | 4 B (`struct move_node`) | 1024 x 4 = 4096 B | about 0x100012cc | survives (stack's deepest word is 0x10001dc8, above this) |
| one digital out | 12 B (`struct digital_move`) | ~577 items, i.e. all RAM up to `dynmem_end()` | `_stack_start` = 0x10001ee0 | zeroes stack words down at 0x10001dc0 -> crash |

RAM layout of that build (`RAM_SIZE` 0x1fe0): static data ends at 0x10000294,
dynamic memory runs 0x10000294..0x10001ee0 (~7.2 KB), configured stack
0x10001ee0..0x10001fe0 (256 B).

## Evidence trail (hypothesis -> test -> result)

| Hypothesis | Test | Result |
|---|---|---|
| Specific to `PIO0_9` | digital out on `PIO2_10` | same crash: ruled out |
| Hotend / heater load | heater was disconnected | ruled out |
| Brown-out / power | read `SYSRSTSTAT` (0x40048030) after crashes: `0x15` and `0x7` | BOD bit (bit 3) never set: ruled out |
| Upper RAM not usable | `debug_write`/`debug_read` of 0xDEADBEEF at 0x10001800 and 0x10001ed8 | read back fine, no reset: ruled out |
| Bug in the move-queue C code | disassembled `command_finalize_config`, `move_reset`, `alloc_chunks` | match the source; ruled out |
| Watchdog setup/feeding is the cause | build without `watchdog.c` | chip still dies (silent, reconnect impossible): the watchdog was only the messenger, the CPU really hangs |
| Fault or livelock? | SWD halt with Pico debugprobe + breakpoint on `DefaultHandler` | **HardFault**, frame below |

### The decisive frame (halted in `DefaultHandler`)

`xPSR=0x61000003` (IPSR 3 = HardFault), `LR=0xfffffff9` (frame on MSP, thread
mode), `msp=0x10001da8`. Stacked frame at 0x10001da8:

| r0 | r1 | r2 | r3 | r12 | lr | pc | xPSR |
|---|---|---|---|---|---|---|---|
| 0x100002cc | 0 | 0x10001ee0 | 0x10001ee0 | 0 | 0x000026bb | **0x00000000** | 0x60000000 |

* SP before the exception = 0x10001da8 + 32 = **0x10001dc8**, i.e. **536 bytes**
  below the top of RAM (0x10001fe0), configured stack 256.
* `lr 0x26bb` -> `addr2line` = `alloc_chunk()` at `src/basecmd.c:39`, the
  instruction after `bl memset` (0x26b6). The next instruction is
  `pop {r4, pc}`.
* `pc = 0` with the stacked xPSR T bit (bit 24) clear is the signature of
  `pop {pc}` from a zeroed stack slot. `r4 = 0` (also popped from the zeroed
  frame) and `r0 = 0x100002cc` (`data`, the move list start) match.
* `ICSR = 0x00425003`: VECTACTIVE 3; the UART IRQ was pending (masked by the
  handler's `cpsid i`). Derived from the register decode, not otherwise used.

### Static check of the stack depth

Frame sizes from the disassembly (pushes + `sub sp`): `reset_handler_stage_two`
8, `sched_main` 72, **`run_tasks` 352** (command dispatch is inlined into it by
LTO), `command_dispatch` 64, `command_finalize_config` 24, `alloc_chunk` 8,
`memset` 20: about 530-550 B with no interrupt nesting, matching the 536 B
observed. So 256 B could never hold even the idle command path; it only worked
because that RAM was unused until a move queue was allocated over it.

## Fix

`src/lpc1115/Kconfig`: `STACK_SIZE` default 1024, with a comment explaining the
constraint. RAM effect: dynamic memory shrinks by 768 B (about 6.4 KB left for
move queues). 1024 leaves roughly 2x headroom over the measured ~540 B chain;
interrupt nesting and the shutdown path add on top of that.

Static worst-case stack estimate for the fixed build (call graph from the
disassembly; frame = pushes + `sub sp`; indirect calls approximated by treating
every `command_*` handler as called from `command_dispatch` and every `*_task`
as called from `run_tasks`): base 80 B (`stage_two` + `sched_main`) + `run_tasks`
360 B; worst main-context chain 716 B (`run_tasks` > `ads131m0x_read_adc` >
`trigger_analog_update` > `__aeabi_lmul`), worst command handler 688 B,
worst task 592 B, finalize path 536 B (observed). Interrupts add the 32 B
exception frame plus 80 B (`CT32B1_Handler`; UART0 handler 40 B), assuming
equal-priority IRQs do not nest. `sched_shutdown` `longjmp`s back to
`sched_main`, so shutdown does not stack on top. Worst case about **828 B**
against 1024 B (about 196 B headroom; the finalize path itself has 488 B). This
is a static estimate, not a measurement; the deepest chains go through optional
drivers that are compiled in but unused here.

Verified on the build (not on hardware): `_stack_start` = 0x10001be0, stack
1024 B (0x10001be0..0x10001fe0), dynamic RAM 6476 B, `alloc_chunk`'s end limit
is the new 0x10001be0, boot vector checksum sums to 0, CRP word 0xFFFFFFFF,
watchdog linked in, `DefaultHandler` is the plain loop.

## Hardware verification (2026-10-26)

STACKFIX build (version `?-20261026_023837-sandbox`, confirmed by the console
handshake), console only, no debugger attached:

```
allocate_oids count=1
config_digital_out oid=0 pin=PIO2_10 value=0 default_value=0 max_duration=0
finalize_config crc=1
get_uptime            (replied at 4.5 s, 9.7 s, 10.3 s, 10.9 s)
```

The MCU kept answering, uptime advanced consistently (about 5.2 s between the
first two replies), `stats` kept arriving and there was no `starting`, so the
watchdog (active in this build) never fired. The old build rebooted ~0.5 s
after `finalize_config` in the same test. The first `stats` after finalize
showed a one-off jump (sum 69259 vs about 34000 normally): the finalize
memset/free-list build taking a few ms, as expected.

Coverage so far is thin: one run, `PIO2_10` only, about 7 s after finalize.
Still to do: `PIO0_9`, a soak of several minutes, a real Klippy config with
`output_pin` + `SET_PIN`, `FIRMWARE_RESTART`, and the stack high-water
measurement.

## Open questions

* The notes record `output_pin` as working on hardware with the earlier
  GPIO-only build (`klipper-lpc1115-gpio-c3750198.bin`). The core files
  (`main.c`, `timer.c`, `serial.c`, linker script) are identical to the current
  tree. The difference is `gpio.c`/`gpio.h`/`internal.h` and the new drivers
  (watchdog, ADC, PWM, SPI, I2C). Likely explanation (unverified): the earlier
  build's stack depth was marginal or shallower because of different LTO
  inlining. Not checked.
* 1024 B leaves about 196 B over the static worst case. If more margin is
  wanted, 1280 B costs another 256 B of move-queue RAM; or trim the optional
  drivers on the deepest chains (ADS131M0x, trigger analog, software I2C) in
  `menuconfig`.
* Reduce stack use? `run_tasks` having a 352-byte frame is large; worth looking
  at what got inlined, but not required with a 1 KiB stack.
* No overflow detection exists. A cheap safeguard would be a stack-usage check
  at build time (`-fstack-usage`) and/or painting the stack at boot and
  reporting the high-water mark.

## How to reproduce (old build) and verify (fixed build)

Stop Klipper, open the console on the SAM4E8E bridge (device `...-if02`):

```
~/klippy-env/bin/python ~/klipper/klippy/console.py -b 250000 /dev/serial/by-id/usb-Klipper_sam4e8e_..._-if02
allocate_oids count=1
config_digital_out oid=0 pin=PIO2_10 value=0 default_value=0 max_duration=0
finalize_config crc=1
get_uptime
```

Old build: `starting` ~0.5 s after `finalize_config`. Fixed build should keep
answering `get_uptime` and `stats`. Then run a real Klippy config with
`output_pin`.

## Techniques worth reusing

* The firmware dictionary already contains `debug_read` / `debug_write`
  (`src/debugcmds.c`). `DUMP 0x40048030 1 32` reads `SYSRSTSTAT`
  (bit0 POR, bit1 external reset, bit2 WDT, bit3 BOD, bit4 software reset,
  per UM10398 from memory; not re-checked against the manual). The bits are
  sticky; a `debug_write ... val=31` attempt to clear them did not visibly
  clear them, so treat the register as cumulative since power-up.
  `debug_write order=2 addr=<decimal> val=<decimal>`; send one `DUMP` and wait
  for the answer, overlapping requests make `console.py` raise a `KeyError`.
* Hang vs watchdog: build without `watchdog.c`. A real fault freezes the chip
  (no replies, reconnect fails); a feed problem would survive.
* Fault beacon: replace `DefaultHandler` in `src/generic/armcm_boot.c` with a
  naked handler that picks MSP/PSP from `LR` bit 2 and loops printing
  `FLT ipsr pc lr xpsr frame r0..r3` as ASCII polled out of UART0
  (THR 0x40008000, LSR 0x40008014 bit 5). Read with
  `stty -F $DEV 250000 raw -echo; timeout 3 cat $DEV | tr -c '[:print:]\n' '.'`.
  Superseded here by SWD, but works without a probe. Built only in throwaway
  copies, never merged.
* SWD with a Pico debugprobe (CMSIS-DAP, VID:PID 2e8a:000c): SWCLK = `PIO0_10`,
  SWDIO = `PIO1_3`, GND, optional nRESET. OpenOCD 0.12.0:
  `openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/lpc11xx.cfg`.
  DPIDR 0x0bb11477, Cortex-M0 r0p0, 4 breakpoints, 2 watchpoints. The default
  adapter speed is 10 kHz, so `load` of 42 KB takes minutes: use
  `monitor adapter speed 1000` and `set remotetimeout 60` in GDB. OpenOCD warns
  that the ELF's vector-7 checksum differs from the computed one (the ELF
  holds a placeholder; the checksum script patches only the `.bin`); OpenOCD
  writes the correct one. `monitor reset halt` lands in the boot ROM
  (pc 0x1fff0040).
* Reading an exception frame in GDB: the stacked pc is the **value** at
  `$msp+24` (frame: r0 r1 r2 r3 r12 lr pc xPSR). `info symbol` on that value,
  not on the address.
* `PIO1_3` (SWDIO) is also AD4. Do not use `PIO0_10`/`PIO1_3` as GPIO/ADC/SPI
  while debugging.
* Halting under a debugger can trigger the 0.5 s watchdog; use a build without
  `watchdog.c` for long halts. Seen for real on 2026-10-26: attaching GDB to the
  STACKFIX build (watchdog on) gave `external reset detected`, repeated
  `Timeout waiting for DCRDR transfer ready`, `BUG: unknown Part ID: 0x55c7`
  (garbage read mid-reset) and a GDB `qSupported` timeout. Recovery: close
  GDB/OpenOCD, power-cycle, then flash without leaving the core halted:
  `openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/lpc11xx.cfg -c "adapter speed 400" -c "program <fw>.bin verify reset exit 0x0"`
  (`program` flashes under reset, before the firmware starts the watchdog), or
  use `lpc_flash.py`. The firmware version string in the console handshake
  tells which build is running.

## Artifacts

In `archives/` (untracked, not in `~/public`):

| File | What | SHA-256 |
|---|---|---|
| `klipper-lpc1115-NOWDT-diag-a91bf639.bin` | current tree minus `watchdog.c` | `a91bf639edb52ab5592a0f38fdcc934003297eae1aecd9233e9b6fe5bf89c26a` |
| `klipper-lpc1115-FAULTBEACON-diag-9156b8d6.bin` / `.elf` | no watchdog + beacon `DefaultHandler`, STACK_SIZE 256 | `9156b8d6fd7e37c4627250665766c8eb44f432c682f77101998ad2f025c6dc3a` |

| `klipper-lpc1115-STACKFIX-ecd76e00.bin` / `.elf` | **fix build**: STACK_SIZE 1024, watchdog on, normal `DefaultHandler` (42,036 B, version `?-20261026_023837-sandbox`) | `ecd76e00f5dde5cd39d3a99cba65641ae9d422691f753da774280778faa1f457` |

The STACKFIX `.bin` is checked (see above) and has been flashed and run (see
"Hardware verification"). The `.elf` holds a placeholder in vector 7; flash the
`.bin`, or let OpenOCD patch the checksum when it loads the ELF.
