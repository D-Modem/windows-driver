# Installing and trying it

Nobody has done these steps on a real machine yet.

## Windows 2000 and XP

1. Open Add Hardware, then Modems.
2. Select "Don't detect my modem", then Have Disk.
3. Select `dmodem.inf`. Put `dmodem.sys` in the same folder.
4. Start `dmodem-svc.exe`. It finds `\\.\DModemEngine0`.
5. In HyperTerminal, dial a number on the new modem. You get `CONNECT`,
   a greeting, and an echo of what you type.

`dmodem-svc --selftest` makes the same call inside the engine, without
the driver. It tells how much of the CPU the call uses. Do this test
first on an old machine.

## Windows 98 SE and ME

1. Put `dmwdm98.inf`, `dmmdm98.inf` and `dmodem.sys` in one folder.
2. Open Add New Hardware, then "Other devices", then Have Disk.
3. Select the folder. Windows installs the modem after the WDM device.

## Not done yet

- The engine as a service (NT) or from RunServices (9x).
- The back ends for hardware.
- A run on a real Windows.
