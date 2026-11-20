/*
 * Mutexes on top of SRWLOCK.
 *
 * Normal mutexes are a plain SRWLOCK. Recursive and error-checking mutexes
 * add an owner thread id (and a depth for recursive ones). The owner field
 * is written only by the thread holding the lock, so another thread can
 * never mistake it for its own id.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. See COPYING.LIB.
 */

#include "internal.h"

static int mutex_alloc(pthread_mutex_t *out, int kind)
{
	struct pthread_mutex_t_ *m = (struct pthread_mutex_t_ *)calloc(1, sizeof(*m));

	if (!m) {
		return ENOMEM;
	}
	InitializeSRWLock(&m->lock);
	m->kind = kind;
	*out = m;
	return 0;
}

static BOOL is_static_initializer(pthread_mutex_t m)
{
	return m == PTHREAD_MUTEX_INITIALIZER || m == PTHREAD_RECURSIVE_MUTEX_INITIALIZER ||
	       m == PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
}

int ptw_mutex_resolve(pthread_mutex_t *mutex, pthread_mutex_t *out)
{
	pthread_mutex_t m, created;
	int r = 0;

	AcquireSRWLockExclusive(&ptw_static_init_lock);
	m = *mutex;
	if (m == PTHREAD_MUTEX_INITIALIZER) {
		r = mutex_alloc(&created, PTHREAD_MUTEX_NORMAL);
	} else if (m == PTHREAD_RECURSIVE_MUTEX_INITIALIZER) {
		r = mutex_alloc(&created, PTHREAD_MUTEX_RECURSIVE);
	} else if (m == PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) {
		r = mutex_alloc(&created, PTHREAD_MUTEX_ERRORCHECK);
	} else if (m == NULL) {
		r = EINVAL; /* destroyed while we waited */
	} else {
		created = m; /* another thread resolved it first */
	}

	if (r == 0) {
		if (created != m) {
			PTW_STORE_HANDLE(mutex, created);
		}
		*out = created;
	}
	ReleaseSRWLockExclusive(&ptw_static_init_lock);
	return r;
}

static inline int mutex_get(pthread_mutex_t *mutex, struct pthread_mutex_t_ **out)
{
	pthread_mutex_t m;

	if (!mutex) {
		return EINVAL;
	}

	m = (pthread_mutex_t)PTW_LOAD_HANDLE(mutex);
	/* A zeroed or destroyed mutex: pthreads-win32 returned EINVAL rather
	 * than crashing, and existing code depends on that. */
	if (m == NULL) {
		return EINVAL;
	}
	if (is_static_initializer(m)) {
		return ptw_mutex_resolve(mutex, out);
	}

	*out = m;
	return 0;
}

int PTW32_CDECL pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
	int kind = PTHREAD_MUTEX_NORMAL;

	if (!mutex) {
		return EINVAL;
	}

	if (attr && *attr) {
		if ((*attr)->pshared == PTHREAD_PROCESS_SHARED) {
			return ENOSYS;
		}
		kind = (*attr)->kind;
	}

	return mutex_alloc(mutex, kind);
}

int PTW32_CDECL pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	pthread_mutex_t m;

	if (!mutex) {
		return EINVAL;
	}

	m = (pthread_mutex_t)PTW_LOAD_HANDLE(mutex);
	if (m == NULL) {
		return EINVAL;
	}

	if (is_static_initializer(m)) {
		int r = 0;

		AcquireSRWLockExclusive(&ptw_static_init_lock);
		m = *mutex;
		if (is_static_initializer(m)) {
			*mutex = NULL;
		} else {
			r = EBUSY; /* resolved meanwhile; let the caller retry */
		}
		ReleaseSRWLockExclusive(&ptw_static_init_lock);
		return r;
	}

	if (!TryAcquireSRWLockExclusive(&m->lock)) {
		return EBUSY;
	}

	PTW_STORE_HANDLE(mutex, NULL);
	ReleaseSRWLockExclusive(&m->lock);
	free(m);
	return 0;
}

