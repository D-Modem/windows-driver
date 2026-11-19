/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_port.c - the COM port (dm_port.h)
 *
 *    The serial semantics are those Windows documents for serial.sys
 *    (the Serial Controller Driver Design Guide, SERIAL_TIMEOUTS, the
 *    IOCTL_SERIAL_* pages), as D-Modem's port.cpp has them.
 *
 */

#include "dm_port.h"

DM_STATIC_ASSERT(sizeof(struct dm_serial_timeouts) == 20, timeouts);
DM_STATIC_ASSERT(sizeof(struct dm_serial_handflow) == 16, handflow);
DM_STATIC_ASSERT(sizeof(struct dm_serial_chars) == 6, chars);
DM_STATIC_ASSERT(sizeof(struct dm_serial_line_control) == 3, linectl);
DM_STATIC_ASSERT(sizeof(struct dm_serial_status) == 20, status);
DM_STATIC_ASSERT(sizeof(struct dm_serial_stats) == 24, stats);
DM_STATIC_ASSERT(sizeof(struct dm_serial_commprop) == 64, commprop);

#define MAXT 0xffffffffu

/* SERIAL_COMMPROP's values: an RS-232 port (SERIAL_SP_RS232) taking any
   baud rate (SERIAL_BAUD_USER), 5 to 8 data bits, every stop bit and
   parity setting, both timeouts and the handshakes */
#define SP_SERIALCOMM 1u
#define SP_RS232 1u
#define BAUD_USER 0x10000000u
#define PCF_ALL 0x1ffu
#define SP_ALL 0x7fu
#define BAUD_ALL 0x1007ffffu
#define DATABITS_ALL 0x0fu
#define STOPPARITY_ALL 0x1f07u

/* ---- requests and batches */

static void q_push(struct dm_req_queue *q, struct dm_req *r)
{
	r->next = NULL;
	if (q->tail)
		q->tail->next = r;
	else
		q->head = r;
	q->tail = r;
}

static struct dm_req *q_pop(struct dm_req_queue *q)
{
	struct dm_req *r = q->head;
	if (r) {
		q->head = r->next;
		if (!q->head)
			q->tail = NULL;
		r->next = NULL;
	}
	return r;
}

/* r out of q: 1 if it was there */
static int q_remove(struct dm_req_queue *q, struct dm_req *r)
{
	struct dm_req *prev = NULL, *it;
	for (it = q->head; it; prev = it, it = it->next) {
		if (it != r)
			continue;
		if (prev)
			prev->next = it->next;
		else
			q->head = it->next;
		if (q->tail == it)
			q->tail = prev;
		it->next = NULL;
		return 1;
	}
	return 0;
}

/* see dm_port.h */
void dm_batch_init(struct dm_batch *b, dm_u64 now)
{
	memset(b, 0, sizeof(*b));
	b->now = now;
}

/* see dm_port.h */
struct dm_req *dm_batch_next(struct dm_batch *b)
{
	return q_pop(&b->done);
}

/* r done: completed once the driver's lock is let go */
static void finish(struct dm_batch *b, struct dm_req *r, dm_u32 status,
		   dm_u32 info)
{
	r->status = status;
	r->info = info;
	q_push(&b->done, r);
}

/* every request of q done with status */
static void finish_all(struct dm_batch *b, struct dm_req_queue *q,
		       dm_u32 status)
{
	struct dm_req *r;
	while ((r = q_pop(q)) != NULL)
		finish(b, r, status, r->done);
}

/* ---- the rings */

static dm_u32 rx_room_left(const struct dm_port *p)
{
	return DM_PORT_RX_SIZE - p->rx_n;
}

static void rx_push1(struct dm_port *p, dm_u8 c)
{
	p->rx[(p->rx_get + p->rx_n) % DM_PORT_RX_SIZE] = c;
	p->rx_n++;
}

static dm_u8 rx_pop1(struct dm_port *p)
{
	dm_u8 c = p->rx[p->rx_get];
	p->rx_get = (p->rx_get + 1) % DM_PORT_RX_SIZE;
	p->rx_n--;
	return c;
}

static void tx_push1(struct dm_port *p, dm_u8 c)
{
	p->tx[(p->tx_get + p->tx_n) % DM_PORT_TX_SIZE] = c;
	p->tx_n++;
}

