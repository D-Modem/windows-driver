/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem.c - dmodem.sys, D-Modem's WDM modem driver for Windows 2000
 *               and XP (and, through CCPORT, 98 SE and ME): the COM port
 *               under Unimodem's modem.sys, with D-Modem's engine on the
 *               port's other side through the engine channel
 *               (dmodem_ioctl.h).
 *
 *    Phase 1 of docs/design.md: the modem has no hardware yet (the
 *    engine brings its own line: SIP).  The hardware back ends come next,
 *    on the same device.  A USB modem's devnode is usb.c's instead: there
 *    this driver passes the modem's transfers through to dmodemd
 *    (dmodem_usb.h).
 *
 *    The modem's devnode (Root\DModem, the Modem class) has modem.sys
 *    as its function driver and this one as its lower filter
 *    (inf/dmodem.inf), as Microsoft's fakemodem sample has it; this
 *    driver answers every read, write and serial IOCTL itself, and passes
 *    only PnP, power and WMI down.  Its device:
 *      - \Device\DModem<n>, its COM port's name (PortName, which the
 *        modem class installer gives the device) as \DosDevices\<name>
 *        and in HARDWARE\DEVICEMAP\SERIALCOMM, for programs that list
 *        COM ports;
 *      - GUID_DEVINTERFACE_MODEM's interface;
 *      - one handle at a time (a serial port's rule; a second is
 *        refused): its reads, writes and serial IOCTLs to the port
 *        (common/dm_port.c), the ones that wait cancelable.
 *    Beside it, \Device\DModemEngine<n> (\\.\DModemEngine<n>), the
 *    engine's, also one handle at a time (common/dm_link.c).
 *
 *    Only the kernel API that Windows 2000 and 98 SE's NTKERN both have
 *    (make check: check-imports.py --nt 2000 and --wdm 98se).  One spin
 *    lock per modem holds the port and the link; requests complete once
 *    it is let go.
 *
 */

#include "dm_nt.h"

#include "dm_link.h"
#include "dm_port.h"
#include "dmodem_ioctl.h"

/* GUID_DEVINTERFACE_MODEM (ntddmodm.h) */
static const GUID modem_interface = { 0x2c7089aa, 0x2e0e, 0x11d1,
	{ 0xb1, 0x14, 0x00, 0xc0, 0x4f, 0xc2, 0xaa, 0xe4 } };

/* a modem: the extension of its COM port's device */
struct dm_modem {
	struct dm_dev hdr;
	PDEVICE_OBJECT self, lower, pdo, engine;
	ULONG index;

	KSPIN_LOCK lock;
	struct dm_port port;
	struct dm_link link;
	KTIMER timer;
	KDPC timer_dpc;
	PIRP wire_read;         /* the engine's waiting WIRE_READ */

	LONG port_opens, engine_opens;
	BOOLEAN started, gone;
	/* the requests being dispatched, for removal to wait on */
	LONG io;
	KEVENT io_none;

	char port_name[32];
	WCHAR link_buf[64], serialcomm_buf[64];
	UNICODE_STRING link_name;       /* \DosDevices\<port>, if made */
	UNICODE_STRING serialcomm_name; /* SERIALCOMM's value, if written */
	UNICODE_STRING iface;           /* the modem interface's link */
	WCHAR engine_link_buf[64];
	UNICODE_STRING engine_link;
};

static LONG next_index = -1;

/* ---- time, the outstanding requests */

/* ms since the system started */
static dm_u64 now_ms(void)
{
	return (dm_u64)KeQueryInterruptTime() / 10000;
}

/* a request on its way through the modem (0: the modem has gone) */
static int io_enter(struct dm_modem *m)
{
	InterlockedIncrement(&m->io);
	if (m->gone) {
		if (InterlockedDecrement(&m->io) == 0)
			KeSetEvent(&m->io_none, IO_NO_INCREMENT, FALSE);
		return 0;
	}
	return 1;
}

static void io_leave(struct dm_modem *m)
{
	if (InterlockedDecrement(&m->io) == 0)
		KeSetEvent(&m->io_none, IO_NO_INCREMENT, FALSE);
}

static NTSTATUS complete(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
	irp->IoStatus.Status = st;
	irp->IoStatus.Information = info;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	return st;
}

