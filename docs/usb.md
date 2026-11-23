# USB modems on 98 SE, ME, 2000 and XP

These versions of Windows have no WinUSB. Thus `dmodemd` of D-Modem cannot
use libusb on them. `dmodem.sys` is the driver of the USB modem in its
place. It sends the transfers of the modem (control, bulk, interrupt and
isochronous) to `dmodemd`. In D-Modem, `usb_win9x.cpp` does the libusb
calls on these transfers.

1. Connect the modem. When Windows asks for its driver, give the folder
   that has `dmusb.inf` and `dmodem.sys`. If the modem already has a
   driver, use Device Manager: the modem, Properties, Driver, Update
   Driver, then Have Disk.
2. Start `dmodemd.exe` of D-Modem (its `win98` build). Use
   `dmodemd.exe --usb list`, then `dmodemd.exe --usb auto`, with a COM
   port or TCP as its DTE.

`dmusb.inf` is one file for 98 SE, ME, 2000 and XP. It has the SmartUSB56,
the pico-serial sticks, and the Agere, Apple and Conexant USB modems.

The device of each modem is `\\.\DModemUsb<n>`. Only one process at a
time can use it. The IOCTLs are in `include/dmodem_usb.h`. D-Modem has a
copy of that file.

Windows 98 SE and ME have no USB 2.0. The Agere modem streams only at
high speed. Thus it does not operate on them.

Nobody has tried this on a real Windows yet.
