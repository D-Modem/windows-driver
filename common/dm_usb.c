/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_usb.c - the USB pass-through's checks and tables: see dm_usb.h.
 *
 */

#include "dm_usb.h"

DM_STATIC_ASSERT(sizeof(struct dmodem_usb_xfer) == 40, xfer_size);
DM_STATIC_ASSERT(sizeof(struct dmodem_usb_packet) == 12, packet_size);
DM_STATIC_ASSERT(sizeof(struct dmodem_usb_info) == 32, info_size);

/* see dm_usb.h */
int dm_usb_check(const struct dmodem_usb_xfer *x, dm_u32 inlen,
		 dm_u32 outlen)
{
	const struct dmodem_usb_packet *p;
	dm_u32 size, sum = 0, i;

	if (inlen < sizeof(*x) || outlen < sizeof(*x))
		return -1;
	if (x->length > DMODEM_USB_DATA_MAX)
		return -1;
	switch (x->type) {
	case DMODEM_USB_XFER_CONTROL:
		if (x->endpoint != 0 || x->npackets)
			return -1;
		/* (wLength is the data's length) */
		if (x->length != (dm_u32)(x->setup[6] | x->setup[7] << 8))
			return -1;
		break;
	case DMODEM_USB_XFER_ISO:
		if (!x->npackets || x->npackets > DMODEM_USB_PACKETS_MAX)
			return -1;
		/* fall through */
	case DMODEM_USB_XFER_BULK:
	case DMODEM_USB_XFER_INTERRUPT:
		if ((x->endpoint & 0x0f) == 0 || (x->endpoint & 0x70))
			return -1;
		if (x->type != DMODEM_USB_XFER_ISO && x->npackets)
			return -1;
		break;
	default:
		return -1;
	}
	size = (dm_u32)DMODEM_USB_XFER_SIZE(x->npackets, x->length);
	/* (the same buffer both ways: METHOD_BUFFERED's one system
	   buffer, which the data and the packets' results go back in) */
	if (inlen != size || outlen != size)
		return -1;
	p = (const struct dmodem_usb_packet *)(x + 1);
	for (i = 0; i < x->npackets; i++) {
		if (p[i].length > DMODEM_USB_DATA_MAX)
			return -1;
		sum += p[i].length;
	}
	if (x->npackets && sum != x->length)
		return -1;
	return 0;
}

/* see dm_usb.h */
int dm_usb_control(const dm_u8 setup[8], dm_u16 *function)
{
	dm_u8 type = (setup[0] >> 5) & 3, to = setup[0] & 0x1f;
	dm_u8 req = setup[1];
	/* (by the recipient: device, interface, endpoint, other) */
	static const dm_u16 vendor[4] = { DM_URB_VENDOR_DEVICE,
		DM_URB_VENDOR_INTERFACE, DM_URB_VENDOR_ENDPOINT,
		DM_URB_VENDOR_OTHER };
	static const dm_u16 klass[4] = { DM_URB_CLASS_DEVICE,
		DM_URB_CLASS_INTERFACE, DM_URB_CLASS_ENDPOINT,
		DM_URB_CLASS_OTHER };
	static const dm_u16 status[4] = { DM_URB_GET_STATUS_FROM_DEVICE,
		DM_URB_GET_STATUS_FROM_INTERFACE,
		DM_URB_GET_STATUS_FROM_ENDPOINT,
		DM_URB_GET_STATUS_FROM_OTHER };
	static const dm_u16 clear[4] = { DM_URB_CLEAR_FEATURE_TO_DEVICE,
		DM_URB_CLEAR_FEATURE_TO_INTERFACE,
		DM_URB_CLEAR_FEATURE_TO_ENDPOINT,
		DM_URB_CLEAR_FEATURE_TO_OTHER };
	static const dm_u16 set[4] = { DM_URB_SET_FEATURE_TO_DEVICE,
		DM_URB_SET_FEATURE_TO_INTERFACE,
		DM_URB_SET_FEATURE_TO_ENDPOINT,
		DM_URB_SET_FEATURE_TO_OTHER };
	static const dm_u16 descriptor[3] = {
		DM_URB_GET_DESCRIPTOR_FROM_DEVICE,
		DM_URB_GET_DESCRIPTOR_FROM_INTERFACE,
		DM_URB_GET_DESCRIPTOR_FROM_ENDPOINT };
	int in = (setup[0] & 0x80) != 0;
	dm_u16 wlength = (dm_u16)(setup[6] | setup[7] << 8);

	if (to > 3)
		return 0;
	if (type == 2) {
		*function = vendor[to];
		return DM_CTL_VENDOR_CLASS;
	}
	if (type == 1) {
		*function = klass[to];
		return DM_CTL_VENDOR_CLASS;
	}
	if (type != 0)
		return 0;
	switch (req) {
	case 0x00:      /* GET_STATUS */
		if (!in || wlength != 2)
			return 0;
		*function = status[to];
		return DM_CTL_STATUS;
	case 0x01:      /* CLEAR_FEATURE */
	case 0x03:      /* SET_FEATURE */
		if (in || wlength)
			return 0;
		*function = req == 0x01 ? clear[to] : set[to];
		return DM_CTL_FEATURE;
	case 0x06:      /* GET_DESCRIPTOR */
		if (!in || to > 2)
			return 0;
		*function = descriptor[to];
		return DM_CTL_DESCRIPTOR;
	default:
		return 0;
	}
}

