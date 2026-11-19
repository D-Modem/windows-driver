/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_link.c - a driver's end of the engine channel (dm_link.h)
 *
 */

#include "dm_link.h"

/* see dm_link.h */
void dm_link_init(struct dm_link *l, struct dm_port *p, const char *name)
{
	dm_u32 i = 0;
	memset(l, 0, sizeof(*l));
	l->port = p;
	while (name && name[i] && i < sizeof(l->name) - 1) {
		l->name[i] = name[i];
		i++;
	}
	l->name[i] = 0;
}

/* see dm_link.h: the engine's HELLO is waited for before its other
   frames are believed, but the port's modem is there at once (the
   engine has opened the channel) */
void dm_link_attach(struct dm_link *l, struct dm_batch *b)
{
	l->attached = 1;
	l->hello_out = 1;
	l->hello_in = 0;
	l->bad = 0;
	l->state_out = 1;
	l->held_n = 0;
	dm_port_connect(l->port, b, 1);
}

/* see dm_link.h */
void dm_link_detach(struct dm_link *l, struct dm_batch *b)
{
	if (!l->attached)
		return;
	l->attached = 0;
	l->hello_out = l->hello_in = 0;
	l->held_n = 0;
	dm_port_connect(l->port, b, 0);
}

/* see dm_link.h */
int dm_link_flush(struct dm_link *l, struct dm_batch *b)
{
	dm_u32 k;
	if (!l->held_n)
		return 1;
	k = dm_port_rx_put(l->port, b, l->held, l->held_n);
	if (k < l->held_n)
		memmove(l->held, l->held + k, l->held_n - k);
	l->held_n -= k;
	return l->held_n == 0;
}

/* see dm_link.h */
dm_u32 dm_link_feed(struct dm_link *l, struct dm_batch *b, const dm_u8 *in,
		    dm_u32 n)
{
	dm_u32 used = 0;
	if (!l->attached || l->bad)
		return 0;
	while (used < n) {
		struct dm_frame f;
		int len;
		if (!dm_link_flush(l, b))
			break;
		len = dm_wire_next(in + used, n - used, &f);
		if (len == 0)
			break;
		if (len < 0) {
			l->bad = 1;
			break;
		}
		if (f.type == DM_FRAME_HELLO) {
			dm_u16 v;
			if (!dm_wire_get_hello(&f, &v, NULL, 0) ||
			    v != DM_WIRE_VERSION) {
				l->bad = 1;
				break;
			}
			l->hello_in = 1;
		} else if (!l->hello_in) {
			/* (anything before the engine's HELLO) */
			l->bad = 1;
			break;
		} else if (f.type == DM_FRAME_LINES) {
			dm_u8 lines;
			if (dm_wire_get_lines(&f, &lines))
				dm_port_set_lines(l->port, b, lines);
		} else if (f.type == DM_FRAME_DATA) {
			dm_u32 k = dm_port_rx_put(l->port, b, f.p, f.n);
			if (k < f.n) {
				memcpy(l->held, f.p + k, f.n - k);
				l->held_n = f.n - k;
			}
		}
		/* (STATE is the driver's to send: ignored) */
		used += (dm_u32)len;
	}
	return used;
}

/* the app's side differs from what the engine was last told */
static int state_changed(const struct dm_link *l)
{
	return l->state_out ||
		dm_port_app_state(l->port) != l->sent_state ||
		l->port->baud != l->sent_baud;
}

/* see dm_link.h */
int dm_link_out_ready(const struct dm_link *l)
{
	if (!l->attached)
		return 0;
	return l->hello_out || state_changed(l) || l->port->tx_n > 0;
}

/* see dm_link.h */
dm_u32 dm_link_pull(struct dm_link *l, struct dm_batch *b, dm_u8 *out,
		    dm_u32 room)
{
	dm_u32 n = 0, k;
	if (!l->attached)
		return 0;
	if (l->hello_out) {
		k = dm_wire_hello(out, room, l->name);
		if (!k)
			return 0;
		n += k;
		l->hello_out = 0;
	}
	if (state_changed(l)) {
		dm_u8 st = dm_port_app_state(l->port);
		k = dm_wire_state(out + n, room - n, st, l->port->baud);
		if (!k)
			return n;
		n += k;
		l->sent_state = st;
		l->sent_baud = l->port->baud;
		l->state_out = 0;
	}
	while (l->port->tx_n && room - n > DM_WIRE_HEAD) {
		dm_u32 max = room - n - DM_WIRE_HEAD;
		if (max > DM_WIRE_MAX_DATA)
			max = DM_WIRE_MAX_DATA;
		k = dm_port_tx_take(l->port, b, out + n + DM_WIRE_HEAD, max);
		if (!k)
			break;
		out[n] = DM_FRAME_DATA;
		out[n + 1] = (dm_u8)(k & 0xff);
		out[n + 2] = (dm_u8)(k >> 8);
		n += DM_WIRE_HEAD + k;
	}
	return n;
}
