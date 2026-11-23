# windows-driver

windows-driver makes D-Modem the driver of a softmodem on legacy Windows:
Windows 98 SE, ME, 2000 and XP first, then NT 4.0 and 95.

The modem shows under Modems as a usual modem, with a COM port. Dial-Up
Networking, HyperTerminal and all TAPI or COM-port programs can use it. The
modem itself (the data pumps, V.42/V.44 and the AT commands) is D-Modem. It
runs in user mode, in the engine `dmodem-svc.exe`. The kernel driver
`dmodem.sys` is only a COM port and, at a later time, the path for the
samples of the hardware.

- `dmodem.sys`: one WDM driver for 98 SE, ME, 2000 and XP, built with [clang-retro](https://github.com/clang-retro)
- `dmodem-svc.exe`: the engine, with libdmodem behind the COM port of the driver
- The INF files for each version of Windows, made from the INF of D-Modem
- USB modems on Windows versions that have no WinUSB: `dmodem.sys` gives their transfers to `dmodemd`

Status: phase 1 (a modem without hardware). The line is a loopback in the
engine. Nobody has tried the driver on a real Windows yet.

## Quick start

```sh
make test                              # the tests of the shared code
make driver CLANG_DOS=/opt/clang-dos   # dmodem.sys
make engine DMODEM=../D-Modem          # dmodem-svc.exe
```

## Documentation

- [Building](docs/building.md): all the `make` targets, and what CI does
- [Installing and trying it](docs/installing.md): the loopback modem on 2000, XP, 98 SE and ME
- [USB modems](docs/usb.md): `dmodem.sys` as the driver of a USB modem for `dmodemd`
- [Source layout](docs/layout.md): what each file does
- [Design](docs/design.md): the goal, the phases and the hardware
- [Documentation style](docs/STYLE.md): the rules for ASD-STE100 Simplified Technical English

## Licence

AGPL-3.0-only ([LICENSE](LICENSE)), or a commercial licence from the D-Modem Authors ([COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md)). Contributions come with a contributor agreement ([CONTRIBUTING.md](CONTRIBUTING.md)).
