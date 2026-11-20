/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_usb.h - the USB pass-through's checks and tables (dmodem_usb.h),
 *               in freestanding C, so that the build machine's tests
 *               (tests/usb_test.c) cover them: the engine's transfers
 *               checked before the driver uses them, the configuration
 *               descriptor's interfaces and endpoints, the control
 *               requests as the USB stack's URB functions, the USB
 *               stack's status as libusb's.
 *
 *    The driver (nt/usb.c) does the rest: the URBs and the IRPs.
 *
 */
#ifndef DM_USB_H
#define DM_USB_H

#include "dm_types.h"
#include "dmodem_usb.h"

/* the URB functions that dm_usb_control() gives (usb.h's numbers, the
   USB stack's since Windows 98) */
#define DM_URB_GET_DESCRIPTOR_FROM_DEVICE    0x000b
#define DM_URB_SET_FEATURE_TO_DEVICE         0x000d
#define DM_URB_SET_FEATURE_TO_INTERFACE      0x000e
#define DM_URB_SET_FEATURE_TO_ENDPOINT       0x000f
#define DM_URB_CLEAR_FEATURE_TO_DEVICE       0x0010
#define DM_URB_CLEAR_FEATURE_TO_INTERFACE    0x0011
#define DM_URB_CLEAR_FEATURE_TO_ENDPOINT     0x0012
#define DM_URB_GET_STATUS_FROM_DEVICE        0x0013
#define DM_URB_GET_STATUS_FROM_INTERFACE     0x0014
#define DM_URB_GET_STATUS_FROM_ENDPOINT      0x0015
#define DM_URB_VENDOR_DEVICE                 0x0017
#define DM_URB_VENDOR_INTERFACE              0x0018
#define DM_URB_VENDOR_ENDPOINT               0x0019
#define DM_URB_CLASS_DEVICE                  0x001a
#define DM_URB_CLASS_INTERFACE               0x001b
#define DM_URB_CLASS_ENDPOINT                0x001c
#define DM_URB_CLASS_OTHER                   0x001f
#define DM_URB_VENDOR_OTHER                  0x0020
#define DM_URB_GET_STATUS_FROM_OTHER         0x0021
#define DM_URB_CLEAR_FEATURE_TO_OTHER        0x0022
#define DM_URB_SET_FEATURE_TO_OTHER          0x0023
#define DM_URB_GET_DESCRIPTOR_FROM_ENDPOINT  0x0024
#define DM_URB_GET_DESCRIPTOR_FROM_INTERFACE 0x0028

/* the kinds of URB a control request needs (dm_usb_control's) */
#define DM_CTL_VENDOR_CLASS 1  /* _URB_CONTROL_VENDOR_OR_CLASS_REQUEST */
#define DM_CTL_DESCRIPTOR   2  /* _URB_CONTROL_DESCRIPTOR_REQUEST */
#define DM_CTL_FEATURE      3  /* _URB_CONTROL_FEATURE_REQUEST */
#define DM_CTL_STATUS       4  /* _URB_CONTROL_GET_STATUS_REQUEST */

/* the USB stack's status values that dm_usb_status() knows (usb.h's) */
#define DM_USBD_SUCCESS             0x00000000u
#define DM_USBD_STALL_PID           0xc0000004u
#define DM_USBD_DEV_NOT_RESPONDING  0xc0000005u
#define DM_USBD_DATA_OVERRUN        0xc0000008u
#define DM_USBD_BUFFER_OVERRUN      0xc000000cu
#define DM_USBD_ENDPOINT_HALTED     0xc0000030u
#define DM_USBD_DEVICE_GONE         0xc0007000u
#define DM_USBD_CANCELED            0xc0010000u
#define DM_USBD_ISO_NOT_ACCESSED_BY_HW   0xc0020000u
#define DM_USBD_ISO_NOT_ACCESSED_LATE    0xc0040000u
#define DM_USBD_ISO_NA_LATE_USBPORT      0xc0050000u
#define DM_USBD_ISO_NOT_ACCESSED_BY_HW_2 0xc0060000u
/* (USBD_STATUS_ERROR's bit: an error, whatever the rest) */
#define DM_USBD_IS_ERROR(s) (((s) & 0xc0000000u) == 0xc0000000u)
#define DM_USBD_IS_PENDING(s) (((s) & 0xc0000000u) == 0x40000000u)

/* the transfer in the IOCTL's buffer (in: inlen octets, out: outlen),
   checked: 0 if it is good, -1 if not.  Its packets (*packets) and data
   (*data) then follow it in the buffer */
int dm_usb_check(const struct dmodem_usb_xfer *x, dm_u32 inlen,
		 dm_u32 outlen);

/* a control transfer's setup packet: the URB kind (DM_CTL_*) and
   function (*function) for it, or 0 if it is not one the driver sends
   (SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE: the USB stack's, or
   the IOCTLs'; and the standard requests it does not know) */
int dm_usb_control(const dm_u8 setup[8], dm_u16 *function);

/* the USB stack's status (usbd) as a transfer's result (DMODEM_USB_*);
   timed_out: the driver cancelled it for its timeout */
dm_u32 dm_usb_status(dm_u32 usbd, int timed_out);

/* a configuration descriptor (cfg, len octets): 0 if its descriptors
   are whole and wTotalLength is len, else -1 */
int dm_usb_cfg_check(const dm_u8 *cfg, dm_u32 len);

/* its interface descriptors, from *pos (0 to start): the next one's
   number, alternate setting and endpoints, its offset as the return
   value, -1 when there are no more.  *pos moves past it */
int dm_usb_cfg_next_alt(const dm_u8 *cfg, dm_u32 len, dm_u32 *pos,
			dm_u8 *itf, dm_u8 *alt, dm_u8 *endpoints);

/* the interface descriptor of interface itf at alternate setting alt:
   its offset, and its endpoints (*endpoints), or -1 */
int dm_usb_cfg_find(const dm_u8 *cfg, dm_u32 len, dm_u8 itf, dm_u8 alt,
		    dm_u8 *endpoints);

/* the interfaces at alternate setting 0 (the ones a configuration's
   selection names) */
dm_u32 dm_usb_cfg_interfaces(const dm_u8 *cfg, dm_u32 len);

#endif
