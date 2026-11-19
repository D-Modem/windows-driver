/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    loopback_test.cpp - the engine's phase 1 line (engine/loopback.h)
 *                        on this machine: ATD on our modem rings the
 *                        answerer, the two connect, the greeting comes,
 *                        what we type comes back, and ATH hangs up.  As
 *                        fast as the machine goes (the modems' clock is
 *                        their samples).
 *
 */

#include <cstdio>
#include <cstring>
#include <string>

#include "loopback.h"
#include "sock.h"

#include "test.h"

static dme::Loopback *lb;
static int app;                 /* the app's end of our modem's DTE */
static std::string seen;        /* what our modem said */

/* steps until what our modem said has want (from), or ms of its clock
   pass: true if it came */
static bool until(const char *want, size_t from, int ms)
{
	char buf[512];
	for (int t = 0; t < ms; t += 20) {
		int n;
		lb->step();
		while ((n = dme::sock_read(app, buf, sizeof(buf))) > 0)
			seen.append(buf, (size_t)n);
		if (seen.find(want, from) != std::string::npos)
			return true;
	}
	fprintf(stderr, "  (waited for \"%s\"; the modem said: \"%s\")\n",
		want, seen.substr(from).c_str());
	return false;
}

static void say(const char *s)
{
	CHECK_EQ(dme::sock_write(app, s, (int)strlen(s)), (int)strlen(s));
}

TEST(dial_echo_hangup)
{
	int dte[2];
	size_t at;

	CHECK(dme::sock_pair(dte));
	app = dte[1];
	lb = new dme::Loopback;
	CHECK(lb->open(dte[0]));

	/* what Unimodem sends first (inf/dmodem.inf's Init, its settings'
	   tone dialling and X4) */
	say("AT\r");
	CHECK(until("OK", 0, 1000));
	at = seen.size();
	say("AT&FE0V1&C1&D2S0=0\r");
	CHECK(until("OK", at, 1000));
	at = seen.size();
	say("ATX4\r");
	CHECK(until("OK", at, 1000));

	at = seen.size();
	say("ATDT5551234\r");
	CHECK(until("CONNECT", at, 60000));
	CHECK(lb->online());
	CHECK(until("what you type comes back", at, 10000));
	fprintf(stderr, "  connected: %u/%u bit/s\n",
		(unsigned)lb->status().tx_rate, (unsigned)lb->status().rx_rate);

	at = seen.size();
	say("hello, 2000\r");
	CHECK(until("hello, 2000", at, 10000));

	/* the app's DTR gone (AT&D2): on hook, the answerer too */
	at = seen.size();
	lb->hangup();
	CHECK(until("NO CARRIER", at, 10000));
	CHECK(!lb->offhook());

	/* and again: the answerer is ready for the next call */
	at = seen.size();
	say("ATD1\r");
	CHECK(until("CONNECT", at, 60000));

	delete lb;
	dme::sock_close(dte[1]);
}

int main(void)
{
	RUN(dial_echo_hangup);
	return DONE();
}
