/*
 * Read/write locks on top of SRWLOCK.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. See COPYING.LIB.
 */

#include "internal.h"

static int rwlock_alloc(pthread_rwlock_t *out)
{
	struct pthread_rwlock_t_ *l = (struct pthread_rwlock_t_ *)calloc(1, sizeof(*l));

	if (!l) {
		return ENOMEM;
	}
	InitializeSRWLock(&l->lock);
	*out = l;
	return 0;
}

static int rwlock_get(pthread_rwlock_t *rwlock, struct pthread_rwlock_t_ **out)
{
	pthread_rwlock_t l;
	int r = 0;

	if (!rwlock) {
		return EINVAL;
	}

	l = (pthread_rwlock_t)PTW_LOAD_HANDLE(rwlock);
	if (l == NULL) {
		return EINVAL;
	}

	if (l == PTHREAD_RWLOCK_INITIALIZER) {
		AcquireSRWLockExclusive(&ptw_static_init_lock);
		l = *rwlock;
		if (l == PTHREAD_RWLOCK_INITIALIZER) {
			pthread_rwlock_t created;
			r = rwlock_alloc(&created);
			if (r == 0) {
				PTW_STORE_HANDLE(rwlock, created);
				l = created;
			}
		} else if (l == NULL) {
			r = EINVAL;
		}
		ReleaseSRWLockExclusive(&ptw_static_init_lock);
	}

	if (r == 0) {
		*out = l;
	}
	return r;
}

int PTW32_CDECL pthread_rwlock_init(pthread_rwlock_t *lock, const pthread_rwlockattr_t *attr)
{
	if (!lock) {
		return EINVAL;
	}
	if (attr && *attr && (*attr)->pshared == PTHREAD_PROCESS_SHARED) {
		return ENOSYS;
	}
	return rwlock_alloc(lock);
}

int PTW32_CDECL pthread_rwlock_destroy(pthread_rwlock_t *lock)
{
	pthread_rwlock_t l;

	if (!lock) {
		return EINVAL;
	}

	l = (pthread_rwlock_t)PTW_LOAD_HANDLE(lock);
	if (l == NULL) {
		return EINVAL;
	}

	if (l == PTHREAD_RWLOCK_INITIALIZER) {
		int r = 0;
		AcquireSRWLockExclusive(&ptw_static_init_lock);
		if (*lock == PTHREAD_RWLOCK_INITIALIZER) {
			*lock = NULL;
		} else {
			r = EBUSY;
		}
		ReleaseSRWLockExclusive(&ptw_static_init_lock);
		return r;
	}

	if (!TryAcquireSRWLockExclusive(&l->lock)) {
		return EBUSY;
	}
	PTW_STORE_HANDLE(lock, NULL);
	ReleaseSRWLockExclusive(&l->lock);
	free(l);
	return 0;
}

int PTW32_CDECL pthread_rwlock_rdlock(pthread_rwlock_t *lock)
{
	struct pthread_rwlock_t_ *l;
	int r = rwlock_get(lock, &l);

	if (r != 0) {
		return r;
	}
	if (l->writer == GetCurrentThreadId()) {
		return EDEADLK;
	}
	AcquireSRWLockShared(&l->lock);
	InterlockedIncrement(&l->readers);
	return 0;
}

int PTW32_CDECL pthread_rwlock_tryrdlock(pthread_rwlock_t *lock)
{
	struct pthread_rwlock_t_ *l;
	int r = rwlock_get(lock, &l);

	if (r != 0) {
		return r;
	}
	if (!TryAcquireSRWLockShared(&l->lock)) {
		return EBUSY;
	}
	InterlockedIncrement(&l->readers);
	return 0;
}

int PTW32_CDECL pthread_rwlock_wrlock(pthread_rwlock_t *lock)
{
	struct pthread_rwlock_t_ *l;
	DWORD self;
	int r = rwlock_get(lock, &l);

	if (r != 0) {
		return r;
	}
	self = GetCurrentThreadId();
	if (l->writer == self) {
		return EDEADLK;
	}
	AcquireSRWLockExclusive(&l->lock);
	l->writer = self;
	return 0;
}