/* ---- the port's requests and their batches */

/* a held request's state: the IRP's, freed when it completes */
struct held {
	struct dm_req r;
	BOOLEAN orphan;         /* done while its cancel routine was on its
				   way: that completes it */
};

static struct held *held_of(PIRP irp)
{
	return (struct held *)irp->Tail.Overlay.DriverContext[0];
}

static void complete_held(PIRP irp, NTSTATUS st, ULONG_PTR info)
{
	struct held *h = held_of(irp);
	irp->Tail.Overlay.DriverContext[0] = NULL;
	if (h)
		ExFreePool(h);
	irp->IoStatus.Status = st;
	irp->IoStatus.Information = info;
	IoCompleteRequest(irp, IO_SERIAL_INCREMENT);
}

static void timer_set(struct dm_modem *m, dm_u64 due, dm_u64 now)
{
	LARGE_INTEGER t;
	if (!due) {
		KeCancelTimer(&m->timer);
		return;
	}
	/* relative, in 100 ns; at least 1 ms */
	t.QuadPart = -(LONGLONG)((due > now ? due - now : 1) * 10000);
	KeSetTimer(&m->timer, t, &m->timer_dpc);
}

static VOID NTAPI wire_read_cancel(PDEVICE_OBJECT dev, PIRP irp);

/* the engine's waiting WIRE_READ given what there is for it, taken out
   for completion (*out), if there is something */
static void wire_pump(struct dm_modem *m, struct dm_batch *b, PIRP *out)
{
	PIRP irp = m->wire_read;
	PIO_STACK_LOCATION sl;
	dm_u32 n;
	if (!irp || !dm_link_out_ready(&m->link))
		return;
	if (IoSetCancelRoutine(irp, NULL) == NULL)
		return;         /* (its cancel routine has it) */
	m->wire_read = NULL;
	sl = IoGetCurrentIrpStackLocation(irp);
	n = dm_link_pull(&m->link, b, irp->AssociatedIrp.SystemBuffer,
			 sl->Parameters.DeviceIoControl.OutputBufferLength);
	irp->IoStatus.Status = STATUS_SUCCESS;
	irp->IoStatus.Information = n;
	*out = irp;
}

/* the end of a call on the port: the lock (taken at irql) let go, then
   the requests completed and the timer set */
static void batch_end(struct dm_modem *m, struct dm_batch *b, KIRQL irql)
{
	PIRP wire = NULL, irp;
	struct dm_req *r;
	struct dm_req_queue done = { NULL, NULL };

	dm_port_end(&m->port, b);
	if (b->room && dm_link_flush(&m->link, b))
		dm_port_end(&m->port, b);
	wire_pump(m, b, &wire);
	dm_port_end(&m->port, b);
	if (b->timer)
		timer_set(m, b->due, b->now);
	/* each done request: ours to complete unless its cancel routine is
	   on its way (that one completes it, with what the port gave) */
	while ((r = dm_batch_next(b)) != NULL) {
		irp = r->os;
		if (IoSetCancelRoutine(irp, NULL) == NULL) {
			((struct held *)r)->orphan = TRUE;
			continue;
		}
		r->next = NULL;
		if (done.tail)
			done.tail->next = r;
		else
			done.head = r;
		done.tail = r;
	}
	KeReleaseSpinLock(&m->lock, irql);

	while ((r = done.head) != NULL) {
		done.head = r->next;
		complete_held(r->os, (NTSTATUS)r->status, r->info);
	}
	if (wire)
		IoCompleteRequest(wire, IO_NO_INCREMENT);
}

/* the cancel routine of a held read, write or wait */
static VOID NTAPI held_cancel(PDEVICE_OBJECT dev, PIRP irp)
{
	struct dm_modem *m = ((struct dm_dev *)dev->DeviceExtension)->m;
	struct held *h = held_of(irp);
	struct dm_batch b;
	KIRQL irql;
	int found;

	IoReleaseCancelSpinLock(irp->CancelIrql);
	KeAcquireSpinLock(&m->lock, &irql);
	dm_batch_init(&b, now_ms());
	found = dm_port_cancel(&m->port, &b, &h->r);
	if (found || h->orphan) {
		NTSTATUS st = found ? STATUS_CANCELLED : (NTSTATUS)h->r.status;
		ULONG_PTR info = found ? h->r.done : h->r.info;
		batch_end(m, &b, irql);
		complete_held(irp, st, info);
		return;
	}
	/* (not ours: the port completed it, and batch_end will) */
	batch_end(m, &b, irql);
}

