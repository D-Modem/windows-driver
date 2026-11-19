/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    link_test.c - the engine channel: the wire frames (dm_wire.h), a
 *                  byte at a time and broken ones, and a driver's end of
 *                  the channel (dm_link.h) with an engine on the other
 *                  side: HELLOs, the app's side and characters out, the
 *                  modem's lines and characters in, held while the port
 *                  is full, the engine gone.
 *
 */

#include <stdlib.h>
#include <string.h>

#include "dm_link.h"
#include "test.h"

/* every frame, then read back a byte at a time */
TEST(wire_frames)
{
	static dm_u8 out[64 + DM_WIRE_MAX_DATA];
	dm_u8 buf[DM_WIRE_HEAD + DM_WIRE_MAX_DATA];
	dm_u32 n = 0, got = 0, have = 0, i, baud = 0;
	dm_u16 ver = 0;
	dm_u8 lines = 0, state = 0;
	char name[16] = "";
	int frames = 0, data = 0;

	n += dm_wire_hello(out + n, sizeof(out) - n, "COM7");
	n += dm_wire_lines(out + n, sizeof(out) - n,
			   DM_LINE_DSR | DM_LINE_CTS);
	n += dm_wire_state(out + n, sizeof(out) - n,
			   DM_STATE_OPEN | DM_STATE_DTR, 115200);
	n += dm_wire_put(out + n, sizeof(out) - n, DM_FRAME_DATA, "hello", 5);
	CHECK_EQ(n, 3 + 6 + 3 + 1 + 3 + 5 + 3 + 5);
	CHECK_EQ(dm_wire_put(out, 4, DM_FRAME_DATA, "hello", 5), 0);

	/* a byte at a time, as the IOCTLs may split it */
	for (i = 0; i < n; i++) {
		struct dm_frame f;
		int len;
		buf[have++] = out[i];
		while ((len = dm_wire_next(buf, have, &f)) > 0) {
			frames++;
			if (f.type == DM_FRAME_HELLO)
				CHECK(dm_wire_get_hello(&f, &ver, name,
							sizeof(name)));
			if (f.type == DM_FRAME_LINES)
				CHECK(dm_wire_get_lines(&f, &lines));
			if (f.type == DM_FRAME_STATE)
				CHECK(dm_wire_get_state(&f, &state, &baud));
			if (f.type == DM_FRAME_DATA)
				data += !memcmp(f.p, "hello", 5) && f.n == 5;
			memmove(buf, buf + len, have - (dm_u32)len);
			have -= (dm_u32)len;
			got += (dm_u32)len;
		}
		CHECK(len == 0);
	}
	CHECK_EQ(frames, 4);
	CHECK_EQ(got, n);
	CHECK_EQ(ver, DM_WIRE_VERSION);
	CHECK(!strcmp(name, "COM7"));
	CHECK_EQ(lines, DM_LINE_DSR | DM_LINE_CTS);
	CHECK_EQ(state, DM_STATE_OPEN | DM_STATE_DTR);
	CHECK_EQ(baud, 115200);
	CHECK_EQ(data, 1);
}

/* frames that can't be: an unknown type, DATA too long, a short LINES */
TEST(wire_bad)
{
	struct dm_frame f;
	dm_u8 z[3] = { 'Z', 0, 0 }, d[3] = { DM_FRAME_DATA, 0xff, 0xff };
	dm_u8 l[3] = { DM_FRAME_LINES, 0, 0 }, lines;
	CHECK_EQ(dm_wire_next(z, 1, &f), -1);
	CHECK_EQ(dm_wire_next(d, 3, &f), -1);
	CHECK_EQ(dm_wire_next(l, 3, &f), 3);
	CHECK(!dm_wire_get_lines(&f, &lines));
}

/* ---- the link, with an engine */

static struct dm_port port;
static struct dm_link link;
static struct dm_batch b;
static int h_room;