static dm_u8 tx_pop1(struct dm_port *p)
{
	dm_u8 c = p->tx[p->tx_get];
	p->tx_get = (p->tx_get + 1) % DM_PORT_TX_SIZE;
	p->tx_n--;
	return c;
}

static void rx_clear(struct dm_port *p)
{
	p->rx_get = p->rx_n = 0;
}

static void tx_clear(struct dm_port *p)
{
	p->tx_get = p->tx_n = 0;
}

/* ---- the port */

/* see dm_port.h */
void dm_port_init(struct dm_port *p)
{
	memset(p, 0, sizeof(*p));
	p->baud = 115200;
	p->lc.word_length = 8;
	p->hf.control_handshake = DM_HF_DTR_CONTROL;
	p->hf.flow_replace = DM_HF_RTS_CONTROL;
	p->chars.xon = 0x11;
	p->chars.xoff = 0x13;
}

/* ev happened: a waiting IOCTL_SERIAL_WAIT_ON_MASK given it if the mask
   has it, else kept for the next */
static void event(struct dm_port *p, struct dm_batch *b, dm_u32 ev)
{
	ev &= p->wait_mask;
	if (!ev)
		return;
	p->events |= ev;
	if (p->wait) {
		dm_u32 got = p->events;
		p->events = 0;
		memcpy(p->wait->buf, &got, 4);
		finish(b, p->wait, DM_ST_SUCCESS, 4);
		p->wait = NULL;
	}
}

/* the earliest of the head read's and the head write's timeouts, 0 if
   neither has one */
static dm_u64 next_due(const struct dm_port *p)
{
	dm_u64 due = 0, t[3] = { 0, 0, 0 };
	int i;
	if (p->reads.head) {
		t[0] = p->reads.head->total_due;
		t[1] = p->reads.head->interval_due;
	}
	if (p->writes.head)
		t[2] = p->writes.head->total_due;
	for (i = 0; i < 3; i++)
		if (t[i] && (!due || t[i] < due))
			due = t[i];
	return due;
}

/* see dm_port.h: the transmit buffer's emptying (EV_TXEMPTY), the
   receive buffer's room for rx_put again, the timer */
void dm_port_end(struct dm_port *p, struct dm_batch *b)
{
	dm_u64 due;
	if (p->tx_busy && !p->tx_n && !p->writes.head) {
		p->tx_busy = 0;
		event(p, b, DM_EV_TXEMPTY);
	}
	if (p->rx_blocked && rx_room_left(p) >= DM_PORT_RX_SIZE / 4) {
		p->rx_blocked = 0;
		b->room = 1;
	}
	if (p->rx_n < DM_PORT_RX_SIZE * 8 / 10)
		p->rx_full = 0;
	due = next_due(p);
	if (due != p->due) {
		p->due = due;
		b->timer = 1;
	}
	b->due = p->due;
}

/* the reads, in turn, given what has come; the head read timed from
   when it became the head (SERIAL_TIMEOUTS):
     - interval MAXULONG, multiplier and constant 0: at once, with what
       there is;
     - interval and multiplier MAXULONG, constant c: at once if there is
       something, else at the first character or after c ms;
     - else full, or the total time (multiplier * length + constant)
       gone, or the interval gone between two characters */
static void pump_reads(struct dm_port *p, struct dm_batch *b)
{
	const struct dm_serial_timeouts *t = &p->to;
	dm_u64 now = b->now;

	while (p->reads.head) {
		struct dm_req *r = p->reads.head;
		int at_once = t->read_interval == MAXT &&
			t->read_multiplier == 0 && t->read_constant == 0;
		int first = t->read_interval == MAXT &&
			t->read_multiplier == MAXT && t->read_constant != 0 &&
			t->read_constant != MAXT;
		dm_u32 want = r->size, got = 0, st = DM_ST_SUCCESS;
		int end;

		if (!r->started) {
			dm_u64 total = 0;
			r->started = 1;
			if (first)
				total = t->read_constant;
			else if (!at_once && (t->read_multiplier ||
					      t->read_constant))
				total = (dm_u64)t->read_multiplier * want +
					t->read_constant;
			r->total_due = total ? now + total : 0;
		}
		while (r->done < want && p->rx_n) {
			r->buf[r->done++] = rx_pop1(p);
			got++;
		}
		if (got && !at_once && !first && t->read_interval &&
		    t->read_interval != MAXT)
			r->interval_due = now + t->read_interval;

		end = r->done == want || at_once || (first && r->done > 0);
		if (!end && ((r->total_due && now >= r->total_due) ||
			     (r->interval_due && now >= r->interval_due))) {
			end = 1;
			st = DM_ST_TIMEOUT;
		}
		if (!end)
			break;
		q_pop(&p->reads);
		finish(b, r, st, r->done);
	}
}

