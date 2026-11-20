/*
 * Shared helpers: clocks and the static-initializer guard.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. See COPYING.LIB.
 */

#include "internal.h"

SRWLOCK ptw_static_init_lock = SRWLOCK_INIT;

/* 100ns intervals between 1601-01-01 and 1970-01-01. */
#define PTW_EPOCH_DELTA 116444736000000000ULL

unsigned __int64 ptw_now_100ns(void)
{
	FILETIME ft;
	ULARGE_INTEGER u;

	GetSystemTimePreciseAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	return u.QuadPart - PTW_EPOCH_DELTA;
}

DWORD ptw_abstime_to_ms(const struct timespec *abstime)
{
	unsigned __int64 now, deadline, delta_ms;

	if (abstime == NULL) {
		return INFINITE;
	}

	if (abstime->tv_sec < 0) {
		return 0;
	}

	deadline = (unsigned __int64)abstime->tv_sec * 10000000ULL + (unsigned __int64)(abstime->tv_nsec / 100);
	now = ptw_now_100ns();
	if (deadline <= now) {
		return 0;
	}

	delta_ms = (deadline - now + 9999ULL) / 10000ULL;
	/* INFINITE is reserved; clamp just below it. */
	if (delta_ms >= (unsigned __int64)INFINITE) {
		return INFINITE - 1;
	}
	return (DWORD)delta_ms;
}
