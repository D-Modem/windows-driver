/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usb_test.c - the USB pass-through's checks and tables (dm_usb.h):
 *                 the engine's transfers, good and bad; the control
 *                 requests as URB functions; the USB stack's status as
 *                 libusb's; a SmartUSB56's configuration descriptor and
 *                 broken ones.
 *
 */

#include <stdlib.h>
#include <string.h>

#include "dm_usb.h"
#include "test.h"

static dm_u8 buf[DMODEM_USB_XFER_SIZE(8, 512)];

static struct dmodem_usb_xfer *xfer(dm_u8 type, dm_u8 ep, dm_u16 npackets,
				    dm_u32 length)
{
	struct dmodem_usb_xfer *x = (struct dmodem_usb_xfer *)buf;
	memset(buf, 0, sizeof(buf));
	x->type = type;
	x->endpoint = ep;
	x->npackets = npackets;
	x->length = length;
	return x;
}

static struct dmodem_usb_packet *packets(struct dmodem_usb_xfer *x)
{
	return (struct dmodem_usb_packet *)(x + 1);
}

TEST(check_transfers)
{
	struct dmodem_usb_xfer *x;
	dm_u32 n;

	/* control: wLength is the data's length */
	x = xfer(DMODEM_USB_XFER_CONTROL, 0, 0, 2);
	x->setup[0] = 0xc0;
	x->setup[6] = 2;
	n = (dm_u32)DMODEM_USB_XFER_SIZE(0, 2);
	CHECK_EQ(dm_usb_check(x, n, n), 0);
	CHECK_EQ(dm_usb_check(x, n - 1, n), -1);
	CHECK_EQ(dm_usb_check(x, n, n + 1), -1);
	x->setup[6] = 3;
	CHECK_EQ(dm_usb_check(x, n, n), -1);
	x->setup[6] = 2;
	x->endpoint = 0x81;
	CHECK_EQ(dm_usb_check(x, n, n), -1);

	/* bulk and interrupt: an endpoint, no packets */
	x = xfer(DMODEM_USB_XFER_BULK, 0x82, 0, 64);
	n = (dm_u32)DMODEM_USB_XFER_SIZE(0, 64);
	CHECK_EQ(dm_usb_check(x, n, n), 0);
	x->endpoint = 0x80;
	CHECK_EQ(dm_usb_check(x, n, n), -1);
	x->endpoint = 0x92;
	CHECK_EQ(dm_usb_check(x, n, n), -1);
	x = xfer(DMODEM_USB_XFER_INTERRUPT, 0x83, 1, 64);
	n = (dm_u32)DMODEM_USB_XFER_SIZE(1, 64);
	CHECK_EQ(dm_usb_check(x, n, n), -1);

	/* isochronous: the packets' lengths add up to the data's */
	x = xfer(DMODEM_USB_XFER_ISO, 0x81, 5, 5 * 32);
	n = (dm_u32)DMODEM_USB_XFER_SIZE(5, 5 * 32);
	for (int i = 0; i < 5; i++)
		packets(x)[i].length = 32;
	CHECK_EQ(dm_usb_check(x, n, n), 0);
	packets(x)[4].length = 31;
	CHECK_EQ(dm_usb_check(x, n, n), -1);
	packets(x)[4].length = 32;
	x->npackets = 0;
	CHECK_EQ(dm_usb_check(x, n, n), -1);
	x->npackets = 300;
	CHECK_EQ(dm_usb_check(x, n, n), -1);
	/* (a length that wraps the size: refused) */
	x = xfer(DMODEM_USB_XFER_ISO, 0x81, 1, 0xffffffffu);
	packets(x)[0].length = 0xffffffffu;
	CHECK_EQ(dm_usb_check(x, 52, 52), -1);

	/* the rest */
	x = xfer(7, 0x81, 0, 0);
	n = (dm_u32)DMODEM_USB_XFER_SIZE(0, 0);
	CHECK_EQ(dm_usb_check(x, n, n), -1);
	CHECK_EQ(dm_usb_check(x, 4, 4), -1);
}

