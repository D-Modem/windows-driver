/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    dmodem_svc.cpp - dmodem-svc.exe, D-Modem's engine for dmodem.sys on
 *                     Windows 98 SE, ME, 2000 and XP: the modem (libdmodem)
 *                     behind the driver's COM port, through the engine
 *                     channel (\\.\DModemEngine<n>, dmodem_ioctl.h).
 *
 *    Phase 1: the line is the loopback (loopback.h): ATD<anything> on the
 *    COM port reaches an answering modem in this process, with an echo
 *    service.
 *
 *    usage: dmodem-svc [-d LEVEL] [N]
 *           dmodem-svc [-d LEVEL] --selftest
 *        N           the modem's number (\\.\DModemEngine<N>; default:
 *                    the first there is)
 *        -d          libdmodem's log level (0 errors only, the default)
 *        --selftest  no driver: a loopback call (Unimodem's commands, a
 *                    dial, the greeting, an echo, a hang-up) as fast as
 *                    the machine goes, and how much of the 20 ms of each
 *                    step the modems took: under 100% the machine keeps
 *                    up with a call (both modems: a real one is half)
 *
 *    One thread.  Every 20 ms: the driver's frames (a WIRE_READ kept in
 *    flight, overlapped), the modem's DTE both ways, 20 ms of the line.
 *    The driver's device not there (not installed, not started, or gone):
 *    tried again every second.  A console program for now; the service
 *    (NT) and RunServices (9x) come later.
 *
 *    Only what Windows 98 and 2000 have: ANSI calls, no CancelIoEx, the
 *    time from timeGetTime (GetTickCount ticks by 55 ms on 9x).
 *
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>

extern "C" {
#include "dm_port.h"
#include "dm_wire.h"
}
#include "dmodem_ioctl.h"
#include "loopback.h"
#include "sock.h"

namespace {

volatile LONG quit;

BOOL WINAPI on_ctrl(DWORD)
{
	InterlockedExchange(&quit, 1);
	return TRUE;
}

/* the engine device n's path */
std::string engine_path(unsigned n)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "\\\\.\\" DMODEM_ENGINE_NAME "%u", n);
	return buf;
}

/* the driver's device: n's (want >= 0) or the first that opens */
HANDLE open_engine(int want, unsigned *which)
{
	for (unsigned n = want >= 0 ? (unsigned)want : 0;
	     n < (want >= 0 ? (unsigned)want + 1 : 8u); n++) {
		HANDLE h = CreateFileA(engine_path(n).c_str(),
				       GENERIC_READ | GENERIC_WRITE, 0,
				       nullptr, OPEN_EXISTING,
				       FILE_FLAG_OVERLAPPED, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			*which = n;
			return h;
		}
	}
	return INVALID_HANDLE_VALUE;
}

/* an IOCTL on the overlapped handle, waited for: its bytes, or -1 */
long ioctl_wait(HANDLE h, DWORD code, void *in, DWORD inlen, void *out,
		DWORD outlen, HANDLE ev)
{
	OVERLAPPED ov = {};
	DWORD got = 0;
	ov.hEvent = ev;
	ResetEvent(ev);
	if (!DeviceIoControl(h, code, in, inlen, out, outlen, &got, &ov)) {
		if (GetLastError() != ERROR_IO_PENDING)
			return -1;
		if (!GetOverlappedResult(h, &ov, &got, TRUE))
			return -1;
	}
	return (long)got;
}

/* one session on the driver's device, until it fails or we quit */
class Session {
public:
	Session(HANDLE h, dme::Loopback &lb, int app)
		: h_(h), lb_(lb), app_(app) {}
	~Session();
	/* the session; false once the driver's channel fails */
	bool run();

private:
	bool start_read();
	bool finish_read(bool *done);
	void take();
	bool write_out();

	HANDLE h_;
	dme::Loopback &lb_;
	int app_;                       /* our end of the modem's DTE */
	HANDLE rev_ = nullptr, wev_ = nullptr;
	OVERLAPPED rov_ = {};
	bool reading_ = false;
	dm_u8 rbuf_[2 * (DM_WIRE_HEAD + DM_WIRE_MAX_DATA)];
	std::string in_;                /* the driver's bytes, not yet frames */
	std::string to_modem_;          /* the app's characters for the modem */
	std::string out_;               /* our frames for the driver */
	int lines_ = -1;                /* the LINES last sent */
	bool dtr_ = false;              /* the app's DTR, port open */
};

Session::~Session()
{
	if (reading_) {
		DWORD n;
		CancelIo(h_);   /* (this thread's: 98 and 2000 have no Ex) */
		GetOverlappedResult(h_, &rov_, &n, TRUE);
	}
	if (rev_)
		CloseHandle(rev_);
	if (wev_)
		CloseHandle(wev_);
}

