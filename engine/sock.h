/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 D-Modem Authors
 *
 *    sock.h - the modems' DTE ends: libdmodem talks to its DTE on a
 *             non-blocking stream socket (dmodem.h: on Windows a Winsock
 *             SOCKET, elsewhere a descriptor), so the engine makes a
 *             connected pair for each modem and keeps the other end.
 *             Winsock 2 as 98, ME, 2000 and XP have it: a listener on
 *             the loopback address for the pair, no newer calls.
 *
 */
#ifndef DM_ENGINE_SOCK_H
#define DM_ENGINE_SOCK_H

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dme {

#ifdef _WIN32

/* Winsock started (once) */
inline bool sock_init()
{
	static bool ok = false;
	if (!ok) {
		WSADATA w;
		ok = WSAStartup(MAKEWORD(2, 2), &w) == 0;
	}
	return ok;
}

inline void sock_close(int s)
{
	if (s >= 0)
		closesocket((SOCKET)s);
}

/* two connected non-blocking stream sockets in sv; false if not */
inline bool sock_pair(int sv[2])
{
	SOCKET l, a = INVALID_SOCKET, b = INVALID_SOCKET;
	struct sockaddr_in in;
	int len = sizeof(in), on = 1;
	u_long nb = 1;

	if (!sock_init())
		return false;
	l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (l == INVALID_SOCKET)
		return false;
	memset(&in, 0, sizeof(in));
	in.sin_family = AF_INET;
	in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(l, (struct sockaddr *)&in, sizeof(in)) ||
	    getsockname(l, (struct sockaddr *)&in, &len) || listen(l, 1))
		goto fail;
	a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (a == INVALID_SOCKET ||
	    connect(a, (struct sockaddr *)&in, sizeof(in)))
		goto fail;
	b = accept(l, nullptr, nullptr);
	if (b == INVALID_SOCKET)
		goto fail;
	closesocket(l);
	setsockopt(a, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
	setsockopt(b, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
	ioctlsocket(a, FIONBIO, &nb);
	ioctlsocket(b, FIONBIO, &nb);
	sv[0] = (int)a;
	sv[1] = (int)b;
	return true;
fail:
	closesocket(l);
	if (a != INVALID_SOCKET)
		closesocket(a);
	return false;
}

/* up to n bytes: how many, 0 if none now, -1 if the other end closed */
inline int sock_read(int s, void *buf, int n)
{
	int r = recv((SOCKET)s, (char *)buf, n, 0);
	if (r == SOCKET_ERROR)
		return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
	return r == 0 ? -1 : r;
}

/* up to n bytes written: how many (0 if none now), -1 on an error */
inline int sock_write(int s, const void *buf, int n)
{
	int r = send((SOCKET)s, (const char *)buf, n, 0);
	if (r == SOCKET_ERROR)
		return WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
	return r;
}

#else

inline void sock_close(int s)
{
	if (s >= 0)
		close(s);
}

inline bool sock_pair(int sv[2])
{
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
		return false;
	fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
	fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL) | O_NONBLOCK);
	return true;
}

inline int sock_read(int s, void *buf, int n)
{
	ssize_t r = read(s, buf, (size_t)n);
	if (r < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	return r == 0 ? -1 : (int)r;
}

inline int sock_write(int s, const void *buf, int n)
{
	ssize_t r = send(s, buf, (size_t)n, MSG_NOSIGNAL);
	if (r < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	return (int)r;
}

#endif

} // namespace dme

#endif
