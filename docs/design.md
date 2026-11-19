# D-Modem as a softmodem driver on legacy Windows: design

Status: first draft, 14 Nov 2026; in the windows-driver repository since 19 Nov 2026,
with phase 1's first code (the port core, the engine channel, `nt/dmodem.c`).

## Goal

Install D-Modem on the own hardware ID of a winmodem, in place of the driver of the vendor,
on Windows 98 SE, ME, 2000 and XP (then NT 4.0, then 95/98 FE). The
board shows under Modems as a normal modem. Dial-Up Networking,
HyperTerminal, fax programs and all programs that use TAPI or a COM port operate
through it. The data pumps, V.42/V.44 and AT commands are those of D-Modem
(libdmodem), not those of the vendor.

Hardware in scope, in this order: USB HSF and Smart Link modems, Intel ICH AC'97
(MC97) modems, HD-audio modem codecs, AC'97 modems of other chipsets, then PCI
HSF/HCF research.

## What already exists in D-Modem to build on

- **libdmodem** (`core/`, `dsp/`, `link/`, `frontend/dmodemd/dmodem.h`): the
  full modem behind `dmodem_process(in, out, n)`, with a DTE byte stream. It has no
  threads, timers or signals of its own. C++17. The pumps use much floating point.
- **Line back ends in dmodemd**: AC'97 SmartDAA hook (`cx_daa.cpp`,
  `ac97_ports.cpp`), Si3054/Si3056 codecs (`si3054.cpp`, `si3056.cpp`), HDA
  codec setup (`hda_codec.cpp`), USB lines (`usb_line.cpp`, `usb_cadmus2.cpp`,
  `usb_agere.cpp`, `usb_apple.cpp`, `usb_wb.cpp`), with `AnalogHook` for hook,
  flash and pulse timing on the sample clock.
- **The Windows 7+ virtual modem** (`platforms/windows/winmodem/`):
  - a UMDF COM port with serial.sys semantics (`port.h/.cpp`),
  - the driver-to-dmodemd frame protocol (`wire.h`: HELLO, DATA, LINES, STATE),
  - a Unimodem INF generator (`make_inf.py`) with the responses of D-Modem,
  - an installer.
- **The Modem Development Kits of Microsoft** in the project files
  (`unimodem/MDK-1.40`, `unimodem/MDK-Win2000-DDK`): the 98 WDM modem stack
  (CCPORT, WDMMDMLD, ChildID), the 2000 stack (modem.sys), sample INFs.

None of the Windows 7+ driver runs on older Windows: UMDF does not exist before
XP SP2 (and never on 2000/9x), and port.cpp uses the STL and `std::mutex`.

## The main decision: where the DSP runs

Vendor softmodems (HSF, Smart Link, Agere) run the full DSP in the kernel.
**This design keeps libdmodem in user mode**, in a small engine process. A
thin kernel driver does only what must be in the kernel: the IRPs of the COM port
and the DMA, interrupts and registers of the hardware.

Why:
- libdmodem is C++17, with floating point in all parts. In an NT kernel, x86
  floating point must have `KeSaveFloatingPointState` around each use, and 9x
  ring 0 is worse. The STL, exceptions and RTTI are not available. A port of it
  into a kernel is a rewrite of its build, not a recompile.
- A bug in the DSP cannot cause a blue screen on the machine.
- The engine code (`dmodem_process`, the line back ends) is the same as on Linux. Thus,
  the tests that exist continue to cover it.
- dmodemd already operates with buffered sample paths that have jitter (SIP, USB). Thus, a
  20-40 ms ring between kernel and user mode is in the tolerance of the modem,
  if the ring never slips (tx and rx stay sample-locked for
  the echo canceller. An underrun is filled with silence and counted, as
  `usb_line` does).

Cost: the modem operates only while the engine runs. On NT, it is a service
(auto-start, so that dial-up operates before a user logs on). On 9x, it starts from
`RunServices`. If the engine is not attached, the port still opens, but it answers
nothing, with DSR off. Thus, Unimodem reports that the modem does not respond, and it does not
hang.

Fallback if the user-mode scheduling is too coarse on 9x on a slow machine: move
the pumps into the driver for 9x only (phase 6 below). Measure first.

## Driver model per Windows

One kernel driver, `dmodem.sys`, is the **function driver on the own device node of the hardware**
(the PCI modem function, the USB device, the HD-audio modem
function group). This is the vendor model, so it is a true drop-in replacement:
- In Device Manager, update the driver on the "HSF modem" that is there now.
- Select the INF of D-Modem.

