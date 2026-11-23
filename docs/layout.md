# Source layout

| Path | What it is |
| --- | --- |
| `common/dm_port.*` | The COM port: the reads, writes, timeouts, waits and IOCTLs of `serial.sys`, in freestanding C. It has no locks or callbacks. Each call returns a batch of completed requests |
| `common/dm_wire.*`, `common/dm_link.*` | The engine channel: the wire frames of D-Modem (HELLO, DATA, LINES, STATE), and the end of the channel in the driver |
| `include/dmodem_ioctl.h` | The IOCTLs of the engine on `\\.\DModemEngine<n>`, for the driver and for the engine |
| `nt/dmodem.c` | `dmodem.sys`, a WDM driver. On 2000 and XP, it is the lower filter under `modem.sys` of Unimodem. On 98 SE and ME, it operates through CCPORT |
| `nt/usb.c`, `common/dm_usb.*`, `include/dmodem_usb.h` | The USB pass-through ([usb.md](usb.md)) |
| `inf/dmodem.inf` | The install on 2000 and XP (Add Hardware, Modems, Have Disk) |
| `inf/dmwdm98.inf`, `inf/dmmdm98.inf` | The install on 98 SE and ME: the WDM device (`dmodem.sys` under `CCPORT.SYS`), and the modem that CCPORT makes for it (on `WDMMDMLD.VXD`) |
| `inf/dmusb.inf` | The install of the USB pass-through, for 98 SE, ME, 2000 and XP |
| `tools/make_inf.py` | Makes `dmodem.inf`, `dmwdm98.inf` and `dmmdm98.inf` from the INF of D-Modem |
| `engine/` | `dmodem-svc.exe`, the engine: libdmodem behind the COM port of the driver. In phase 1, the line is a loopback. `ATD` with any number connects to an answering modem in the engine. The two modems do a real handshake, and the engine echoes what you type |
| `tests/` | The tests of the port core (from `port_test.cpp` of D-Modem), of the channel, of the USB pass-through and of the loopback line |