TEST(control_requests)
{
	dm_u8 s[8] = { 0 };
	dm_u16 f = 0;

	/* vendor and class: each recipient */
	s[0] = 0x40;
	CHECK_EQ(dm_usb_control(s, &f), DM_CTL_VENDOR_CLASS);
	CHECK_EQ(f, DM_URB_VENDOR_DEVICE);
	s[0] = 0xc1;
	CHECK_EQ(dm_usb_control(s, &f), DM_CTL_VENDOR_CLASS);
	CHECK_EQ(f, DM_URB_VENDOR_INTERFACE);
	s[0] = 0x21;
	CHECK_EQ(dm_usb_control(s, &f), DM_CTL_VENDOR_CLASS);
	CHECK_EQ(f, DM_URB_CLASS_INTERFACE);
	s[0] = 0xa2;
	CHECK_EQ(dm_usb_control(s, &f), DM_CTL_VENDOR_CLASS);
	CHECK_EQ(f, DM_URB_CLASS_ENDPOINT);
	s[0] = 0x23;
	CHECK_EQ(dm_usb_control(s, &f), DM_CTL_VENDOR_CLASS);
	CHECK_EQ(f, DM_URB_CLASS_OTHER);
	s[0] = 0x44;
	CHECK_EQ(dm_usb_control(s, &f), 0);
	s[0] = 0x60;            /* (type 3: reserved) */
	CHECK_EQ(dm_usb_control(s, &f), 0);

	/* standard: the remote wakeup feature, a status, a descriptor */
	s[0] = 0x00;
	s[1] = 0x03;
	CHECK_EQ(dm_usb_control(s, &f), DM_CTL_FEATURE);
	CHECK_EQ(f, DM_URB_SET_FEATURE_TO_DEVICE);
	s[1] = 0x01;
	s[0] = 0x02;
	CHECK_EQ(dm_usb_control(s, &f), DM_CTL_FEATURE);
	CHECK_EQ(f, DM_URB_CLEAR_FEATURE_TO_ENDPOINT);
	s[0] = 0x80;
	s[1] = 0x00;
	s[6] = 2;
	CHECK_EQ(dm_usb_control(s, &f), DM_CTL_STATUS);
	CHECK_EQ(f, DM_URB_GET_STATUS_FROM_DEVICE);
	s[1] = 0x06;
	s[6] = 18;
	CHECK_EQ(dm_usb_control(s, &f), DM_CTL_DESCRIPTOR);
	CHECK_EQ(f, DM_URB_GET_DESCRIPTOR_FROM_DEVICE);
	s[0] = 0x81;
	CHECK_EQ(dm_usb_control(s, &f), DM_CTL_DESCRIPTOR);
	CHECK_EQ(f, DM_URB_GET_DESCRIPTOR_FROM_INTERFACE);

	/* the USB stack's own, and the IOCTLs' */
	s[0] = 0x00;
	s[6] = 0;
	s[1] = 0x05;            /* SET_ADDRESS */
	CHECK_EQ(dm_usb_control(s, &f), 0);
	s[1] = 0x09;            /* SET_CONFIGURATION */
	CHECK_EQ(dm_usb_control(s, &f), 0);
	s[0] = 0x01;
	s[1] = 0x0b;            /* SET_INTERFACE */
	CHECK_EQ(dm_usb_control(s, &f), 0);
	/* (a feature with data, a status the wrong way) */
	s[0] = 0x00;
	s[1] = 0x03;
	s[6] = 1;
	CHECK_EQ(dm_usb_control(s, &f), 0);
	s[1] = 0x00;
	s[6] = 2;
	CHECK_EQ(dm_usb_control(s, &f), 0);
}

TEST(status)
{
	CHECK_EQ(dm_usb_status(0, 0), DMODEM_USB_COMPLETED);
	CHECK_EQ(dm_usb_status(0, 1), DMODEM_USB_COMPLETED);
	CHECK_EQ(dm_usb_status(DM_USBD_CANCELED, 0), DMODEM_USB_CANCELLED);
	CHECK_EQ(dm_usb_status(DM_USBD_CANCELED, 1), DMODEM_USB_TIMED_OUT);
	CHECK_EQ(dm_usb_status(DM_USBD_STALL_PID, 0), DMODEM_USB_STALL);
	CHECK_EQ(dm_usb_status(DM_USBD_ENDPOINT_HALTED, 0), DMODEM_USB_STALL);
	CHECK_EQ(dm_usb_status(DM_USBD_DEV_NOT_RESPONDING, 0),
		 DMODEM_USB_NO_DEVICE);
	CHECK_EQ(dm_usb_status(DM_USBD_DEVICE_GONE, 0), DMODEM_USB_NO_DEVICE);
	CHECK_EQ(dm_usb_status(DM_USBD_BUFFER_OVERRUN, 0), DMODEM_USB_OVERFLOW);
	CHECK_EQ(dm_usb_status(DM_USBD_ISO_NOT_ACCESSED_LATE, 0),
		 DMODEM_USB_ERROR);
	/* (pending: not an error) */
	CHECK_EQ(dm_usb_status(0x40000000u, 0), DMODEM_USB_COMPLETED);
}

