# Support for HD44780 (20x4 text) LCD displays - 4-bit mode
#
# Copyright (C) 2018  Kevin O'Connor <kevin@koconnor.net>
# Copyright (C) 2018  Eric Callahan <arksine.code@gmail.com>
# Copyright (C) 2025  Mark (rewritten with correct HD44780 timings)
#
# This file may be distributed under the terms of the GNU GPLv3 license.

import logging

BACKGROUND_PRIORITY_CLOCK = 0x7fffffff00000000
LINE_LENGTH_DEFAULT = 20
LINE_LENGTH_OPTIONS = [16, 20]

TextGlyphs = { 'right_arrow': b'\x7e' }

# HD44780 timing constants (active low-speed timing for compatibility)
# These are minimum delays according to HD44780 datasheet
HD44780_DELAY = .000100  # 100µs between commands (conservative)
# The mcu busy-waits for this delay between bytes, so keep the user
# configurable value within a small, safe range.
HD44780_DELAY_MAX = .000200


class HD44780:
    def __init__(self, config):
        self.printer = config.get_printer()
        # pin config
        ppins = self.printer.lookup_object('pins')
        pins = [ppins.lookup_pin(config.get(name + '_pin'))
                for name in ['rs', 'e', 'd4', 'd5', 'd6', 'd7']]
        self.hd44780_protocol_init = config.getboolean(
            'hd44780_protocol_init', True)
        self.line_length = config.getchoice(
            'line_length', LINE_LENGTH_OPTIONS, LINE_LENGTH_DEFAULT)
        self.hd44780_delay = config.getfloat(
            'hd44780_delay', HD44780_DELAY, above=0., maxval=HD44780_DELAY_MAX)
        mcu = None
        for pin_params in pins:
            if mcu is not None and pin_params['chip'] != mcu:
                raise ppins.error("hd44780 all pins must be on same mcu")
            mcu = pin_params['chip']
        self.pins = [pin_params['pin'] for pin_params in pins]
        self.mcu = mcu
        self.oid = self.mcu.create_oid()
        self.mcu.register_config_callback(self.build_config)
        self.send_data_cmd = self.send_cmds_cmd = None
        self.send_init_cmd = None
        self.icons = {}
        # framebuffers
        self.text_framebuffers = [bytearray(b' '*2*self.line_length),
                                  bytearray(b' '*2*self.line_length)]
        self.glyph_framebuffer = bytearray(64)
        self.all_framebuffers = [
            # Text framebuffers
            (self.text_framebuffers[0], bytearray(b'~'*2*self.line_length),
             0x80),
            (self.text_framebuffers[1], bytearray(b'~'*2*self.line_length),
             0xc0),
            # Glyph framebuffer
            (self.glyph_framebuffer, bytearray(b'~'*64), 0x40)]

    def build_config(self):
        self.mcu.add_config_cmd(
            "config_hd44780 oid=%d rs_pin=%s e_pin=%s"
            " d4_pin=%s d5_pin=%s d6_pin=%s d7_pin=%s delay_ticks=%d" % (
                self.oid, self.pins[0], self.pins[1],
                self.pins[2], self.pins[3], self.pins[4], self.pins[5],
                self.mcu.seconds_to_clock(self.hd44780_delay)))
        cmd_queue = self.mcu.alloc_command_queue()
        self.send_cmds_cmd = self.mcu.lookup_command(
            "hd44780_send_cmds oid=%c cmds=%*s", cq=cmd_queue)
        self.send_data_cmd = self.mcu.lookup_command(
            "hd44780_send_data oid=%c data=%*s", cq=cmd_queue)
        self.send_init_cmd = self.mcu.lookup_command(
            "hd44780_send_init oid=%c nibbles=%*s", cq=cmd_queue)

    def send(self, cmds, is_data=False):
        cmd_type = self.send_cmds_cmd
        if is_data:
            cmd_type = self.send_data_cmd
        cmd_type.send([self.oid, cmds], reqclock=BACKGROUND_PRIORITY_CLOCK)

    def flush(self):
        # Find all differences in the framebuffers and send them to the chip
        for new_data, old_data, fb_id in self.all_framebuffers:
            if new_data == old_data:
                continue
            # Find the position of all changed bytes in this framebuffer
            diffs = [[i, 1] for i, (n, o) in enumerate(zip(new_data, old_data))
                     if n != o]
            # Batch together changes that are close to each other
            for i in range(len(diffs)-2, -1, -1):
                pos, count = diffs[i]
                nextpos, nextcount = diffs[i+1]
                if pos + 4 >= nextpos and nextcount < 16:
                    diffs[i][1] = nextcount + (nextpos - pos)
                    del diffs[i+1]
            # Transmit changes
            for pos, count in diffs:
                chip_pos = pos
                self.send([fb_id + chip_pos])
                self.send(new_data[pos:pos+count], is_data=True)
            old_data[:] = new_data

    def init(self):
        curtime = self.printer.get_reactor().monotonic()
        print_time = self.mcu.estimated_print_time(curtime)

        if self.hd44780_protocol_init:
            # =============================================================
            # HD44780 4-bit initialization sequence
            # Based on HD44780 datasheet and Repetier-Firmware timing
            # =============================================================
            #
            # The HD44780 powers up in 8-bit mode. To switch to 4-bit mode,
            # we must send specific nibbles (4-bit values) with proper timing.
            #
            # CRITICAL: The first commands must be sent as NIBBLES only,
            # not as full bytes, because the display is still in 8-bit mode!

            # Wait for power stabilization (datasheet says 15ms minimum,
            # we use 50ms for safety - some displays need more)
            print_time += 0.050

            # Send initialization nibbles using the special init command
            # This sends single nibbles with RS=0 (command mode)
            #
            # Sequence (from datasheet Figure 24 - 4-bit interface):
            # 1. Send 0x03 (Function Set 8-bit) - wait 4.1ms
            # 2. Send 0x03 (Function Set 8-bit) - wait 100µs
            # 3. Send 0x03 (Function Set 8-bit) - wait 100µs
            # 4. Send 0x02 (Function Set 4-bit) - now in 4-bit mode

            # Nibble sequence with delays after each
            # Format: (nibble_value, delay_after_in_seconds)
            init_nibbles = [
                (0x03, 0.0050),   # First 0x03: wait 5ms (datasheet: 4.1ms min)
                (0x03, 0.0002),   # Second 0x03: wait 200µs (datasheet: 100µs min)
                (0x03, 0.0002),   # Third 0x03: wait 200µs
                (0x02, 0.0002),   # Switch to 4-bit mode: wait 200µs
            ]

            for nibble, delay in init_nibbles:
                minclock = self.mcu.print_time_to_clock(print_time)
                self.send_init_cmd.send([self.oid, [nibble]], minclock=minclock)
                print_time += delay

            # Now in 4-bit mode - send configuration commands as full bytes
            # These are sent as two nibbles each (high nibble first)
            #
            # Format: (command_byte, delay_after_in_seconds)
            init_cmds = [
                # Function Set: 4-bit, 2-line, 5x8 font
                # DL=0 (4-bit), N=1 (2-line), F=0 (5x8)
                (0x28, 0.000050),

                # Display Control: Display ON, Cursor OFF, Blink OFF
                # D=1 (display on), C=0 (cursor off), B=0 (blink off)
                (0x0C, 0.000050),

                # Clear Display - this takes 1.52ms!
                (0x01, 0.002),

                # Entry Mode Set: Increment, No Shift
                # I/D=1 (increment), S=0 (no shift)
                (0x06, 0.000050),
            ]

            for cmd, delay in init_cmds:
                minclock = self.mcu.print_time_to_clock(print_time)
                self.send_cmds_cmd.send([self.oid, [cmd]], minclock=minclock)
                print_time += delay

        else:
            # Minimal initialization (assumes display is already in 4-bit mode)
            print_time += 0.002
            minclock = self.mcu.print_time_to_clock(print_time)
            # Just home the cursor
            self.send_cmds_cmd.send([self.oid, [0x02]], minclock=minclock)
            print_time += 0.002

        self.flush()

    def write_text(self, x, y, data):
        if x + len(data) > self.line_length:
            data = data[:self.line_length - min(x, self.line_length)]
        pos = x + ((y & 0x02) >> 1) * self.line_length
        self.text_framebuffers[y & 1][pos:pos+len(data)] = data

    def set_glyphs(self, glyphs):
        for glyph_name, glyph_data in glyphs.items():
            data = glyph_data.get('icon5x8')
            if data is not None:
                self.icons[glyph_name] = data

    def write_glyph(self, x, y, glyph_name):
        data = self.icons.get(glyph_name)
        if data is not None:
            slot, bits = data
            self.write_text(x, y, [slot])
            self.glyph_framebuffer[slot * 8:(slot + 1) * 8] = bits
            return 1
        char = TextGlyphs.get(glyph_name)
        if char is not None:
            # Draw character
            self.write_text(x, y, char)
            return 1
        return 0

    def write_graphics(self, x, y, data):
        pass

    def clear(self):
        spaces = b' ' * 2*self.line_length
        self.text_framebuffers[0][:] = spaces
        self.text_framebuffers[1][:] = spaces

    def get_dimensions(self):
        return (self.line_length, 4)
