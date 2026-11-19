/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_wire.h - the frames between a D-Modem driver's COM port and the
 *                engine (dmodemd --driver, or dmodem-svc), the same
 *                frames as D-Modem's Windows 10 driver uses on its pipe
 *                (platforms/windows/winmodem/wire.h, version 1):
 *
 *        type (1 byte), length (2 bytes, little-endian), payload
 *
 *    HELLO   both ways, first: the version (2 bytes, DM_WIRE_VERSION)
 *            and, from the driver, its COM port's name
 *    DATA    both ways: the app's characters to the modem, the modem's
 *            to the app (at most DM_WIRE_MAX_DATA bytes a frame)
 *    LINES   the engine to the driver: the modem's control lines, 1
 *            byte of DM_LINE_* (DCD, RI, DSR, CTS)
 *    STATE   the driver to the engine: the app's side, 1 byte of
 *            DM_STATE_* (the port open, DTR, RTS) and the baud rate
 *            (4 bytes)
 *
 *    Here they travel in the buffers of the engine channel's IOCTLs
 *    (dmodem_ioctl.h) instead of a pipe: the engine's dmodemd code reads
 *    and writes the same stream either way.
 *
 */
#ifndef DM_WIRE_H
#define DM_WIRE_H

#include "dm_types.h"

#define DM_WIRE_VERSION 1u
#define DM_WIRE_HEAD 3u
#define DM_WIRE_MAX_DATA 4096u

#define DM_FRAME_HELLO 'H'
#define DM_FRAME_DATA  'D'
#define DM_FRAME_LINES 'L'
#define DM_FRAME_STATE 'S'

struct dm_frame {
	dm_u8 type;
	const dm_u8 *p;         /* the payload, in the buffer parsed */
	dm_u32 n;
};

/* a frame of type t with payload p (n bytes) at out (room bytes): the
   bytes it took, 0 if it doesn't fit (or n is too long) */
dm_u32 dm_wire_put(dm_u8 *out, dm_u32 room, dm_u8 t, const void *p,
		   dm_u32 n);
/* HELLO: our version and the port's name (NUL-terminated, at most 255
   characters of it) */
dm_u32 dm_wire_hello(dm_u8 *out, dm_u32 room, const char *port);
/* LINES: lines (DM_LINE_*) */
dm_u32 dm_wire_lines(dm_u8 *out, dm_u32 room, dm_u8 lines);
/* STATE: state (DM_STATE_*) and the baud rate */
dm_u32 dm_wire_state(dm_u8 *out, dm_u32 room, dm_u8 state, dm_u32 baud);

/* the frame at the start of buf (n bytes) into *f: its whole length, 0
   if it isn't all there yet, -1 if it can't be (an unknown type, DATA
   too long: the stream is lost) */
int dm_wire_next(const dm_u8 *buf, dm_u32 n, struct dm_frame *f);

/* a HELLO's version (and, from the driver, its port name, copied with
   a NUL into name, size bytes); a LINES frame's lines; a STATE frame's
   state and baud rate: 0 if the frame is short */
int dm_wire_get_hello(const struct dm_frame *f, dm_u16 *version,
		      char *name, dm_u32 size);
int dm_wire_get_lines(const struct dm_frame *f, dm_u8 *lines);
int dm_wire_get_state(const struct dm_frame *f, dm_u8 *state, dm_u32 *baud);

#endif