/* a SmartUSB56's configuration: interface 0 at alternate settings 0
   (no endpoints) and 1 (isochronous in and out, interrupt in) */
static const dm_u8 st7554_cfg[] = {
	9, 2, 48, 0, 1, 1, 0, 0x80, 50,
	9, 4, 0, 0, 0, 0xff, 0, 0, 0,
	9, 4, 0, 1, 3, 0xff, 0, 0, 0,
	7, 5, 0x81, 1, 32, 0, 1,
	7, 5, 0x02, 1, 32, 0, 1,
	7, 5, 0x83, 3, 8, 0, 16,
};

TEST(configuration)
{
	dm_u8 cfg[sizeof(st7554_cfg)], itf, alt, n;
	dm_u32 pos = 0;
	int at;

	memcpy(cfg, st7554_cfg, sizeof(cfg));
	CHECK_EQ(dm_usb_cfg_check(cfg, sizeof(cfg)), 0);
	CHECK_EQ(dm_usb_cfg_interfaces(cfg, sizeof(cfg)), 1);
	at = dm_usb_cfg_next_alt(cfg, sizeof(cfg), &pos, &itf, &alt, &n);
	CHECK_EQ(at, 9);
	CHECK_EQ(itf, 0);
	CHECK_EQ(alt, 0);
	CHECK_EQ(n, 0);
	at = dm_usb_cfg_next_alt(cfg, sizeof(cfg), &pos, &itf, &alt, &n);
	CHECK_EQ(at, 18);
	CHECK_EQ(alt, 1);
	CHECK_EQ(n, 3);
	CHECK_EQ(dm_usb_cfg_next_alt(cfg, sizeof(cfg), &pos, &itf, &alt, &n),
		 -1);
	CHECK_EQ(dm_usb_cfg_find(cfg, sizeof(cfg), 0, 1, &n), 18);
	CHECK_EQ(n, 3);
	CHECK_EQ(dm_usb_cfg_find(cfg, sizeof(cfg), 0, 2, &n), -1);
	CHECK_EQ(dm_usb_cfg_find(cfg, sizeof(cfg), 1, 0, &n), -1);

	/* broken: wTotalLength not the length, a descriptor past the end,
	   a zero length, not a configuration */
	CHECK_EQ(dm_usb_cfg_check(cfg, sizeof(cfg) - 1), -1);
	cfg[41] = 9;
	CHECK_EQ(dm_usb_cfg_check(cfg, sizeof(cfg)), -1);
	cfg[41] = 0;
	CHECK_EQ(dm_usb_cfg_check(cfg, sizeof(cfg)), -1);
	/* (and the walk stops there) */
	pos = 0;
	CHECK_EQ(dm_usb_cfg_next_alt(cfg, sizeof(cfg), &pos, &itf, &alt, &n),
		 9);
	CHECK_EQ(dm_usb_cfg_next_alt(cfg, sizeof(cfg), &pos, &itf, &alt, &n),
		 18);
	CHECK_EQ(dm_usb_cfg_next_alt(cfg, sizeof(cfg), &pos, &itf, &alt, &n),
		 -1);
	memcpy(cfg, st7554_cfg, sizeof(cfg));
	cfg[1] = 4;
	CHECK_EQ(dm_usb_cfg_check(cfg, sizeof(cfg)), -1);
	CHECK_EQ(dm_usb_cfg_check(cfg, 4), -1);

	/* two interfaces (an Apple USB Modem's kind) */
	{
		static const dm_u8 two[] = {
			9, 2, 41, 0, 2, 1, 0, 0x80, 50,
			9, 4, 0, 0, 1, 0xff, 2, 0, 0,
			7, 5, 0x83, 3, 8, 0, 16,
			9, 4, 1, 0, 0, 1, 2, 0, 0,
			7, 0x24, 1, 1, 1, 1, 0,
		};
		CHECK_EQ(dm_usb_cfg_check(two, sizeof(two)), 0);
		CHECK_EQ(dm_usb_cfg_interfaces(two, sizeof(two)), 2);
		CHECK_EQ(dm_usb_cfg_find(two, sizeof(two), 1, 0, &n), 25);
		CHECK_EQ(n, 0);
	}
}

int main(void)
{
	RUN(check_transfers);
	RUN(control_requests);
	RUN(status);
	RUN(configuration);
	return DONE();
}