int PTW32_CDECL pthread_mutex_lock(pthread_mutex_t *mutex)
{
	struct pthread_mutex_t_ *m;
	DWORD self;
	int r = mutex_get(mutex, &m);

	if (r != 0) {
		return r;
	}

	switch (m->kind) {
	case PTHREAD_MUTEX_RECURSIVE:
		self = GetCurrentThreadId();
		if (m->owner == self) {
			if (m->depth == LONG_MAX) {
				return EAGAIN;
			}
			m->depth++;
			return 0;
		}
		AcquireSRWLockExclusive(&m->lock);
		m->owner = self;
		m->depth = 1;
		return 0;

	case PTHREAD_MUTEX_ERRORCHECK:
		self = GetCurrentThreadId();
		if (m->owner == self) {
			return EDEADLK;
		}
		AcquireSRWLockExclusive(&m->lock);
		m->owner = self;
		return 0;

	default:
		AcquireSRWLockExclusive(&m->lock);
		m->owner = GetCurrentThreadId();
		return 0;
	}
}

int PTW32_CDECL pthread_mutex_trylock(pthread_mutex_t *mutex)
{
	struct pthread_mutex_t_ *m;
	DWORD self;
	int r = mutex_get(mutex, &m);

	if (r != 0) {
		return r;
	}

	self = GetCurrentThreadId();
	if (m->kind == PTHREAD_MUTEX_RECURSIVE && m->owner == self) {
		if (m->depth == LONG_MAX) {
			return EAGAIN;
		}
		m->depth++;
		return 0;
	}

	if (!TryAcquireSRWLockExclusive(&m->lock)) {
		return EBUSY;
	}

	m->owner = self;
	m->depth = 1;
	return 0;
}

int PTW32_CDECL pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime)
{
	struct pthread_mutex_t_ *m;
	DWORD self;
	unsigned spins = 0;
	int r = mutex_get(mutex, &m);

	if (r != 0) {
		return r;
	}

	self = GetCurrentThreadId();
	if (m->owner == self) {
		if (m->kind == PTHREAD_MUTEX_RECURSIVE) {
			if (m->depth == LONG_MAX) {
				return EAGAIN;
			}
			m->depth++;
			return 0;
		}
		if (m->kind == PTHREAD_MUTEX_ERRORCHECK) {
			return EDEADLK;
		}
	}

	/* SRWLOCK has no timed acquire. Timed locking is rare, so poll with
	 * a short spin followed by sleeps. */
	for (;;) {
		if (TryAcquireSRWLockExclusive(&m->lock)) {
			m->owner = self;
			m->depth = 1;
			return 0;
		}
		if (ptw_abstime_to_ms(abstime) == 0) {
			return ETIMEDOUT;
		}

		if (spins < 64) {
			YieldProcessor();
			spins++;
		} else if (spins < 128) {
			SwitchToThread();
			spins++;
		} else {
			Sleep(1);
		}
	}
}

int PTW32_CDECL pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	pthread_mutex_t mh;
	struct pthread_mutex_t_ *m;
	DWORD self;

	if (!mutex) {
		return EINVAL;
	}

	mh = (pthread_mutex_t)PTW_LOAD_HANDLE(mutex);
	if (mh == NULL) {
		return EINVAL;
	}
	if (is_static_initializer(mh)) {
		return EPERM; /* never locked */
	}
	m = mh;

	switch (m->kind) {
	case PTHREAD_MUTEX_RECURSIVE:
		self = GetCurrentThreadId();
		if (m->owner != self) {
			return EPERM;
		}
		if (--m->depth > 0) {
			return 0;
		}
		m->owner = 0;
		ReleaseSRWLockExclusive(&m->lock);
		return 0;

	case PTHREAD_MUTEX_ERRORCHECK:
		self = GetCurrentThreadId();
		if (m->owner != self) {
			return EPERM;
		}
		m->owner = 0;
		m->depth = 0;
		ReleaseSRWLockExclusive(&m->lock);
		return 0;

	default:
		/* Normal mutexes may be unlocked by any thread, as with
		 * pthreads-win32. Unlocking an unlocked one is a no-op rather
		 * than corrupting the SRWLOCK. */
		if (InterlockedExchange((volatile LONG *)&m->owner, 0) == 0) {
			return 0;
		}
		m->depth = 0;
		ReleaseSRWLockExclusive(&m->lock);
		return 0;
	}
}