static void begin(void) { dm_batch_init(&b, 1000); }
static void end(void)
{
	dm_port_end(&port, &b);
	while (dm_batch_next(&b))
		;
	h_room += b.room;
}

/* what the driver has for the engine, appended to out at *n */
static void pull(dm_u8 *out, dm_u32 *n, dm_u32 room)
{
	begin();
	*n += dm_link_pull(&link, &b, out + *n, room - *n);
	end();
}

/* the engine's frames in: how many bytes went */
static dm_u32 feed(const dm_u8 *in, dm_u32 n)
{
	dm_u32 k;
	begin();
	k = dm_link_feed(&link, &b, in, n);
	end();
	return k;
}

/* the app's port opened, with its default DTR and RTS */
static void app_open(void)
{
	begin();
	dm_port_open(&port, &b);
	end();
}

TEST(link_hello_state_data)
{
	static dm_u8 out[8192];
	dm_u8 in[64];
	dm_u32 n = 0, k = 0, baud = 0, pos = 0;
	struct dm_frame f;
	dm_u8 st = 0;
	char name[16];
	dm_u16 v;
	dm_u8 tmp[16];

	dm_port_init(&port);
	dm_link_init(&link, &port, "COM5");
	CHECK(!dm_link_out_ready(&link));
	app_open();

	begin();
	dm_link_attach(&link, &b);
	end();
	CHECK(port.connected);
	CHECK(dm_link_out_ready(&link));
	pull(out, &n, sizeof(out));
	/* our HELLO, then the app's side */
	CHECK(dm_wire_next(out, n, &f) > 0);
	CHECK(dm_wire_get_hello(&f, &v, name, sizeof(name)));
	CHECK(!strcmp(name, "COM5"));
	pos = DM_WIRE_HEAD + f.n;
	CHECK(dm_wire_next(out + pos, n - pos, &f) > 0);
	CHECK(dm_wire_get_state(&f, &st, &baud));
	CHECK_EQ(st, DM_STATE_OPEN | DM_STATE_DTR | DM_STATE_RTS);
	CHECK_EQ(baud, 115200);
	CHECK(!dm_link_out_ready(&link));

	/* frames before the engine's HELLO: its stream is lost */
	k = dm_wire_lines(in, sizeof(in), DM_LINE_DSR);
	CHECK_EQ(feed(in, k), 0);
	CHECK(link.bad);
	begin();
	dm_link_detach(&link, &b);
	dm_link_attach(&link, &b);
	end();
	n = 0;
	pull(out, &n, sizeof(out));

	/* the engine's HELLO, lines and characters: in a piece, and a
	   frame split in two */
	k = dm_wire_hello(in, sizeof(in), NULL);
	k += dm_wire_lines(in + k, sizeof(in) - k,
			   DM_LINE_DSR | DM_LINE_CTS | DM_LINE_DCD);
	k += dm_wire_put(in + k, sizeof(in) - k, DM_FRAME_DATA,
			 "CONNECT\r\n", 9);
	CHECK_EQ(feed(in, k - 4), k - 3 - 9);
	CHECK_EQ(feed(in + k - 3 - 9, 3 + 9), 3 + 9);
	CHECK(!link.bad);
	CHECK_EQ(port.lines, DM_LINE_DSR | DM_LINE_CTS | DM_LINE_DCD);
	CHECK_EQ(port.rx_n, 9);

	/* the app's characters and a baud rate out */
	{
		struct dm_req w;
		dm_u8 text[] = "ATDT555\r";
		dm_u32 info, br = 57600;
		memset(&w, 0, sizeof(w));
		w.buf = text;
		w.size = 8;
		begin();
		dm_port_write(&port, &b, &w);
		dm_port_ioctl(&port, &b, DM_IO_SET_BAUD_RATE, &br, 4, NULL, 0,
			      &info, NULL);
		end();
		CHECK_EQ(w.status, DM_ST_SUCCESS);
	}
	CHECK(dm_link_out_ready(&link));
	n = 0;
	pull(out, &n, sizeof(out));
	CHECK(dm_wire_next(out, n, &f) > 0);
	CHECK(dm_wire_get_state(&f, &st, &baud));
	CHECK_EQ(baud, 57600);
	pos = DM_WIRE_HEAD + f.n;
	CHECK(dm_wire_next(out + pos, n - pos, &f) > 0);
	CHECK_EQ(f.type, DM_FRAME_DATA);
	CHECK(f.n == 8 && !memcmp(f.p, "ATDT555\r", 8));

	/* a pull with little room: frames whole, the rest next time */
	{
		struct dm_req w;
		static dm_u8 text[100];
		memset(text, 'a', sizeof(text));
		memset(&w, 0, sizeof(w));
		w.buf = text;
		w.size = sizeof(text);
		begin();
		dm_port_write(&port, &b, &w);
		end();
		n = 0;
		pull(tmp, &n, sizeof(tmp));
		CHECK_EQ(n, 16);
		CHECK_EQ(port.tx_n, 100 - 13);
	}

	/* the engine gone: the modem's lines off */
	begin();
	dm_link_detach(&link, &b);
	end();
	CHECK(!port.connected);
	CHECK(!dm_link_out_ready(&link));
}

