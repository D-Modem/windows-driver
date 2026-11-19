/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    port_test.c - the COM port (dm_port.h): reads by each kind of
 *                  serial timeout, writes held and timed, the wait mask's
 *                  events, the modem's lines and the app's DTR and RTS,
 *                  LSRMST_INSERT, purges, the receive buffer's room, the
 *                  settings and properties.  The cases of D-Modem's
 *                  port_test.cpp, run against the C port.
 *
 */

#include <stdlib.h>
#include <string.h>

#include "dm_port.h"
#include "test.h"

#define MAXT 0xffffffffu

static struct dm_port port;
static dm_u64 g_now;
/* what the batches asked of the driver, counted (as PortHost's calls) */
static int h_tx, h_room, h_state;
static dm_u64 h_due;

/* a request and its completions */
struct req {
	struct dm_req r;
	int completed;
	dm_u8 mem[DM_PORT_RX_SIZE + DM_PORT_TX_SIZE + 64];
};

static struct req *req_new(dm_u32 n)
{
	struct req *q = calloc(1, sizeof(*q));
	q->r.buf = q->mem;
	q->r.size = n;
	q->r.os = q;
	return q;
}

/* a write's request for s (n bytes) */
static struct req *req_w(const char *s, dm_u32 n)
{
	struct req *q = req_new(n);
	memcpy(q->mem, s, n);
	return q;
}

/* a wait's events */
static dm_u32 events(const struct req *q)
{
	dm_u32 e;
	memcpy(&e, q->mem, 4);
	return e;
}

/* a read's text is s (n bytes) */
static int text_is(const struct req *q, const char *s, dm_u32 n)
{
	return q->r.info == n && memcmp(q->mem, s, n) == 0;
}

static struct dm_batch b;

/* a call begun and ended, as a driver does it */
static void begin(void)
{
	dm_batch_init(&b, g_now);
}

static void end(void)
{
	struct dm_req *r;
	dm_port_end(&port, &b);
	while ((r = dm_batch_next(&b)) != NULL)
		((struct req *)r->os)->completed++;
	h_tx += b.tx;
	h_room += b.room;
	h_state += b.state;
	if (b.timer)
		h_due = b.due;
}

static void p_read(struct req *q) { begin(); dm_port_read(&port, &b, &q->r); end(); }
static void p_write(struct req *q) { begin(); dm_port_write(&port, &b, &q->r); end(); }
static void p_tick(void) { begin(); dm_port_tick(&port, &b); end(); }
static void p_open(void) { begin(); dm_port_open(&port, &b); end(); }
static void p_close(void) { begin(); dm_port_close(&port, &b); end(); }
static void p_connect(int up) { begin(); dm_port_connect(&port, &b, up); end(); }
static void p_lines(dm_u8 l) { begin(); dm_port_set_lines(&port, &b, l); end(); }

static int p_cancel(struct req *q)
{
	int k;
	begin();
	k = dm_port_cancel(&port, &b, &q->r);
	end();
	return k;
}

static dm_u32 p_ioctl(dm_u32 code, const void *in, dm_u32 inlen, void *out,
		      dm_u32 outlen, dm_u32 *info, struct req *q)
{
	dm_u32 st, i = 0;
	begin();
	st = dm_port_ioctl(&port, &b, code, in, inlen, out, outlen,
			   info ? info : &i, q ? &q->r : NULL);
	end();
	return st;
}

static dm_u32 set(dm_u32 code, const void *in, dm_u32 n)
{
	return p_ioctl(code, in, n, NULL, 0, NULL, NULL);
}

static dm_u32 get32(dm_u32 code)
{
	dm_u32 v = 0xdeadbeef, info = 0;
	CHECK_EQ(p_ioctl(code, NULL, 0, &v, 4, &info, NULL), DM_ST_SUCCESS);
	CHECK_EQ(info, 4);
	return v;
}

static dm_u32 wait_on(struct req *q)
{
	return p_ioctl(DM_IO_WAIT_ON_MASK, NULL, 0, q->mem, 4, NULL, q);
}