int PTW32_CDECL pthread_mutex_consistent(pthread_mutex_t *mutex)
{
	struct pthread_mutex_t_ *m;
	int r = mutex_get(mutex, &m);

	/* Robust mutexes are not supported, so no mutex is ever in the
	 * "owner died" state this would repair. */
	return r != 0 ? r : EINVAL;
}

/* ---------------------------------------------------------------------------
 * Mutex attributes
 * ------------------------------------------------------------------------ */

int PTW32_CDECL pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
	struct pthread_mutexattr_t_ *a;

	if (!attr) {
		return EINVAL;
	}
	a = (struct pthread_mutexattr_t_ *)calloc(1, sizeof(*a));
	if (!a) {
		return ENOMEM;
	}
	a->kind = PTHREAD_MUTEX_DEFAULT;
	a->pshared = PTHREAD_PROCESS_PRIVATE;
	a->robust = PTHREAD_MUTEX_STALLED;
	*attr = a;
	return 0;
}

int PTW32_CDECL pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
	if (!attr || !*attr) {
		return EINVAL;
	}
	free(*attr);
	*attr = NULL;
	return 0;
}

int PTW32_CDECL pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr, int *pshared)
{
	if (!attr || !*attr || !pshared) {
		return EINVAL;
	}
	*pshared = (*attr)->pshared;
	return 0;
}

int PTW32_CDECL pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared)
{
	if (!attr || !*attr || (pshared != PTHREAD_PROCESS_SHARED && pshared != PTHREAD_PROCESS_PRIVATE)) {
		return EINVAL;
	}
	(*attr)->pshared = PTHREAD_PROCESS_PRIVATE;
	return pshared == PTHREAD_PROCESS_SHARED ? ENOSYS : 0;
}

int PTW32_CDECL pthread_mutexattr_settype(pthread_mutexattr_t *attr, int kind)
{
	if (!attr || !*attr) {
		return EINVAL;
	}
	switch (kind) {
	case PTHREAD_MUTEX_NORMAL:
	case PTHREAD_MUTEX_RECURSIVE:
	case PTHREAD_MUTEX_ERRORCHECK:
		(*attr)->kind = kind;
		return 0;
	default:
		return EINVAL;
	}
}

int PTW32_CDECL pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *kind)
{
	if (!attr || !*attr || !kind) {
		return EINVAL;
	}
	*kind = (*attr)->kind;
	return 0;
}

int PTW32_CDECL pthread_mutexattr_setkind_np(pthread_mutexattr_t *attr, int kind)
{
	return pthread_mutexattr_settype(attr, kind);
}

int PTW32_CDECL pthread_mutexattr_getkind_np(pthread_mutexattr_t *attr, int *kind)
{
	return pthread_mutexattr_gettype(attr, kind);
}

int PTW32_CDECL pthread_mutexattr_setrobust(pthread_mutexattr_t *attr, int robust)
{
	if (!attr || !*attr) {
		return EINVAL;
	}
	if (robust == PTHREAD_MUTEX_STALLED) {
		(*attr)->robust = robust;
		return 0;
	}
	if (robust == PTHREAD_MUTEX_ROBUST) {
		return ENOTSUP;
	}
	return EINVAL;
}

int PTW32_CDECL pthread_mutexattr_getrobust(const pthread_mutexattr_t *attr, int *robust)
{
	if (!attr || !*attr || !robust) {
		return EINVAL;
	}
	*robust = (*attr)->robust;
	return 0;
}