int PTW32_CDECL pthread_rwlock_trywrlock(pthread_rwlock_t *lock)
{
	struct pthread_rwlock_t_ *l;
	int r = rwlock_get(lock, &l);

	if (r != 0) {
		return r;
	}
	if (!TryAcquireSRWLockExclusive(&l->lock)) {
		return EBUSY;
	}
	l->writer = GetCurrentThreadId();
	return 0;
}

/* SRWLOCK has no timed acquire; poll like pthread_mutex_timedlock(). */
static int timed_acquire(struct pthread_rwlock_t_ *l, BOOL exclusive, const struct timespec *abstime)
{
	unsigned spins = 0;

	for (;;) {
		if (exclusive ? TryAcquireSRWLockExclusive(&l->lock) : TryAcquireSRWLockShared(&l->lock)) {
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

int PTW32_CDECL pthread_rwlock_timedrdlock(pthread_rwlock_t *lock, const struct timespec *abstime)
{
	struct pthread_rwlock_t_ *l;
	int r = rwlock_get(lock, &l);

	if (r != 0) {
		return r;
	}
	if (l->writer == GetCurrentThreadId()) {
		return EDEADLK;
	}
	r = timed_acquire(l, FALSE, abstime);
	if (r == 0) {
		InterlockedIncrement(&l->readers);
	}
	return r;
}

int PTW32_CDECL pthread_rwlock_timedwrlock(pthread_rwlock_t *lock, const struct timespec *abstime)
{
	struct pthread_rwlock_t_ *l;
	DWORD self;
	int r = rwlock_get(lock, &l);

	if (r != 0) {
		return r;
	}
	self = GetCurrentThreadId();
	if (l->writer == self) {
		return EDEADLK;
	}
	r = timed_acquire(l, TRUE, abstime);
	if (r == 0) {
		l->writer = self;
	}
	return r;
}

int PTW32_CDECL pthread_rwlock_unlock(pthread_rwlock_t *lock)
{
	pthread_rwlock_t lh;
	struct pthread_rwlock_t_ *l;

	if (!lock) {
		return EINVAL;
	}
	lh = (pthread_rwlock_t)PTW_LOAD_HANDLE(lock);
	if (lh == NULL) {
		return EINVAL;
	}
	if (lh == PTHREAD_RWLOCK_INITIALIZER) {
		return EPERM; /* never locked */
	}
	l = lh;

	if (l->writer == GetCurrentThreadId()) {
		l->writer = 0;
		ReleaseSRWLockExclusive(&l->lock);
		return 0;
	}

	/* Releasing a shared lock nobody holds would corrupt the SRWLOCK. */
	for (;;) {
		LONG readers = ptw_load_long_acquire(&l->readers);
		if (readers <= 0) {
			return EPERM;
		}
		if (InterlockedCompareExchange(&l->readers, readers - 1, readers) == readers) {
			break;
		}
	}
	ReleaseSRWLockShared(&l->lock);
	return 0;
}

int PTW32_CDECL pthread_rwlockattr_init(pthread_rwlockattr_t *attr)
{
	struct pthread_rwlockattr_t_ *a;

	if (!attr) {
		return EINVAL;
	}
	a = (struct pthread_rwlockattr_t_ *)calloc(1, sizeof(*a));
	if (!a) {
		return ENOMEM;
	}
	a->pshared = PTHREAD_PROCESS_PRIVATE;
	*attr = a;
	return 0;
}

int PTW32_CDECL pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr)
{
	if (!attr || !*attr) {
		return EINVAL;
	}
	free(*attr);
	*attr = NULL;
	return 0;
}

int PTW32_CDECL pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attr, int *pshared)
{
	if (!attr || !*attr || !pshared) {
		return EINVAL;
	}
	*pshared = (*attr)->pshared;
	return 0;
}

int PTW32_CDECL pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int pshared)
{
	if (!attr || !*attr || (pshared != PTHREAD_PROCESS_SHARED && pshared != PTHREAD_PROCESS_PRIVATE)) {
		return EINVAL;
	}
	(*attr)->pshared = PTHREAD_PROCESS_PRIVATE;
	return pshared == PTHREAD_PROCESS_SHARED ? ENOSYS : 0;
}
