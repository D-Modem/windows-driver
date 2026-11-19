/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    usb.c - dmodem.sys as a USB modem's function driver: the modem's
 *            transfers passed through to D-Modem's engine (dmodemd and
 *            its usb_win9x.cpp, libusb-1.0's calls), which drives the
 *            modem as it does through libusb on Linux.  The IOCTLs are
 *            in dmodem_usb.h; their checks and tables in common/dm_usb.c.
 *
 *    The modem's devnode (USB\VID_xxxx&PID_xxxx, inf/dmusb.inf) has this
 *    driver as its function driver, above the USB stack's PDO.  Its
 *    device is \Device\DModemUsb<n>, as \DosDevices\DModemUsb<n> for
 *    the engine (\\.\DModemUsb<n>), with D-Modem's USB interface.
 *
 *    Each transfer is an URB in an IRP of its own, sent down while the
 *    engine's IOCTL waits (pending, cancelable); the URB's buffer is the
 *    IOCTL's own system buffer.  A transfer is cancelled (IoCancelIrp of
 *    its IRP) for the engine's CANCEL, its IOCTL's cancellation and its
 *    timeout; a timer (every TICK_MS while one is wanted) tries again
 *    until the USB stack gives the IRP back.
 *
 *    Only the kernel API that Windows 2000 and 98 SE's NTKERN both have,
 *    and no USBD.SYS function (98 and 98 SE export some only by
 *    decorated names): the URBs are made here.
 *
 */

#include "dm_nt.h"

#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>

#include "dm_usb.h"

/* D-Modem's USB interface: {68D47B44-D8E5-45A1-8BE0-74A69D7490BF} */
static const GUID usb_interface = { 0x68d47b44, 0xd8e5, 0x45a1,
	{ 0x8b, 0xe0, 0x74, 0xa6, 0x9d, 0x74, 0x90, 0xbf } };

#define TICK_MS 20
#define MAX_PIPES 32
/* the most octets of one transfer on each pipe (the USB stack of 98
   and 2000 wants a size; 4 KB is theirs by default) */
#define PIPE_MAX_TRANSFER 4096

struct pipe {
	USBD_PIPE_HANDLE h;
	dm_u8 ep, type, itf;
};

struct dm_usb {
	struct dm_dev hdr;
	PDEVICE_OBJECT self, lower, pdo;
	ULONG index;

	/* the transfers in flight, the timer */
	KSPIN_LOCK lock;
	LIST_ENTRY xfers;
	KTIMER timer;
	KDPC timer_dpc;
	BOOLEAN timer_on;

	/* the requests being dispatched and the transfers in flight, for
	   removal to wait on */
	LONG io;
	KEVENT io_none;
	BOOLEAN started, gone;
	LONG owner;             /* the owner's file object */

	/* the configuration: changed and read with cfg_mutex held */
	KEVENT cfg_mutex;
	USBD_CONFIGURATION_HANDLE config;
	dm_u8 config_value;
	dm_u8 device_desc[18];
	dm_u8 *cfg;             /* its descriptor, cfg_len octets */
	dm_u32 cfg_len;
	struct pipe pipes[MAX_PIPES];
	dm_u32 npipes;

	WCHAR link_buf[64];
	UNICODE_STRING link;    /* \DosDevices\DModemUsb<n>, if made */
	UNICODE_STRING iface;
};

/* a transfer: the engine's IOCTL (irp), the IRP of its URB (sub) */
struct xfer {
	LIST_ENTRY entry;
	struct dm_usb *u;
	PIRP irp, sub;
	struct dmodem_usb_xfer *x;      /* in irp's system buffer */
	LONG refs;
	dm_u32 tag;
	dm_u64 deadline;        /* ms, 0: none */
	BOOLEAN cancel, timed_out;
	URB urb;                /* (an isochronous one is longer) */
};

/* the numbers in use (\\.\DModemUsb<n>) */
static LONG slots;

/* ---- small things */

static dm_u64 now_ms(void)
{
	return (dm_u64)KeQueryInterruptTime() / 10000;
}

static int io_enter(struct dm_usb *u)
{
	InterlockedIncrement(&u->io);
	if (u->gone) {
		if (InterlockedDecrement(&u->io) == 0)
			KeSetEvent(&u->io_none, IO_NO_INCREMENT, FALSE);
		return 0;
	}
	return 1;
}

static void io_leave(struct dm_usb *u)
{
	if (InterlockedDecrement(&u->io) == 0)
		KeSetEvent(&u->io_none, IO_NO_INCREMENT, FALSE);
}

static void cfg_lock(struct dm_usb *u)
{
	KeWaitForSingleObject(&u->cfg_mutex, Executive, KernelMode, FALSE,
			      NULL);
}

static void cfg_unlock(struct dm_usb *u)
{
	KeSetEvent(&u->cfg_mutex, IO_NO_INCREMENT, FALSE);
}

static ULONG slot_take(void)
{
	LONG old, i;
	for (;;) {
		old = slots;
		for (i = 0; i < (LONG)DMODEM_USB_MAX; i++)
			if (!(old & (1L << i)))
				break;
		if (i == (LONG)DMODEM_USB_MAX)
			return DMODEM_USB_MAX;
		if (InterlockedCompareExchange(&slots, old | (1L << i), old) ==
		    old)
			return (ULONG)i;
	}
}

static void slot_give(ULONG i)
{
	LONG old;
	if (i >= DMODEM_USB_MAX)
		return;
	do
		old = slots;
	while (InterlockedCompareExchange(&slots, old & ~(1L << i), old) !=
	       old);
}

static void append_ascii(UNICODE_STRING *u, const char *s)
{
	while (*s && u->Length + sizeof(WCHAR) <= u->MaximumLength) {
		u->Buffer[u->Length / sizeof(WCHAR)] = (WCHAR)*s++;
		u->Length += sizeof(WCHAR);
	}
}

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