static void timeouts(dm_u32 ri, dm_u32 rm, dm_u32 rc, dm_u32 wm, dm_u32 wc)
{
	struct dm_serial_timeouts t = { ri, rm, rc, wm, wc };
	CHECK_EQ(set(DM_IO_SET_TIMEOUTS, &t, sizeof(t)), DM_ST_SUCCESS);
}

/* s (n bytes) from the modem: how many were taken */
static dm_u32 rx(const char *s, dm_u32 n)
{
	dm_u32 k;
	begin();
	k = dm_port_rx_put(&port, &b, (const dm_u8 *)s, n);
	end();
	return k;
}
#define RX(s) rx(s, sizeof(s) - 1)

/* what the modem would take, into out: how many */
static dm_u32 tx(dm_u8 *out)
{
	dm_u32 k;
	begin();
	k = dm_port_tx_take(&port, &b, out, DM_PORT_TX_SIZE);
	end();
	return k;
}

/* a port opened, the engine there with DSR and CTS on */
static void setup(void)
{
	g_now = 1000;
	h_tx = h_room = h_state = 0;
	h_due = 0;
	dm_port_init(&port);
	p_open();
	p_connect(1);
	p_lines(DM_LINE_DSR | DM_LINE_CTS);
}

/* interval MAXULONG, the rest 0: a read at once, with what there is */
TEST(read_at_once)
{
	struct req *r, *r2;
	setup();
	timeouts(MAXT, 0, 0, 0, 0);
	r = req_new(8);
	p_read(r);
	CHECK_EQ(r->completed, 1);
	CHECK_EQ(r->r.status, DM_ST_SUCCESS);
	CHECK_EQ(r->r.info, 0);
	RX("OK\r\n");
	r2 = req_new(8);
	p_read(r2);
	CHECK(text_is(r2, "OK\r\n", 4));
}

/* MAXULONG, MAXULONG, c: the first characters, or nothing after c ms */
TEST(read_first_character)
{
	struct req *r, *r2;
	setup();
	timeouts(MAXT, MAXT, 500, 0, 0);
	r = req_new(16);
	p_read(r);
	CHECK_EQ(r->completed, 0);
	CHECK_EQ(h_due, 1500);
	g_now += 100;
	RX("AT");
	CHECK_EQ(r->completed, 1);
	CHECK_EQ(r->r.status, DM_ST_SUCCESS);
	CHECK(text_is(r, "AT", 2));
	CHECK_EQ(h_due, 0);

	r2 = req_new(16);
	p_read(r2);
	g_now += 499;
	p_tick();
	CHECK_EQ(r2->completed, 0);
	g_now += 1;
	p_tick();
	CHECK_EQ(r2->completed, 1);
	CHECK_EQ(r2->r.status, DM_ST_TIMEOUT);
	CHECK_EQ(r2->r.info, 0);
}

/* multiplier and constant: the total time, with what came */
TEST(read_total_timeout)
{
	struct req *r;
	setup();
	timeouts(0, 10, 100, 0, 0);
	r = req_new(4);
	p_read(r);
	CHECK_EQ(h_due, 1140);
	RX("ab");
	CHECK_EQ(r->completed, 0);
	g_now = 1139;
	p_tick();
	CHECK_EQ(r->completed, 0);
	g_now = 1140;
	p_tick();
	CHECK_EQ(r->completed, 1);
	CHECK_EQ(r->r.status, DM_ST_TIMEOUT);
	CHECK(text_is(r, "ab", 2));
}

/* an interval alone: timed from each character */
TEST(read_interval_timeout)
{
	struct req *r;
	setup();
	timeouts(50, 0, 0, 0, 0);
	r = req_new(10);
	p_read(r);
	p_tick();
	CHECK_EQ(r->completed, 0);
	CHECK_EQ(h_due, 0);     /* (no interval before a character) */
	RX("x");
	CHECK_EQ(h_due, 1050);
	g_now = 1040;
	RX("y");
	CHECK_EQ(h_due, 1090);
	g_now = 1089;
	p_tick();
	CHECK_EQ(r->completed, 0);
	g_now = 1090;
	p_tick();
	CHECK_EQ(r->r.status, DM_ST_TIMEOUT);
	CHECK(text_is(r, "xy", 2));
}