/* the writes, in turn, into the transmit buffer (all of it at once
   without the engine: dropped); the head write timed by the total time
   (multiplier * length + constant) */
static void pump_writes(struct dm_port *p, struct dm_batch *b)
{
	dm_u64 now = b->now;

	while (p->writes.head) {
		struct dm_req *r = p->writes.head;
		dm_u32 want = r->size, k = 0, st = DM_ST_SUCCESS;

		if (!r->started) {
			dm_u64 total = (dm_u64)p->to.write_multiplier * want +
				p->to.write_constant;
			r->started = 1;
			r->total_due = total ? now + total : 0;
		}
		if (!p->connected) {
			p->stats.transmitted += want - r->done;
			r->done = want;
		}
		while (r->done < want && p->tx_n < DM_PORT_TX_SIZE) {
			tx_push1(p, r->buf[r->done++]);
			k++;
		}
		if (k) {
			p->stats.transmitted += k;
			b->tx = 1;
		}
		if (want)
			p->tx_busy = 1;
		if (r->done < want) {
			if (!r->total_due || now < r->total_due)
				break;
			st = DM_ST_TIMEOUT;
		}
		q_pop(&p->writes);
		finish(b, r, st, r->done);
	}
}

/* the modem status register: the lines (none without the engine) and
   the deltas since it was read */
static dm_u8 msr(const struct dm_port *p)
{
	dm_u8 l = p->connected ? p->lines : 0, m = p->deltas;
	if (l & DM_LINE_CTS)
		m |= DM_MSR_CTS;
	if (l & DM_LINE_DSR)
		m |= DM_MSR_DSR;
	if (l & DM_LINE_RI)
		m |= DM_MSR_RI;
	if (l & DM_LINE_DCD)
		m |= DM_MSR_DCD;
	return m;
}

/* a received character into the buffer, escaped for LSRMST_INSERT: 0
   if there is no room */
static int rx_push(struct dm_port *p, dm_u8 c)
{
	if (p->lsrmst && c == p->lsrmst) {
		if (rx_room_left(p) < 2)
			return 0;
		rx_push1(p, c);
		rx_push1(p, DM_LSRMST_ESCAPE);
		return 1;
	}
	if (rx_room_left(p) < 1)
		return 0;
	rx_push1(p, c);
	return 1;
}

/* the modem status as it now is after old, in-band for LSRMST_INSERT
   (escape, LSRMST_MST, the register with its deltas) if it changed */
static void put_msr(struct dm_port *p, dm_u8 msr_old)
{
	dm_u8 now = msr(p);
	if (!p->lsrmst || (now & 0xf0) == (msr_old & 0xf0))
		return;
	if (rx_room_left(p) >= 3) {
		rx_push1(p, p->lsrmst);
		rx_push1(p, DM_LSRMST_MST);
		rx_push1(p, now);
	}
}

/* the app's DTR and RTS: the app's state given to the engine when they
   change */
static void set_dtr_rts(struct dm_port *p, struct dm_batch *b, int dtr,
			int rts)
{
	if (!dtr != !p->dtr || !rts != !p->rts) {
		p->dtr = dtr ? 1 : 0;
		p->rts = rts ? 1 : 0;
		b->state = 1;
	}
}

/* DTR and RTS as the handflow sets them: on with its control or
   handshake (a virtual port's buffer has room: handshake's line stays
   up), off with neither */
static void apply_handflow(struct dm_port *p, struct dm_batch *b)
{
	set_dtr_rts(p, b, (p->hf.control_handshake & DM_HF_DTR_MASK) != 0,
		    (p->hf.flow_replace & DM_HF_RTS_MASK) != 0);
}

/* see dm_port.h: DTR and RTS as the handflow has them; the deltas, the
   events and the counters from nothing */
