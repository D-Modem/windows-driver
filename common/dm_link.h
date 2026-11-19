/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_link.h - a driver's end of the engine channel: the wire frames
 *                (dm_wire.h) between the COM port (dm_port.h) and the
 *                engine.
 *
 *      - The engine attaches (it opens the channel) and detaches (it
 *        closes it, or goes away): the port's modem there or gone.
 *      - Out, to the engine: our HELLO first, then the app's side
 *        (STATE) whenever it changes, and the app's characters (DATA).
 *      - In, from the engine: its HELLO, the modem's lines (LINES) and
 *        characters (DATA).  What the port has no room for is held
 *        here, and the engine's next frames wait until it has gone in.
 *
 *    Like the port it takes no lock and calls nothing back: the driver
 *    calls it under the same lock as the port, with a batch.
 *
 */
#ifndef DM_LINK_H
#define DM_LINK_H

#include "dm_port.h"
#include "dm_wire.h"

struct dm_link {
	struct dm_port *port;
	char name[32];          /* the COM port's, for our HELLO */
	dm_u8 attached;
	dm_u8 hello_out;        /* our HELLO is yet to go */
	dm_u8 hello_in;         /* the engine's has come */
	dm_u8 bad;              /* the engine's stream was lost */
	dm_u8 sent_state;       /* the app's side as last sent */
	dm_u32 sent_baud;
	dm_u8 state_out;        /* a STATE is to go (once attached) */
	dm_u8 held[DM_WIRE_MAX_DATA]; /* the modem's characters the port
					 had no room for */
	dm_u32 held_n;
};

/* a link for port p, named (the COM port's name, as "COM3") */
void dm_link_init(struct dm_link *l, struct dm_port *p, const char *name);

/* the engine attached, detached */
void dm_link_attach(struct dm_link *l, struct dm_batch *b);
void dm_link_detach(struct dm_link *l, struct dm_batch *b);

/* the engine's frames, n bytes of them: how many bytes were taken (whole
   frames only: the rest again later). Fewer than n with l->bad clear
   means the port is full (b->room says when to call again); l->bad set:
   the engine's stream can't be read (detach it). */
dm_u32 dm_link_feed(struct dm_link *l, struct dm_batch *b, const dm_u8 *in,
		    dm_u32 n);
/* held characters into the port again (after b->room): 1 if none are
   held any more */
int dm_link_flush(struct dm_link *l, struct dm_batch *b);

/* is there something for the engine now */
int dm_link_out_ready(const struct dm_link *l);
/* frames for the engine into out (room bytes, at least
   DM_WIRE_HEAD + 255 + 2): how many bytes */
dm_u32 dm_link_pull(struct dm_link *l, struct dm_batch *b, dm_u8 *out,
		    dm_u32 room);

#endif
