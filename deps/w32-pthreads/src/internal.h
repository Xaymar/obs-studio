/*
 * Native Win32 implementation of the w32-pthreads ABI.
 *
 * This library keeps the exported symbols, calling convention and type
 * layouts of pthreads-win32 2.9.1 (as shipped by OBS Studio as
 * w32-pthreads.dll) so that binaries built against the old headers keep
 * working, but implements everything on top of native Windows primitives
 * (SRWLOCK, CONDITION_VARIABLE, INIT_ONCE, SYNCHRONIZATION_BARRIER, TLS and
 * kernel semaphores) instead of emulating them.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. See COPYING.LIB.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <process.h>
#include <setjmp.h>
#include <stdlib.h>
#include <errno.h>

#include "../pthread.h"
#include "../semaphore.h"

/* ---------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

#define PTW_STATIC_SENTINEL(type, n) ((type)(size_t)(-(n)))

/* Acquire loads / release stores for values that are read without a lock. */
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static __forceinline void *ptw_load_ptr_acquire(void *const volatile *p)
{
#if defined(_M_ARM64)
	return (void *)__ldar64((unsigned __int64 volatile *)p);
#else
	void *v = *p;
	_ReadWriteBarrier();
	return v;
#endif
}
static __forceinline LONG ptw_load_long_acquire(LONG const volatile *p)
{
#if defined(_M_ARM64)
	return (LONG)__ldar32((unsigned __int32 volatile *)p);
#else
	LONG v = *p;
	_ReadWriteBarrier();
	return v;
#endif
}
#else
static inline void *ptw_load_ptr_acquire(void *const volatile *p)
{
	return __atomic_load_n((void *const *)p, __ATOMIC_ACQUIRE);
}
static inline LONG ptw_load_long_acquire(LONG const volatile *p)
{
	return __atomic_load_n((LONG const *)p, __ATOMIC_ACQUIRE);
}
#endif

#define PTW_LOAD_HANDLE(pp) ptw_load_ptr_acquire((void *const volatile *)(pp))
#define PTW_STORE_HANDLE(pp, v) InterlockedExchangePointer((PVOID volatile *)(pp), (PVOID)(v))

/* Converts an absolute CLOCK_REALTIME deadline into a relative timeout in
 * milliseconds, rounded up. Returns 0 if the deadline has passed. A NULL
 * deadline means "wait forever". */
DWORD ptw_abstime_to_ms(const struct timespec *abstime);

/* Current CLOCK_REALTIME time in 100ns units since the Unix epoch. */
unsigned __int64 ptw_now_100ns(void);

/* ---------------------------------------------------------------------------
 * Threads
 * ------------------------------------------------------------------------ */

enum ptw_thread_state {
	PTW_THREAD_RUNNING = 0,
	PTW_THREAD_EXITING = 1,
	PTW_THREAD_EXITED = 2,
};

typedef struct ptw_thread ptw_thread;
struct ptw_thread {
	/* Identity. `handle.p` points back to this struct and `handle.x` is the
	 * reuse counter, which changes every time the struct is recycled, so
	 * stale pthread_t values can be detected (ESRCH). */
	pthread_t handle;
	unsigned __int64 seq;

	HANDLE win_handle; /* NULL once joined/detached and closed */
	DWORD win_id;

	void *(PTW32_CDECL *start)(void *);
	void *arg;
	void *result;

	/* Lifetime: one reference held by the running thread, one by whoever
	 * may still join it. The struct is recycled when both are gone. */
	volatile LONG refs;
	volatile LONG detached;
	volatile LONG state;
	BOOL implicit; /* created by pthread_self() for a non-pthread thread */

	/* Cancellation. */
	volatile LONG cancel_pending;
	int cancel_state;
	int cancel_type;
	HANDLE cancel_event; /* manual reset, created on demand */
	SRWLOCK cancel_lock;
	/* Condition variable this thread is blocked on, so pthread_cancel()
	 * can wake it (guarded by cancel_lock). */
	CONDITION_VARIABLE *waiting_cv;

