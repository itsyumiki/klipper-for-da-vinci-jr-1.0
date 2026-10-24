Welcome to the Klipper project!

[![Klipper](docs/img/klipper-logo-small.png)](https://www.klipper3d.org/)

https://www.klipper3d.org/

The Klipper firmware controls 3d-Printers. It combines the power of a
general purpose computer with one or more micro-controllers. See the
[features document](https://www.klipper3d.org/Features.html) for more
information on why you should use the Klipper software.

Start by [installing Klipper software](https://www.klipper3d.org/Installation.html).

Klipper software is Free Software. See the [license](COPYING) or read
the [documentation](https://www.klipper3d.org/Overview.html). We
depend on the generous support from our
[sponsors](https://www.klipper3d.org/Sponsors.html).

## Da Vinci Jr 1.0 fork notes

This is a fork of Klipper for the XYZprinting Da Vinci Jr 1.0 (Atmel
SAM4E8E). It tracks upstream Klipper and adds:

- An optional second USB serial port that is bridged to UART1
  (PA5/PA6). The Klipper console stays on the first port. Enable it
  with `CONFIG_ATSAM_UART1_BRIDGE` (see
  `test/configs/sam4e8e-uart-bridge.config`). With the option off the
  firmware is unchanged. See [UART1 bridge](docs/UART1_Bridge.md).

Status: the bridge builds and the USB descriptors check out offline. It
has not been tested on hardware yet.

Gotcha: ModemManager may probe the second serial port with AT commands.
Add a udev rule that sets `ID_MM_DEVICE_IGNORE` for `1d50:614e` on the
host (details in the bridge document).