void dm_port_open(struct dm_port *p, struct dm_batch *b)
{
	p->open = 1;
	b->state = 1;
	p->deltas = 0;
	p->events = 0;
	p->wait_mask = 0;
	p->lsrmst = 0;
	memset(&p->stats, 0, sizeof(p->stats));
	rx_clear(p);
	tx_clear(p);
	apply_handflow(p, b);
}

/* see dm_port.h */
void dm_port_close(struct dm_port *p, struct dm_batch *b)
{
	finish_all(b, &p->reads, DM_ST_CANCELLED);
	finish_all(b, &p->writes, DM_ST_CANCELLED);
	if (p->wait) {
		finish(b, p->wait, DM_ST_CANCELLED, 0);
		p->wait = NULL;
	}
	rx_clear(p);
	tx_clear(p);
	p->tx_busy = 0;
	p->wait_mask = 0;
	p->events = 0;
	p->lsrmst = 0;
	set_dtr_rts(p, b, 0, 0);
	p->open = 0;
	b->state = 1;
	if (p->rx_blocked)
		b->room = 1;
	p->rx_blocked = 0;
}

static void req_reset(struct dm_req *r)
{
	r->next = NULL;
	r->done = 0;
	r->started = 0;
	r->total_due = r->interval_due = 0;
	r->status = DM_ST_PENDING;
	r->info = 0;
}

/* see dm_port.h */
void dm_port_read(struct dm_port *p, struct dm_batch *b, struct dm_req *r)
{
	req_reset(r);
	if (!r->size) {
		finish(b, r, DM_ST_SUCCESS, 0);
		return;
	}
	q_push(&p->reads, r);
	pump_reads(p, b);
}

/* see dm_port.h */
void dm_port_write(struct dm_port *p, struct dm_batch *b, struct dm_req *r)
{
	req_reset(r);
	if (!r->size) {
		finish(b, r, DM_ST_SUCCESS, 0);
		return;
	}
	q_push(&p->writes, r);
	pump_writes(p, b);
}

/* see dm_port.h */
int dm_port_cancel(struct dm_port *p, struct dm_batch *b, struct dm_req *r)
{
	int found = q_remove(&p->reads, r) || q_remove(&p->writes, r);
	if (p->wait == r) {
		p->wait = NULL;
		found = 1;
	}
	/* (the next read or write is the head now: timed from now) */
	pump_reads(p, b);
	pump_writes(p, b);
	return found;
}

/* see dm_port.h */
void dm_port_tick(struct dm_port *p, struct dm_batch *b)
{
	pump_reads(p, b);
	pump_writes(p, b);
}

/* each line of ch changed: a delta and an event (RI's rising edge
   EV_RING, its falling one TERI, as a UART has it) */
static void line_events(struct dm_port *p, struct dm_batch *b, dm_u8 ch,
			dm_u8 lines)
{
	if (ch & DM_LINE_CTS) {
		p->deltas |= DM_MSR_DCTS;
		event(p, b, DM_EV_CTS);
	}
	if (ch & DM_LINE_DSR) {
		p->deltas |= DM_MSR_DDSR;
		event(p, b, DM_EV_DSR);
	}
	if (ch & DM_LINE_DCD) {
		p->deltas |= DM_MSR_DDCD;
		event(p, b, DM_EV_RLSD);
	}
	if (ch & DM_LINE_RI) {
		if (lines & DM_LINE_RI)
			event(p, b, DM_EV_RING);
		else
			p->deltas |= DM_MSR_TERI;
	}
}

/* see dm_port.h: the engine gone takes its lines and the characters it
   had yet to take */
void dm_port_connect(struct dm_port *p, struct dm_batch *b, int up)
{
	dm_u8 old = msr(p), was = p->connected ? p->lines : 0;
	p->connected = up ? 1 : 0;
	p->lines = 0;
	if (!up) {
		tx_clear(p);
		pump_writes(p, b);
	}
	line_events(p, b, was, 0);
	if (p->open)
		put_msr(p, old);
	pump_reads(p, b);
}

/* see dm_port.h */
void dm_port_set_lines(struct dm_port *p, struct dm_batch *b, dm_u8 lines)
{
	dm_u8 old, ch;
	if (!p->connected)
		return;
	old = msr(p);
	ch = (dm_u8)(lines ^ p->lines);
	p->lines = lines;
	line_events(p, b, ch, lines);
	if (p->open)
		put_msr(p, old);
	pump_reads(p, b);
}

