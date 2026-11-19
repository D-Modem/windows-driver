/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dm_port.h - the COM port every D-Modem driver gives Windows: what
 *                an app (or Unimodem above it) asks of a serial port,
 *                with D-Modem's engine on the port's other side.
 *
 *    The app's side is a serial port's requests as serial.sys answers
 *    them: reads with the serial timeouts, writes held while the
 *    transmit buffer is full, IOCTL_SERIAL_WAIT_ON_MASK and its events,
 *    the modem's lines (CTS, DSR, DCD, RI) with their deltas, the app's
 *    DTR and RTS, LSRMST_INSERT's in-band status, and the settings a
 *    port keeps.  The engine's side gives the characters for the app
 *    and the modem's lines, and takes the app's characters.  Without
 *    the engine the lines are all off and the app's characters are
 *    dropped, as with a modem switched off.
 *
 *    The same semantics as D-Modem's Windows 10 virtual modem
 *    (platforms/windows/winmodem/port.cpp), in freestanding C so that
 *    the NT/WDM driver, the 95/98 VxD and the tests on the build machine
 *    share it.  It takes no lock and calls nothing back: the driver
 *    holds its own lock around each call, and each call gathers what is
 *    to be done after the lock is let go in a struct dm_batch (the
 *    requests completed, the engine to be told).
 *
 */
#ifndef DM_PORT_H
#define DM_PORT_H

#include "dm_types.h"

/* the NTSTATUS values requests complete with */
#define DM_ST_SUCCESS            0x00000000u
#define DM_ST_TIMEOUT            0x00000102u
#define DM_ST_PENDING            0x00000103u
#define DM_ST_CANCELLED          0xC0000120u
#define DM_ST_INVALID_PARAMETER  0xC000000Du
#define DM_ST_BUFFER_TOO_SMALL   0xC0000023u
#define DM_ST_NOT_SUPPORTED      0xC00000BBu

/* the serial IOCTLs: CTL_CODE(FILE_DEVICE_SERIAL_PORT, fn,
   METHOD_BUFFERED, FILE_ANY_ACCESS) (ntddser.h's IOCTL_SERIAL_*) */