/* ---- URBs sent and waited for (PASSIVE_LEVEL) */

static NTSTATUS urb_wait(struct dm_usb *u, URB *urb)
{
	KEVENT ev;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	NTSTATUS st;

	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB,
					    u->lower, NULL, 0, NULL, 0, TRUE,
					    &ev, &iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = urb;
	st = IoCallDriver(u->lower, irp);
	if (st == STATUS_PENDING) {
		KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		st = iosb.Status;
	}
	if (NT_SUCCESS(st) && !USBD_SUCCESS(urb->UrbHeader.Status))
		st = STATUS_UNSUCCESSFUL;
	return st;
}

static NTSTATUS get_descriptor(struct dm_usb *u, dm_u8 type, dm_u8 index,
			       void *buf, ULONG len, ULONG *got)
{
	struct _URB_CONTROL_DESCRIPTOR_REQUEST urb;
	NTSTATUS st;
	memset(&urb, 0, sizeof(urb));
	urb.Hdr.Length = sizeof(urb);
	urb.Hdr.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
	urb.TransferBuffer = buf;
	urb.TransferBufferLength = len;
	urb.Index = index;
	urb.DescriptorType = type;
	st = urb_wait(u, (URB *)&urb);
	*got = urb.TransferBufferLength;
	return st;
}

/* each pipe of an interface's information into the table (its old ones
   out first) */
static void pipes_take(struct dm_usb *u, USBD_INTERFACE_INFORMATION *ii)
{
	dm_u32 i, j = 0;
	for (i = 0; i < u->npipes; i++)
		if (u->pipes[i].itf != ii->InterfaceNumber)
			u->pipes[j++] = u->pipes[i];
	u->npipes = j;
	for (i = 0; i < ii->NumberOfPipes && u->npipes < MAX_PIPES; i++) {
		struct pipe *p = &u->pipes[u->npipes++];
		p->h = ii->Pipes[i].PipeHandle;
		p->ep = ii->Pipes[i].EndpointAddress;
		p->type = (dm_u8)ii->Pipes[i].PipeType;
		p->itf = ii->InterfaceNumber;
	}
}

/* every pipe's transfers aborted (the default pipe's excepted) */
static void pipes_abort(struct dm_usb *u)
{
	struct _URB_PIPE_REQUEST urb;
	dm_u32 i;
	cfg_lock(u);
	for (i = 0; i < u->npipes; i++) {
		memset(&urb, 0, sizeof(urb));
		urb.Hdr.Length = sizeof(urb);
		urb.Hdr.Function = URB_FUNCTION_ABORT_PIPE;
		urb.PipeHandle = u->pipes[i].h;
		urb_wait(u, (URB *)&urb);
	}
	cfg_unlock(u);
}

/* configuration value (0: unconfigured) selected, its descriptor read
   again, every interface at alternate setting 0; cfg_mutex held */
static NTSTATUS select_config(struct dm_usb *u, dm_u8 value)
{
	struct _URB_SELECT_CONFIGURATION *urb;
	USB_CONFIGURATION_DESCRIPTOR head;
	USBD_INTERFACE_INFORMATION *ii;
	dm_u8 *cfg = NULL, itf, alt, nep;
	ULONG got, size, len = 0;
	dm_u32 pos = 0, i;
	NTSTATUS st;
	int at;

	u->npipes = 0;
	u->config = NULL;
	u->config_value = 0;
	if (!value) {
		struct _URB_SELECT_CONFIGURATION none;
		memset(&none, 0, sizeof(none));
		none.Hdr.Length = sizeof(none);
		none.Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
		return urb_wait(u, (URB *)&none);
	}
	/* the configuration's descriptor: which index has value */
	for (i = 0; i < u->device_desc[17]; i++) {
		st = get_descriptor(u, USB_CONFIGURATION_DESCRIPTOR_TYPE,
				    (dm_u8)i, &head, sizeof(head), &got);
		if (!NT_SUCCESS(st))
			return st;
		if (got >= sizeof(head) && head.bConfigurationValue == value)
			break;
	}
	if (i == u->device_desc[17])
		return STATUS_INVALID_PARAMETER;
	len = head.wTotalLength;
	if (len < sizeof(head) || len > DMODEM_USB_CONFIG_MAX)
		return STATUS_DEVICE_DATA_ERROR;
	cfg = ExAllocatePoolWithTag(NonPagedPool, len, DM_TAG);
	if (!cfg)
		return STATUS_INSUFFICIENT_RESOURCES;
	st = get_descriptor(u, USB_CONFIGURATION_DESCRIPTOR_TYPE, (dm_u8)i,
			    cfg, len, &got);
	if (NT_SUCCESS(st) && (got != len || dm_usb_cfg_check(cfg, len)))
		st = STATUS_DEVICE_DATA_ERROR;
	if (!NT_SUCCESS(st)) {
		ExFreePool(cfg);
		return st;
	}
	if (u->cfg)
		ExFreePool(u->cfg);
	u->cfg = cfg;
	u->cfg_len = len;

	/* the request: each interface's alternate setting 0 */
	size = sizeof(*urb) - sizeof(USBD_INTERFACE_INFORMATION);
	while (dm_usb_cfg_next_alt(cfg, len, &pos, &itf, &alt, &nep) >= 0)
		if (alt == 0)
			size += GET_USBD_INTERFACE_SIZE(nep);
	if (size < sizeof(*urb))
		size = sizeof(*urb);
	urb = ExAllocatePoolWithTag(NonPagedPool, size, DM_TAG);
	if (!urb)
		return STATUS_INSUFFICIENT_RESOURCES;
	memset(urb, 0, size);
	urb->Hdr.Length = (USHORT)size;
	urb->Hdr.Function = URB_FUNCTION_SELECT_CONFIGURATION;
	urb->ConfigurationDescriptor = (PUSB_CONFIGURATION_DESCRIPTOR)cfg;
	ii = &urb->Interface;
	pos = 0;
	while ((at = dm_usb_cfg_next_alt(cfg, len, &pos, &itf, &alt,
					 &nep)) >= 0) {
		if (alt != 0)
			continue;
		ii->Length = (USHORT)GET_USBD_INTERFACE_SIZE(nep);
		ii->InterfaceNumber = itf;
		ii->AlternateSetting = 0;
		ii->NumberOfPipes = nep;
		for (i = 0; i < nep; i++)
			ii->Pipes[i].MaximumTransferSize = PIPE_MAX_TRANSFER;
		ii = (USBD_INTERFACE_INFORMATION *)((dm_u8 *)ii + ii->Length);
	}
	st = urb_wait(u, (URB *)urb);
	if (NT_SUCCESS(st)) {
		u->config = urb->ConfigurationHandle;
		u->config_value = value;
		ii = &urb->Interface;
		while ((dm_u8 *)ii < (dm_u8 *)urb + size && ii->Length) {
			pipes_take(u, ii);
			ii = (USBD_INTERFACE_INFORMATION *)((dm_u8 *)ii +
							    ii->Length);
		}
	}
	ExFreePool(urb);
	return st;
}