bool Session::start_read()
{
	DWORD got = 0;
	memset(&rov_, 0, sizeof(rov_));
	rov_.hEvent = rev_;
	ResetEvent(rev_);
	if (!DeviceIoControl(h_, IOCTL_DMODEM_WIRE_READ, nullptr, 0, rbuf_,
			     sizeof(rbuf_), &got, &rov_) &&
	    GetLastError() != ERROR_IO_PENDING)
		return false;
	reading_ = true;
	return true;
}

/* the WIRE_READ done (*done) and taken, or still pending */
bool Session::finish_read(bool *done)
{
	DWORD got = 0;
	*done = false;
	if (WaitForSingleObject(rev_, 0) != WAIT_OBJECT_0)
		return true;
	reading_ = false;
	if (!GetOverlappedResult(h_, &rov_, &got, FALSE))
		return false;
	*done = true;
	in_.append((const char *)rbuf_, got);
	take();
	return true;
}

/* the driver's frames in in_: HELLO checked, STATE's DTR followed,
   DATA for the modem */
void Session::take()
{
	struct dm_frame f;
	int len;
	while ((len = dm_wire_next((const dm_u8 *)in_.data(),
				   (dm_u32)in_.size(), &f)) > 0) {
		if (f.type == DM_FRAME_HELLO) {
			dm_u16 v = 0;
			char port[32] = "";
			dm_wire_get_hello(&f, &v, port, sizeof(port));
			fprintf(stderr, "dmodem-svc: the driver's port is %s "
				"(channel version %u)\n", port, (unsigned)v);
		} else if (f.type == DM_FRAME_STATE) {
			dm_u8 st;
			dm_u32 baud;
			if (dm_wire_get_state(&f, &st, &baud)) {
				bool dtr = (st & DM_STATE_OPEN) &&
					(st & DM_STATE_DTR);
				/* (DTR dropped: AT&D2's hang-up, as Unimodem
				   ends a call) */
				if (dtr_ && !dtr)
					lb_.hangup();
				dtr_ = dtr;
			}
		} else if (f.type == DM_FRAME_DATA) {
			to_modem_.append((const char *)f.p, f.n);
		}
		in_.erase(0, (size_t)len);
	}
	if (len < 0)
		in_.clear();    /* (can't be: the driver's frames are ours) */
}

/* our frames to the driver as far as it takes them */
bool Session::write_out()
{
	if (out_.empty())
		return true;
	long k = ioctl_wait(h_, IOCTL_DMODEM_WIRE_WRITE, &out_[0],
			    (DWORD)out_.size(), nullptr, 0, wev_);
	if (k < 0)
		return false;
	out_.erase(0, (size_t)k);
	return true;
}

bool Session::run()
{
	dm_u8 tmp[DM_WIRE_HEAD + 64];
	char buf[DM_WIRE_MAX_DATA];
	DWORD next;

	rev_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	wev_ = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	if (!rev_ || !wev_)
		return false;
	/* our HELLO first */
	out_.append((const char *)tmp, dm_wire_hello(tmp, sizeof(tmp), ""));

	next = timeGetTime();
	while (!quit) {
		bool done;
		int n;

		if (!reading_ && !start_read())
			return false;
		if (!finish_read(&done))
			return false;

		/* the app's characters to the modem */
		if (!to_modem_.empty()) {
			n = dme::sock_write(app_, to_modem_.data(),
					    (int)to_modem_.size());
			if (n > 0)
				to_modem_.erase(0, (size_t)n);
		}

		lb_.step();

		/* the modem's characters and lines to the app; while the
		   driver holds back (its port full) no more than a frame
		   or two wait here */
		while (out_.size() < 2 * sizeof(buf) &&
		       (n = dme::sock_read(app_, buf, sizeof(buf))) > 0) {
			dm_u8 head[DM_WIRE_HEAD];
			head[0] = DM_FRAME_DATA;
			head[1] = (dm_u8)(n & 0xff);
			head[2] = (dm_u8)(n >> 8);
			out_.append((const char *)head, sizeof(head));
			out_.append(buf, (size_t)n);
		}
		int lines = DM_LINE_DSR | DM_LINE_CTS |
			(lb_.online() ? DM_LINE_DCD : 0);
		if (lines != lines_) {
			out_.append((const char *)tmp,
				    dm_wire_lines(tmp, sizeof(tmp),
						  (dm_u8)lines));
			lines_ = lines;
		}
		if (!write_out())
			return false;

		/* 20 ms a step, caught up if late */
		next += 20;
		DWORD now = timeGetTime();
		if ((LONG)(next - now) > 0)
			Sleep(next - now);
		else if ((LONG)(now - next) > 1000)
			next = now;     /* (asleep, suspended: not all of it) */
	}
	return true;
}

void usage()
{
	fprintf(stderr, "usage: dmodem-svc [-d LEVEL] [N | --selftest]\n");
	exit(2);
}

/* --selftest's DTE: steps until what the modem said has want (from
   at), or ms of the modems' clock pass */
struct SelfTest {
	dme::Loopback lb;
	int app = -1;
	std::string seen;
	DWORD busy = 0, worst = 0, steps = 0;