/* no timeouts: each read until full, in turn */
TEST(read_until_full)
{
	struct req *a, *c;
	setup();
	timeouts(0, 0, 0, 0, 0);
	a = req_new(3);
	c = req_new(2);
	p_read(a);
	p_read(c);
	RX("12");
	CHECK_EQ(a->completed, 0);
	RX("345");
	CHECK(text_is(a, "123", 3));
	CHECK(text_is(c, "45", 2));
	CHECK_EQ(a->r.status, DM_ST_SUCCESS);
}

/* no engine: a write dropped at once, EV_TXEMPTY */
TEST(write_without_engine)
{
	struct req *w, *ev;
	dm_u32 m = DM_EV_TXEMPTY;
	dm_u8 out[DM_PORT_TX_SIZE];
	setup();
	p_connect(0);
	CHECK_EQ(set(DM_IO_SET_WAIT_MASK, &m, 4), DM_ST_SUCCESS);
	w = req_w("ATZ\r", 4);
	p_write(w);
	CHECK_EQ(w->completed, 1);
	CHECK_EQ(w->r.info, 4);
	CHECK_EQ(tx(out), 0);
	ev = req_new(4);
	CHECK_EQ(wait_on(ev), DM_ST_PENDING);
	CHECK_EQ(ev->completed, 1);
	CHECK_EQ(events(ev), DM_EV_TXEMPTY);
}

/* a write bigger than the buffer held until the engine takes it */
TEST(write_held_until_taken)
{
	static char big[DM_PORT_TX_SIZE + 10];
	dm_u8 out[DM_PORT_TX_SIZE];
	struct req *w;
	setup();
	memset(big, 'x', sizeof(big));
	w = req_w(big, sizeof(big));
	p_write(w);
	CHECK_EQ(w->completed, 0);
	CHECK(h_tx >= 1);
	CHECK_EQ(tx(out), DM_PORT_TX_SIZE);
	CHECK_EQ(w->completed, 1);
	CHECK_EQ(w->r.info, sizeof(big));
	CHECK_EQ(tx(out), 10);
}

/* a held write's total time */
TEST(write_timeout)
{
	static char big[DM_PORT_TX_SIZE + 10];
	struct req *w;
	setup();
	memset(big, 'x', sizeof(big));
	timeouts(0, 0, 0, 1, 100);
	w = req_w(big, sizeof(big));
	p_write(w);
	CHECK_EQ(h_due, 1000 + 100 + sizeof(big));
	g_now = h_due;
	p_tick();
	CHECK_EQ(w->r.status, DM_ST_TIMEOUT);
	CHECK_EQ(w->r.info, DM_PORT_TX_SIZE);
}

/* IOCTL_SERIAL_WAIT_ON_MASK: an event, one before the wait, a new
   mask, none */