/* irp held by the port (a read, write or wait): a struct held for it,
   the call made under the lock, its cancel routine set while it waits */
static NTSTATUS port_hold(struct dm_modem *m, PIRP irp, int what,
			  dm_u8 *buf, dm_u32 size)
{
	struct held *h = ExAllocatePoolWithTag(NonPagedPool, sizeof(*h),
					       DM_TAG);
	struct dm_batch b;
	KIRQL irql;
	dm_u32 st = DM_ST_PENDING, info = 0;

	if (!h)
		return complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	memset(h, 0, sizeof(*h));
	h->r.buf = buf;
	h->r.size = size;
	h->r.os = irp;
	irp->Tail.Overlay.DriverContext[0] = h;
	IoMarkIrpPending(irp);
	IoSetCancelRoutine(irp, held_cancel);

	KeAcquireSpinLock(&m->lock, &irql);
	dm_batch_init(&b, now_ms());
	if (what == IRP_MJ_READ) {
		dm_port_read(&m->port, &b, &h->r);
	} else if (what == IRP_MJ_WRITE) {
		dm_port_write(&m->port, &b, &h->r);
	} else {
		st = dm_port_ioctl(&m->port, &b, DM_IO_WAIT_ON_MASK, NULL, 0,
				   buf, size, &info, &h->r);
	}
	if (st != DM_ST_PENDING) {
		/* (refused: never held; if its cancel routine is on its way,
		   that completes it, with this status) */
		BOOLEAN ours = IoSetCancelRoutine(irp, NULL) != NULL;
		if (!ours) {
			h->r.status = st;
			h->r.info = info;
			h->orphan = TRUE;
		}
		batch_end(m, &b, irql);
		if (ours)
			complete_held(irp, (NTSTATUS)st, info);
		return STATUS_PENDING;
	}
	/* cancelled before its cancel routine was set: as if cancelled
	   now (the routine, if it was set and called, does the same) */
	if (irp->Cancel && IoSetCancelRoutine(irp, NULL) != NULL) {
		if (dm_port_cancel(&m->port, &b, &h->r)) {
			dm_u32 done = h->r.done;
			batch_end(m, &b, irql);
			complete_held(irp, STATUS_CANCELLED, done);
			return STATUS_PENDING;
		}
		/* (done already: the batch completes it, its cancel routine
		   cleared; put it back so that batch_end takes it) */
		IoSetCancelRoutine(irp, held_cancel);
	}
	batch_end(m, &b, irql);
	return STATUS_PENDING;
}

/* the port's timer: the timeouts that are due */
static VOID NTAPI timer_dpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
	struct dm_modem *m = ctx;
	struct dm_batch b;
	KIRQL irql;
	(void)dpc;
	(void)a1;
	(void)a2;
	KeAcquireSpinLock(&m->lock, &irql);
	dm_batch_init(&b, now_ms());
	m->port.due = 0;        /* (fired: set again if still due) */
	dm_port_tick(&m->port, &b);
	batch_end(m, &b, irql);
}

/* ---- the COM port's dispatch */

static NTSTATUS port_create(struct dm_modem *m, PIRP irp)
{
	struct dm_batch b;
	KIRQL irql;
	if (!m->started)
		return complete(irp, STATUS_DEVICE_NOT_READY, 0);
	if (InterlockedIncrement(&m->port_opens) != 1) {
		InterlockedDecrement(&m->port_opens);
		return complete(irp, STATUS_ACCESS_DENIED, 0);
	}
	KeAcquireSpinLock(&m->lock, &irql);
	dm_batch_init(&b, now_ms());
	dm_port_open(&m->port, &b);
	batch_end(m, &b, irql);
	return complete(irp, STATUS_SUCCESS, 0);
}

