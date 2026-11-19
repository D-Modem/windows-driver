#!/usr/bin/env python3
#
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 D-Modem Authors
#
#    make_inf.py - inf/dmodem.inf: dmodem.sys's install for Windows 2000
#                  and XP, with Unimodem's settings and responses for
#                  D-Modem taken as they are from D-Modem's own INF
#                  (platforms/windows/winmodem/dmodem.inf, which D-Modem's
#                  make_inf.py makes from the core's result codes), so that
#                  the two never differ.
#
#    usage: make_inf.py DMODEM_INF OUT            write OUT
#           make_inf.py DMODEM_INF OUT --check    exit 1 if OUT is not
#                                                 what this would write
#

import sys

# the sections of D-Modem's INF that are Unimodem's, not the driver's
TAKEN = ["DModem_All", "DModem_Settings", "DModem_Responses"]

HEAD = r"""; SPDX-License-Identifier: AGPL-3.0-only
; Copyright (C) 2026 D-Modem Authors
;
; dmodem.inf - D-Modem's modem for Windows 2000 and XP: a modem whose COM
; port is dmodem.sys, a WDM driver below Unimodem's modem.sys (its lower
; filter), served by D-Modem's engine on \\.\DModemEngine<n>.
;
; Made by tools/make_inf.py from D-Modem's dmodem.inf (its Unimodem
; settings and responses): change those, not this.  Install with Add
; Hardware (Control Panel), "Modems", "Don't detect my modem", "Have
; Disk": a root device, Root\DModem.  Windows 98 SE and ME will have their
; own INF.

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

TAIL = r"""
[Strings]
Provider    = "D-Modem"
Mfg         = "D-Modem"
ModemName   = "D-Modem"
Disk        = "D-Modem driver"
ServiceName = "D-Modem modem driver"
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


def inf(src):
    have = sections(open(src, newline="").read().replace("\r\n", "\n"))
    out = [HEAD]
    for name in TAKEN:
        if name not in have:
            sys.exit("make_inf.py: %s has no [%s]" % (src, name))
        body = have[name]
        # (the section's own lines, not a comment meant for the next)
        while body and not body[-1].strip():
            body.pop()
        out.append("\n" + "\n".join(body).lstrip("\n") + "\n")
    out.append(TAIL)
    return "".join(out).replace("\n", "\r\n")


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: make_inf.py DMODEM_INF OUT [--check]")
    text = inf(sys.argv[1])
    if "--check" in sys.argv[3:]:
        try:
            have = open(sys.argv[2], newline="").read()
        except OSError:
            have = None
        if have != text:
            print("%s is not what make_inf.py makes: make inf" % sys.argv[2])
            return 1
        print("%s: %d responses" % (sys.argv[2],
                                    text.count("HKR,Responses")))
        return 0
    with open(sys.argv[2], "w", newline="") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
