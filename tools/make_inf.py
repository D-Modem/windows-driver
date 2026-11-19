#!/usr/bin/env python3
#
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
#    make_inf.py - the INFs of inf/, with Unimodem's settings and
#                  responses for D-Modem taken as they are from D-Modem's
#                  own INF (platforms/windows/winmodem/dmodem.inf, which
#                  D-Modem's make_inf.py makes from the core's result
#                  codes), so that they never differ:
#                    dmodem.inf    Windows 2000 and XP: the modem, with
#                                  dmodem.sys below modem.sys
#                    dmwdm98.inf   Windows 98 SE and ME: the WDM device,
#                                  dmodem.sys with CCPORT.SYS above it,
#                                  whose child is the modem (ChildID)
#                    dmmdm98.inf   Windows 98 SE and ME: that modem, on
#                                  WDMMDMLD.VXD (VCOMM's port driver for
#                                  CCPORT)
#                  (98's two after the MDK's "WDM Modem" chapter:
#                  unimodem/MDK-Win2000-DDK/MDK.DOC)
#
#    usage: make_inf.py DMODEM_INF OUTDIR            write OUTDIR's INFs
#           make_inf.py DMODEM_INF OUTDIR --check    exit 1 if they are
#                                                    not what this writes
#

import os
import sys

# the sections of D-Modem's INF that are Unimodem's, not the driver's
TAKEN = ["DModem_All", "DModem_Settings", "DModem_Responses"]

NT5 = r"""; SPDX-License-Identifier: AGPL-3.0-only
; Copyright (C) 2026 D-Modem Authors
;
; dmodem.inf - D-Modem's modem for Windows 2000 and XP: a modem whose COM
; port is dmodem.sys, a WDM driver below Unimodem's modem.sys (its lower
; filter), served by D-Modem's engine on \\.\DModemEngine<n>.
;
; Made by tools/make_inf.py from D-Modem's dmodem.inf (its Unimodem
; settings and responses): change those, not this.  Install with Add
; Hardware (Control Panel), "Modems", "Don't detect my modem", "Have
; Disk": a root device, Root\DModem.  Windows 98 SE and ME have
; dmwdm98.inf and dmmdm98.inf.

[Version]
Signature   = "$Windows NT$"
Class       = Modem
ClassGuid   = {4D36E96D-E325-11CE-BFC1-08002BE10318}
Provider    = %Provider%
CatalogFile = dmodem.cat
DriverVer   = 11/19/2026,0.1.0.0

[DestinationDirs]
DefaultDestDir = 12

[SourceDisksNames]
1 = %Disk%

[SourceDisksFiles]
dmodem.sys = 1

; (no platform decoration: Windows 2000 reads only the plain section)
[Manufacturer]
%Mfg% = DModem

[DModem]
%ModemName% = DModem_Install, Root\DModem

; ---- the driver: dmodem.sys below modem.sys (modem.sys is the Modem
; class's function driver; the class installer gives the device its
; PortName, which dmodem.sys takes as its COM port's name)

[DModem_Install.NT]
CopyFiles = DModem_Files
AddReg    = DModem_All, DModem_Settings, DModem_Responses

[DModem_Files]
dmodem.sys

[DModem_Install.NT.HW]
AddReg = DModem_HW

[DModem_HW]
HKR,,"LowerFilters",0x00010000,"dmodem"

[DModem_Install.NT.Services]
AddService = dmodem,0x00000000,DModem_Service

[DModem_Service]
DisplayName   = %ServiceName%
ServiceType   = 1
StartType     = 3
ErrorControl  = 1
ServiceBinary = %12%\dmodem.sys

; ---- Unimodem's, from D-Modem's dmodem.inf
"""

NT5_TAIL = r"""
[Strings]
Provider    = "D-Modem"
Mfg         = "D-Modem"
ModemName   = "D-Modem"
Disk        = "D-Modem driver"
ServiceName = "D-Modem modem driver"
"""

# the modem's ID on 98: the child CCPORT makes of the WDM device (its
# ChildID), as the MDK has it, the bus's name (the device is a root
# one) and an EISA-style ID of its own, not the device's
CHILD_ID = r"ROOT\DMD0001"