Then the same device becomes a D-Modem device. A root-enumerated instance with no
hardware (SIP only, for tests) uses the same driver.

| Windows | Kernel model | How Unimodem reaches the port | Notes |
|---|---|---|---|
| **2000, XP** (x86; XP x64 later) | WDM, NT 5 | modem.sys over the serial-compatible FDO of `dmodem.sys` (serial IOCTLs, `PortName`, `SERIALCOMM`, `GUID_DEVINTERFACE_COMPORT`) | Primary target. Only DDIs that 2000 has (no `IoCsq*` except through `csq.lib`, no in-stack queued spinlocks). No driver signing enforced on 32-bit or XP x64. |
| **98 SE, ME** | the same WDM `dmodem.sys` (WDM 1.10 subset) | VCOMM → WDMMDMLD.VXD → CCPORT.SYS → `dmodem.sys`, with `DevLoader=*ntkern`, `NTMPDriver="dmodem.sys,ccport.sys"` and a `ChildID` for the modem devnode that CCPORT enumerates (MDK-Win2000-DDK, "WDM Mapping Drivers") | The same binary as 2000/XP. This is how vendors shipped one HSF driver for 98SE to XP. |
| **NT 4.0** | legacy NT kernel driver (no PnP): finds the PCI function with `HalGetBusData`, creates `\Device\DModem0` and a `COMn` link at load, writes `SERIALCOMM` | The TSP of Unimodem opens `\\.\COMn` | Shares the port core and back ends with the WDM driver. Only the load/PnP shell is different. No USB on NT 4. |
| **95, 98 FE** | VxDs: a VCOMM port driver (`PortDriver=dmport.vxd`) and the hardware access | VCOMM directly | Last. clang-retro builds VxDs (`win95-vxd.cfg`, `vxd-link.py`, below). The CCPORT support of 98 FE is too early to be reliable. |

`dmodem.sys` has three parts:

1. **Port core**: the serial semantics of the `port.cpp` of today (read/write
   queues, serial timeouts, wait masks, DTR/RTS/DCD/RI/DSR/CTS, purge, baud),
   written again as freestanding C with a small lock/queue abstraction. The same
   source builds in user mode on Linux with a pthread shim, so that `port_test` continues to
   test it.
2. **Engine channel**: what the named pipe does today, as IOCTLs on a private
   device interface (named pipes do not exist on 9x, and DeviceIoControl is
   the one path that operates in the same way on 98 SE to XP):
   - `ATTACH`: the engine claims the device. It gets the back end kind, hardware
     IDs and sample formats.
   - `WIRE_READ` / `WIRE_WRITE`: `wire.h` frames, not changed (the DATA and STATE of the app
     up, the DATA and LINES of the modem down). We can use the `--driver` code of dmodemd again
     almost without changes.
   - `SAMPLES_READ` / `SAMPLES_WRITE`: overlapped, two or three of each kept in
     flight, one period (10-20 ms) each. A copy of 640 bytes for each 20 ms is a very small load, and
     it prevents the need to map kernel memory into a process on 9x.
   - `HW_CTL`: register access for the logic that stays in user mode: AC'97
     codec registers, HDA verbs, USB control/interrupt transfers, hook relay.
     Then cx_daa, si3054/si3056 and usb_cadmus2 run without changes in the engine.
3. **Hardware back ends**: for each family, only DMA, interrupts and register
   transport (below).

## The engine (`dmodem-svc.exe`)

A new small front end on libdmodem: one modem for each attached device, the
`AnalogHook` back ends from dmodemd with their transport replaced by `HW_CTL`
and the sample IOCTLs. SIP, libusb and sockets are not necessary (an in-memory DTE
similar to a pipe replaces an internal DTE socket pair, or Winsock 2 on
98/2000 if that is simpler).

