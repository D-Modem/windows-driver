/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_wire.c - the engine channel's frames (dm_wire.h)
 *
 */

#include "dm_wire.h"

/* see dm_wire.h */
dm_u32 dm_wire_put(dm_u8 *out, dm_u32 room, dm_u8 t, const void *p,
		   dm_u32 n)
{
	if (n > 0xffff || room < DM_WIRE_HEAD + n)
		return 0;
	out[0] = t;
	out[1] = (dm_u8)(n & 0xff);
	out[2] = (dm_u8)(n >> 8);
	if (n)
		memcpy(out + DM_WIRE_HEAD, p, n);
	return DM_WIRE_HEAD + n;
}

/* see dm_wire.h */
dm_u32 dm_wire_hello(dm_u8 *out, dm_u32 room, const char *port)
{
	dm_u8 p[2 + 255];
	dm_u32 n = 0;
	p[0] = (dm_u8)(DM_WIRE_VERSION & 0xff);
	p[1] = (dm_u8)(DM_WIRE_VERSION >> 8);
	while (port && port[n] && n < 255) {
		p[2 + n] = (dm_u8)port[n];
		n++;
	}
	return dm_wire_put(out, room, DM_FRAME_HELLO, p, 2 + n);
}

/* see dm_wire.h */
dm_u32 dm_wire_lines(dm_u8 *out, dm_u32 room, dm_u8 lines)
{
	return dm_wire_put(out, room, DM_FRAME_LINES, &lines, 1);
}

/* see dm_wire.h */
dm_u32 dm_wire_state(dm_u8 *out, dm_u32 room, dm_u8 state, dm_u32 baud)
{
	dm_u8 p[5];
	p[0] = state;
	p[1] = (dm_u8)baud;
	p[2] = (dm_u8)(baud >> 8);
	p[3] = (dm_u8)(baud >> 16);
	p[4] = (dm_u8)(baud >> 24);
	return dm_wire_put(out, room, DM_FRAME_STATE, p, sizeof(p));
}

/* see dm_wire.h */
int dm_wire_next(const dm_u8 *buf, dm_u32 n, struct dm_frame *f)
{
	dm_u32 len;
	if (n < 1)
		return 0;
	switch (buf[0]) {
	case DM_FRAME_HELLO:
	case DM_FRAME_DATA:
	case DM_FRAME_LINES:
	case DM_FRAME_STATE:
		break;
	default:
		return -1;
	}
	if (n < DM_WIRE_HEAD)
		return 0;
	len = buf[1] | (dm_u32)buf[2] << 8;
	if (buf[0] == DM_FRAME_DATA && len > DM_WIRE_MAX_DATA)
		return -1;
	if (n < DM_WIRE_HEAD + len)
		return 0;
	f->type = buf[0];
	f->p = buf + DM_WIRE_HEAD;
	f->n = len;
	return (int)(DM_WIRE_HEAD + len);
}

/* see dm_wire.h */
int dm_wire_get_hello(const struct dm_frame *f, dm_u16 *version,
		      char *name, dm_u32 size)
{
	dm_u32 i, k;
	if (f->type != DM_FRAME_HELLO || f->n < 2)
		return 0;
	*version = (dm_u16)(f->p[0] | f->p[1] << 8);
	if (name && size) {
		k = f->n - 2;
		if (k > size - 1)
			k = size - 1;
		for (i = 0; i < k; i++)
			name[i] = (char)f->p[2 + i];
		name[k] = 0;
	}
	return 1;
}

/* see dm_wire.h */
int dm_wire_get_lines(const struct dm_frame *f, dm_u8 *lines)
{
	if (f->type != DM_FRAME_LINES || f->n < 1)
		return 0;
	*lines = f->p[0];
	return 1;
}

/* see dm_wire.h */
int dm_wire_get_state(const struct dm_frame *f, dm_u8 *state, dm_u32 *baud)
{
	if (f->type != DM_FRAME_STATE || f->n < 5)
		return 0;
	*state = f->p[0];
	*baud = f->p[1] | (dm_u32)f->p[2] << 8 | (dm_u32)f->p[3] << 16 |
		(dm_u32)f->p[4] << 24;
	return 1;
}
