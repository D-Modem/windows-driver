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
| `inf/dmodem.inf` | its install on 2000/XP (Add Hardware, Modems, Have Disk) |
| `inf/dmwdm98.inf`, `inf/dmmdm98.inf` | its install on 98 SE/ME: the WDM device (dmodem.sys under CCPORT.SYS) and the modem CCPORT makes for it (on WDMMDMLD.VXD). All three INFs are made by `tools/make_inf.py` from D-Modem's INF |
| `engine/` | `dmodem-svc.exe`, the engine: D-Modem's libdmodem behind the driver's COM port. Phase 1's line is a loopback: `ATD` anything reaches an answering modem in the engine, the two run a real handshake, and it echoes what you type |
| `tests/` | the port core's tests (from D-Modem's port_test.cpp), the channel's, and the loopback line's |

## Building

The driver is built with [clang-retro](https://github.com/clang-retro)'s
toolchain, not a Microsoft DDK: one `dmodem.sys` for 2000, XP, 98 SE and ME.

```sh
make test                          # the shared code's tests (cc, ASan/UBSan)
make driver CLANG_DOS=/opt/clang-dos
make check  CLANG_DOS=/opt/clang-dos   # imports: 2000, 98 SE, ME
make inf DMODEM=../D-Modem         # inf/*.inf from D-Modem's
make engine DMODEM=../D-Modem      # dmodem-svc.exe (cmake), imports: 98, 2000
make engine-test DMODEM=../D-Modem # the loopback line, on this machine
```

The engine needs a D-Modem checkout (`DMODEM`) with D-Modem/D-Modem#148
(libdmodem on 98 to XP). CI does all of these (`.github/workflows/ci.yml`)
with clang-retro's `setup-toolchain` action and uploads the driver, the
engine and the INFs.

## Trying it (not yet done on a real machine)

On 2000 or XP: Add Hardware, Modems, "Don't detect my modem", Have Disk,
`dmodem.inf` (with `dmodem.sys` beside it). Then run `dmodem-svc.exe`
(it finds `\\.\DModemEngine0`) and dial any number from HyperTerminal on
the new modem: CONNECT, a greeting, and an echo. `dmodem-svc --selftest`
runs the same call inside the engine alone and says how much of the CPU
it takes, which is worth running first on an old machine.

On 98 SE or ME: Add New Hardware, "Other devices", Have Disk, with
`dmwdm98.inf`, `dmmdm98.inf` and `dmodem.sys` in one folder; the modem
follows by itself.

Not yet: the engine as a service (NT) or from RunServices (9x), the
hardware back ends, and any run on a real Windows.

## Licence

AGPL-3.0-only, as D-Modem.