TEST(wait_on_mask)
{
	struct req *ev, *ev2, *ev3, *ev4, *ev5;
	dm_u32 m = DM_EV_RXCHAR | DM_EV_RLSD | DM_EV_RING;
	setup();
	CHECK_EQ(set(DM_IO_SET_WAIT_MASK, &m, 4), DM_ST_SUCCESS);
	CHECK_EQ(get32(DM_IO_GET_WAIT_MASK), m);
	ev = req_new(4);
	CHECK_EQ(wait_on(ev), DM_ST_PENDING);
	CHECK_EQ(ev->completed, 0);
	ev2 = req_new(4);
	CHECK_EQ(wait_on(ev2), DM_ST_INVALID_PARAMETER);
	p_lines(DM_LINE_DSR | DM_LINE_CTS | DM_LINE_DCD);
	CHECK_EQ(ev->completed, 1);
	CHECK_EQ(events(ev), DM_EV_RLSD);

	/* (an event before the wait: the wait at once) */
	RX("x");
	p_lines(DM_LINE_DSR | DM_LINE_CTS | DM_LINE_DCD | DM_LINE_RI);
	ev3 = req_new(4);
	wait_on(ev3);
	CHECK_EQ(ev3->completed, 1);
	CHECK_EQ(events(ev3), DM_EV_RXCHAR | DM_EV_RING);

	/* a new mask ends a waiting wait with none */
	ev4 = req_new(4);
	wait_on(ev4);
	CHECK_EQ(ev4->completed, 0);
	m = DM_EV_DSR;
	set(DM_IO_SET_WAIT_MASK, &m, 4);
	CHECK_EQ(ev4->completed, 1);
	CHECK_EQ(events(ev4), 0);

	/* no mask: no wait */
	m = 0;
	set(DM_IO_SET_WAIT_MASK, &m, 4);
	ev5 = req_new(4);
	CHECK_EQ(wait_on(ev5), DM_ST_INVALID_PARAMETER);
}

/* the event character (EV_RXFLAG) and the buffer at 80% (EV_RX80FULL) */
TEST(rx_flag_and_full)
{
	struct dm_serial_chars c = { 0, 0, 0, '\n', 0x11, 0x13 };
	dm_u32 m = DM_EV_RXFLAG | DM_EV_RX80FULL;
	static char z[DM_PORT_RX_SIZE * 8 / 10];
	struct req *ev, *ev2;
	setup();
	CHECK_EQ(set(DM_IO_SET_CHARS, &c, sizeof(c)), DM_ST_SUCCESS);
	set(DM_IO_SET_WAIT_MASK, &m, 4);
	ev = req_new(4);
	wait_on(ev);
	RX("abc");
	CHECK_EQ(ev->completed, 0);
	RX("\r\n");
	CHECK_EQ(events(ev), DM_EV_RXFLAG);
	ev2 = req_new(4);
	wait_on(ev2);
	memset(z, 'z', sizeof(z));
	rx(z, sizeof(z));
	CHECK_EQ(events(ev2), DM_EV_RX80FULL);
}

/* the modem's lines and their deltas, the engine going and coming */
TEST(modem_status)
{
	setup();
	CHECK_EQ(get32(DM_IO_GET_MODEMSTATUS),
		 DM_MSR_DSR | DM_MSR_CTS | DM_MSR_DDSR | DM_MSR_DCTS);
	CHECK_EQ(get32(DM_IO_GET_MODEMSTATUS), DM_MSR_DSR | DM_MSR_CTS);
	p_lines(DM_LINE_DSR | DM_LINE_CTS | DM_LINE_DCD | DM_LINE_RI);
	p_lines(DM_LINE_DSR | DM_LINE_CTS | DM_LINE_DCD);
	CHECK_EQ(get32(DM_IO_GET_MODEMSTATUS),
		 DM_MSR_DSR | DM_MSR_CTS | DM_MSR_DCD | DM_MSR_DDCD |
			 DM_MSR_TERI);
	/* the engine gone: every line off */
	p_connect(0);
	CHECK_EQ(get32(DM_IO_GET_MODEMSTATUS),
		 DM_MSR_DDSR | DM_MSR_DCTS | DM_MSR_DDCD);
	/* (its lines are its own again when it comes back) */
	p_lines(DM_LINE_DCD);
	CHECK_EQ(get32(DM_IO_GET_MODEMSTATUS), 0);
	p_connect(1);
	p_lines(DM_LINE_DSR);
	CHECK_EQ(get32(DM_IO_GET_MODEMSTATUS), DM_MSR_DSR | DM_MSR_DDSR);
}

/* the app's DTR and RTS: set, by the handflow, refused in handshake
   modes */