	bool until(const char *want, size_t at, int ms) {
		char buf[512];
		for (int t = 0; t < ms; t += 20) {
			int n;
			DWORD t0 = timeGetTime();
			lb.step();
			DWORD d = timeGetTime() - t0;
			busy += d;
			worst = d > worst ? d : worst;
			steps++;
			while ((n = dme::sock_read(app, buf, sizeof(buf))) > 0)
				seen.append(buf, (size_t)n);
			if (seen.find(want, at) != std::string::npos)
				return true;
		}
		fprintf(stderr, "selftest: no \"%s\"; the modem said \"%s\"\n",
			want, seen.substr(at).c_str());
		return false;
	}
	bool say(const char *cmd, const char *want, int ms) {
		size_t at = seen.size();
		if (dme::sock_write(app, cmd, (int)strlen(cmd)) !=
		    (int)strlen(cmd))
			return false;
		return until(want, at, ms);
	}
};

int selftest()
{
	SelfTest t;
	int dte[2];
	bool ok;

	if (!dme::sock_pair(dte) || !t.lb.open(dte[0])) {
		fprintf(stderr, "selftest: no modem (Winsock 2?)\n");
		return 1;
	}
	t.app = dte[1];
	ok = t.say("AT&FE0V1&C1&D2S0=0\r", "OK", 1000) &&
		t.say("ATX4\r", "OK", 1000) &&
		t.say("ATDT5551234\r", "CONNECT", 60000) &&
		t.until(dme::Loopback::GREETING + 2, 0, 10000) &&
		t.say("hello\r", "hello", 10000);
	if (ok)
		fprintf(stderr, "selftest: connected at %u/%u bit/s, the echo "
			"came back\n", (unsigned)t.lb.status().tx_rate,
			(unsigned)t.lb.status().rx_rate);
	t.lb.hangup();
	ok = ok && t.until("NO CARRIER", t.seen.size(), 10000);
	if (t.steps)
		fprintf(stderr, "selftest: %lu steps of 20 ms, the modems took "
			"%lu%% of it on average, %lu ms at worst\n",
			(unsigned long)t.steps,
			(unsigned long)(t.busy * 100 / (t.steps * 20)),
			(unsigned long)t.worst);
	fprintf(stderr, "selftest: %s\n", ok ? "passed" : "FAILED");
	dme::sock_close(dte[0]);
	dme::sock_close(dte[1]);
	return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
	int want = -1, level = 0;
	bool waiting = false, self = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			level = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--selftest"))
			self = true;
		else if (argv[i][0] >= '0' && argv[i][0] <= '9')
			want = atoi(argv[i]);
		else
			usage();
	}
	dmodem_set_debug(level);
	timeBeginPeriod(1);
	if (self) {
		int r = selftest();
		timeEndPeriod(1);
		return r;
	}
	SetConsoleCtrlHandler(on_ctrl, TRUE);
	/* (the line's 20 ms must not wait for the rest of the machine) */
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	while (!quit) {
		unsigned which = 0;
		HANDLE h = open_engine(want, &which);
		if (h == INVALID_HANDLE_VALUE) {
			if (!waiting)
				fprintf(stderr, "dmodem-svc: waiting for "
					"dmodem.sys (\\\\.\\%s%s)\n",
					DMODEM_ENGINE_NAME,
					want >= 0 ? std::to_string(want).c_str()
						  : "<n>");
			waiting = true;
			Sleep(1000);
			continue;
		}
		waiting = false;

		/* what the modem is */
		{
			struct dmodem_info info = {};
			HANDLE ev = CreateEventA(nullptr, TRUE, FALSE,
						 nullptr);
			if (ev && ioctl_wait(h, IOCTL_DMODEM_INFO, nullptr, 0,
					     &info, sizeof(info), ev) ==
			    (long)sizeof(info))
				fprintf(stderr, "dmodem-svc: modem %u: %s, "
					"driver %s\n", which,
					info.port[0] ? info.port : "(no port)",
					info.driver);
			if (ev)
				CloseHandle(ev);
		}

		/* a new modem for each session: the call and its state
		   go with the driver's */
		int dte[2];
		if (!dme::sock_pair(dte)) {
			fprintf(stderr, "dmodem-svc: no sockets (Winsock 2)\n");
			CloseHandle(h);
			return 1;
		}
		{
			dme::Loopback lb;
			if (!lb.open(dte[0])) {
				fprintf(stderr, "dmodem-svc: no modem\n");
				CloseHandle(h);
				return 1;
			}
			Session s(h, lb, dte[1]);
			if (!s.run())
				fprintf(stderr, "dmodem-svc: the driver's "
					"channel failed (error %lu): again\n",
					GetLastError());
		}
		dme::sock_close(dte[0]);
		dme::sock_close(dte[1]);
		CloseHandle(h);
		if (!quit)
			Sleep(1000);
	}
	timeEndPeriod(1);
	return 0;
}
