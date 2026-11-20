/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_nt.h - what the parts of dmodem.sys share: the start of every
 *              device's extension, and the USB pass-through's entry
 *              points (usb.c) for the driver's own (dmodem.c).
 *
 */
#ifndef DM_NT_H
#define DM_NT_H

#include <ntddk.h>

#define DM_TAG 'mdMD'
#define DM_VERSION "0.2.0"

/* the kinds of device */
#define KIND_PORT 1     /* a modem's COM port (dmodem.c) */
#define KIND_ENGINE 2   /* its engine's device (dmodem.c) */
#define KIND_USB 3      /* a USB modem (usb.c) */

struct dm_modem;

/* the start of every device's extension */
struct dm_dev {
	int kind;
	struct dm_modem *m;     /* the modem's (NULL once it has gone; NULL
				   for KIND_USB) */
};

static inline NTSTATUS dm_complete(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
	irp->IoStatus.Status = st;
	irp->IoStatus.Information = info;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return st;
}

/* usb.c: whether pdo is a USB modem's (its devnode's enumerator is USB,
   or its driver key has DModemUsb = 1: inf/dmusb.inf) */
BOOLEAN dm_usb_wanted(PDEVICE_OBJECT pdo);
/* its device, on top of pdo */
NTSTATUS dm_usb_add(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo);
/* every request to it */
NTSTATUS dm_usb_dispatch(PDEVICE_OBJECT dev, PIRP irp);

#endif