TEST(dtr_and_rts)
{
	struct dm_serial_handflow h = { 0, DM_HF_RTS_HANDSHAKE, 0, 0 }, g;
	dm_u32 mcr = 0, info;
	int st;
	setup();
	/* open: the default handflow's DTR and RTS on */
	CHECK_EQ(dm_port_app_state(&port),
		 DM_STATE_OPEN | DM_STATE_DTR | DM_STATE_RTS);
	CHECK_EQ(get32(DM_IO_GET_DTRRTS), DM_DTRRTS_DTR | DM_DTRRTS_RTS);
	st = h_state;
	CHECK_EQ(set(DM_IO_CLR_DTR, NULL, 0), DM_ST_SUCCESS);
	CHECK_EQ(dm_port_app_state(&port), DM_STATE_OPEN | DM_STATE_RTS);
	CHECK_EQ(h_state, st + 1);
	set(DM_IO_SET_DTR, NULL, 0);
	CHECK_EQ(dm_port_app_state(&port),
		 DM_STATE_OPEN | DM_STATE_DTR | DM_STATE_RTS);

	/* SetCommState's DTR_CONTROL_DISABLE and RTS_CONTROL_HANDSHAKE */
	CHECK_EQ(set(DM_IO_SET_HANDFLOW, &h, sizeof(h)), DM_ST_SUCCESS);
	CHECK_EQ(dm_port_app_state(&port), DM_STATE_OPEN | DM_STATE_RTS);
	CHECK_EQ(set(DM_IO_CLR_RTS, NULL, 0), DM_ST_INVALID_PARAMETER);
	CHECK_EQ(p_ioctl(DM_IO_GET_HANDFLOW, NULL, 0, &g, sizeof(g), &info,
			 NULL), DM_ST_SUCCESS);
	CHECK_EQ(g.flow_replace, DM_HF_RTS_HANDSHAKE);
	h.control_handshake = DM_HF_DTR_HANDSHAKE;
	set(DM_IO_SET_HANDFLOW, &h, sizeof(h));
	CHECK_EQ(set(DM_IO_SET_DTR, NULL, 0), DM_ST_INVALID_PARAMETER);

	h.control_handshake = DM_HF_DTR_CONTROL;
	h.flow_replace = DM_HF_RTS_CONTROL;
	set(DM_IO_SET_HANDFLOW, &h, sizeof(h));
	set(DM_IO_SET_MODEM_CONTROL, &mcr, 4);
	CHECK_EQ(dm_port_app_state(&port), DM_STATE_OPEN);

	p_close();
	CHECK_EQ(dm_port_app_state(&port), 0);
}

/* closing cancels what waits and empties the buffers */
TEST(close_cancels)
{
	static char q[DM_PORT_TX_SIZE + 1];
	struct req *r, *w, *ev, *r2;
	dm_u32 m = DM_EV_RXCHAR;
	dm_u8 out[DM_PORT_TX_SIZE];
	setup();
	timeouts(0, 0, 0, 0, 0);
	memset(q, 'q', sizeof(q));
	r = req_new(4);
	w = req_w(q, sizeof(q));
	ev = req_new(4);
	set(DM_IO_SET_WAIT_MASK, &m, 4);
	p_read(r);
	p_write(w);
	CHECK_EQ(wait_on(ev), DM_ST_PENDING);
	p_close();
	CHECK_EQ(r->r.status, DM_ST_CANCELLED);
	CHECK_EQ(w->r.status, DM_ST_CANCELLED);
	CHECK_EQ(w->r.info, DM_PORT_TX_SIZE);
	CHECK_EQ(ev->r.status, DM_ST_CANCELLED);
	CHECK_EQ(tx(out), 0);
	/* the modem's characters for a closed port go nowhere */
	CHECK_EQ(RX("RING\r\n"), 6);
	p_open();
	r2 = req_new(8);
	timeouts(MAXT, 0, 0, 0, 0);
	p_read(r2);
	CHECK_EQ(r2->r.info, 0);
}