#define DM_SERIAL_IOCTL(fn) (0x1b0000u | ((dm_u32)(fn) << 2))
#define DM_IO_SET_BAUD_RATE      DM_SERIAL_IOCTL(1)
#define DM_IO_SET_QUEUE_SIZE     DM_SERIAL_IOCTL(2)
#define DM_IO_SET_LINE_CONTROL   DM_SERIAL_IOCTL(3)
#define DM_IO_SET_BREAK_ON       DM_SERIAL_IOCTL(4)
#define DM_IO_SET_BREAK_OFF      DM_SERIAL_IOCTL(5)
#define DM_IO_IMMEDIATE_CHAR     DM_SERIAL_IOCTL(6)
#define DM_IO_SET_TIMEOUTS       DM_SERIAL_IOCTL(7)
#define DM_IO_GET_TIMEOUTS       DM_SERIAL_IOCTL(8)
#define DM_IO_SET_DTR            DM_SERIAL_IOCTL(9)
#define DM_IO_CLR_DTR            DM_SERIAL_IOCTL(10)
#define DM_IO_RESET_DEVICE       DM_SERIAL_IOCTL(11)
#define DM_IO_SET_RTS            DM_SERIAL_IOCTL(12)
#define DM_IO_CLR_RTS            DM_SERIAL_IOCTL(13)
#define DM_IO_SET_XOFF           DM_SERIAL_IOCTL(14)
#define DM_IO_SET_XON            DM_SERIAL_IOCTL(15)
#define DM_IO_GET_WAIT_MASK      DM_SERIAL_IOCTL(16)
#define DM_IO_SET_WAIT_MASK      DM_SERIAL_IOCTL(17)
#define DM_IO_WAIT_ON_MASK       DM_SERIAL_IOCTL(18)
#define DM_IO_PURGE              DM_SERIAL_IOCTL(19)
#define DM_IO_GET_BAUD_RATE      DM_SERIAL_IOCTL(20)
#define DM_IO_GET_LINE_CONTROL   DM_SERIAL_IOCTL(21)
#define DM_IO_GET_CHARS          DM_SERIAL_IOCTL(22)
#define DM_IO_SET_CHARS          DM_SERIAL_IOCTL(23)
#define DM_IO_GET_HANDFLOW       DM_SERIAL_IOCTL(24)
#define DM_IO_SET_HANDFLOW       DM_SERIAL_IOCTL(25)
#define DM_IO_GET_MODEMSTATUS    DM_SERIAL_IOCTL(26)
#define DM_IO_GET_COMMSTATUS     DM_SERIAL_IOCTL(27)
#define DM_IO_XOFF_COUNTER       DM_SERIAL_IOCTL(28)
#define DM_IO_GET_PROPERTIES     DM_SERIAL_IOCTL(29)
#define DM_IO_GET_DTRRTS         DM_SERIAL_IOCTL(30)
#define DM_IO_LSRMST_INSERT      DM_SERIAL_IOCTL(31)
#define DM_IO_CONFIG_SIZE        DM_SERIAL_IOCTL(32)
#define DM_IO_GET_COMMCONFIG     DM_SERIAL_IOCTL(33)
#define DM_IO_SET_COMMCONFIG     DM_SERIAL_IOCTL(34)
#define DM_IO_GET_STATS          DM_SERIAL_IOCTL(35)
#define DM_IO_CLEAR_STATS        DM_SERIAL_IOCTL(36)
#define DM_IO_GET_MODEM_CONTROL  DM_SERIAL_IOCTL(37)
#define DM_IO_SET_MODEM_CONTROL  DM_SERIAL_IOCTL(38)
#define DM_IO_SET_FIFO_CONTROL   DM_SERIAL_IOCTL(39)

/* the wait mask's events (winbase.h's EV_*) */
#define DM_EV_RXCHAR    0x0001u
#define DM_EV_RXFLAG    0x0002u
#define DM_EV_TXEMPTY   0x0004u
#define DM_EV_CTS       0x0008u
#define DM_EV_DSR       0x0010u
#define DM_EV_RLSD      0x0020u
#define DM_EV_BREAK     0x0040u
#define DM_EV_ERR       0x0080u
#define DM_EV_RING      0x0100u
#define DM_EV_PERR      0x0200u
#define DM_EV_RX80FULL  0x0400u
#define DM_EV_EVENT1    0x0800u
#define DM_EV_EVENT2    0x1000u
#define DM_EV_ALL       0x1fffu

/* the modem status register (SERIAL_MSR_*): deltas, then the lines */
#define DM_MSR_DCTS  0x01u
#define DM_MSR_DDSR  0x02u
#define DM_MSR_TERI  0x04u
#define DM_MSR_DDCD  0x08u
#define DM_MSR_CTS   0x10u
#define DM_MSR_DSR   0x20u
#define DM_MSR_RI    0x40u
#define DM_MSR_DCD   0x80u

/* the modem's lines as the engine gives them (wire.h's LINE_*) */
#define DM_LINE_DCD  1u
#define DM_LINE_RI   2u
#define DM_LINE_DSR  4u
#define DM_LINE_CTS  8u
/* the app's side as the engine is told (wire.h's STATE_*) */
#define DM_STATE_OPEN  1u
#define DM_STATE_DTR   2u
#define DM_STATE_RTS   4u

