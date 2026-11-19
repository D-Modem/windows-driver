/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem_ioctl.h - the engine channel: how D-Modem's engine (the
 *                     modem: dmodemd --driver, or dmodem-svc) reaches a
 *                     D-Modem driver's device, on every Windows the
 *                     drivers run on (98 SE, ME, 2000, XP; later NT 4.0,
 *                     95).  For the driver and the engine both: C, no
 *                     Windows header needed.
 *
 *    Each modem has a second device beside its COM port, for its engine
 *    alone: \\.\DModemEngine<n> (n from 0, in the order the modems
 *    started).  One handle at a time.  On it:
 *
 *    IOCTL_DMODEM_INFO (out: struct dmodem_info)
 *        what the modem is: the driver's version, the COM port's name,
 *        the hardware behind it (DMODEM_HW_*).
 *    IOCTL_DMODEM_WIRE_READ (out: frames, up to the buffer's size)
 *        the frames for the engine (dm_wire.h): the driver's HELLO
 *        first, then the app's side (STATE) and characters (DATA).  It
 *        waits (overlapped) until there is something; one at a time.
 *        Opening the device is the engine's coming: the port's modem is
 *        there (its lines as the engine gives them).  Closing it is the
 *        engine's going: the modem's lines off, the app's characters
 *        dropped.
 *    IOCTL_DMODEM_WIRE_WRITE (in: frames)
 *        the engine's frames: its HELLO first, then the modem's lines
 *        (LINES) and characters (DATA).  It returns at once, with the
 *        bytes taken (whole frames) as its count: fewer than given when
 *        the app's receive buffer is full, the rest to be written again
 *        after the next WIRE_READ or in 10 ms or so.  It fails
 *        (STATUS_INVALID_PARAMETER) once the frames can't be read: the
 *        engine should close the device and open it again.
 *
 *    To come with the hardware back ends (STATUS_NOT_SUPPORTED on a
 *    modem without hardware):
 *    IOCTL_DMODEM_SAMPLES_READ, IOCTL_DMODEM_SAMPLES_WRITE
 *        one period of the line's samples each way, overlapped, two or
 *        three in flight.
 *    IOCTL_DMODEM_HW_CTL
 *        the hardware's registers for the line code that stays in the
 *        engine: AC'97 codec registers, HD-audio verbs, USB transfers.
 *
 */
#ifndef DMODEM_IOCTL_H
#define DMODEM_IOCTL_H

/* CTL_CODE(FILE_DEVICE_UNKNOWN, fn, METHOD_BUFFERED, FILE_ANY_ACCESS),
   with D-Modem's functions from 0x800 */
#define DMODEM_IOCTL(fn) (0x00220000u | ((unsigned)(fn) << 2))
#define IOCTL_DMODEM_INFO           DMODEM_IOCTL(0x800)
#define IOCTL_DMODEM_WIRE_READ      DMODEM_IOCTL(0x801)
#define IOCTL_DMODEM_WIRE_WRITE     DMODEM_IOCTL(0x802)
#define IOCTL_DMODEM_SAMPLES_READ   DMODEM_IOCTL(0x810)
#define IOCTL_DMODEM_SAMPLES_WRITE  DMODEM_IOCTL(0x811)
#define IOCTL_DMODEM_HW_CTL         DMODEM_IOCTL(0x820)

/* the engine's device: L"\\Device\\DModemEngine" and
   L"\\DosDevices\\DModemEngine", then the modem's number */
#define DMODEM_ENGINE_NAME "DModemEngine"

/* this header's version (struct dmodem_info's) */
#define DMODEM_IOCTL_VERSION 1u

/* the hardware behind a modem */
#define DMODEM_HW_NONE        0u  /* none: a virtual modem (SIP only) */
#define DMODEM_HW_USB         1u
#define DMODEM_HW_ICH_MC97    2u
#define DMODEM_HW_HDA         3u
#define DMODEM_HW_MC97_OTHER  4u

struct dmodem_info {
	unsigned int version;         /* DMODEM_IOCTL_VERSION */
	unsigned int hw;              /* DMODEM_HW_* */
	unsigned int vendor, device;  /* the hardware's PCI or USB IDs,
					 0 for none */
	char port[32];                /* the COM port's name ("COM3"),
					 NUL-terminated, "" if it has none */
	char driver[32];              /* the driver's version */
};

#endif