/* see dm_usb.h */
dm_u32 dm_usb_status(dm_u32 usbd, int timed_out)
{
	if (usbd == DM_USBD_SUCCESS || !DM_USBD_IS_ERROR(usbd))
		return DMODEM_USB_COMPLETED;
	switch (usbd) {
	case DM_USBD_CANCELED:
		return timed_out ? DMODEM_USB_TIMED_OUT : DMODEM_USB_CANCELLED;
	case DM_USBD_STALL_PID:
	case DM_USBD_ENDPOINT_HALTED:
		return DMODEM_USB_STALL;
	case DM_USBD_DEV_NOT_RESPONDING:
	case DM_USBD_DEVICE_GONE:
		return DMODEM_USB_NO_DEVICE;
	case DM_USBD_DATA_OVERRUN:
	case DM_USBD_BUFFER_OVERRUN:
		return DMODEM_USB_OVERFLOW;
	default:
		return timed_out ? DMODEM_USB_TIMED_OUT : DMODEM_USB_ERROR;
	}
}

/* see dm_usb.h */
int dm_usb_cfg_check(const dm_u8 *cfg, dm_u32 len)
{
	dm_u32 at = 0;
	if (len < 9 || cfg[0] < 9 || cfg[1] != 2)
		return -1;
	if ((dm_u32)(cfg[2] | cfg[3] << 8) != len)
		return -1;
	while (at < len) {
		if (cfg[at] < 2 || cfg[at] > len - at)
			return -1;
		at += cfg[at];
	}
	return 0;
}

/* see dm_usb.h */
int dm_usb_cfg_next_alt(const dm_u8 *cfg, dm_u32 len, dm_u32 *pos,
			dm_u8 *itf, dm_u8 *alt, dm_u8 *endpoints)
{
	dm_u32 at = *pos;
	if (!at)
		at = cfg[0];    /* (past the configuration's own) */
	while (at + 2 <= len && cfg[at] >= 2 && cfg[at] <= len - at) {
		dm_u32 here = at;
		at += cfg[at];
		if (cfg[here + 1] == 4 && cfg[here] >= 9) {
			*itf = cfg[here + 2];
			*alt = cfg[here + 3];
			*endpoints = cfg[here + 4];
			*pos = at;
			return (int)here;
		}
	}
	*pos = len;
	return -1;
}

/* see dm_usb.h */
int dm_usb_cfg_find(const dm_u8 *cfg, dm_u32 len, dm_u8 itf, dm_u8 alt,
		    dm_u8 *endpoints)
{
	dm_u32 pos = 0;
	dm_u8 i, a, n;
	int at;
	while ((at = dm_usb_cfg_next_alt(cfg, len, &pos, &i, &a, &n)) >= 0)
		if (i == itf && a == alt) {
			*endpoints = n;
			return at;
		}
	return -1;
}

/* see dm_usb.h */
dm_u32 dm_usb_cfg_interfaces(const dm_u8 *cfg, dm_u32 len)
{
	dm_u32 pos = 0, n = 0;
	dm_u8 i, a, e;
	while (dm_usb_cfg_next_alt(cfg, len, &pos, &i, &a, &e) >= 0)
		if (a == 0)
			n++;
	return n;
}