/* the handflow's bits (SERIAL_DTR_*, SERIAL_RTS_*) */
#define DM_HF_DTR_MASK        0x03u
#define DM_HF_DTR_CONTROL     0x01u
#define DM_HF_DTR_HANDSHAKE   0x02u
#define DM_HF_NULL_STRIPPING  0x08u
#define DM_HF_RTS_MASK        0xc0u
#define DM_HF_RTS_CONTROL     0x40u
#define DM_HF_RTS_HANDSHAKE   0x80u
#define DM_HF_TRANSMIT_TOGGLE 0xc0u
/* IOCTL_SERIAL_PURGE's (SERIAL_PURGE_*) */
#define DM_PURGE_TXABORT  1u
#define DM_PURGE_RXABORT  2u
#define DM_PURGE_TXCLEAR  4u
#define DM_PURGE_RXCLEAR  8u
/* IOCTL_SERIAL_GET_DTRRTS's (SERIAL_DTR_STATE, SERIAL_RTS_STATE) */
#define DM_DTRRTS_DTR  1u
#define DM_DTRRTS_RTS  2u
/* LSRMST_INSERT's in-band status: the escape, then one of these */
#define DM_LSRMST_ESCAPE      0u
#define DM_LSRMST_LSR_DATA    1u
#define DM_LSRMST_LSR_NODATA  2u
#define DM_LSRMST_MST         3u

/* the requests' structures, as ntddser.h has them */
struct dm_serial_timeouts {
	dm_u32 read_interval, read_multiplier, read_constant;
	dm_u32 write_multiplier, write_constant;
};
struct dm_serial_handflow {
	dm_u32 control_handshake, flow_replace;
	dm_s32 xon_limit, xoff_limit;
};
struct dm_serial_chars {
	dm_u8 eof, error, brk, event, xon, xoff;
};
struct dm_serial_line_control {
	dm_u8 stop_bits, parity, word_length;
};
struct dm_serial_status {
	dm_u32 errors, hold_reasons, in_queue, out_queue;
	dm_u8 eof_received, wait_for_immediate;
};
struct dm_serial_stats {
	dm_u32 received, transmitted, frame_errors, overrun_errors,
		buffer_overruns, parity_errors;
};
/* SERIAL_COMMPROP (its ProvChar[1] and the padding after it) */
struct dm_serial_commprop {
	dm_u16 packet_length, packet_version;
	dm_u32 service_mask, reserved1, max_tx_queue, max_rx_queue, max_baud;
	dm_u32 prov_sub_type, prov_capabilities, settable_params;
	dm_u32 settable_baud;
	dm_u16 settable_data, settable_stop_parity;
	dm_u32 current_tx_queue, current_rx_queue, prov_spec1, prov_spec2;
	dm_u16 prov_char[2];
};

/* an app's read, write or IOCTL_SERIAL_WAIT_ON_MASK while the port
   holds it.  The driver fills in buf and size (a read's room, a write's
   characters, a wait's 4 bytes of output) and os (its IRP); the port
   the rest.  Once done, status and info (the bytes read or written, a
   wait's 4) are what to complete it with. */
struct dm_req {
	struct dm_req *next;    /* the port's queues, then the batch's list */
	dm_u8 *buf;
	dm_u32 size;
	void *os;
	dm_u32 status;
	dm_u32 info;
	/* the port's */
	dm_u32 done;            /* bytes so far */
	dm_u8 started;          /* the head of its queue: timed */
	dm_u64 total_due, interval_due; /* ms; 0: none */
};

/* a FIFO of requests */
struct dm_req_queue {
	struct dm_req *head, *tail;
};

/* what a call leaves for the driver to do once its lock is let go */
struct dm_batch {
	dm_u64 now;             /* ms, by the driver's clock: set by
				   dm_batch_init */
	struct dm_req_queue done; /* complete each (status, info) */
	dm_u8 tx;               /* characters for the engine
				   (dm_port_tx_take) */
	dm_u8 room;             /* room for the engine's characters again
				   (dm_port_rx_put) */
	dm_u8 state;            /* the app's side (dm_port_app_state,
				   baud) has changed */
	dm_u8 timer;            /* the next timeout has changed: set the
				   timer for due (0: none), then call
				   dm_port_tick */
	dm_u64 due;
};