/* interface itf at alternate setting alt; cfg_mutex held */
static NTSTATUS select_alt(struct dm_usb *u, dm_u32 itf, dm_u32 alt)
{
	struct _URB_SELECT_INTERFACE *urb;
	dm_u8 nep;
	ULONG size, i;
	NTSTATUS st;

	if (!u->config || itf > 255 || alt > 255 ||
	    dm_usb_cfg_find(u->cfg, u->cfg_len, (dm_u8)itf, (dm_u8)alt,
			    &nep) < 0)
		return STATUS_INVALID_PARAMETER;
	size = sizeof(*urb) - sizeof(USBD_INTERFACE_INFORMATION) +
		GET_USBD_INTERFACE_SIZE(nep);
	if (size < sizeof(*urb))
		size = sizeof(*urb);
	urb = ExAllocatePoolWithTag(NonPagedPool, size, DM_TAG);
	if (!urb)
		return STATUS_INSUFFICIENT_RESOURCES;
	memset(urb, 0, size);
	urb->Hdr.Length = (USHORT)size;
	urb->Hdr.Function = URB_FUNCTION_SELECT_INTERFACE;
	urb->ConfigurationHandle = u->config;
	urb->Interface.Length = (USHORT)GET_USBD_INTERFACE_SIZE(nep);
	urb->Interface.InterfaceNumber = (UCHAR)itf;
	urb->Interface.AlternateSetting = (UCHAR)alt;
	urb->Interface.NumberOfPipes = nep;
	for (i = 0; i < nep; i++)
		urb->Interface.Pipes[i].MaximumTransferSize =
			PIPE_MAX_TRANSFER;
	st = urb_wait(u, (URB *)urb);
	if (NT_SUCCESS(st))
		pipes_take(u, &urb->Interface);
	ExFreePool(urb);
	return st;
}

static NTSTATUS reset_pipe(struct dm_usb *u, dm_u32 ep)
{
	struct _URB_PIPE_REQUEST urb;
	dm_u32 i;
	for (i = 0; i < u->npipes; i++)
		if (u->pipes[i].ep == ep)
			break;
	if (i == u->npipes)
		return STATUS_INVALID_PARAMETER;
	memset(&urb, 0, sizeof(urb));
	urb.Hdr.Length = sizeof(urb);
	urb.Hdr.Function = URB_FUNCTION_RESET_PIPE;
	urb.PipeHandle = u->pipes[i].h;
	return urb_wait(u, (URB *)&urb);
}

/* ---- the transfers */

static void xfer_put(struct xfer *t)
{
	struct dm_usb *u = t->u;
	if (InterlockedDecrement(&t->refs) != 0)
		return;
	IoFreeIrp(t->sub);
	ExFreePool(t);
	io_leave(u);
}

/* the timer on, if it is not (lock held): it holds an io of its own
   while it is on */
static void timer_want(struct dm_usb *u)
{
	LARGE_INTEGER due;
	if (u->timer_on)
		return;
	u->timer_on = TRUE;
	InterlockedIncrement(&u->io);
	due.QuadPart = -(LONGLONG)TICK_MS * 10000;
	KeSetTimer(&u->timer, due, &u->timer_dpc);
}

/* the cancellations the transfers want, and their timeouts */
static VOID NTAPI timer_dpc(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
	struct dm_usb *u = ctx;
	struct xfer *todo[16];
	LIST_ENTRY *e;
	dm_u64 now = now_ms();
	BOOLEAN again = FALSE;
	LARGE_INTEGER due;
	int n = 0, i;
	(void)dpc;
	(void)a1;
	(void)a2;

	KeAcquireSpinLockAtDpcLevel(&u->lock);
	for (e = u->xfers.Flink; e != &u->xfers; e = e->Flink) {
		struct xfer *t = CONTAINING_RECORD(e, struct xfer, entry);
		if (!t->cancel && t->deadline && now >= t->deadline) {
			t->cancel = TRUE;
			t->timed_out = TRUE;
		}
		if (t->cancel || t->deadline)
			again = TRUE;
		if (t->cancel && n < 16) {
			InterlockedIncrement(&t->refs);
			todo[n++] = t;
		}
	}
	if (again) {
		due.QuadPart = -(LONGLONG)TICK_MS * 10000;
		KeSetTimer(&u->timer, due, &u->timer_dpc);
	} else {
		u->timer_on = FALSE;
	}
	KeReleaseSpinLockFromDpcLevel(&u->lock);
	for (i = 0; i < n; i++) {
		IoCancelIrp(todo[i]->sub);
		xfer_put(todo[i]);
	}
	if (!again)
		io_leave(u);
}