/* the port full: the engine's characters held, its next frames
   waiting, all of them in once the app reads */
TEST(link_held)
{
	static dm_u8 in[3 * (DM_WIRE_HEAD + DM_WIRE_MAX_DATA) + 64];
	static dm_u8 out[64];
	static dm_u8 data[DM_WIRE_MAX_DATA];
	static dm_u8 room[DM_PORT_RX_SIZE];
	dm_u32 k = 0, used = 0, n = 0, i, frames = 6, sent;
	struct dm_req r;
	struct dm_serial_timeouts t = { 0xffffffffu, 0, 0, 0, 0 };
	dm_u32 info;

	dm_port_init(&port);
	dm_link_init(&link, &port, "COM5");
	app_open();
	begin();
	dm_link_attach(&link, &b);
	dm_port_ioctl(&port, &b, DM_IO_SET_TIMEOUTS, &t, sizeof(t), NULL, 0,
		      &info, NULL);
	end();
	pull(out, &n, sizeof(out));
	k = dm_wire_hello(in, sizeof(in), NULL);
	CHECK_EQ(feed(in, k), k);
	h_room = 0;

	/* 6 frames of 4096 is more than the 16384 the port holds */
	memset(data, 'm', sizeof(data));
	sent = 0;
	for (i = 0; i < frames; i++) {
		k = dm_wire_put(in, sizeof(in), DM_FRAME_DATA, data,
				sizeof(data));
		used = feed(in, k);
		if (used < k)
			break;
		sent++;
	}
	CHECK_EQ(port.rx_n, DM_PORT_RX_SIZE);
	CHECK(link.held_n > 0);
	CHECK_EQ(sent, 5);      /* (4 fill the port, the 5th is held) */
	CHECK_EQ(link.held_n, DM_WIRE_MAX_DATA);
	CHECK_EQ(used, 0);      /* (the 6th waits) */

	/* the app reads: room, the held characters in, the 6th frame goes */
	memset(&r, 0, sizeof(r));
	r.buf = room;
	r.size = sizeof(room);
	begin();
	dm_port_read(&port, &b, &r);
	end();
	CHECK_EQ(r.info, DM_PORT_RX_SIZE);
	CHECK_EQ(h_room, 1);
	begin();
	CHECK(dm_link_flush(&link, &b));
	end();
	CHECK_EQ(feed(in, k), k);
	CHECK_EQ(port.rx_n, 2 * DM_WIRE_MAX_DATA);
}

int main(void)
{
	RUN(wire_frames);
	RUN(wire_bad);
	RUN(link_hello_state_data);
	RUN(link_held);
	return DONE();
}
