/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    loopback.cpp - phase 1's line (loopback.h)
 *
 */

#include "loopback.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sock.h"

namespace dme {

const char Loopback::GREETING[] =
	"\r\nD-Modem loopback: what you type comes back.\r\n";

/* a ring each 6 s; after 10 unanswered, the caller's no answer is the
   caller's own (its S7) */
static const uint32_t RING_STEPS = 6000 / 20, MAX_RINGS = 10;
/* the exchange's dial tone for 3 s off hook (the modem waits for it,
   S6), then the answerer rings */
static const uint32_t DIAL_TONE_STEPS = 3000 / 20;

/* the dial tone (350 Hz and 440 Hz, as North America's, about -13
   dBm0 each) from sample at on, n of them */
static void dial_tone(int16_t *out, uint32_t at, int n)
{
	const double pi = 3.14159265358979323846;
	for (int i = 0; i < n; i++) {
		double t = (double)(at + (uint32_t)i) / 8000.0;
		out[i] = (int16_t)(2600.0 * (std::sin(2 * pi * 350 * t) +
					    std::sin(2 * pi * 440 * t)));
	}
}

Loopback::~Loopback()
{
	dmodem_destroy(ours);
	dmodem_destroy(far_);
	sock_close(far_dte[0]);
	sock_close(far_dte[1]);
}

bool Loopback::open(int dte)
{
	struct dmodem_config c = {};

	dmodem_init();
	c.name = "dmodem";
	c.dte_fd = dte;
	ours = dmodem_create(&c);
	if (!ours || !sock_pair(far_dte))
		return false;
	/* the answerer: no echo of commands, no result codes (its DTE is
	   the echo service, which reads only the call's data), answering
	   on the first ring */
	c.name = "answer";
	c.dte_fd = far_dte[0];
	c.init = "ATE0Q1S0=1\r";
	far_ = dmodem_create(&c);
	return far_ != nullptr;
}

void Loopback::hangup()
{
	if (ours_.offhook)
		dmodem_hangup(ours);
}

/* the answerer's DTE: the greeting once connected, then what it
   receives sent back */
void Loopback::echo_service()
{
	char buf[512];
	int n;

	if (far_st.link != DMODEM_ONLINE) {
		greeted = false;
		echo.clear();
		/* (whatever it said off line: nothing to echo) */
		while (sock_read(far_dte[1], buf, sizeof(buf)) > 0)
			;
		return;
	}
	if (!greeted) {
		echo = GREETING;
		greeted = true;
	}
	while ((n = sock_read(far_dte[1], buf, sizeof(buf))) > 0)
		echo.append(buf, (size_t)n);
	if (!echo.empty()) {
		n = sock_write(far_dte[1], echo.data(), (int)echo.size());
		if (n > 0)
			echo.erase(0, (size_t)n);
	}
}

void Loopback::step()
{
	int16_t from_ours[STEP], from_far[STEP];

	dmodem_poll_dte(ours);
	dmodem_poll_dte(far_);
	dmodem_status(ours, &ours_);
	dmodem_status(far_, &far_st);

	/* the call: ours off hook rings the answerer, which then answers;
	   ours on hook ends it */
	if (ours_.offhook && !far_st.offhook && off_steps < DIAL_TONE_STEPS) {
		off_steps++;
	} else if (ours_.offhook && !far_st.offhook) {
		if (!ringing || ++ring_steps >= RING_STEPS) {
			if (rings < MAX_RINGS)
				dmodem_ring(far_, !ringing, nullptr);
			rings += ringing ? 1 : 0;
			ringing = true;
			ring_steps = 0;
		}
	} else if (!ours_.offhook) {
		if (far_st.offhook)
			dmodem_hangup(far_);
	}

	dmodem_process(ours, to_ours, from_ours, STEP);
	dmodem_process(far_, to_far, from_far, STEP);
	/* the line: each one's audio to the other while both are off hook,
	   silence to both otherwise */
	bool call = ours_.offhook && far_st.offhook;
	for (int i = 0; i < STEP; i++) {
		to_ours[i] = call ? from_far[i] : 0;
		to_far[i] = call ? from_ours[i] : 0;
	}
	if (ours_.offhook && !far_st.offhook && off_steps < DIAL_TONE_STEPS)
		dial_tone(to_ours, off_steps * STEP, STEP);
	echo_service();
	/* (as the step left them, for online() and offhook()) */
	dmodem_status(ours, &ours_);
	dmodem_status(far_, &far_st);
	if (!ours_.offhook) {
		/* (the next call from the start: its dial tone, its rings) */
		ringing = false;
		rings = 0;
		off_steps = 0;
	}
	if (getenv("LB_TRACE"))
		fprintf(stderr, "lb: ours %d/%d far %d/%d off %u ring %d/%u\n",
			ours_.offhook, ours_.link, far_st.offhook, far_st.link,
			off_steps, ringing, rings);
}

} // namespace dme