	/* Cleanup handlers (pthread_cleanup_push/pop). */
	ptw32_cleanup_t *cleanup_top;

	/* pthread_exit() unwinds to the start trampoline. */
	BOOL has_exit_jmp;
	jmp_buf exit_jmp;

	int sched_priority;

	ptw_thread *next_free;
};

/* Returns the calling thread's descriptor, creating an implicit one for
 * threads not created through pthread_create(). May return NULL only if
 * memory is exhausted. */
ptw_thread *ptw_self(void);

/* Validates a pthread_t and returns its descriptor, or NULL (ESRCH). */
ptw_thread *ptw_thread_lookup(pthread_t t);

/* Runs cleanup handlers and key destructors, then leaves the thread. */
__declspec(noreturn) void ptw_thread_exit(ptw_thread *self, void *value);

/* Waits on `h` (and the calling thread's cancel event). Returns 0 when `h`
 * was signaled, ETIMEDOUT on timeout, EINVAL on failure. Acts on a pending
 * cancellation request if the calling thread has cancellation enabled. */
int ptw_cancelable_wait(HANDLE h, DWORD ms);

/* Registers the condition variable the calling thread blocks on (NULL to
 * clear), so pthread_cancel() can wake it. */
void ptw_set_waiting_cv(ptw_thread *self, CONDITION_VARIABLE *cv);

/* Acts on a pending cancellation request, if enabled. */
void ptw_testcancel(ptw_thread *self);

/* Process / thread lifecycle hooks (dllmain.c). */
void ptw_process_attach(void);
void ptw_process_detach(void);
void ptw_thread_detach(void);

/* Thread-specific data (key.c). Runs destructors for the calling thread. */
void ptw_run_key_destructors(void);

extern DWORD ptw_self_tls;

/* ---------------------------------------------------------------------------
 * Mutexes
 * ------------------------------------------------------------------------ */

struct pthread_mutex_t_ {
	SRWLOCK lock;
	volatile DWORD owner; /* thread id of the owner, 0 if unlocked */
	LONG depth;           /* recursion depth for recursive mutexes */
	int kind;             /* PTHREAD_MUTEX_NORMAL/RECURSIVE/ERRORCHECK */
};

struct pthread_mutexattr_t_ {
	int kind;
	int pshared;
	int robust;
};

/* Resolves a static initializer to a real mutex. Returns 0 or an errno. */
int ptw_mutex_resolve(pthread_mutex_t *mutex, pthread_mutex_t *out);

/* ---------------------------------------------------------------------------
 * Other objects
 * ------------------------------------------------------------------------ */

struct pthread_cond_t_ {
	CONDITION_VARIABLE cv;
	volatile LONG waiters; /* threads inside pthread_cond_(timed)wait */
	volatile LONG wakeups; /* signals not yet taken by a waiter */
};

struct pthread_condattr_t_ {
	int pshared;
};

struct pthread_rwlock_t_ {
	SRWLOCK lock;
	volatile DWORD writer; /* thread id holding the exclusive lock */
	volatile LONG readers;
};

struct pthread_rwlockattr_t_ {
	int pshared;
};

struct pthread_spinlock_t_ {
	volatile LONG locked;
};

struct pthread_barrier_t_ {
	SYNCHRONIZATION_BARRIER barrier;
};

struct pthread_barrierattr_t_ {
	int pshared;
};

struct pthread_attr_t_ {
	unsigned long valid;
	void *stackaddr;
	size_t stacksize;
	int detachstate;
	struct sched_param param;
	int inheritsched;
	int contentionscope;
};

#define PTW_ATTR_VALID 0xC4C0FFEEUL

struct pthread_key_t_ {
	DWORD tls;
	void(PTW32_CDECL *destructor)(void *);
	struct pthread_key_t_ *next;
	struct pthread_key_t_ *prev;
};

struct sem_t_ {
	HANDLE sem;
	SRWLOCK lock;
	LONG value; /* count, or -(number of blocked waiters) */
};

/* Lazily-initialized static objects share one guard. */
extern SRWLOCK ptw_static_init_lock;
