/*
 * sched_* functions, non-portable extensions and the DLL entry point.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. See COPYING.LIB.
 */

#include "internal.h"

/* ---------------------------------------------------------------------------
 * sched.h
 * ------------------------------------------------------------------------ */

int __cdecl sched_yield(void)
{
	Sleep(0);
	return 0;
}

int __cdecl sched_get_priority_min(int policy)
{
	if (policy < SCHED_MIN || policy > SCHED_MAX) {
		errno = EINVAL;
		return -1;
	}
	return THREAD_PRIORITY_IDLE;
}

int __cdecl sched_get_priority_max(int policy)
{
	if (policy < SCHED_MIN || policy > SCHED_MAX) {
		errno = EINVAL;
		return -1;
	}
	return THREAD_PRIORITY_TIME_CRITICAL;
}

static int check_pid(pid_t pid, DWORD access)
{
	HANDLE h;

	if (pid == 0 || (DWORD)pid == GetCurrentProcessId()) {
		return 0;
	}

	h = OpenProcess(access, FALSE, (DWORD)pid);
	if (!h) {
		errno = GetLastError() == ERROR_ACCESS_DENIED ? EPERM : ESRCH;
		return -1;
	}
	CloseHandle(h);
	return 0;
}

int __cdecl sched_setscheduler(pid_t pid, int policy)
{
	if (check_pid(pid, PROCESS_SET_INFORMATION) != 0) {
		return -1;
	}
	if (policy != SCHED_OTHER) {
		errno = ENOSYS;
		return -1;
	}
	return SCHED_OTHER;
}

int __cdecl sched_getscheduler(pid_t pid)
{
	if (check_pid(pid, PROCESS_QUERY_INFORMATION) != 0) {
		return -1;
	}
	return SCHED_OTHER;
}

/* ---------------------------------------------------------------------------
 * Concurrency level (advisory only, as before)
 * ------------------------------------------------------------------------ */

static volatile LONG concurrency = 0;

int PTW32_CDECL pthread_setconcurrency(int level)
{
	if (level < 0) {
		return EINVAL;
	}
	InterlockedExchange(&concurrency, level);
	return 0;
}

int PTW32_CDECL pthread_getconcurrency(void)
{
	return (int)ptw_load_long_acquire(&concurrency);
}

/* ---------------------------------------------------------------------------
 * Non-portable extensions
 * ------------------------------------------------------------------------ */

int PTW32_CDECL pthread_num_processors_np(void)
{
	DWORD_PTR process_mask, system_mask;
	int count = 0;

	if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
		while (process_mask) {
			count += (int)(process_mask & 1);
			process_mask >>= 1;
		}
	}
	return count > 0 ? count : 1;
}

int PTW32_CDECL pthread_win32_test_features_np(int feature_mask)
{
	/* Interlocked compare-exchange is always available; blocked threads
	 * cannot be cancelled asynchronously. */
	return (feature_mask & PTW32_SYSTEM_INTERLOCKED_COMPARE_EXCHANGE) != 0;
}

/* Only meaningful for SEH-based cleanup builds; the C-cleanup build OBS
 * shipped returned 0, so do the same. */
DWORD PTW32_CDECL ptw32_get_exception_services_code(void)
{
	return 0;
}

/* ---------------------------------------------------------------------------
 * Process and thread lifecycle
 * ------------------------------------------------------------------------ */

int PTW32_CDECL pthread_win32_process_attach_np(void)
{
	return TRUE;
}

int PTW32_CDECL pthread_win32_process_detach_np(void)
{
	ptw_thread_detach();
	return TRUE;
}

int PTW32_CDECL pthread_win32_thread_attach_np(void)
{
	return TRUE;
}

int PTW32_CDECL pthread_win32_thread_detach_np(void)
{
	ptw_thread_detach();
	return TRUE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
	(void)instance;

	switch (reason) {
	case DLL_PROCESS_ATTACH:
		/* Everything initializes lazily. Thread notifications must stay
		 * enabled: DLL_THREAD_DETACH runs key destructors for threads
		 * not created by pthread_create(). */
		break;
	case DLL_THREAD_DETACH:
		ptw_thread_detach();
		break;
	case DLL_PROCESS_DETACH:
		/* On process exit (reserved != NULL) other threads are already
		 * gone; do not touch shared state. */
		if (reserved == NULL) {
			ptw_thread_detach();
		}
		break;
	default:
		break;
	}
	return TRUE;
}