/* the engine's IOCTL cancelled: its transfer's too */
static VOID NTAPI xfer_irp_cancel(PDEVICE_OBJECT dev, PIRP irp)
{
	struct xfer *t = irp->Tail.Overlay.DriverContext[0];
	struct dm_usb *u = t->u;
	KIRQL irql;
	(void)dev;
	IoReleaseCancelSpinLock(irp->CancelIrql);
	KeAcquireSpinLock(&u->lock, &irql);
	t->cancel = TRUE;
	timer_want(u);
	KeReleaseSpinLock(&u->lock, irql);
	IoCancelIrp(t->sub);
	xfer_put(t);
}

/* the URB's IRP back: the result into the engine's buffer, its IOCTL
   completed */
static NTSTATUS NTAPI xfer_done(PDEVICE_OBJECT dev, PIRP sub, PVOID ctx)
{
	struct xfer *t = ctx;
	struct dm_usb *u = t->u;
	struct dmodem_usb_xfer *x = t->x;
	struct dmodem_usb_packet *p = (struct dmodem_usb_packet *)(x + 1);
	PIRP irp = t->irp;
	dm_u32 usbd = (dm_u32)t->urb.UrbHeader.Status, i;
	KIRQL irql, cirql;
	(void)dev;

	KeAcquireSpinLock(&u->lock, &irql);
	RemoveEntryList(&t->entry);
	KeReleaseSpinLock(&u->lock, irql);

	/* (an IRP that failed with a URB that says nothing) */
	if (!NT_SUCCESS(sub->IoStatus.Status) && !DM_USBD_IS_ERROR(usbd))
		usbd = sub->IoStatus.Status == STATUS_CANCELLED ?
			DM_USBD_CANCELED : 0xc0000000u;
	x->usbd = usbd;
	x->status = dm_usb_status(usbd, t->timed_out);
	if (x->type == DMODEM_USB_XFER_ISO) {
		struct _URB_ISOCH_TRANSFER *iso = &t->urb.UrbIsochronousTransfer;
		BOOLEAN in = (x->endpoint & 0x80) != 0;
		x->actual = 0;
		for (i = 0; i < x->npackets; i++) {
			dm_u32 ps = (dm_u32)iso->IsoPacket[i].Status;
			p[i].status = dm_usb_status(ps, t->timed_out);
			if (p[i].status != DMODEM_USB_COMPLETED)
				p[i].actual = 0;
			else if (in)
				p[i].actual = iso->IsoPacket[i].Length;
			else
				p[i].actual = p[i].length;
			x->actual += p[i].actual;
		}
		/* (libusb's: an isochronous transfer is done whatever its
		   packets' results, unless it was cancelled or the modem
		   went) */
		if (x->status != DMODEM_USB_CANCELLED &&
		    x->status != DMODEM_USB_TIMED_OUT &&
		    x->status != DMODEM_USB_NO_DEVICE)
			x->status = DMODEM_USB_COMPLETED;
	} else if (x->type == DMODEM_USB_XFER_CONTROL) {
		x->actual = t->urb.UrbControlTransfer.TransferBufferLength;
	} else {
		x->actual = t->urb.UrbBulkOrInterruptTransfer.TransferBufferLength;
	}
	if (x->actual > x->length)
		x->actual = x->length;

	/* the IOCTL's cancel routine out of the way: if it is on its way,
	   it has the cancel spin lock until it took what it needs */
	if (IoSetCancelRoutine(irp, NULL) != NULL) {
		xfer_put(t);    /* (its reference) */
	} else {
		IoAcquireCancelSpinLock(&cirql);
		IoReleaseCancelSpinLock(cirql);
	}
	irp->Tail.Overlay.DriverContext[0] = NULL;
	irp->IoStatus.Status = STATUS_SUCCESS;
	irp->IoStatus.Information =
		DMODEM_USB_XFER_SIZE(x->npackets, x->length);
	IoCompleteRequest(irp, IO_NO_INCREMENT);
	xfer_put(t);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS xfer_start(struct dm_usb *u, PIRP irp, ULONG inlen,
			   ULONG outlen)
{
	struct dmodem_usb_xfer *x = irp->AssociatedIrp.SystemBuffer;
	struct dmodem_usb_packet *p;
	struct xfer *t;
	PIO_STACK_LOCATION next;
	USBD_PIPE_HANDLE pipe = NULL;
	dm_u8 *data;
	dm_u16 function = 0;
	int kind = 0;
	BOOLEAN in;
	ULONG size, i, off;
	KIRQL irql;

	if (!x || dm_usb_check(x, inlen, outlen))
		return dm_complete(irp, STATUS_INVALID_PARAMETER, 0);
	x->status = DMODEM_USB_ERROR;
	x->usbd = 0;
	x->actual = 0;
	p = (struct dmodem_usb_packet *)(x + 1);
	data = (dm_u8 *)(p + x->npackets);
	for (i = 0; i < x->npackets; i++)
		p[i].actual = p[i].status = 0;
	if (x->type == DMODEM_USB_XFER_CONTROL) {
		kind = dm_usb_control(x->setup, &function);
		if (!kind)
			return dm_complete(irp, STATUS_INVALID_PARAMETER, 0);
		in = (x->setup[0] & 0x80) != 0;
	} else {
		cfg_lock(u);
		for (i = 0; i < u->npipes; i++)
			if (u->pipes[i].ep == x->endpoint &&
			    u->pipes[i].type == x->type)
				pipe = u->pipes[i].h;
		cfg_unlock(u);
		if (!pipe)
			return dm_complete(irp, STATUS_INVALID_PARAMETER, 0);
		in = (x->endpoint & 0x80) != 0;
	}

	size = sizeof(URB);
	if (x->type == DMODEM_USB_XFER_ISO &&
	    GET_ISO_URB_SIZE(x->npackets) > size)
		size = GET_ISO_URB_SIZE(x->npackets);
	t = ExAllocatePoolWithTag(NonPagedPool,
				  FIELD_OFFSET(struct xfer, urb) + size,
				  DM_TAG);
	if (!t)
		return dm_complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	memset(t, 0, FIELD_OFFSET(struct xfer, urb) + size);
	t->sub = IoAllocateIrp(u->lower->StackSize, FALSE);
	if (!t->sub) {
		ExFreePool(t);
		return dm_complete(irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	}
	t->u = u;
	t->irp = irp;
	t->x = x;
	t->tag = x->tag;
	t->refs = 2;            /* (the URB's, the cancel routine's) */
	if (x->timeout_ms)
		t->deadline = now_ms() + x->timeout_ms;

	switch (x->type) {
	case DMODEM_USB_XFER_CONTROL: {
		dm_u16 value = (dm_u16)(x->setup[2] | x->setup[3] << 8);
		dm_u16 index = (dm_u16)(x->setup[4] | x->setup[5] << 8);
		void *buf = x->length ? data : NULL;
		ULONG flags = in ? USBD_TRANSFER_DIRECTION_IN |
			USBD_SHORT_TRANSFER_OK : 0;
		if (kind == DM_CTL_VENDOR_CLASS) {
			struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *r =
				&t->urb.UrbControlVendorClassRequest;
			r->Hdr.Length = sizeof(*r);
			r->Hdr.Function = function;
			r->TransferFlags = flags;
			r->TransferBuffer = buf;
			r->TransferBufferLength = x->length;
			r->Request = x->setup[1];
			r->Value = value;
			r->Index = index;
		} else if (kind == DM_CTL_DESCRIPTOR) {
			struct _URB_CONTROL_DESCRIPTOR_REQUEST *r =
				&t->urb.UrbControlDescriptorRequest;
			r->Hdr.Length = sizeof(*r);
			r->Hdr.Function = function;
			r->TransferBuffer = buf;
			r->TransferBufferLength = x->length;
			r->Index = (UCHAR)value;
			r->DescriptorType = (UCHAR)(value >> 8);
			r->LanguageId = index;
		} else if (kind == DM_CTL_FEATURE) {
			struct _URB_CONTROL_FEATURE_REQUEST *r =
				&t->urb.UrbControlFeatureRequest;
			r->Hdr.Length = sizeof(*r);
			r->Hdr.Function = function;
			r->FeatureSelector = value;
			r->Index = index;
		} else {
			struct _URB_CONTROL_GET_STATUS_REQUEST *r =
				&t->urb.UrbControlGetStatusRequest;
			r->Hdr.Length = sizeof(*r);
			r->Hdr.Function = function;
			r->TransferBuffer = buf;
			r->TransferBufferLength = x->length;
			r->Index = index;
		}
		break;
	}
	case DMODEM_USB_XFER_BULK:
	case DMODEM_USB_XFER_INTERRUPT: {
		struct _URB_BULK_OR_INTERRUPT_TRANSFER *r =
			&t->urb.UrbBulkOrInterruptTransfer;
		r->Hdr.Length = sizeof(*r);
		r->Hdr.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
		r->PipeHandle = pipe;
		r->TransferFlags = in ? USBD_TRANSFER_DIRECTION_IN |
			USBD_SHORT_TRANSFER_OK : 0;
		r->TransferBuffer = x->length ? data : NULL;
		r->TransferBufferLength = x->length;
		break;
	}
	default: {      /* DMODEM_USB_XFER_ISO */
		struct _URB_ISOCH_TRANSFER *r = &t->urb.UrbIsochronousTransfer;
		r->Hdr.Length = (USHORT)GET_ISO_URB_SIZE(x->npackets);
		r->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
		r->PipeHandle = pipe;
		r->TransferFlags = USBD_START_ISO_TRANSFER_ASAP |
			(in ? USBD_TRANSFER_DIRECTION_IN : 0);
		r->TransferBuffer = data;
		r->TransferBufferLength = x->length;
		r->NumberOfPackets = x->npackets;
		for (i = 0, off = 0; i < x->npackets; i++) {
			r->IsoPacket[i].Offset = off;
			off += p[i].length;
		}
		break;
	}
	}

	next = IoGetNextIrpStackLocation(t->sub);
	next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
	next->Parameters.DeviceIoControl.IoControlCode =
		IOCTL_INTERNAL_USB_SUBMIT_URB;
	next->Parameters.Others.Argument1 = &t->urb;
	IoSetCompletionRoutine(t->sub, xfer_done, t, TRUE, TRUE, TRUE);

	/* in the list, the IOCTL pending and cancelable; one io for the
	   transfer until its last reference goes */
	InterlockedIncrement(&u->io);
	irp->Tail.Overlay.DriverContext[0] = t;
	IoMarkIrpPending(irp);
	KeAcquireSpinLock(&u->lock, &irql);
	InsertTailList(&u->xfers, &t->entry);
	if (t->deadline)
		timer_want(u);
	KeReleaseSpinLock(&u->lock, irql);
	IoSetCancelRoutine(irp, xfer_irp_cancel);
	if (irp->Cancel && IoSetCancelRoutine(irp, NULL) != NULL) {
		/* (cancelled already: the URB goes down all the same, and
		   is cancelled at once) */
		KeAcquireSpinLock(&u->lock, &irql);
		t->cancel = TRUE;
		timer_want(u);
		KeReleaseSpinLock(&u->lock, irql);
		InterlockedDecrement(&t->refs); /* (the cancel routine's) */
	}
	IoCallDriver(u->lower, t->sub);
	return STATUS_PENDING;
}

/* every transfer cancelled (tag: only that one's, if one) */
static int xfers_cancel(struct dm_usb *u, BOOLEAN all, dm_u32 tag)
{
	struct xfer *todo[16];
	LIST_ENTRY *e;
	KIRQL irql;
	int n = 0, found = 0, i;

	KeAcquireSpinLock(&u->lock, &irql);
	for (e = u->xfers.Flink; e != &u->xfers; e = e->Flink) {
		struct xfer *t = CONTAINING_RECORD(e, struct xfer, entry);
		if (!all && t->tag != tag)
			continue;
		found++;
		t->cancel = TRUE;
		if (n < 16) {
			InterlockedIncrement(&t->refs);
			todo[n++] = t;
		}
	}
	if (found)
		timer_want(u);
	KeReleaseSpinLock(&u->lock, irql);
	for (i = 0; i < n; i++) {
		IoCancelIrp(todo[i]->sub);
		xfer_put(todo[i]);
	}
	return found;
}

/* ---- the engine's requests */

static NTSTATUS info(struct dm_usb *u, PIRP irp, ULONG outlen)
{
	struct dmodem_usb_info *i = irp->AssociatedIrp.SystemBuffer;
	ULONG need;
	cfg_lock(u);
	need = sizeof(*i) + sizeof(u->device_desc) + u->cfg_len;
	if (outlen < need) {
		cfg_unlock(u);
		return dm_complete(irp, STATUS_BUFFER_TOO_SMALL, 0);
	}
	memset(i, 0, sizeof(*i));
	i->version = DMODEM_USB_VERSION;
	i->index = u->index;
	i->config = u->config_value;
	/* (98 SE and ME have no USB 2.0 of their own, and 2000's and XP's
	   USB stack doesn't say before XP SP1: full speed for a USB 1.x
	   device, else not known) */
	i->speed = (u->device_desc[2] | u->device_desc[3] << 8) < 0x0200 ?
		DMODEM_USB_SPEED_FULL : DMODEM_USB_SPEED_UNKNOWN;
	i->device_len = sizeof(u->device_desc);
	i->config_len = u->cfg_len;
	i->owned = u->owner != 0;
	memcpy(i + 1, u->device_desc, sizeof(u->device_desc));
	if (u->cfg_len)
		memcpy((dm_u8 *)(i + 1) + sizeof(u->device_desc), u->cfg,
		       u->cfg_len);
	cfg_unlock(u);
	return dm_complete(irp, STATUS_SUCCESS, need);
}

/* the caller owns the modem (it does now, if no one did) */
static int owns(struct dm_usb *u, PIRP irp)
{
	LONG fo = (LONG)(ULONG_PTR)IoGetCurrentIrpStackLocation(irp)->FileObject;
	LONG was = InterlockedCompareExchange(&u->owner, fo, 0);
	return was == 0 || was == fo;
}

static NTSTATUS ioctl(struct dm_usb *u, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	ULONG code = sl->Parameters.DeviceIoControl.IoControlCode;
	ULONG inlen = sl->Parameters.DeviceIoControl.InputBufferLength;
	ULONG outlen = sl->Parameters.DeviceIoControl.OutputBufferLength;
	dm_u32 *in = irp->AssociatedIrp.SystemBuffer;
	NTSTATUS st;

	if (!u->started)
		return dm_complete(irp, STATUS_DEVICE_NOT_READY, 0);
	if (code == IOCTL_DMODEM_USB_INFO)
		return info(u, irp, outlen);
	switch (code) {
	case IOCTL_DMODEM_USB_TRANSFER:
	case IOCTL_DMODEM_USB_CANCEL:
	case IOCTL_DMODEM_USB_SET_CONFIG:
	case IOCTL_DMODEM_USB_SET_ALT:
	case IOCTL_DMODEM_USB_RESET_PIPE:
		break;
	default:
		return dm_complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	}
	if (!owns(u, irp))
		return dm_complete(irp, STATUS_DEVICE_BUSY, 0);
	switch (code) {
	case IOCTL_DMODEM_USB_TRANSFER:
		return xfer_start(u, irp, inlen, outlen);
	case IOCTL_DMODEM_USB_CANCEL:
		if (inlen < 4)
			return dm_complete(irp, STATUS_INVALID_PARAMETER, 0);
		return dm_complete(irp, xfers_cancel(u, FALSE, in[0]) ?
				   STATUS_SUCCESS : STATUS_NOT_FOUND, 0);
	case IOCTL_DMODEM_USB_SET_CONFIG:
		if (inlen < 4 || in[0] > 255)
			return dm_complete(irp, STATUS_INVALID_PARAMETER, 0);
		cfg_lock(u);
		st = select_config(u, (dm_u8)in[0]);
		cfg_unlock(u);
		return dm_complete(irp, st, 0);
	case IOCTL_DMODEM_USB_SET_ALT:
		if (inlen < 8)
			return dm_complete(irp, STATUS_INVALID_PARAMETER, 0);
		cfg_lock(u);
		st = select_alt(u, in[0], in[1]);
		cfg_unlock(u);
		return dm_complete(irp, st, 0);
	default:        /* IOCTL_DMODEM_USB_RESET_PIPE */
		if (inlen < 4)
			return dm_complete(irp, STATUS_INVALID_PARAMETER, 0);
		cfg_lock(u);
		st = reset_pipe(u, in[0]);
		cfg_unlock(u);
		return dm_complete(irp, st, 0);
	}
}

/* the owner's handle closing: its transfers aborted, the modem free */
static void cleanup(struct dm_usb *u, PIRP irp)
{
	LONG fo = (LONG)(ULONG_PTR)IoGetCurrentIrpStackLocation(irp)->FileObject;
	if (u->owner != fo)
		return;
	xfers_cancel(u, TRUE, 0);
	pipes_abort(u);
	InterlockedCompareExchange(&u->owner, 0, fo);
}

/* ---- PnP */

static NTSTATUS NTAPI sent_back(PDEVICE_OBJECT dev, PIRP irp, PVOID ctx)
{
	(void)dev;
	(void)irp;
	KeSetEvent((PKEVENT)ctx, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS send_down_wait(struct dm_usb *u, PIRP irp)
{
	KEVENT ev;
	NTSTATUS st;
	KeInitializeEvent(&ev, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(irp);
	IoSetCompletionRoutine(irp, sent_back, &ev, TRUE, TRUE, TRUE);
	st = IoCallDriver(u->lower, irp);
	if (st == STATUS_PENDING) {
		KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
		st = irp->IoStatus.Status;
	}
	return st;
}

static NTSTATUS pass_down(struct dm_usb *u, PIRP irp)
{
	IoSkipCurrentIrpStackLocation(irp);
	return IoCallDriver(u->lower, irp);
}

/* the device descriptor, the first configuration, the names */
static NTSTATUS start(struct dm_usb *u)
{
	UNICODE_STRING dev;
	WCHAR devbuf[64];
	ULONG got;
	NTSTATUS st;

	st = get_descriptor(u, USB_DEVICE_DESCRIPTOR_TYPE, 0, u->device_desc,
			    sizeof(u->device_desc), &got);
	if (NT_SUCCESS(st) && (got != sizeof(u->device_desc) ||
			       u->device_desc[1] != 1 ||
			       u->device_desc[17] == 0))
		st = STATUS_DEVICE_DATA_ERROR;
	if (!NT_SUCCESS(st))
		return st;
	cfg_lock(u);
	{
		USB_CONFIGURATION_DESCRIPTOR head;
		st = get_descriptor(u, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0,
				    &head, sizeof(head), &got);
		if (NT_SUCCESS(st) && got < sizeof(head))
			st = STATUS_DEVICE_DATA_ERROR;
		if (NT_SUCCESS(st))
			st = select_config(u, head.bConfigurationValue);
	}
	cfg_unlock(u);
	if (!NT_SUCCESS(st))
		return st;

	ustr_init(&dev, devbuf, sizeof(devbuf));
	append_ascii(&dev, "\\Device\\" DMODEM_USB_NAME);
	append_number(&dev, u->index);
	ustr_init(&u->link, u->link_buf, sizeof(u->link_buf));
	append_ascii(&u->link, "\\DosDevices\\" DMODEM_USB_NAME);
	append_number(&u->link, u->index);
	if (!NT_SUCCESS(IoCreateSymbolicLink(&u->link, &dev)))
		u->link.Length = 0;
	if (NT_SUCCESS(IoRegisterDeviceInterface(u->pdo, &usb_interface, NULL,
						 &u->iface)))
		IoSetDeviceInterfaceState(&u->iface, TRUE);
	else
		u->iface.Buffer = NULL;
	return STATUS_SUCCESS;
}

/* the transfers stopped, the names gone (the modem stopped or gone) */
static void stop(struct dm_usb *u)
{
	xfers_cancel(u, TRUE, 0);
	pipes_abort(u);
	if (u->iface.Buffer) {
		IoSetDeviceInterfaceState(&u->iface, FALSE);
		RtlFreeUnicodeString(&u->iface);
		u->iface.Buffer = NULL;
	}
	if (u->link.Length) {
		IoDeleteSymbolicLink(&u->link);
		u->link.Length = 0;
	}
}

static NTSTATUS pnp(struct dm_usb *u, PIRP irp)
{
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS st;

	switch (sl->MinorFunction) {
	case IRP_MN_START_DEVICE:
		st = send_down_wait(u, irp);
		if (NT_SUCCESS(st) && !u->started) {
			st = start(u);
			if (NT_SUCCESS(st))
				u->started = TRUE;
		}
		return dm_complete(irp, st, 0);
	case IRP_MN_QUERY_CAPABILITIES:
		/* (a modem pulled out is no surprise to us) */
		st = send_down_wait(u, irp);
		if (NT_SUCCESS(st))
			sl->Parameters.DeviceCapabilities.Capabilities->
				SurpriseRemovalOK = TRUE;
		return dm_complete(irp, st, irp->IoStatus.Information);
	case IRP_MN_SURPRISE_REMOVAL:
	case IRP_MN_STOP_DEVICE:
		if (u->started) {
			u->started = FALSE;
			stop(u);
		}
		irp->IoStatus.Status = STATUS_SUCCESS;
		return pass_down(u, irp);
	case IRP_MN_REMOVE_DEVICE: {
		PDEVICE_OBJECT lower = u->lower;
		if (u->started) {
			u->started = FALSE;
			stop(u);
		}
		u->gone = TRUE;
		xfers_cancel(u, TRUE, 0);
		/* (this request is one of the outstanding ones) */
		io_leave(u);
		KeWaitForSingleObject(&u->io_none, Executive, KernelMode,
				      FALSE, NULL);
		irp->IoStatus.Status = STATUS_SUCCESS;
		IoSkipCurrentIrpStackLocation(irp);
		st = IoCallDriver(lower, irp);
		IoDetachDevice(lower);
		if (u->cfg)
			ExFreePool(u->cfg);
		slot_give(u->index);
		IoDeleteDevice(u->self);
		return st;
	}
	case IRP_MN_QUERY_STOP_DEVICE:
	case IRP_MN_QUERY_REMOVE_DEVICE:
	case IRP_MN_CANCEL_STOP_DEVICE:
	case IRP_MN_CANCEL_REMOVE_DEVICE:
		irp->IoStatus.Status = STATUS_SUCCESS;
		return pass_down(u, irp);
	default:
		return pass_down(u, irp);
	}
}

/* ---- the driver's entry points (dm_nt.h) */

NTSTATUS dm_usb_dispatch(PDEVICE_OBJECT dev, PIRP irp)
{
	struct dm_usb *u = dev->DeviceExtension;
	PIO_STACK_LOCATION sl = IoGetCurrentIrpStackLocation(irp);
	NTSTATUS st;

	if (!io_enter(u)) {
		if (sl->MajorFunction == IRP_MJ_POWER)
			PoStartNextPowerIrp(irp);
		return dm_complete(irp, STATUS_DELETE_PENDING, 0);
	}
	switch (sl->MajorFunction) {
	case IRP_MJ_PNP:
		st = pnp(u, irp);
		if (sl->MinorFunction == IRP_MN_REMOVE_DEVICE)
			return st;      /* (left already, u gone) */
		break;
	case IRP_MJ_POWER:
		PoStartNextPowerIrp(irp);
		IoSkipCurrentIrpStackLocation(irp);
		st = PoCallDriver(u->lower, irp);
		break;
	case IRP_MJ_SYSTEM_CONTROL:
		st = pass_down(u, irp);
		break;
	case IRP_MJ_CREATE:
		st = dm_complete(irp, u->started ? STATUS_SUCCESS :
				 STATUS_DEVICE_NOT_READY, 0);
		break;
	case IRP_MJ_CLEANUP:
		cleanup(u, irp);
		st = dm_complete(irp, STATUS_SUCCESS, 0);
		break;
	case IRP_MJ_CLOSE:
		st = dm_complete(irp, STATUS_SUCCESS, 0);
		break;
	case IRP_MJ_DEVICE_CONTROL:
		st = ioctl(u, irp);
		break;
	default:
		st = dm_complete(irp, STATUS_INVALID_DEVICE_REQUEST, 0);
		break;
	}
	io_leave(u);
	return st;
}

/* the value DModemUsb of the devnode's driver key (0 if none) */
static ULONG driver_key_flag(PDEVICE_OBJECT pdo)
{
	HANDLE key;
	UNICODE_STRING v;
	union {
		KEY_VALUE_PARTIAL_INFORMATION i;
		UCHAR b[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 8];
	} r;
	ULONG len, flag = 0;

	if (!NT_SUCCESS(IoOpenDeviceRegistryKey(pdo, PLUGPLAY_REGKEY_DRIVER,
						KEY_READ, &key)))
		return 0;
	RtlInitUnicodeString(&v, L"DModemUsb");
	if (NT_SUCCESS(ZwQueryValueKey(key, &v, KeyValuePartialInformation,
				       &r.i, sizeof(r), &len)) &&
	    r.i.DataLength >= 1)
		flag = r.i.Data[0];
	ZwClose(key);
	return flag;
}

BOOLEAN dm_usb_wanted(PDEVICE_OBJECT pdo)
{
	WCHAR name[8];
	ULONG len = 0;
	if (NT_SUCCESS(IoGetDeviceProperty(pdo, DevicePropertyEnumeratorName,
					   sizeof(name), name, &len)) &&
	    len == 4 * sizeof(WCHAR) && (name[0] | 0x20) == 'u' &&
	    (name[1] | 0x20) == 's' && (name[2] | 0x20) == 'b' && !name[3])
		return TRUE;
	return driver_key_flag(pdo) != 0;
}

NTSTATUS dm_usb_add(PDRIVER_OBJECT drv, PDEVICE_OBJECT pdo)
{
	PDEVICE_OBJECT dev;
	struct dm_usb *u;
	UNICODE_STRING name;
	WCHAR buf[64];
	ULONG index = slot_take();
	NTSTATUS st;

	if (index >= DMODEM_USB_MAX)
		return STATUS_INSUFFICIENT_RESOURCES;
	ustr_init(&name, buf, sizeof(buf));
	append_ascii(&name, "\\Device\\" DMODEM_USB_NAME);
	append_number(&name, index);
	st = IoCreateDevice(drv, sizeof(struct dm_usb), &name,
			    FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
			    FALSE, &dev);
	if (!NT_SUCCESS(st)) {
		slot_give(index);
		return st;
	}
	u = dev->DeviceExtension;
	memset(u, 0, sizeof(*u));
	u->hdr.kind = KIND_USB;
	u->self = dev;
	u->pdo = pdo;
	u->index = index;
	u->io = 1;              /* (the device's own, let go at removal) */
	KeInitializeEvent(&u->io_none, NotificationEvent, FALSE);
	KeInitializeEvent(&u->cfg_mutex, SynchronizationEvent, TRUE);
	KeInitializeSpinLock(&u->lock);
	InitializeListHead(&u->xfers);
	KeInitializeTimer(&u->timer);
	KeInitializeDpc(&u->timer_dpc, timer_dpc, u);

	u->lower = IoAttachDeviceToDeviceStack(dev, pdo);
	if (!u->lower) {
		IoDeleteDevice(dev);
		slot_give(index);
		return STATUS_NO_SUCH_DEVICE;
	}
	dev->Flags |= DO_BUFFERED_IO | DO_POWER_PAGABLE;
	dev->Flags &= ~DO_DEVICE_INITIALIZING;
	return STATUS_SUCCESS;
}
