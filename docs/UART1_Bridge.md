# USB serial to UART1 bridge (SAM4E8E)

This is an optional feature of this fork. It adds a second USB serial
port (CDC-ACM) to the SAM4E8E USB device. Every byte written to the
second port is sent out of UART1, and every byte received on UART1 is
sent to the host. The Klipper console stays on the first USB serial
port and behaves exactly as before.

Status: implemented and builds. It has **not** been tested on hardware
yet.

## Building

```
cp test/configs/sam4e8e-uart-bridge.config .config
make olddefconfig
make
```

Or, in `make menuconfig`, select the SAM4E8E micro-controller with the
USB communication interface and enable "USB CDC to UART1 bridge
(PA5/PA6)". The option depends on `MACH_SAM4E` and `ATSAM_USB`. With
the option disabled the firmware is functionally unchanged.

## Pins

| Signal | Pin | Peripheral |
|---|---|---|
| UART1 RX (URXD1) | PA5 | C |
| UART1 TX (UTXD1) | PA6 | C |

Both pins are reserved (`RESERVE_PINS_uart1_bridge`), so a Klipper
config cannot use them for something else by accident. Only UART1 is
used. The console UART0 pins (PA9/PA10) are not touched.

**Warning:** on the Da Vinci Jr 1.0 board PA5/PA6 are wired to the
LPC1115 UART. Do not send arbitrary bytes to the second port while the
LPC is attached unless you know what it will do with them.

## What the host sees

The device is a composite device (class 0xEF/0x02/0x01) that uses
interface association descriptors, so each serial port is bound as a
separate function by Linux, macOS and Windows 10+.

| Interface | Function | Endpoints |
|---|---|---|
| 0, 1 | Port 1: Klipper console (unchanged) | EP1 bulk IN, EP2 bulk OUT, EP3 interrupt IN |
| 2, 3 | Port 2: UART1 bridge | EP4 bulk IN, EP5 bulk OUT, EP6 interrupt IN |

All endpoints use 64-byte packets (8 bytes for the interrupt
endpoints). The SAM4E UDP only allows one type and direction per
endpoint number, so each port needs three endpoint numbers. EP7 is
spare. The configuration descriptor is 131 bytes long.

On Linux the ports show up as `/dev/ttyACM0` (port 1) and
`/dev/ttyACM1` (port 2). In `/dev/serial/by-id/` the names end in
`-if00` (Klipper console) and `-if02` (bridge).

## Behaviour

- The data path is a transparent byte stream. There is no framing; any
  framing belongs in the host program.
- UART1 interrupts use a 256-byte ring buffer in each direction (255
  usable bytes) at interrupt priority 1.
- Baud rate and parity follow the standard CDC `SET_LINE_CODING`
  request. The default is 115200 8N1 (`USB_CDC_BRIDGE_BAUD`).
- The UART only supports 8 data bits and 1 stop bit. Parity can be
  none, odd, even, mark or space. The maximum baud rate is the
  peripheral clock divided by 16.
- Unsupported line codings (other data bits, 1.5 or 2 stop bits, bad
  parity value, impossible baud rate) are acknowledged on USB but then
  ignored. `GET_LINE_CODING` always reports the coding that is really in
  effect.
- The bridge has its own line coding state. Setting 1200 baud on the
  bridge does not request the bootloader; only port 1 does that.
- DTR and RTS (`SET_CONTROL_LINE_STATE`) are stored but not used.
- The bridge keeps running when Klipper enters the shutdown state.

## Host setup

ModemManager may probe the new port with AT commands, which would show
up as garbage on UART1. Tell it to ignore the device with a udev rule
such as:

```
ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="614e", ENV{ID_MM_DEVICE_IGNORE}="1"
```

This is a rule for the host, not for the firmware. Install it yourself
(for example in `/etc/udev/rules.d/`) and reload the rules.

## Testing

1. Enumeration: `lsusb -d 1d50:614e -v` should show class 0xEF, 4
   interfaces and two interface association descriptors.
   `ls /dev/serial/by-id/` should list the `-if00` and `-if02` ports.
2. Klipper: the existing `serial:` path must keep working, both while
   port 2 is idle and while it carries traffic.
3. Loopback: with a jumper from PA6 to PA5 (and the LPC isolated, or a
   USB-UART adapter on the lines), write a known payload to port 2 and
   read it back. Repeat at 115200, 460800 and 921600, changing the baud
   rate from the host each time to exercise `SET_LINE_CODING`.

   ```python
   import serial, sys, time
   p = sys.argv[1]
   baud = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
   s = serial.Serial(p, baud, timeout=1)
   payload = bytes(range(256)) * 4
   s.write(payload); s.flush(); time.sleep(0.5)
   got = s.read(len(payload))
   print(len(got), "bytes back; match =", got == payload)
   ```

4. 1200 baud: set 1200 baud on port 2. The MCU must not reboot into the
   bootloader.

An offline sanity check of the descriptors is possible without hardware
by walking the configuration descriptor in `out/klipper.bin`: it should
list interfaces 0 to 3, endpoints 0x81, 0x02, 0x83, 0x84, 0x05 and 0x86,
interface associations (0, 2) and (2, 2), and a `wTotalLength` of 131.

## Known limitations

- 8 data bits and 1 stop bit only.
- No break handling.
- The receive ring drops bytes if the host stops reading. The counter
  (`rx_overflow`) is not exposed yet, and neither is the UART error
  counter.
- DTR and RTS are stored but unused.
- The UART1 interrupt preempts the stepper timer (priority 2). The
  handler only moves bytes in and out of the ring buffers, but it does
  add a little jitter to the step timing for every UART byte. The
  amount is an estimate (about half a microsecond per byte) and has not
  been measured.
