/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    loopback.h - phase 1's line: the modem (the one the COM port is,
 *                 "ours") and, at the other end of an in-process line,
 *                 an answering modem with an echo service.  ATD<anything>
 *                 on ours rings the answerer, which answers on the first
 *                 ring; the two run a real handshake (V.34, V.90's
 *                 analogue side, V.42 and V.42bis, as AT+MS and the
 *                 rest pick) and then the answerer sends a greeting and
 *                 echoes what it gets.  No SIP, no hardware: what
 *                 Unimodem, HyperTerminal and the driver can be tried on.
 *
 *    One step is 20 ms of both modems' audio; the caller paces the
 *    steps (in real time, or as fast as it likes in a test).
 *
 */
#ifndef DM_ENGINE_LOOPBACK_H
#define DM_ENGINE_LOOPBACK_H

#include <cstdint>
#include <string>

#include <dmodem.h>

namespace dme {

class Loopback {
public:
	enum { STEP = 160 };    /* samples a step: 20 ms at 8000 Hz */

	~Loopback();
	/* both modems; dte: our modem's DTE end (the caller keeps the
	   other end of its pair: what the COM port's app writes goes in
	   there, what the modem says comes out); false if they can't be */
	bool open(int dte);
	/* 20 ms of both modems */
	void step();

	/* our modem's state */
	bool online() const { return ours_.link == DMODEM_ONLINE; }
	bool offhook() const { return ours_.offhook != 0; }
	const struct dmodem_status &status() const { return ours_; }
	/* our modem hung up (the app's DTR gone, with AT&D2) */
	void hangup();

	/* what the answerer says first once connected */
	static const char GREETING[];

private:
	void echo_service();

	dmodem *ours = nullptr, *far_ = nullptr;
	int far_dte[2] = { -1, -1 };    /* [0] the answerer's, [1] ours */
	int16_t to_ours[STEP] = {}, to_far[STEP] = {};
	struct dmodem_status ours_ = {}, far_st = {};
	bool ringing = false;
	uint32_t ring_steps = 0, rings = 0;
	uint32_t off_steps = 0;         /* ours off hook, the far end not */
	bool greeted = false;
	std::string echo;               /* for the answerer's DTE */
};

} // namespace dme

#endif