/* see dm_port.h: with the app's port closed the characters go nowhere;
   NUL stripped if the handflow says so; the event character EV_RXFLAG;
   the buffer at 80% EV_RX80FULL */
dm_u32 dm_port_rx_put(struct dm_port *p, struct dm_batch *b,
		      const dm_u8 *buf, dm_u32 n)
{
	dm_u32 i;
	int any = 0, flag = 0;
	if (!p->open)
		return n;
	for (i = 0; i < n; i++) {
		dm_u8 c = buf[i];
		if (c == 0 && (p->hf.flow_replace & DM_HF_NULL_STRIPPING))
			continue;
		if (!rx_push(p, c)) {
			p->rx_blocked = 1;
			break;
		}
		any = 1;
		p->stats.received++;
		if (c == p->chars.event)
			flag = 1;
	}
	if (any)
		event(p, b, DM_EV_RXCHAR);
	if (flag)
		event(p, b, DM_EV_RXFLAG);
	if (!p->rx_full && p->rx_n >= DM_PORT_RX_SIZE * 8 / 10) {
		p->rx_full = 1;
		event(p, b, DM_EV_RX80FULL);
	}
	pump_reads(p, b);
	return i;
}

/* see dm_port.h */
dm_u32 dm_port_tx_take(struct dm_port *p, struct dm_batch *b, dm_u8 *buf,
		       dm_u32 max)
{
	dm_u32 k = 0;
	while (k < max && p->tx_n)
		buf[k++] = tx_pop1(p);
	if (k)
		pump_writes(p, b);
	return k;
}

/* see dm_port.h */
dm_u8 dm_port_app_state(const struct dm_port *p)
{
	return (dm_u8)((p->open ? DM_STATE_OPEN : 0) |
		       (p->dtr ? DM_STATE_DTR : 0) |
		       (p->rts ? DM_STATE_RTS : 0));
}

/* v (n bytes) as the output */
static dm_u32 give(void *out, dm_u32 outlen, dm_u32 *info, const void *v,
		   dm_u32 n)
{
	if (outlen < n || !out)
		return DM_ST_BUFFER_TOO_SMALL;
	memcpy(out, v, n);
	*info = n;
	return DM_ST_SUCCESS;
}