#define DM_PORT_RX_SIZE 16384u
#define DM_PORT_TX_SIZE 4096u

struct dm_port {
	dm_u8 open, connected;
	dm_u8 dtr, rts;
	dm_u8 lines;            /* the modem's (DM_LINE_*) */
	dm_u8 deltas;           /* DM_MSR_D*, TERI since read */
	dm_u32 baud;
	struct dm_serial_line_control lc;
	struct dm_serial_handflow hf;
	struct dm_serial_chars chars;
	struct dm_serial_timeouts to;
	struct dm_serial_stats stats;
	dm_u32 wait_mask, events;
	dm_u8 lsrmst;           /* the escape, 0: none */
	dm_u8 tx_busy;          /* written since TXEMPTY */
	dm_u8 rx_full;          /* RX80FULL given */
	dm_u8 rx_blocked;       /* rx_put turned some away */

	/* the buffers, rings */
	dm_u8 rx[DM_PORT_RX_SIZE];
	dm_u32 rx_get, rx_n;
	dm_u8 tx[DM_PORT_TX_SIZE];
	dm_u32 tx_get, tx_n;

	struct dm_req_queue reads, writes;
	struct dm_req *wait;
	dm_u64 due;             /* the timer as last given */
};

/* a batch for one call, at now (ms) */
void dm_batch_init(struct dm_batch *b, dm_u64 now);
/* the batch's next completed request, or NULL */
struct dm_req *dm_batch_next(struct dm_batch *b);

/* a closed port with the engine away */
void dm_port_init(struct dm_port *p);

/* Each of these is called with the driver's lock held, and ends with
   dm_port_end(p, b) before the lock is let go. */

/* the app's side: its handle opened; closed (every request it has
   waiting cancelled, DTR and RTS dropped, the buffers emptied) */
void dm_port_open(struct dm_port *p, struct dm_batch *b);
void dm_port_close(struct dm_port *p, struct dm_batch *b);
/* a read or a write: completed now (in b) or held */
void dm_port_read(struct dm_port *p, struct dm_batch *b, struct dm_req *r);
void dm_port_write(struct dm_port *p, struct dm_batch *b,
		   struct dm_req *r);
/* an IOCTL: its status, and in *info the bytes of out it filled.
   DM_IO_WAIT_ON_MASK takes r and gives DM_ST_PENDING (r completes in
   this batch or a later one), unless it is refused */
dm_u32 dm_port_ioctl(struct dm_port *p, struct dm_batch *b, dm_u32 code,
		     const void *in, dm_u32 inlen, void *out, dm_u32 outlen,
		     dm_u32 *info, struct dm_req *r);
/* r taken back from the port (1) for the driver to complete as
   cancelled with r->done bytes; 0: the port doesn't hold it */
int dm_port_cancel(struct dm_port *p, struct dm_batch *b, struct dm_req *r);
/* the timeouts that are due */
void dm_port_tick(struct dm_port *p, struct dm_batch *b);

/* the engine's side: there or not (not: its lines off, the app's
   characters dropped) */
void dm_port_connect(struct dm_port *p, struct dm_batch *b, int up);
/* the modem's lines (DM_LINE_*) */
void dm_port_set_lines(struct dm_port *p, struct dm_batch *b, dm_u8 lines);
/* the modem's characters for the app: how many were taken (the rest
   when there is room: b->room) */
dm_u32 dm_port_rx_put(struct dm_port *p, struct dm_batch *b,
		      const dm_u8 *buf, dm_u32 n);
/* up to max of the app's characters for the engine */
dm_u32 dm_port_tx_take(struct dm_port *p, struct dm_batch *b, dm_u8 *buf,
		       dm_u32 max);
/* the app's side (DM_STATE_*), its baud rate */
dm_u8 dm_port_app_state(const struct dm_port *p);

/* what follows every call: the transmit buffer's emptying, the receive
   buffer's room, the timer */
void dm_port_end(struct dm_port *p, struct dm_batch *b);

#endif