/* a read cancelled with what it had; the next one the head */
TEST(cancel)
{
	struct req *a, *c;
	setup();
	timeouts(0, 0, 0, 0, 0);
	a = req_new(4);
	c = req_new(4);
	p_read(a);
	p_read(c);
	RX("ab");
	CHECK(p_cancel(a));
	CHECK_EQ(a->r.done, 2);
	CHECK_EQ(a->completed, 0);      /* (the driver completes it) */
	CHECK(!p_cancel(a));
	RX("cdef");
	CHECK(text_is(c, "cdef", 4));
}

/* IOCTL_SERIAL_PURGE: a read aborted, the buffer cleared */
TEST(purge)
{
	struct dm_serial_status s;
	struct req *r;
	dm_u32 pg = DM_PURGE_RXABORT, info;
	setup();
	timeouts(0, 0, 0, 0, 0);
	r = req_new(4);
	p_read(r);
	CHECK_EQ(set(DM_IO_PURGE, &pg, 4), DM_ST_SUCCESS);
	CHECK_EQ(r->r.status, DM_ST_CANCELLED);
	RX("junk");
	pg = DM_PURGE_RXCLEAR;
	set(DM_IO_PURGE, &pg, 4);
	CHECK_EQ(p_ioctl(DM_IO_GET_COMMSTATUS, NULL, 0, &s, sizeof(s), &info,
			 NULL), DM_ST_SUCCESS);
	CHECK_EQ(s.in_queue, 0);
	pg = 0;
	CHECK_EQ(set(DM_IO_PURGE, &pg, 4), DM_ST_INVALID_PARAMETER);
}

/* the buffer full: the rest turned away, b->room when there is room */
TEST(receive_room)
{
	static char big[DM_PORT_RX_SIZE + 100];
	struct req *r;
	setup();
	memset(big, 'r', sizeof(big));
	CHECK_EQ(rx(big, sizeof(big)), DM_PORT_RX_SIZE);
	CHECK_EQ(h_room, 0);
	timeouts(MAXT, 0, 0, 0, 0);
	r = req_new(DM_PORT_RX_SIZE / 2);
	p_read(r);
	CHECK_EQ(r->r.info, DM_PORT_RX_SIZE / 2);
	CHECK_EQ(h_room, 1);
}

/* LSRMST_INSERT: the escape doubled, the modem status in-band */
TEST(lsrmst_insert)
{
	dm_u8 esc = 0xff;
	struct req *r;
	setup();
	CHECK_EQ(set(DM_IO_LSRMST_INSERT, &esc, 1), DM_ST_SUCCESS);
	rx("a\xff" "b", 3);
	p_lines(DM_LINE_DSR | DM_LINE_CTS | DM_LINE_DCD);
	timeouts(MAXT, 0, 0, 0, 0);
	r = req_new(16);
	p_read(r);
	CHECK(text_is(r, "a\xff\x00" "b\xff\x03\xbb", 7));
}

/* the handflow's NUL stripping */
TEST(null_stripping)
{
	struct dm_serial_handflow h = {
		DM_HF_DTR_CONTROL, DM_HF_RTS_CONTROL | DM_HF_NULL_STRIPPING,
		0, 0
	};
	struct req *r;
	setup();
	set(DM_IO_SET_HANDFLOW, &h, sizeof(h));
	rx("a\0b", 3);
	timeouts(MAXT, 0, 0, 0, 0);
	r = req_new(8);
	p_read(r);
	CHECK(text_is(r, "ab", 2));
}

