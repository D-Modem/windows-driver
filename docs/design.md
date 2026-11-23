# Design: D-Modem as a softmodem driver on legacy Windows

Status: the first draft is from 14 November 2026. The repository has the
code of phase 1 since 19 November 2026: the port core, the engine
channel, `nt/dmodem.c`, the INF files for 2000/XP and 98 SE/ME, and
`dmodem-svc.exe` (`engine/`) with the loopback line. It also has the
first part of the USB back end (`nt/usb.c`, refer to
[The USB back end](#the-usb-back-end)). Nobody has run this code on a
real Windows yet.

## Goal

Install D-Modem on the hardware ID of a winmodem, in place of the driver
of the vendor. Do this on Windows 98 SE, ME, 2000 and XP first, then on
NT 4.0, then on 95 and 98 FE. The board shows under Modems as a usual
modem. Dial-Up Networking, HyperTerminal, fax programs and all programs
that use TAPI or a COM port operate through it. The data pumps, V.42/V.44
and the AT commands are those of D-Modem (libdmodem), not those of the
vendor.

The hardware, in this sequence:

1. USB HSF and Smart Link modems.
2. Intel ICH AC'97 (MC97) modems.
3. HD-audio modem codecs.
4. AC'97 modems of other chipsets.
5. PCI HSF and HCF modems (research).

## What D-Modem already has

- **libdmodem** (`core/`, `dsp/`, `link/`, `frontend/dmodemd/dmodem.h`):
  the full modem behind `dmodem_process(in, out, n)`, with a DTE byte
  stream. It has no threads, timers or signals of its own. It is C++17.
  The pumps use much floating point.
- **The line back ends of dmodemd**: the AC'97 SmartDAA hook
  (`cx_daa.cpp`, `ac97_ports.cpp`), the Si3054 and Si3056 codecs
  (`si3054.cpp`, `si3056.cpp`), the HDA codec setup (`hda_codec.cpp`)
  and the USB lines (`usb_line.cpp`, `usb_cadmus2.cpp`, `usb_agere.cpp`,
  `usb_apple.cpp`, `usb_wb.cpp`). `AnalogHook` times the hook, the flash
  and the pulses on the sample clock.
- **The virtual modem for Windows 7 and later**
  (`platforms/windows/winmodem/`):
  - A UMDF COM port with the semantics of `serial.sys` (`port.h`,
    `port.cpp`).
  - The frame protocol between the driver and dmodemd (`wire.h`: HELLO,
    DATA, LINES, STATE).
  - A Unimodem INF generator (`make_inf.py`) with the responses of
    D-Modem.
  - An installer.
- **The Modem Development Kits of Microsoft** in the project files
  (`unimodem/MDK-1.40`, `unimodem/MDK-Win2000-DDK`): the WDM modem stack
  of 98 (CCPORT, WDMMDMLD, ChildID), the stack of 2000 (`modem.sys`) and
  sample INF files.

The driver for Windows 7 and later does not run on older Windows. UMDF
starts with XP SP2 (and is not on 2000 or 9x), and `port.cpp` uses the
STL and `std::mutex`.

## The main decision: where the DSP runs

The softmodems of the vendors (HSF, Smart Link, Agere) run the full DSP
in the kernel. **This design keeps libdmodem in user mode**, in a small
engine process. A small kernel driver does only the work that must be in
the kernel: the IRPs of the COM port, and the DMA, interrupts and
registers of the hardware.

The reasons:

- libdmodem is C++17, with floating point in all parts. In an NT kernel,
  each use of x86 floating point must have `KeSaveFloatingPointState`
  around it. Ring 0 of 9x is worse. The STL, exceptions and RTTI are not
  available. To put libdmodem in a kernel, you must write its build
  again. A new compile is not sufficient.
- A bug in the DSP cannot stop the machine with a blue screen.
- The engine code (`dmodem_process` and the line back ends) is the same
  as on Linux. Thus the tests that we have continue to cover it.
- dmodemd already operates with sample paths that have buffers and
  jitter (SIP, USB). Thus a ring of 20 to 40 ms between the kernel and
  user mode is in the tolerance of the modem, if the ring never slips.
  The transmit and receive samples must stay locked together for the
  echo canceller. The engine fills an underrun with silence and counts
  it, as `usb_line` does.

The cost: the modem operates only while the engine runs. On NT, the
engine is a service that starts automatically, so that dial-up operates
before a user logs in. On 9x, it starts from `RunServices`. If the
engine is not attached, the port opens, but it does not answer, and DSR
is off. Thus Unimodem reports that the modem does not respond, and it
does not stop.

If the scheduling of user mode on a slow 9x machine is too coarse, move
the pumps into the driver, for 9x only (phase 6). Measure first.

## The driver model for each version of Windows

One kernel driver, `dmodem.sys`, is the **function driver on the device
node of the hardware** (the PCI modem function, the USB device or the
HD-audio modem function group). This is the model of the vendors. Thus
it replaces the driver of the vendor directly:

1. In Device Manager, update the driver of the modem that is there now
   (for example, "HSF modem").
2. Select the INF of D-Modem.

Then the same device is a D-Modem device. A root-enumerated instance
without hardware (only SIP, for tests) uses the same driver.

| Windows | Kernel model | How Unimodem gets to the port | Notes |
|---|---|---|---|
| **2000, XP** (x86, and XP x64 later) | WDM, NT 5 | `modem.sys` over the serial-compatible FDO of `dmodem.sys` (serial IOCTLs, `PortName`, `SERIALCOMM`, `GUID_DEVINTERFACE_COMPORT`) | The primary target. Use only the DDIs that 2000 has: no `IoCsq*` (except through `csq.lib`) and no in-stack queued spinlocks. 32-bit Windows and XP x64 do not enforce driver signatures. |
| **98 SE, ME** | The same WDM `dmodem.sys` (the WDM 1.10 subset) | VCOMM, then `WDMMDMLD.VXD`, then `CCPORT.SYS`, then `dmodem.sys`. The INF has `DevLoader=*ntkern`, `NTMPDriver="dmodem.sys,ccport.sys"` and a `ChildID` for the modem devnode that CCPORT enumerates (MDK-Win2000-DDK, "WDM Mapping Drivers") | The same binary as on 2000 and XP. The vendors used this method to ship one HSF driver for 98 SE to XP. |
| **NT 4.0** | A legacy NT kernel driver (no PnP). It finds the PCI function with `HalGetBusData`. At load, it makes `\Device\DModem0` and a `COMn` link, and writes `SERIALCOMM` | The TSP of Unimodem opens `\\.\COMn` | It has the same port core and back ends as the WDM driver. Only the load and PnP part is different. NT 4.0 has no USB. |
| **95, 98 FE** | VxDs: a VCOMM port driver (`PortDriver=dmport.vxd`) and the hardware access | VCOMM directly | The last phase. clang-retro builds VxDs (`win95-vxd.cfg`, `vxd-link.py`). The CCPORT of 98 FE is too early to be reliable. |

`dmodem.sys` has three parts:

1. **The port core** (`common/dm_port.c`): the serial semantics of
   `port.cpp` (read and write queues, serial timeouts, wait masks,
   DTR/RTS/DCD/RI/DSR/CTS, purge and baud rate), written again in
   freestanding C. The port core takes no lock and calls nothing back.
   The driver holds its own lock around each call. Each call collects,
   in a `struct dm_batch`, the work to do after the driver releases the
   lock: the completed requests, and a message to the engine. The same
   source builds on Linux, so that `tests/port_test.c` tests it.
2. **The engine channel** (`include/dmodem_ioctl.h`): IOCTLs on a second
   device for each modem, `\\.\DModemEngine<n>`. Named pipes are not on
   9x, and DeviceIoControl operates in the same way on 98 SE to XP.
   - `IOCTL_DMODEM_INFO`: the version of the driver, the name of the COM
     port and the type of hardware (`DMODEM_HW_*`). The engine attaches
     when it opens the device, and detaches when it closes the device.
   - `IOCTL_DMODEM_WIRE_READ` and `IOCTL_DMODEM_WIRE_WRITE`: the frames
     of `wire.h`, without changes. The DATA and STATE of the app go up,
     and the DATA and LINES of the modem go down. The engine can use the
     `--driver` code of dmodemd again, almost without changes.
   - `IOCTL_DMODEM_SAMPLES_READ` and `IOCTL_DMODEM_SAMPLES_WRITE`
     (with the hardware back ends): overlapped requests, two or three in
     each direction, one period (10 to 20 ms) each. A copy of 640 bytes
     for each 20 ms is a very small load, and the driver does not have to
     map kernel memory into a process on 9x.
   - `IOCTL_DMODEM_HW_CTL` (with the hardware back ends): register
     access for the logic that stays in user mode: AC'97 codec
     registers, HDA verbs, USB control and interrupt transfers, and the
     hook relay. Then `cx_daa`, `si3054`, `si3056` and `usb_cadmus2` run
     in the engine without changes.
3. **The hardware back ends**: for each family, only the DMA, the
   interrupts and the register transport (refer to
   [Hardware back ends](#hardware-back-ends)).

## The engine (`dmodem-svc.exe`)

Phase 1 (19 November 2026): `engine/dmodem_svc.cpp` links libdmodem
(the `dmodem` target of D-Modem). The clang-retro `win32-picolibc`
configuration and `-D_GNU_SOURCE` compile the core, the pumps and the
link without changes. D-Modem/D-Modem#148 has the few changes to
`portable.h`.

The engine has one thread:

- One WIRE_READ is always pending (overlapped).
- The DTE of the modem is on a loopback socket pair.
- The engine steps the modem each 20 ms, timed by `timeGetTime`.

Its line is `engine/loopback.cpp`. Off hook, the line gives a dial tone
for 3 s. Then it rings an answering libdmodem modem with an echo
service. The two library modems (with no analog line) negotiate a
digital mode (62666 bit/s) on Linux. `--selftest` makes that call
without the driver. It reports the CPU time of each 20 ms step: the
measurement of phase 0. The imports of the engine pass the check for
98 and 2000 (`check-imports.py`, `--nt 5.0`).

At a later time, the engine is a new small front end on libdmodem: one
modem for each attached device, with the `AnalogHook` back ends of
dmodemd. `HW_CTL` and the sample IOCTLs replace their transport. The
engine does not need SIP, libusb or sockets. An in-memory DTE replaces
the DTE socket pair (or Winsock 2 on 98 and 2000, if that is easier).

The toolchain is **clang-retro** (<https://clang-retro.github.io/>, with
the Win32 SDK documentation at <https://clang-retro.github.io/win32-sdk/>).
Sydney selected it. Thus nothing here needs an old compiler or the build
system of an old DDK.

- It is a current clang/LLVM for `i386-pc-windows*-gnu`, with sysroots
  for 95/98/ME/NT 4.0 and for 2000/XP.
- It has libc++, libc++abi and libunwind (`-stdlib=libc++`).
  `std::thread` and the mutexes use Win32, with workarounds for 9x. Thus
  the C++17 of libdmodem builds without changes.
- The C runtime is the `msvcrt.dll` of the system by default. The
  `win32-picolibc` sysroot fills the gaps of msvcrt.
- The engine uses only the ANSI Win32 calls, because 9x does not have
  the Unicode-only APIs.
- `check-imports.py` makes the build fail on each import that 98 SE or
  2000 does not have (`make check` for the driver, `make engine` for the
  engine).
- It runs on Linux hosts (`packaging/get-toolchain.sh`). Its GitHub
  action (`clang-retro/dos-sdk/.github/actions/setup-toolchain`) installs
  it for CI.
- Its licence is the same as the licence of D-Modem (AGPL-3.0-only).

Real time: the sample thread runs at `THREAD_PRIORITY_TIME_CRITICAL` in
`REALTIME_PRIORITY_CLASS` on NT, and at the highest priority on 9x.

## Build and test

### Kernel drivers

clang-retro also builds the kernel drivers. Its "Kernel-mode drivers"
page (<https://clang-retro.github.io/win32-sdk/drivers/>) and the
`tools/` of win32-sdk cover each driver that this design needs. We
checked this on 17 November 2026, after the commits `fada7ce`, `091caaf`,
`54ee868` and `aba7c39`.

- **NT .sys files**: `--config=win2000-ddk` (2000), `winxp-ddk` (XP)
  and `nt4-ddk` (NT 4.0). They use the open DDK headers of mingw-w64,
  `-nostdlib`, `DriverEntry` and SEH (`__try`, `__except`,
  `__finally`). A Microsoft DDK is not necessary, thus CI builds the
  driver. `check-imports.py --nt <version>` checks the imports against
  `tools/kernel-versions.txt` (NT 3.1 to Server 2003). Thus a build for
  2000 fails on each call that only XP has. `tools/drvsign.py` signs the
  driver with a test key and makes the catalog. (2000 and 32-bit XP do
  not check signatures.)
- **One .sys file for all four versions**: the `Makefile` builds
  `dmodem.sys` with `nt4-ddk` and `-march=i586`. `nt4-ddk` uses the
  headers of 2000, but with subsystem version 4.00. The NTKERN of 98 SE
  and ME needs that version, and 2000 and XP load it. `nt4-ddk` targets
  the 386, which has no `cmpxchg`. Without `-march=i586`, the build
  links the `__sync_*` functions of libgcc, which call `Sleep`.
  `make check` runs `--nt 2000`, `--wdm 98se` and `--wdm me` on the
  driver.
- **The same .sys file on 98 SE and ME**: `check-imports.py --wdm 98`
  (or `me`) checks against `tools/wdm9x-exports.txt`, the functions of
  the `wdm.lib` of 98 (336) and of ME (346). The plan uses only
  functions that are in it: `IoConnectInterrupt`, `IoGetDmaAdapter`,
  `IoRegisterDeviceInterface`, `IoCreateSymbolicLink`, `IoAllocateMdl`,
  `MmMapLockedPages`, `KeInsertQueueDpc` and `PoCallDriver`. There are
  no cancel-safe queues (`IoCsq*`). Thus the port core has its own queue
  and cancel logic, which NT 4.0 also needs. The `wdm.h` of 98 comes only
  from the 98 DDK of the user (the `ms-w98-wdm` kit). The NT headers and
  this check are sufficient for CI.
- **VxDs** (95, 98, ME, and also 3.1): `--config=win95-vxd.cfg`,
  `vxd-link.py` (JWlink, LE, dynamic or `--static`) and `vxd-check.py`.
  clang-retro has its own headers, not those of Microsoft: `vxd.h` (the
  DDB and the control procedure, `VXD_DEVICE_EX`, `VXD_SERVICES` for
  VxDs that give services, and the init and pageable code sections), and
  `vxdsvc.h` with 383 typed C services (`tools/vxd-services.txt`,
  `tools/vxd-prototypes.txt`). This design uses these services:
  - **VCOMM**: `_VCOMM_Register_Port_Driver`, `_VCOMM_Add_Port`, the
    `_VCOMM_*Comm*` calls, `_VCOMM_Get_Contention_Handler` and
    `_VCOMM_Map_Name_To_Resource`.
  - **VPICD**: `VPICD_Virtualize_IRQ` (with
    `struct VPICD_IRQ_Descriptor`), `VPICD_Phys_EOI`,
    `VPICD_Physically_Unmask` and `VPICD_Physically_Mask`.
  - **DMA memory**: `_PageAllocate` with `PG_SYS`, `PAGECONTIG`,
    `PAGEFIXED` and the physical address as output, `_MapPhysToLinear`
    and `_LinPageLock`. VDMAD is also available, but bus-master PCI does
    not need it.
  - **Signals to the engine**: `VWIN32_DIOCCompletionRoutine` completes
    an overlapped DeviceIoControl, so that the code of the engine is the
    same as on WDM. Also `_VWIN32_SetWin32Event` and
    `_VWIN32_QueueUserApc`.
  - **Timers and events**: `Set_Global_Time_Out`,
    `Schedule_Global_Event` and `Call_Priority_VM_Event`.
  - **PCI and resources**: `_CONFIGMG_*` (the IRQ and I/O ports of the
    devnode from its allocated logical configuration, and the PCI
    configuration space).
  - Freestanding C, without the FPU. This agrees with a DSP in user mode.
- **VCOMM port driver layouts**: `<vcomm.h>` (added on 17 November 2026)
  has the `PortFunctions` table (all 23 entries, with the undocumented
  `pPortDeviceIOCtl`), `PortData` with its queue and event fields, the
  ring-0 `_DCB`, `_COMMPROP`, `_COMSTAT`, `COMMTIMEOUTS`, `IORequest`,
  and the `EV_*`, `CE_*`, `MS_*` and escape constants (`SETDTR` and the
  others, and the `PM_*` power constants of 98 and ME).
  `_Static_assert` checks them against the offsets of the DDK.
  `vxd/portdrv.c` (a loopback port driver that the tests build, but do
  not load) is the start point for `dmport.vxd`. `PortOpen` returns our
  `PortData`, whose functions use the shared port core, and
  `PortGetProperties` gives `PST_MODEM`.
- **The status of clang-retro** (checked on 19 November 2026, after its
  PR #26): it has all the tools that this plan needs.
  - **USBD on 98 SE and ME**: `wdm9x-exports.txt` has the USBD.SYS of
    each version. Our USB driver links `-lusbd9x`, because 98 and 98 SE
    export three USBD functions only with decorated names. A plain
    `-lusbd` loads only on ME and later.
  - **A pending DeviceIoControl in a VxD**: `vxd_lock_global`, return
    `VXD_DIOC_PENDING`, and at a later time call
    `VWIN32_DIOCCompletion(overlapped, bytes)`. The sample is
    `vxd/async.c`. This is the model of the engine channel on 95 and
    98 FE.
  - **VxD subsystem headers** (`sysroots/win9x-msvcrt/include/vxd`):
    `vpicd.h`, `vdmad.h`, `vtd.h`, `configmg.h`, `pci.h`, `vwin32.h`,
    `vpowerd.h` and more. Their layouts are checked against the DDKs of
    95, 98 and ME. `<vxdsvc.h>` has 1,391 services.
  - **The HD-audio bus interface**: `<hdaudio.h>`
    (`HDAUDIO_BUS_INTERFACE`, `HDAUDIO_BUS_INTERFACE_BDL`, the verbs and
    the stream formats), written from the public DDI documentation.
    `tests/ddk/hdafunc.c` queries the interface. Its layouts are checked
    only against the documented sizes. Thus our phase 4 on XP is its
    first real test. HDA modems on 98 and ME stay out of scope, because
    the 98/ME HDA driver of draft PR #24 does not share the bus.
  - **Debugging**: `.SYM` files (`mapsym.py`, `vxd-link.py --sym`) for
    WDEB386 and SoftICE, with line numbers. `VXD_DEBUG` (`Trace_Out`,
    `Debug_Printf`, `VXD_ASSERT`). PDB files for WinDbg and KD on NT 4.0
    and later (`-g -gcodeview -Wl,--pdb=`).
  - Not yet run on a real Windows: the async, hook and bios VxD samples,
    and all the NT and WDM driver samples (only in Wine). Our drivers
    will be among the first.
- **Still to prove on real machines**: the DDK tests of clang-retro run
  in Wine, and not yet on a real Windows. Its VxD examples ran on 98 SE.

### INF files

`tools/make_inf.py` makes the INF files from the INF of D-Modem
(`platforms/windows/winmodem/dmodem.inf`). Now it makes `dmodem.inf`
(2000 and XP: the `.NT` sections, `modem.sys` and the service),
`dmwdm98.inf` and `dmmdm98.inf` (98 SE and ME: CCPORT, ChildID and the
VCOMM keys). An INF for NT 4.0 will come later. All of them use the same
responses and hardware ID tables. `inf/dmusb.inf` (the USB
pass-through) is not made by the script.

### Tests

- **Tests without Windows**: the port core and the engine on Linux and in
  Wine (`make test`, `make engine-test`). At a later time, a fake device
  behind `HW_CTL` and the sample IOCTLs, so that the calls of the engine
  run against the Conexant and Smart Link modems in the interop harness
  of D-Modem.
- **Tests on Windows**: QEMU or VirtualBox machines with 98 SE, 2000 and
  XP, with a real USB modem passed through (the first milestone). Real
  PCs for AC'97 and HDA. 86Box for the installation on 9x.
- **CPU budget**: early, measure the V.34 and V.92 of libdmodem at the
  9600 Hz rate on a CPU of the Pentium II or III class. The drivers of
  the vendors needed approximately a Pentium at 233 to 300 MHz. If the
  floating-point pumps of D-Modem are much slower, this sets the minimum
  machine. Then some pumps can change to fixed point.

## Hardware back ends

| Sequence | Family | Where it binds | Kernel part | Engine part (used again) |
|---|---|---|---|---|
| 1 | **USB**: Conexant Cadmus 2 HSF, Smart Link ST7554 (SmartUSB56, Creative Modem Blaster V.92 USB), Agere, Apple | The USB device (98 SE and later, 2000, XP) | URB pass-through: control, bulk, interrupt and isochronous transfers. Now on `\\.\DModemUsb<n>` for dmodemd (refer to [The USB back end](#the-usb-back-end)). At a later time, the engine sends the transfers through `HW_CTL` | `usb_cadmus2.cpp`, `usb_line.cpp`, `usb_agere.cpp`, `usb_apple.cpp`, on a small libusb-like layer |
| 2 | **Intel ICH AC'97 modem (MC97)**: the modem functions of the 82801AA to ICH7 (8086:2416, 2426, 2446, 2486, 24C6, 24D6, 266D, 27DD and others) | That PCI function, which is separate from the audio function | The MC97 bus-master DMA (the MI and MO buffer descriptor lists), the interrupt, and the codec register access through the ports of the ICH | `cx_daa.cpp`, `ac97_ports.cpp`, `si3054.cpp`, `si3056.cpp` |
| 3 | **HD-audio modem codecs** (Conexant D110/D330, other 14F1 codecs, and Agere/Si3054 MFG function groups) | The `HDAUDIO\FUNC_02&...` children of the HD-audio bus driver of Microsoft (XP SP2, and 2000 SP4 with KB888111, to confirm for 2000) | `GUID_HDAUDIO_BUS_INTERFACE`: stream allocation, DMA and verbs | `hda_codec.cpp` and the DAA code |
| 4 | **AC'97 on ATI, VIA, SiS, ALi and nForce** | Their modem PCI functions | The DMA of each controller | The same codecs and DAAs as in 2 |
| 5 | **PCI HSF Basic2/3, HCF** | Research | Their own DSP or controller chip | Refer to `hsf/plan.md` and `hsf/phoebe-notes.md` |

### The USB back end

The first part of the USB back end (19 November 2026) is in `nt/usb.c`.
It does not use `HW_CTL` or the engine channel. It is a separate device:

- `dmusb.inf` installs `dmodem.sys` as the function driver of the USB
  modem. On 98 SE and ME, it uses `DevLoader=*ntkern` and
  `NTMPDriver=dmodem.sys`. On 2000 and XP, it installs the service
  `dmodem`. The value `DModemUsb=1` in the driver key, or the enumerator
  `USB`, tells `AddDevice` that the devnode is a USB modem.
- The driver selects the first configuration, with all interfaces at
  alternate setting 0. It makes `\Device\DModemUsb<n>` and
  `\DosDevices\DModemUsb<n>`.
- dmodemd of D-Modem (the `win98` build) opens `\\.\DModemUsb<n>`. Its
  `usb_win9x.cpp` does the libusb calls of `usb_line.cpp` with the
  IOCTLs of `include/dmodem_usb.h`. Thus `usb_line.cpp`,
  `usb_cadmus2.cpp`, `usb_agere.cpp`, `usb_apple.cpp` and `usb_wb.cpp`
  operate without changes, and dmodemd is the modem (with a COM port or
  TCP as its DTE).
- Each transfer is a URB in its own IRP. The IOCTL of dmodemd stays
  pending until the USB stack completes the URB. The URB uses the system
  buffer of the IOCTL (METHOD_BUFFERED). Thus the driver does not lock or
  map memory of the process. The CANCEL IOCTL, the cancellation of the
  IOCTL and the timeout of the transfer cancel the IRP of the URB. A
  timer (20 ms) tries again until the USB stack completes it.
- The driver makes the URBs itself. It uses no USBD.SYS function,
  because 98 and 98 SE export some of them only with decorated names.
- `common/dm_usb.c` has the checks of the transfers and the tables
  (control requests to URB functions, USB stack status to libusb status,
  and the configuration descriptor). `tests/usb_test.c` tests them.

The next step: the modem under Unimodem (a COM port in `dmodem.sys`,
with the engine as the modem). Then Dial-Up Networking can use a USB
modem without the DTE of dmodemd.

Some firmware cannot be distributed (the `ROM_IMAGE` of Cadmus 2). The
engine reads it from the copy of the vendor driver of the user, as
dmodemd does now. The installer asks for the folder or the CD of the
vendor driver.

## Phases

0. **The engine first, on 2000/XP with com0com** (com0com supports 2000
   and XP). Build `dmodem-svc.exe` with clang-retro, on libusb-win32
   (`libusb0.sys`, also 98 SE to XP), which controls a USB modem. A
   com0com pair is the port. This shows that the toolchain, the imports
   and the CPU budget are correct on real old Windows, before we write
   kernel code.
1. **`dmodem.sys` on 2000/XP**: the port core, the engine channel, a
   root-enumerated device without hardware (a loopback line), and the
   Unimodem INF. HyperTerminal and Dial-Up Networking dial the loopback,
   without SIP. The code is in the repository. It is not yet tried on a
   real Windows.
2. **The USB back end** in `dmodem.sys` on 2000/XP: the first direct
   replacement of a vendor driver. The first part is in the repository
   (pass-through to dmodemd).
3. **98 SE/ME** through CCPORT, with the same binary.
4. **ICH AC'97**, then **HDA**, on XP.
5. **The NT 4.0 driver** for the PCI back ends.
6. **VxDs for 95 and 98 FE**: `dmport.vxd` as the VCOMM port driver,
   with the same port core (C), and the PCI back ends (ICH AC'97 first)
   on VPICD and fixed DMA pages. The engine gets to it with
   DeviceIoControl, as on WDM. Thus the engine does not change. USB stays
   WDM only, because USB on 98 is WDM. Put the pumps in the kernel for
   9x only if the measurements of phase 0 show that user mode is too
   slow there. A VxD has no FPU. Thus the pumps in the kernel must use
   fixed point.

## Decisions

- The repository is `D-Modem/windows-driver`. It uses a D-Modem checkout
  for libdmodem (the `DMODEM` variable of the `Makefile`). It is not a
  submodule.
- The licence is AGPL-3.0-only, or a commercial licence from the D-Modem
  Authors, as in the other D-Modem repositories.

## Open questions

- Which hardware does Sydney have for each version of Windows? The answer
  sets the sequence of phases 2 and 4.
- Is XP x64 necessary? The WDM driver builds for amd64 with little more
  work, and XP x64 does not enforce signatures.
- To confirm: the clang-retro build of `dmodem.sys` (`nt4-ddk`, subsystem
  4.00) loads on real 2000 and XP, and the same binary loads on 98 SE.
- To confirm on real systems: CreateFile on a WDM symbolic link from a
  process on 98 SE, the exact INF keys of 98 SE against the INF of a
  shipped HSF driver for 98 SE, and the support of KB888111 on
  Windows 2000.