/* see dm_port.h */
dm_u32 dm_port_ioctl(struct dm_port *p, struct dm_batch *b, dm_u32 code,
		     const void *in, dm_u32 inlen, void *out, dm_u32 outlen,
		     dm_u32 *info, struct dm_req *r)
{
	dm_u32 u = 0;
#define NEED(n) do { if (inlen < (n) || !in) \
		return DM_ST_BUFFER_TOO_SMALL; } while (0)

	*info = 0;
	switch (code) {
	case DM_IO_SET_BAUD_RATE:
		NEED(4);
		memcpy(&u, in, 4);
		if (!u)
			return DM_ST_INVALID_PARAMETER;
		if (u != p->baud) {
			p->baud = u;
			b->state = 1;
		}
		return DM_ST_SUCCESS;
	case DM_IO_GET_BAUD_RATE:
		return give(out, outlen, info, &p->baud, 4);
	case DM_IO_SET_LINE_CONTROL: {
		struct dm_serial_line_control lc;
		NEED(sizeof(lc));
		memcpy(&lc, in, sizeof(lc));
		if (lc.word_length < 5 || lc.word_length > 8 ||
		    lc.stop_bits > 2 || lc.parity > 4 ||
		    (lc.stop_bits == 1 && lc.word_length != 5) ||
		    (lc.stop_bits == 2 && lc.word_length == 5))
			return DM_ST_INVALID_PARAMETER;
		p->lc = lc;
		return DM_ST_SUCCESS;
	}
	case DM_IO_GET_LINE_CONTROL:
		return give(out, outlen, info, &p->lc, sizeof(p->lc));
	case DM_IO_SET_TIMEOUTS: {
		struct dm_serial_timeouts t;
		NEED(sizeof(t));
		memcpy(&t, in, sizeof(t));
		if (t.read_interval == MAXT && t.read_multiplier == MAXT &&
		    t.read_constant == MAXT)
			return DM_ST_INVALID_PARAMETER;
		p->to = t;
		return DM_ST_SUCCESS;
	}
	case DM_IO_GET_TIMEOUTS:
		return give(out, outlen, info, &p->to, sizeof(p->to));
	case DM_IO_SET_HANDFLOW: {
		struct dm_serial_handflow h;
		NEED(sizeof(h));
		memcpy(&h, in, sizeof(h));
		if ((h.control_handshake & DM_HF_DTR_MASK) ==
			    DM_HF_DTR_MASK ||
		    h.xon_limit < 0 || h.xoff_limit < 0 ||
		    (dm_u32)h.xon_limit > DM_PORT_RX_SIZE ||
		    (dm_u32)h.xoff_limit > DM_PORT_RX_SIZE)
			return DM_ST_INVALID_PARAMETER;
		p->hf = h;
		apply_handflow(p, b);
		return DM_ST_SUCCESS;
	}
	case DM_IO_GET_HANDFLOW:
		return give(out, outlen, info, &p->hf, sizeof(p->hf));
	case DM_IO_SET_CHARS: {
		struct dm_serial_chars c;
		NEED(sizeof(c));
		memcpy(&c, in, sizeof(c));
		if (c.xon == c.xoff && (p->hf.flow_replace & 0x03))
			return DM_ST_INVALID_PARAMETER;
		p->chars = c;
		return DM_ST_SUCCESS;
	}
	case DM_IO_GET_CHARS:
		return give(out, outlen, info, &p->chars, sizeof(p->chars));
	case DM_IO_SET_DTR:
	case DM_IO_CLR_DTR:
		if ((p->hf.control_handshake & DM_HF_DTR_MASK) ==
		    DM_HF_DTR_HANDSHAKE)
			return DM_ST_INVALID_PARAMETER;
		set_dtr_rts(p, b, code == DM_IO_SET_DTR, p->rts);
		return DM_ST_SUCCESS;
	case DM_IO_SET_RTS:
	case DM_IO_CLR_RTS:
		u = p->hf.flow_replace & DM_HF_RTS_MASK;
		if (u == DM_HF_RTS_HANDSHAKE || u == DM_HF_TRANSMIT_TOGGLE)
			return DM_ST_INVALID_PARAMETER;
		set_dtr_rts(p, b, p->dtr, code == DM_IO_SET_RTS);
		return DM_ST_SUCCESS;
	case DM_IO_GET_DTRRTS:
		u = (p->dtr ? DM_DTRRTS_DTR : 0u) |
		    (p->rts ? DM_DTRRTS_RTS : 0u);
		return give(out, outlen, info, &u, 4);
	case DM_IO_GET_MODEM_CONTROL:
		/* (MCR: DTR, RTS, and OUT2 as a PC's UART has it) */
		u = (p->dtr ? 1u : 0u) | (p->rts ? 2u : 0u) | 8u;
		return give(out, outlen, info, &u, 4);
	case DM_IO_SET_MODEM_CONTROL:
		NEED(4);
		memcpy(&u, in, 4);
		set_dtr_rts(p, b, (u & 1) != 0, (u & 2) != 0);
		return DM_ST_SUCCESS;
	case DM_IO_GET_MODEMSTATUS:
		u = msr(p);
		p->deltas = 0;
		return give(out, outlen, info, &u, 4);
	case DM_IO_SET_WAIT_MASK:
		NEED(4);
		memcpy(&u, in, 4);
		if (u & ~DM_EV_ALL)
			return DM_ST_INVALID_PARAMETER;
		/* (a wait waiting ends with no event) */
		if (p->wait) {
			dm_u32 none = 0;
			memcpy(p->wait->buf, &none, 4);
			finish(b, p->wait, DM_ST_SUCCESS, 4);
			p->wait = NULL;
		}
		p->wait_mask = u;
		p->events = 0;
		return DM_ST_SUCCESS;
	case DM_IO_GET_WAIT_MASK:
		return give(out, outlen, info, &p->wait_mask, 4);
	case DM_IO_WAIT_ON_MASK:
		if (!r || r->size < 4)
			return DM_ST_BUFFER_TOO_SMALL;
		if (p->wait || !p->wait_mask)
			return DM_ST_INVALID_PARAMETER;
		req_reset(r);
		if (p->events) {
			dm_u32 got = p->events;
			p->events = 0;
			memcpy(r->buf, &got, 4);
			finish(b, r, DM_ST_SUCCESS, 4);
		} else {
			p->wait = r;
		}
		return DM_ST_PENDING;
	case DM_IO_PURGE:
		NEED(4);
		memcpy(&u, in, 4);
		if (!u || (u & ~0x0fu))
			return DM_ST_INVALID_PARAMETER;
		if (u & DM_PURGE_TXABORT)
			finish_all(b, &p->writes, DM_ST_CANCELLED);
		if (u & DM_PURGE_RXABORT)
			finish_all(b, &p->reads, DM_ST_CANCELLED);
		if (u & DM_PURGE_TXCLEAR)
			tx_clear(p);
		if (u & DM_PURGE_RXCLEAR)
			rx_clear(p);
		return DM_ST_SUCCESS;
	case DM_IO_GET_COMMSTATUS: {
		struct dm_serial_status s;
		struct dm_req *q;
		memset(&s, 0, sizeof(s));
		s.in_queue = p->rx_n;
		s.out_queue = p->tx_n;
		for (q = p->writes.head; q; q = q->next)
			s.out_queue += q->size - q->done;
		return give(out, outlen, info, &s, sizeof(s));
	}
	case DM_IO_GET_PROPERTIES: {
		struct dm_serial_commprop c;
		memset(&c, 0, sizeof(c));
		c.packet_length = sizeof(c);
		c.packet_version = 2;
		c.service_mask = SP_SERIALCOMM;
		c.max_tx_queue = DM_PORT_TX_SIZE;
		c.max_rx_queue = DM_PORT_RX_SIZE;
		c.max_baud = BAUD_USER;
		c.prov_sub_type = SP_RS232;
		c.prov_capabilities = PCF_ALL;
		c.settable_params = SP_ALL;
		c.settable_baud = BAUD_ALL;
		c.settable_data = DATABITS_ALL;
		c.settable_stop_parity = STOPPARITY_ALL;
		c.current_tx_queue = DM_PORT_TX_SIZE;
		c.current_rx_queue = DM_PORT_RX_SIZE;
		/* (kernel32 asks with its COMMPROP, ProvChar[1]: 62 bytes
		   and the padding) */
		if (outlen < 62 || !out)
			return DM_ST_BUFFER_TOO_SMALL;
		return give(out, outlen, info, &c,
			    outlen < sizeof(c) ? 62 : sizeof(c));
	}
	case DM_IO_GET_STATS:
		return give(out, outlen, info, &p->stats, sizeof(p->stats));
	case DM_IO_CLEAR_STATS:
		memset(&p->stats, 0, sizeof(p->stats));
		return DM_ST_SUCCESS;
	case DM_IO_LSRMST_INSERT:
		NEED(1);
		p->lsrmst = *(const dm_u8 *)in;
		return DM_ST_SUCCESS;
	case DM_IO_IMMEDIATE_CHAR:
		NEED(1);
		if (p->connected) {
			if (p->tx_n >= DM_PORT_TX_SIZE)
				return DM_ST_INVALID_PARAMETER;
			tx_push1(p, *(const dm_u8 *)in);
			b->tx = 1;
		}
		p->tx_busy = 1;
		p->stats.transmitted++;
		return DM_ST_SUCCESS;
	case DM_IO_CONFIG_SIZE:
		/* (none: kernel32's GetCommConfig makes its own) */
		u = 0;
		return give(out, outlen, info, &u, 4);
	case DM_IO_SET_QUEUE_SIZE:
		NEED(8);
		return DM_ST_SUCCESS;
	case DM_IO_SET_BREAK_ON:
	case DM_IO_SET_BREAK_OFF:
	case DM_IO_SET_XOFF:
	case DM_IO_SET_XON:
	case DM_IO_RESET_DEVICE:
	case DM_IO_SET_FIFO_CONTROL:
		return DM_ST_SUCCESS;
	case DM_IO_XOFF_COUNTER:
	case DM_IO_GET_COMMCONFIG:
	case DM_IO_SET_COMMCONFIG:
		return DM_ST_NOT_SUPPORTED;
	default:
		return DM_ST_INVALID_PARAMETER;
	}
#undef NEED
}