/* the settings kept and refused, the properties, the counters */
TEST(settings)
{
	struct dm_serial_line_control lc = { 0, 2, 7 }, got;
	struct dm_serial_timeouts bad = { MAXT, MAXT, MAXT, 0, 0 };
	struct dm_serial_commprop pr;
	struct dm_serial_stats s;
	dm_u32 baud = 57600, info;
	struct req *w;
	int st;
	setup();
	st = h_state;
	CHECK_EQ(set(DM_IO_SET_BAUD_RATE, &baud, 4), DM_ST_SUCCESS);
	CHECK_EQ(get32(DM_IO_GET_BAUD_RATE), 57600);
	CHECK_EQ(port.baud, 57600);
	CHECK_EQ(h_state, st + 1);
	baud = 0;
	CHECK_EQ(set(DM_IO_SET_BAUD_RATE, &baud, 4), DM_ST_INVALID_PARAMETER);

	CHECK_EQ(set(DM_IO_SET_LINE_CONTROL, &lc, 3), DM_ST_SUCCESS);
	p_ioctl(DM_IO_GET_LINE_CONTROL, NULL, 0, &got, 3, &info, NULL);
	CHECK_EQ(got.word_length, 7);
	CHECK_EQ(got.parity, 2);
	lc.stop_bits = 1;               /* (1.5 stop bits: 5 data bits only) */
	lc.parity = 0;
	lc.word_length = 8;
	CHECK_EQ(set(DM_IO_SET_LINE_CONTROL, &lc, 3), DM_ST_INVALID_PARAMETER);

	CHECK_EQ(set(DM_IO_SET_TIMEOUTS, &bad, sizeof(bad)),
		 DM_ST_INVALID_PARAMETER);
	CHECK_EQ(set(DM_IO_SET_TIMEOUTS, &bad, 4), DM_ST_BUFFER_TOO_SMALL);

	CHECK_EQ(p_ioctl(DM_IO_GET_PROPERTIES, NULL, 0, &pr, sizeof(pr), &info,
			 NULL), DM_ST_SUCCESS);
	CHECK_EQ(info, sizeof(pr));
	CHECK_EQ(pr.max_rx_queue, DM_PORT_RX_SIZE);
	CHECK_EQ(p_ioctl(DM_IO_GET_PROPERTIES, NULL, 0, &pr, 62, &info, NULL),
		 DM_ST_SUCCESS);
	CHECK_EQ(info, 62);
	CHECK_EQ(get32(DM_IO_CONFIG_SIZE), 0);
	CHECK_EQ(p_ioctl(DM_SERIAL_IOCTL(99), NULL, 0, NULL, 0, &info, NULL),
		 DM_ST_INVALID_PARAMETER);

	w = req_w("ATH\r", 4);
	p_write(w);
	RX("OK");
	p_ioctl(DM_IO_GET_STATS, NULL, 0, &s, sizeof(s), &info, NULL);
	CHECK_EQ(s.transmitted, 4);
	CHECK_EQ(s.received, 2);
}

/* the rings wrap: many small reads and writes past their ends */
TEST(rings_wrap)
{
	char msg[100];
	dm_u8 out[DM_PORT_TX_SIZE];
	int i, ok = 1;
	setup();
	timeouts(MAXT, 0, 0, 0, 0);
	for (i = 0; i < 1000; i++) {
		struct req *r = req_new(sizeof(msg)), *w;
		int n = snprintf(msg, sizeof(msg), "line %d of the rings\r\n",
				 i);
		rx(msg, (dm_u32)n);
		p_read(r);
		if (!text_is(r, msg, (dm_u32)n))
			ok = 0;
		w = req_w(msg, (dm_u32)n);
		p_write(w);
		if (tx(out) != (dm_u32)n || memcmp(out, msg, (size_t)n))
			ok = 0;
		free(r);
		free(w);
	}
	CHECK(ok);
	CHECK_EQ(port.rx_n, 0);
	CHECK_EQ(port.tx_n, 0);
}

int main(void)
{
	RUN(read_at_once);
	RUN(read_first_character);
	RUN(read_total_timeout);
	RUN(read_interval_timeout);
	RUN(read_until_full);
	RUN(write_without_engine);
	RUN(write_held_until_taken);
	RUN(write_timeout);
	RUN(wait_on_mask);
	RUN(rx_flag_and_full);
	RUN(modem_status);
	RUN(dtr_and_rts);
	RUN(close_cancels);
	RUN(cancel);
	RUN(purge);
	RUN(receive_room);
	RUN(lsrmst_insert);
	RUN(null_stripping);
	RUN(settings);
	RUN(rings_wrap);
	return DONE();
}