Toolchain: **clang-retro** (https://clang-retro.github.io/, Win32 SDK docs at
https://clang-retro.github.io/win32-sdk/), the choice of Sydney. Thus, nothing here
depends on an old compiler or the build system of an old DDK.
- It is a current clang/LLVM
  for `i386-pc-windows*-gnu`, with sysroots for 95/98/ME/NT 4.0 and 2000/XP.
- libc++, libc++abi and libunwind are there with `-stdlib=libc++`. `std::thread`
  and mutexes are implemented on Win32 with 9x workarounds. Thus, the C++17 of libdmodem
  builds without changes.
- C runtime: the system `msvcrt.dll` by default, or its
  `win32-picolibc` sysroot to fill the gaps of msvcrt.
- Only ANSI Win32 calls (9x has no Unicode-only APIs).
- A `win98_imports` check, similar to the `win7_imports` of today,
  causes the build to fail on any import that 98 SE or 2000 does not have.
- It runs on Linux hosts
  (`packaging/get-toolchain.sh`), and it has a GitHub Action
  (`clang-retro/dos-sdk/.github/actions/setup-toolchain`) for CI.
- The licence is the same as D-Modem (AGPL-3.0-only).

Real time: the sample thread at `THREAD_PRIORITY_TIME_CRITICAL` in
`REALTIME_PRIORITY_CLASS` (NT) / highest priority (9x).

## Build and test

- **Kernel drivers, also with clang-retro.** Its "Kernel-mode drivers" page
  (https://clang-retro.github.io/win32-sdk/drivers/) and win32-sdk's `tools/`
  cover every driver this design needs (checked 17 Nov 2026, after commits
  `fada7ce`, `091caaf`, `54ee868` and `aba7c39`):
  - **NT .sys**: `--config=win2000-ddk` (2000), `winxp-ddk` (XP), `nt4-ddk`
    (NT 4.0), with the open DDK headers of mingw-w64, `-nostdlib`,
    `DriverEntry`, and SEH (`__try`/`__except`/`__finally`). No Microsoft DDK,
    so CI builds it. `check-imports.py --nt <version>` checks the imports
    against `tools/kernel-versions.txt` (NT 3.1 to Server 2003), so a 2000 build
    fails on any XP-only call. `tools/drvsign.py` test-signs and makes the
    catalog (2000 and 32-bit XP don't check signatures anyway).
  - **One .sys for all four**: built with `nt4-ddk` (2000's headers, but
    subsystem version 4.00, which 98 SE/ME's NTKERN needs and 2000/XP
    load) and `-march=i586` (`nt4-ddk` targets the 386, whose missing
    `cmpxchg` would pull libgcc's `__sync_*`, which call `Sleep`). `make
    check` runs `--nt 2000`, `--wdm 98se` and `--wdm me` on it.
  - **The same .sys on 98 SE/ME**: `check-imports.py --wdm 98` (or `me`)
    checks against `tools/wdm9x-exports.txt`, the functions of the 98 and ME
    `wdm.lib` (336 on 98, 346 on ME). Everything the plan uses is there:
    `IoConnectInterrupt`, `IoGetDmaAdapter`, `IoRegisterDeviceInterface`,
    `IoCreateSymbolicLink`, `IoAllocateMdl`, `MmMapLockedPages`,
    `KeInsertQueueDpc`, `PoCallDriver`. No cancel-safe queues (`IoCsq*`), so the
    port core keeps its own queue and cancel logic, which it needs for NT 4 too.
    98's own `wdm.h` only comes from the user's 98 DDK (`ms-w98-wdm` kit); the
    NT headers plus this check are enough for CI.
  - **VxDs** (95, 98, ME; also 3.1): `--config=win95-vxd.cfg`, `vxd-link.py`
    (JWlink, LE, dynamic or `--static`), `vxd-check.py`. Its own headers, not
    Microsoft's: `vxd.h` (DDB and control procedure, `VXD_DEVICE_EX`,
    `VXD_SERVICES` for VxDs that provide services, init and pageable code
    sections) and `vxdsvc.h` with 383 typed C services
    (`tools/vxd-services.txt`, `tools/vxd-prototypes.txt`). For this design:
    - **VCOMM**: `_VCOMM_Register_Port_Driver`, `_VCOMM_Add_Port`, the
      `_VCOMM_*Comm*` calls, `_VCOMM_Get_Contention_Handler`,
      `_VCOMM_Map_Name_To_Resource`.
    - **VPICD**: `VPICD_Virtualize_IRQ` (with `struct VPICD_IRQ_Descriptor`),
      `VPICD_Phys_EOI`, `VPICD_Physically_Unmask`/`_Mask`.
    - **DMA memory**: `_PageAllocate` with `PG_SYS`, `PAGECONTIG`,
      `PAGEFIXED` and the physical address out; `_MapPhysToLinear`,
      `_LinPageLock`. VDMAD is there too (not needed for bus-master PCI).
    - **Waking the engine**: `VWIN32_DIOCCompletionRoutine` (completes an
      overlapped DeviceIoControl, so the engine's code is the same as on
      WDM), `_VWIN32_SetWin32Event`, `_VWIN32_QueueUserApc`.
    - **Timers and events**: `Set_Global_Time_Out`, `Schedule_Global_Event`,
      `Call_Priority_VM_Event`.
    - **PCI and resources**: `_CONFIGMG_*` (the devnode's IRQ and I/O ports
      from its allocated log conf, and PCI config space).
    - Freestanding C, no FPU (which fits keeping the DSP in user mode).
  - **VCOMM port driver layouts**: `<vcomm.h>` (added 17 Nov 2026) has the
    `PortFunctions` table (all 23 entries, with the undocumented
    `pPortDeviceIOCtl`), `PortData` with its queue and event fields, the ring-0
    `_DCB`, `_COMMPROP`, `_COMSTAT`, `COMMTIMEOUTS`, `IORequest`, and the
    `EV_*`, `CE_*`, `MS_*` and escape (`SETDTR`... and the 98/ME `PM_*` power)
    constants, checked against the DDK's offsets by `_Static_assert`s.
    `vxd/portdrv.c` (a loopback port driver, built but not loaded by its tests)
    is the starting point for `dmport.vxd`: `PortOpen` returns our `PortData`,
    whose functions map onto the shared port core, and `PST_MODEM` in
    `PortGetProperties`.
  - clang-retro status (checked 19 Nov 2026, after PR #26 merged): every
    toolchain need of this plan is now covered.
    - **USBD on 98 SE/ME**: `wdm9x-exports.txt` has USBD.SYS per version, and
      our USB driver links `-lusbd9x` (98 and 98 SE export three USBD
      functions only by decorated names; plain `-lusbd` loads only on ME+).
    - **Pending DeviceIoControl in a VxD**: `vxd_lock_global`, return
      `VXD_DIOC_PENDING`, later `VWIN32_DIOCCompletion(overlapped, bytes)`;
      sample `vxd/async.c`. The engine channel's model on 95/98 FE.
    - **VxD subsystem headers** (`sysroots/win9x-msvcrt/include/vxd`):
      `vpicd.h`, `vdmad.h`, `vtd.h`, `configmg.h`, `pci.h`, `vwin32.h`,
      `vpowerd.h` and more, layouts checked against the 95/98/ME DDKs; 1,391
      services in `<vxdsvc.h>`.
    - **HD-audio bus interface**: `<hdaudio.h>` (`HDAUDIO_BUS_INTERFACE`,
      `HDAUDIO_BUS_INTERFACE_BDL`, verbs, stream formats), written from the
      public DDI docs, with `tests/ddk/hdafunc.c` querying the interface.
      Its layouts are checked only against the documented sizes, so our
      phase 4 bring-up on XP is its first real test. HDA modems on 98/ME stay
      out of scope (draft PR #24's 98/ME HDA driver doesn't share the bus).
    - **Debugging**: `.SYM` files (`mapsym.py`, `vxd-link.py --sym`) for
      WDEB386 and SoftICE, with line numbers; `VXD_DEBUG` (`Trace_Out`,
      `Debug_Printf`, `VXD_ASSERT`); PDBs for WinDbg/KD on NT 4.0 and later
      (`-g -gcodeview -Wl,--pdb=`).
    - Not yet run on real Windows: the async, hook and bios VxD samples, and
      every NT/WDM driver sample (Wine only). Ours will be among the first.
  - Still to prove on real machines: clang-retro's DDK tests run in Wine and
    haven't run on real Windows yet; its VxD examples have run on 98 SE.
- **INFs**: `make_inf.py` gets legacy outputs: a 98 SE/ME INF (CCPORT,
  ChildID, VCOMM keys), a 2000/XP INF (`.NT` sections, modem.sys, the
  service), an NT 4 INF. All come from the same responses and hardware ID tables.
- **Tests without Windows**: the port core and the engine under Linux and Wine (as
  today). A fake device behind `HW_CTL` and the sample IOCTLs, so that the calls of the engine
  run against the modems of Conexant and Smart Link in the interop harness that we have now.
- **Tests on Windows**: QEMU or VirtualBox VMs of 98 SE, 2000 and XP with a
  real USB modem passed through (first milestone). Real PCs for AC'97 and HDA.
  86Box for the 9x install flow.
- **CPU budget**: early, do a benchmark of the V.34 and V.92 of libdmodem at the 9600 Hz rate on a
  Pentium II/III-class CPU. The own drivers of the vendors needed about a
  Pentium 233-300 MHz. If the float pumps of D-Modem are much slower, this sets the
  minimum machine, and it can cause a change to fixed point for some pumps.

## Hardware back ends

| Order | Family | Where it binds | Kernel part | Engine part (reused) |
|---|---|---|---|---|
| 1 | **USB**: Conexant Cadmus 2 HSF, Smart Link ST7554 (SmartUSB56, Creative Modem Blaster V.92 USB), Agere, Apple | the USB device (98 SE+, 2000, XP) | URB pass-through: control, bulk, interrupt, isochronous; the engine posts transfers through `HW_CTL` | `usb_cadmus2.cpp`, `usb_line.cpp`, `usb_agere.cpp`, `usb_apple.cpp`, on a small libusb-like shim |
| 2 | **Intel ICH AC'97 modem (MC97)**: 82801AA-ICH7 modem functions (8086:2416, 2426, 2446, 2486, 24C6, 24D6, 266D, 27DD and others) | that PCI function, separate from the audio function | the MC97 bus-master DMA (MI/MO buffer descriptor lists), the interrupt, codec register access through the ports of the ICH | `cx_daa.cpp`, `ac97_ports.cpp`, `si3054.cpp`, `si3056.cpp` |
| 3 | **HD-audio modem codecs** (Conexant D110/D330, other 14F1 and Agere/Si3054 MFG function groups) | `HDAUDIO\FUNC_02&...` children of the HD-audio bus driver of Microsoft (XP SP2 and 2000 SP4 with KB888111; to confirm 2000) | `GUID_HDAUDIO_BUS_INTERFACE`: stream allocation, DMA, verbs | `hda_codec.cpp`, the DAA code |
| 4 | **AC'97 on ATI, VIA, SiS, ALi, nForce** | their modem PCI functions | the DMA of each controller | the same codecs and DAAs as 2 |
| 5 | **PCI HSF Basic2/3, HCF** | research | own DSP or controller chip | refer to `hsf/plan.md` and `hsf/phoebe-notes.md` |

Firmware that is not redistributable (the `ROM_IMAGE` of Cadmus 2) is read from the
copy of the vendor driver of the user, as dmodemd does now. The installer asks for
the driver folder or CD of the vendor.

## Phases

0. **Engine first, on 2000/XP with com0com**, which supports 2000 and XP:
   `dmodem-svc.exe` built with clang-retro, on libusb-win32 (`libusb0.sys`, also 98 SE to XP), which controls
   a USB modem. A com0com pair is the port. This shows that the toolchain, the
   imports and the CPU budget are correct on real old Windows, before we write kernel code.
1. **`dmodem.sys` on 2000/XP**: port core, engine channel, root-enumerated
   with no hardware (a fake line), Unimodem INF. HyperTerminal and DUN dial a
   SIP-less loopback.
2. **USB back end** in `dmodem.sys` on 2000/XP: the first true drop-in replacement.
3. **98 SE/ME** through CCPORT, with the same binary.
4. **ICH AC'97**, then **HDA** on XP.
5. **NT 4.0** shell for the PCI back ends.
6. **95/98 FE** VxDs: `dmport.vxd` as VCOMM port driver with the same port
   core (C), and the PCI back ends (ICH AC'97 first) on VPICD and fixed DMA
   pages. The engine reaches it with DeviceIoControl, as on WDM, so the
   engine does not change. USB stays WDM only (USB on 98 is WDM).
   Kernel-side pumps for 9x only if the numbers of phase 0 show that user mode
   is too slow there; with no FPU in a VxD that would mean fixed point.

## Open questions

- The name of the new repository (Sydney creates it), and if it uses
  D-Modem as a submodule for libdmodem (recommended) or a copy.
- Which hardware Sydney has for each OS, to set the order of phases
  2 and 4.
- Licence: AGPL-3.0-only as D-Modem, or also the commercial terms.
- Is XP x64 wanted? (The WDM driver builds for amd64 with little
  extra work, and x64 XP does not enforce signing.)
- To confirm: the clang-retro build of `dmodem.sys` (`nt4-ddk`, subsystem 4.00) loads on real 2000 and XP, and the same binary on 98 SE.
- To confirm on real systems: CreateFile on a WDM symbolic link from a 98 SE
  process; the exact 98 SE INF keys against a shipped HSF 98 SE INF; the
  Windows 2000 support of KB888111.
