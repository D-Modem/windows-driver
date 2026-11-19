# windows-driver

D-Modem as the driver of a softmodem on legacy Windows: Windows 98 SE, ME,
2000 and XP first, then NT 4.0 and 95. The modem shows under Modems as an
ordinary modem, with a COM port, and Dial-Up Networking, HyperTerminal and
every TAPI or COM-port program use it. The modem itself (data pumps,
V.42/V.44, AT commands) is D-Modem's, running in user mode as its engine;
the kernel driver is a thin COM port and, later, the hardware's samples.

The design: [docs/design.md](docs/design.md).

## What is here (phase 1: a modem without hardware)

| Path | What |
| --- | --- |
| `common/dm_port.*` | the COM port: serial.sys's reads, writes, timeouts, waits and IOCTLs, in freestanding C (no locks or callbacks: each call returns a batch of completed requests) |
| `common/dm_wire.*`, `common/dm_link.*` | the engine channel: D-Modem's wire frames (HELLO, DATA, LINES, STATE) and the driver's end of it |
| `include/dmodem_ioctl.h` | the engine's IOCTLs on `\\.\DModemEngine<n>`, for the driver and the engine both |
| `nt/dmodem.c` | `dmodem.sys`, a WDM driver: the lower filter under Unimodem's `modem.sys` on 2000/XP (and, through CCPORT, 98 SE/ME) |
| `inf/dmodem.inf` | its install on 2000/XP (Add Hardware, Modems, Have Disk), made by `tools/make_inf.py` from D-Modem's INF |
| `tests/` | the port core's tests (from D-Modem's port_test.cpp) and the channel's |

## Building

The driver is built with [clang-retro](https://github.com/clang-retro)'s
toolchain, not a Microsoft DDK: one `dmodem.sys` for 2000, XP, 98 SE and ME.

```sh
make test                          # the shared code's tests (cc, ASan/UBSan)
make driver CLANG_DOS=/opt/clang-dos
make check  CLANG_DOS=/opt/clang-dos   # imports: 2000, 98 SE, ME
make inf DMODEM=../D-Modem         # inf/dmodem.inf from D-Modem's
```

CI does all of these (`.github/workflows/ci.yml`) with clang-retro's
`setup-toolchain` action and uploads the driver and INF.

Not yet: the engine side (`dmodemd --driver` speaking the IOCTLs), the 98 SE
INF, the hardware back ends, and any run on a real Windows.

## Licence

AGPL-3.0-only, as D-Modem.