/* IRP_MJ_CLEANUP: the handle closed, what it had waiting cancelled */
static NTSTATUS port_cleanup(struct dm_modem *m, PIRP irp)
{
	struct dm_batch b;
	KIRQL irql;
	KeAcquireSpinLock(&m->lock, &irql);
	dm_batch_init(&b, now_ms());
	if (m->port.open)
		dm_port_close(&m->port, &b);
	batch_end(m, &b, irql);
	return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS port_close(struct dm_modem *m, PIRP irp)
{
	InterlockedDecrement(&m->port_opens);
	return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS port_ioctl(struct dm_modem *m, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	ULONG code = sl->Parameters.DeviceIoControl.IoControlCode;
	ULONG inlen = sl->Parameters.DeviceIoControl.InputBufferLength;
	ULONG outlen = sl->Parameters.DeviceIoControl.OutputBufferLength;
	void *buf = irp->AssociatedIrp.SystemBuffer;
	dm_u8 tmp[64];
	struct dm_batch b;
	KIRQL irql;
	dm_u32 st, info = 0;

	if (code == DM_IO_WAIT_ON_MASK) {
		if (outlen < 4)
			return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
		return port_hold(m, irp, IRP_MJ_DEVICE_CONTROL, buf, 4);
	}
	/* (METHOD_BUFFERED: in and out share the buffer; the input copied
	   out first, as no serial IOCTL's is bigger than 64 bytes) */
	if (inlen > sizeof(tmp))
		inlen = sizeof(tmp);
	if (inlen && buf)
		memcpy(tmp, buf, inlen);
	KeAcquireSpinLock(&m->lock, &irql);
	dm_batch_init(&b, now_ms());
	st = dm_port_ioctl(&m->port, &b, code, tmp, inlen, buf, outlen, &info,
			   NULL);
	batch_end(m, &b, irql);
	return complete(irp, (NTSTATUS)st, info);
}

static NTSTATUS port_dispatch(struct dm_modem *m, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	switch (sl->MajorFunction) {
	case IRP_MJ_CREATE:
		return port_create(m, irp);
	case IRP_MJ_CLEANUP:
		return port_cleanup(m, irp);
	case IRP_MJ_CLOSE:
		return port_close(m, irp);
	case IRP_MJ_READ:
		return port_hold(m, irp, IRP_MJ_READ,
				 irp->AssociatedIrp.SystemBuffer,
				 sl->Parameters.Read.Length);
	case IRP_MJ_WRITE:
		return port_hold(m, irp, IRP_MJ_WRITE,
				 irp->AssociatedIrp.SystemBuffer,
				 sl->Parameters.Write.Length);
	case IRP_MJ_DEVICE_CONTROL:
		return port_ioctl(m, irp);
	case IRP_MJ_QUERY_INFORMATION:
	case IRP_MJ_SET_INFORMATION:
	case IRP_MJ_FLUSH_BUFFERS:
		/* (as serial.sys: nothing to say or do) */
		return complete(irp, STATUS_SUCCESS, 0);
	default:
		return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	}
}

/* ---- the engine's device */

static VOID NTAPI wire_read_cancel(PDEVICE_OBJECT dev, PIRP irp)
{
	struct dm_modem *m = ((struct dm_dev *)dev->DeviceExtension)->m;
	KIRQL irql;
	IoReleaseCancelSpinLock(irp->CancelIrql);
	KeAcquireSpinLock(&m->lock, &irql);
	if (m->wire_read == irp)
		m->wire_read = NULL;
	KeReleaseSpinLock(&m->lock, irql);
	complete(irp, STATUS_CANCELLED, 0);
}

static NTSTATUS engine_info(struct dm_modem *m, PIRP irp, ULONG outlen)
{
	struct dmodem_info *i = irp->AssociatedIrp.SystemBuffer;
	if (outlen < sizeof(*i))
		return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
	memset(i, 0, sizeof(*i));
	i->version = DMODEM_IOCTL_VERSION;
	i->hw = DMODEM_HW_NONE;
	memcpy(i->port, m->port_name, sizeof(i->port));
	memcpy(i->driver, DM_VERSION, sizeof(DM_VERSION));
	return complete(irp, STATUS_SUCCESS, sizeof(*i));
}

static NTSTATUS engine_dispatch(struct dm_modem *m, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	struct dm_batch b;
	KIRQL irql;
	ULONG code, inlen, outlen;

	switch (sl->MajorFunction) {
	case IRP_MJ_CREATE:
		if (InterlockedIncrement(&m->engine_opens) != 1) {
			InterlockedDecrement(&m->engine_opens);
			return complete(irp, STATUS_ACCESS_DENIED, 0);
		}
		KeAcquireSpinLock(&m->lock, &irql);
		dm_batch_init(&b, now_ms());
		dm_link_attach(&m->link, &b);
		batch_end(m, &b, irql);
		return complete(irp, STATUS_SUCCESS, 0);
	case IRP_MJ_CLEANUP: {
		PIRP wr;
		KeAcquireSpinLock(&m->lock, &irql);
		dm_batch_init(&b, now_ms());
		dm_link_detach(&m->link, &b);
		wr = m->wire_read;
		if (wr && IoSetCancelRoutine(wr, NULL) != NULL)
			m->wire_read = NULL;
		else
			wr = NULL;
		batch_end(m, &b, irql);
		if (wr)
			complete(wr, STATUS_CANCELLED, 0);
		return complete(irp, STATUS_SUCCESS, 0);
	}
	case IRP_MJ_CLOSE:
		InterlockedDecrement(&m->engine_opens);
		return complete(irp, STATUS_SUCCESS, 0);
	case IRP_MJ_DEVICE_CONTROL:
		break;
	default:
		return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	}

	code = sl->Parameters.DeviceIoControl.IoControlCode;
	inlen = sl->Parameters.DeviceIoControl.InputBufferLength;
	outlen = sl->Parameters.DeviceIoControl.OutputBufferLength;
	switch (code) {
	case IOCTL_DMODEM_INFO:
		return engine_info(m, irp, outlen);
	case IOCTL_DMODEM_WIRE_READ:
		/* (room for a HELLO and a STATE at least) */
		if (outlen < 2 * DM_WIRE_HEAD + 2 + 255 + 5)
			return complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
		KeAcquireSpinLock(&m->lock, &irql);
		if (m->wire_read) {
			KeReleaseSpinLock(&m->lock, irql);
			return complete(irp, STATUS_DEVICE_BUSY, 0);
		}
		IoMarkIrpPending(irp);
		IoSetCancelRoutine(irp, wire_read_cancel);
		if (irp->Cancel && IoSetCancelRoutine(irp, NULL) != NULL) {
			KeReleaseSpinLock(&m->lock, irql);
			complete(irp, STATUS_CANCELLED, 0);
			return STATUS_PENDING;
		}
		m->wire_read = irp;
		dm_batch_init(&b, now_ms());
		batch_end(m, &b, irql);         /* (given now, if it can be) */
		return STATUS_PENDING;
	case IOCTL_DMODEM_WIRE_WRITE: {
		dm_u32 used;
		BOOLEAN bad;
		KeAcquireSpinLock(&m->lock, &irql);
		dm_batch_init(&b, now_ms());
		used = dm_link_feed(&m->link, &b,
				    irp->AssociatedIrp.SystemBuffer, inlen);
		bad = m->link.bad;
		batch_end(m, &b, irql);
		return complete(irp, bad ? STATUS_INVALID_PARAMETER :
				STATUS_SUCCESS, used);
	}
	case IOCTL_DMODEM_SAMPLES_READ:
	case IOCTL_DMODEM_SAMPLES_WRITE:
	case IOCTL_DMODEM_HW_CTL:
		return complete(irp, STATUS_NOT_SUPPORTED, 0);
	default:
		return complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	}
}

/* ---- the COM port's name */

/* the device's PortName (from its key) into m->port_name ("" if none) */
static void read_port_name(struct dm_modem *m)
{
	HANDLE key;
	UNICODE_STRING v;
	union {
		KEY_VALUE_PARTIAL_INFORMATION i;
		UCHAR b[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 64];
	} u;
	ULONG len, i, n;
	const WCHAR *w;

	m->port_name[0] = 0;
	if (!NT_SUCCESS(IoOpenDeviceRegistryKey(m->pdo, PLUGPLAY_REGKEY_DEVICE,
						KEY_READ, &key)))
		return;
	RtlInitUnicodeString(&v, L"PortName");
	if (NT_SUCCESS(ZwQueryValueKey(key, &v, KeyValuePartialInformation,
				       &u.i, sizeof(u), &len)) &&
	    u.i.Type == REG_SZ) {
		w = (const WCHAR *)u.i.Data;
		n = u.i.DataLength / sizeof(WCHAR);
		for (i = 0; i < n && i < sizeof(m->port_name) - 1 && w[i];
		     i++)
			m->port_name[i] = (w[i] < 0x80) ? (char)w[i] : '_';
		m->port_name[i] = 0;
	}
	ZwClose(key);
}

/* s (ASCII) appended to u */
static void append_ascii(UNICODE_STRING *u, const char *s)
{
	while (*s && u->Length + sizeof(WCHAR) <= u->MaximumLength) {
		u->Buffer[u->Length / sizeof(WCHAR)] = (WCHAR)*s++;
		u->Length += sizeof(WCHAR);
	}
}

/* n in decimal appended to u */
static void append_number(UNICODE_STRING *u, ULONG n)
{
	char d[12];
	int i = 11;
	d[i] = 0;
	do {
		d[--i] = (char)('0' + n % 10);
		n /= 10;
	} while (n && i > 0);
	append_ascii(u, d + i);
}

static void ustr_init(UNICODE_STRING *u, WCHAR *buf, USHORT size)
{
	u->Buffer = buf;
	u->Length = 0;
	u->MaximumLength = size;
}

/* the COM port's names for programs: \DosDevices\<name>, SERIALCOMM's
   value (the device's name: the COM port's); the modem interface; the
   engine's device */
static NTSTATUS start_names(struct dm_modem *m)
{
	UNICODE_STRING dev, name;
	WCHAR devbuf[64], namebuf[64];
	NTSTATUS st;
	struct dm_dev *e;

	read_port_name(m);
	dm_link_init(&m->link, &m->port, m->port_name);

	ustr_init(&dev, devbuf, sizeof(devbuf));
	append_ascii(&dev, "\\Device\\DModem");
	append_number(&dev, m->index);
	if (m->port_name[0]) {
		ustr_init(&m->link_name, m->link_buf, sizeof(m->link_buf));
		append_ascii(&m->link_name, "\\DosDevices\\");
		append_ascii(&m->link_name, m->port_name);
		if (!NT_SUCCESS(IoCreateSymbolicLink(&m->link_name, &dev)))
			m->link_name.Length = 0;

		ustr_init(&m->serialcomm_name, m->serialcomm_buf,
			  sizeof(m->serialcomm_buf));
		append_ascii(&m->serialcomm_name, "\\Device\\DModem");
		append_number(&m->serialcomm_name, m->index);
		ustr_init(&name, namebuf, sizeof(namebuf));
		append_ascii(&name, m->port_name);
		namebuf[name.Length / sizeof(WCHAR)] = 0;
		m->serialcomm_buf[m->serialcomm_name.Length / sizeof(WCHAR)] =
			0;
		if (!NT_SUCCESS(RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP,
				L"SERIALCOMM", m->serialcomm_buf, REG_SZ,
				namebuf, name.Length + sizeof(WCHAR))))
			m->serialcomm_name.Length = 0;
	}

	if (NT_SUCCESS(IoRegisterDeviceInterface(m->pdo, &modem_interface,
						 NULL, &m->iface)))
		IoSetDeviceInterfaceState(&m->iface, TRUE);

	/* the engine's device, and its link (a new one: no handles) */
	m->engine_opens = 0;
	ustr_init(&name, namebuf, sizeof(namebuf));
	append_ascii(&name, "\\Device\\" DMODEM_ENGINE_NAME);
	append_number(&name, m->index);
	st = IoCreateDevice(m->self->DriverObject, sizeof(struct dm_dev),
			    &name, FILE_DEVICE_UNKNOWN, 0, TRUE, &m->engine);
	if (!NT_SUCCESS(st)) {
		m->engine = NULL;
		return st;
	}
	e = m->engine->DeviceExtension;
	e->kind = KIND_ENGINE;
	e->m = m;
	m->engine->Flags |= DO_BUFFERED_IO;
	m->engine->Flags &= ~DO_DEVICE_INITIALIZING;
	ustr_init(&m->engine_link, m->engine_link_buf,
		  sizeof(m->engine_link_buf));
	append_ascii(&m->engine_link, "\\DosDevices\\" DMODEM_ENGINE_NAME);
	append_number(&m->engine_link, m->index);
	if (!NT_SUCCESS(IoCreateSymbolicLink(&m->engine_link, &name)))
		m->engine_link.Length = 0;
	return STATUS_SUCCESS;
}

/* start_names undone; the engine's device gone */
static void stop_names(struct dm_modem *m)
{
	if (m->iface.Buffer) {
		IoSetDeviceInterfaceState(&m->iface, FALSE);
		RtlFreeUnicodeString(&m->iface);
		m->iface.Buffer = NULL;
	}
	if (m->link_name.Length) {
		IoDeleteSymbolicLink(&m->link_name);
		m->link_name.Length = 0;
	}
	if (m->serialcomm_name.Length) {
		RtlDeleteRegistryValue(RTL_REGISTRY_DEVICEMAP, L"SERIALCOMM",
				       m->serialcomm_buf);
		m->serialcomm_name.Length = 0;
	}
	if (m->engine_link.Length) {
		IoDeleteSymbolicLink(&m->engine_link);
		m->engine_link.Length = 0;
	}
	if (m->engine) {
		((struct dm_dev *)m->engine->DeviceExtension)->m = NULL;
		IoDeleteDevice(m->engine);
		m->engine = NULL;
	}
}

/* everything the port and the engine have waiting completed: the app's
   requests cancelled, the engine detached */
static void stop_port(struct dm_modem *m)
{
	struct dm_batch b;
	KIRQL irql;
	PIRP wr;
	KeAcquireSpinLock(&m->lock, &irql);
	dm_batch_init(&b, now_ms());
	if (m->port.open)
		dm_port_close(&m->port, &b);
	dm_link_detach(&m->link, &b);
	wr = m->wire_read;
	if (wr && IoSetCancelRoutine(wr, NULL) != NULL)
		m->wire_read = NULL;
	else
		wr = NULL;
	batch_end(m, &b, irql);
	KeCancelTimer(&m->timer);
	if (wr)
		complete(wr, STATUS_CANCELLED, 0);
}

/* ---- PnP and power: passed down */

static NTSTATUS pass_down(struct dm_modem *m, PIRP irp)
{
	IoSkipCurrentIrpStackLocation(irp);
	return IoCallDriver(m->lower, irp);
}

static NTSTATUS NTAPI start_done(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
	(void)dev;
	(void)irp;
	KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

/* irp down the stack, waited for */
static NTSTATUS send_down_wait(struct dm_modem *m, PIRP irp)
{
	KEVENT ev;
	NTSTATUS st;
	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(irp);
	IoSetCompletionRoutine(irp, start_done, &ev, TRUE, TRUE, TRUE);
	st = IoCallDriver(m->lower, irp);
	if (st == STATUS_PENDING) {
		KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		st = irp->IoStatus.Status;
	}
	return st;
}

static NTSTATUS pnp(struct dm_modem *m, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS st;

	switch (sl->MinorFunction) {
	case IRP_MN_START_DEVICE:
		st = send_down_wait(m, irp);
		if (NT_SUCCESS(st) && !m->started) {
			st = start_names(m);
			if (NT_SUCCESS(st))
				m->started = TRUE;
			else
				stop_names(m);
		}
		return complete(irp, st, 0);
	case IRP_MN_SURPRISE_REMOVAL:
	case IRP_MN_STOP_DEVICE:
		if (m->started) {
			m->started = FALSE;
			stop_port(m);
			stop_names(m);
		}
		irp->IoStatus.Status = STATUS_SUCCESS;
		return pass_down(m, irp);
	case IRP_MN_REMOVE_DEVICE: {
		PDEVICE_OBJECT lower = m->lower;
		m->gone = TRUE;
		if (m->started) {
			m->started = FALSE;
			stop_port(m);
			stop_names(m);
		}
		/* (this request is one of the outstanding ones) */
		io_leave(m);
		KeWaitForSingleObject(&m->io_none, Executive, KernelMode,
				      FALSE, NULL);
		irp->IoStatus.Status = STATUS_SUCCESS;
		IoSkipCurrentIrpStackLocation(irp);
		st = IoCallDriver(lower, irp);
		IoDetachDevice(lower);
		IoDeleteDevice(m->self);
		return st;
	}
	case IRP_MN_QUERY_STOP_DEVICE:
	case IRP_MN_QUERY_REMOVE_DEVICE:
	case IRP_MN_CANCEL_STOP_DEVICE:
	case IRP_MN_CANCEL_REMOVE_DEVICE:
		irp->IoStatus.Status = STATUS_SUCCESS;
		return pass_down(m, irp);
	default:
		return pass_down(m, irp);
	}
}

static NTSTATUS power(struct dm_modem *m, PIRP irp)
{
	PoStartNextPowerIrp(irp);
	IoSkipCurrentIrpStackLocation(irp);
	return PoCallDriver(m->lower, irp);
}

/* ---- the driver */

static NTSTATUS NTAPI dispatch(PDEVICE_OBJECT dev, PIRP irp)
{
	struct dm_dev *d = dev->DeviceExtension;
	struct dm_modem *m = d->m;
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS st;

	if (d->kind == KIND_USB)
		return dm_usb_dispatch(dev, irp);
	if (!m)
		return complete(irp, STATUS_DELETE_PENDING, 0);
	if (d->kind == KIND_ENGINE) {
		if (!io_enter(m))
			return complete(irp, STATUS_DELETE_PENDING, 0);
		st = engine_dispatch(m, irp);
		io_leave(m);
		return st;
	}
	if (!io_enter(m)) {
		if (sl->MajorFunction == IRP_MJ_POWER)
			PoStartNextPowerIrp(irp);
		return complete(irp, STATUS_DELETE_PENDING, 0);
	}
	switch (sl->MajorFunction) {
	case IRP_MJ_PNP:
		st = pnp(m, irp);
		if (sl->MinorFunction == IRP_MN_REMOVE_DEVICE)
			return st;      /* (left already, m gone) */
		break;
	case IRP_MJ_POWER:
		st = power(m, irp);
		break;
	case IRP_MJ_SYSTEM_CONTROL:
		st = pass_down(m, irp);
		break;
	default:
		st = port_dispatch(m, irp);
		break;
	}
	io_leave(m);
	return st;
}

static NTSTATUS NTAPI add_device(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
	PDEVICE_OBJECT dev;
	struct dm_modem *m;
	UNICODE_STRING name;
	WCHAR buf[64];
	ULONG index;
	NTSTATUS st;

	/* (a USB modem: usb.c's) */
	if (dm_usb_wanted(pdo))
		return dm_usb_add(drv, pdo);
	index = (ULONG)InterlockedIncrement(&next_index);
	ustr_init(&name, buf, sizeof(buf));
	append_ascii(&name, "\\Device\\DModem");
	append_number(&name, index);
	st = IoCreateDevice(drv, sizeof(struct dm_modem), &name,
			    FILE_DEVICE_SERIAL_PORT, FILE_DEVICE_SECURE_OPEN,
			    FALSE, &dev);
	if (!NT_SUCCESS(st))
		return st;
	m = dev->DeviceExtension;
	memset(m, 0, sizeof(*m));
	m->hdr.kind = KIND_PORT;
	m->hdr.m = m;
	m->self = dev;
	m->pdo = pdo;
	m->index = index;
	m->io = 1;              /* (the device's own, let go at removal) */
	KeInitializeEvent(&m->io_none, NotificationEvent, FALSE);
	KeInitializeSpinLock(&m->lock);
	KeInitializeTimer(&m->timer);
	KeInitializeDpc(&m->timer_dpc, timer_dpc, m);
	dm_port_init(&m->port);
	dm_link_init(&m->link, &m->port, "");

	m->lower = IoAttachDeviceToDeviceStack(dev, pdo);
	if (!m->lower) {
		IoDeleteDevice(dev);
		return STATUS_NO_SUCH_DEVICE;
	}
	/* (a filter's power flags are those of the device below it) */
	dev->Flags |= DO_BUFFERED_IO |
		(m->lower->Flags & (DO_POWER_PAGABLE | DO_POWER_INRUSH));
	dev->Flags &= ~DO_DEVICE_INITIALIZING;
	return STATUS_SUCCESS;
}

static VOID NTAPI unload(PDRIVER_OBJECT drv)
{
	(void)drv;
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING path)
{
	int i;
	(void)path;
	for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
		drv->MajorFunction[i] = dispatch;
	drv->DriverExtension->AddDevice = add_device;
	drv->DriverUnload = unload;
	return STATUS_SUCCESS;
}