WDM98 = r"""; SPDX-License-Identifier: AGPL-3.0-only
; Copyright (C) 2026 D-Modem Authors
;
; dmwdm98.inf - D-Modem on Windows 98 SE and ME, its WDM device: dmodem.sys
; (the same file as 2000's and XP's) with CCPORT.SYS above it, NTKERN
; their loader.  CCPORT then makes the modem (ChildID), which
; dmmdm98.inf installs.  Not yet tried on a real 98.
;
; Made by tools/make_inf.py.  Install with Add New Hardware (Control
; Panel), "No, the device isn't in the list", "Other devices", "Have
; Disk" (a root device, *DMD0000), dmodem.sys and both INFs in the same
; folder.

[Version]
Signature = "$CHICAGO$"
Class     = System
ClassGUID = {4D36E97D-E325-11CE-BFC1-08002BE10318}
Provider  = %Provider%
DriverVer = 11/19/2026,0.1.0.0

[DestinationDirs]
DefaultDestDir = 10,System32\Drivers

[SourceDisksNames]
1 = %Disk%,,,

[SourceDisksFiles]
dmodem.sys = 1

[Manufacturer]
%Mfg% = DModem

[DModem]
%DeviceName% = DModem_WDM, *DMD0000

[DModem_WDM]
CopyFiles = DModem_Files
AddReg    = DModem_WDM_Reg

[DModem_Files]
dmodem.sys

; NTKERN loads the stack: dmodem.sys (the device's) and CCPORT (above it,
; VCOMM's way in)
[DModem_WDM_Reg]
HKR,,DevLoader,0,*ntkern
HKR,,NTMPDriver,,"dmodem.sys,ccport.sys"

[DModem_WDM.HW]
AddReg = DModem_WDM_HW

[DModem_WDM_HW]
HKR,,ChildID,2,"@CHILD@"

[PreCopySection]
HKR,,NoSetupUI,,1

[Strings]
Provider   = "D-Modem"
Mfg        = "D-Modem"
DeviceName = "D-Modem WDM device"
Disk       = "D-Modem driver"
"""

MDM98 = r"""; SPDX-License-Identifier: AGPL-3.0-only
; Copyright (C) 2026 D-Modem Authors
;
; dmmdm98.inf - D-Modem on Windows 98 SE and ME, its modem: the device
; CCPORT makes as the child of D-Modem's WDM device (dmwdm98.inf), with
; WDMMDMLD.VXD as its VCOMM port driver (it hands VCOMM CCPORT's).
; Windows finds it by itself once dmwdm98.inf's device starts.  Not yet
; tried on a real 98.
;
; Made by tools/make_inf.py from D-Modem's dmodem.inf (its Unimodem
; settings and responses): change those, not this.

[Version]
Signature = "$CHICAGO$"
Class     = Modem
ClassGUID = {4D36E96D-E325-11CE-BFC1-08002BE10318}
Provider  = %Provider%
DriverVer = 11/19/2026,0.1.0.0

[Manufacturer]
%Mfg% = DModem

[DModem]
%ModemName% = DModem98, @CHILD@

[DModem98]
AddReg = DModem98_Port, DModem_All, DModem_Settings, DModem_Responses

; the modem's port: WDMMDMLD.VXD (CCPORT's), VCOMM the loader, as an
; internal modem's
[DModem98_Port]
HKR,,FriendlyDriver,,Unimodem.vxd
HKR,,DevLoader,,*vcomm
HKR,,PortDriver,,wdmmdmld.vxd
HKR,,Contention,0,

; ---- Unimodem's, from D-Modem's dmodem.inf
"""

MDM98_TAIL = r"""
[Strings]
Provider  = "D-Modem"
Mfg       = "D-Modem"
ModemName = "D-Modem"
"""


def sections(text):
    """{name: its lines, the header's comment lines before it included}"""
    out, name, lines, pending = {}, None, [], []
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            if name:
                out[name] = lines
            name, lines, pending = s[1:-1], pending + [line], []
        elif name is None or s.startswith(";") and not lines[-1].strip():
            # (a comment block after a blank line goes with the next section)
            pending.append(line)
        elif pending:
            pending.append(line)
        else:
            lines.append(line)
    if name:
        out[name] = lines
    return out


def unimodem(src):
    """D-Modem's Unimodem sections, as they are"""
    have = sections(open(src, newline="").read().replace("\r\n", "\n"))
    out = []
    for name in TAKEN:
        if name not in have:
            sys.exit("make_inf.py: %s has no [%s]" % (src, name))
        body = have[name]
        # (the section's own lines, not a comment meant for the next)
        while body and not body[-1].strip():
            body.pop()
        out.append("\n" + "\n".join(body).lstrip("\n") + "\n")
    return "".join(out)


def infs(src):
    """{name: text} of each INF (CR LF lines)"""
    u = unimodem(src)
    # (98's: the modem's DeviceType an internal one's, 02, as D-Modem's)
    texts = {
        "dmodem.inf": NT5 + u + NT5_TAIL,
        "dmwdm98.inf": WDM98,
        "dmmdm98.inf": MDM98 + u + MDM98_TAIL,
    }
    return {k: v.replace("@CHILD@", CHILD_ID).replace("\n", "\r\n")
            for k, v in texts.items()}


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: make_inf.py DMODEM_INF OUTDIR [--check]")
    bad = 0
    for name, text in infs(sys.argv[1]).items():
        path = os.path.join(sys.argv[2], name)
        if "--check" in sys.argv[3:]:
            try:
                have = open(path, newline="").read()
            except OSError:
                have = None
            if have != text:
                print("%s is not what make_inf.py makes: make inf" % path)
                bad = 1
            else:
                print("%s: %d responses" % (path,
                                            text.count("HKR,Responses")))
            continue
        with open(path, "w", newline="") as f:
            f.write(text)
    return bad


if __name__ == "__main__":
    sys.exit(main())
