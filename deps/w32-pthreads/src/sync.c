/*
 * pthread_once, spin locks, barriers and semaphores.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. See COPYING.LIB.
 */

#include "internal.h"

/* ---------------------------------------------------------------------------
 * pthread_once
 *
 * pthread_once_t keeps its old layout { int done; void *lock; int; int; }.
 * PTHREAD_ONCE_INIT zero-fills it, and a zeroed pointer-sized field is a
 * valid INIT_ONCE_STATIC_INIT, so `lock` holds the native INIT_ONCE.
 * ------------------------------------------------------------------------ */

typedef char ptw_once_lock_fits[sizeof(((pthread_once_t *)0)->lock) >= sizeof(INIT_ONCE) ? 1 : -1];

static BOOL CALLBACK once_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	void(PTW32_CDECL * init_routine)(void) = (void(PTW32_CDECL *)(void))param;

	(void)once;
	(void)ctx;
	init_routine();
	return TRUE;
}

int PTW32_CDECL pthread_once(pthread_once_t *once_control, void(PTW32_CDECL *init_routine)(void))
{
	if (!once_control || !init_routine) {
		return EINVAL;
	}

	if (ptw_load_long_acquire((LONG const volatile *)&once_control->done)) {
		return 0;
	}

	if (!InitOnceExecuteOnce((PINIT_ONCE)&once_control->lock, once_cb, (PVOID)init_routine, NULL)) {
		return EINVAL;
	}

	InterlockedExchange((LONG volatile *)&once_control->done, PTW32_TRUE);
	return 0;
}

/* ---------------------------------------------------------------------------
 * Spin locks
 * ------------------------------------------------------------------------ */

static int spin_get(pthread_spinlock_t *lock, struct pthread_spinlock_t_ **out)
{
	pthread_spinlock_t s;
	int r = 0;

	if (!lock) {
		return EINVAL;
	}

	s = (pthread_spinlock_t)PTW_LOAD_HANDLE(lock);
	if (s == NULL) {
		return EINVAL;
	}

	if (s == PTHREAD_SPINLOCK_INITIALIZER) {
		AcquireSRWLockExclusive(&ptw_static_init_lock);
		s = *lock;
		if (s == PTHREAD_SPINLOCK_INITIALIZER) {
			struct pthread_spinlock_t_ *created = (struct pthread_spinlock_t_ *)calloc(1, sizeof(*created));
			if (created) {
				PTW_STORE_HANDLE(lock, created);
				s = created;
			} else {
				r = ENOMEM;
			}
		} else if (s == NULL) {
			r = EINVAL;
		}
		ReleaseSRWLockExclusive(&ptw_static_init_lock);
	}

	if (r == 0) {
		*out = s;
	}
	return r;
}

int PTW32_CDECL pthread_spin_init(pthread_spinlock_t *lock, int pshared)
{
	struct pthread_spinlock_t_ *s;

	if (!lock) {
		return EINVAL;
	}
	if (pshared == PTHREAD_PROCESS_SHARED) {
		return ENOSYS;
	}

	s = (struct pthread_spinlock_t_ *)calloc(1, sizeof(*s));
	if (!s) {
		return ENOMEM;
	}
	*lock = s;
	return 0;
}

int PTW32_CDECL pthread_spin_destroy(pthread_spinlock_t *lock)
{
	pthread_spinlock_t s;

	if (!lock) {
		return EINVAL;
	}
	s = (pthread_spinlock_t)PTW_LOAD_HANDLE(lock);
	if (s == NULL) {
		return EINVAL;
	}

	if (s == PTHREAD_SPINLOCK_INITIALIZER) {
		int r = 0;
		AcquireSRWLockExclusive(&ptw_static_init_lock);
		if (*lock == PTHREAD_SPINLOCK_INITIALIZER) {
			*lock = NULL;
		} else {
			r = EBUSY;
		}
		ReleaseSRWLockExclusive(&ptw_static_init_lock);
		return r;
	}

	if (InterlockedCompareExchange(&s->locked, 1, 0) != 0) {
		return EBUSY;
	}
	PTW_STORE_HANDLE(lock, NULL);
	free(s);
	return 0;
}

int PTW32_CDECL pthread_spin_lock(pthread_spinlock_t *lock)
{
	struct pthread_spinlock_t_ *s;
	unsigned spins = 0;
	int r = spin_get(lock, &s);

	if (r != 0) {
		return r;
	}

	while (InterlockedCompareExchange(&s->locked, 1, 0) != 0) {
		/* Spin on a plain read to keep the cache line shared. */
		do {
			if (++spins < 1024) {
				YieldProcessor();
			} else {
				SwitchToThread();
			}
		} while (ptw_load_long_acquire(&s->locked) != 0);
	}
	return 0;
}

int PTW32_CDECL pthread_spin_trylock(pthread_spinlock_t *lock)
{
	struct pthread_spinlock_t_ *s;
	int r = spin_get(lock, &s);

	if (r != 0) {
		return r;
	}
	return InterlockedCompareExchange(&s->locked, 1, 0) == 0 ? 0 : EBUSY;
}

int PTW32_CDECL pthread_spin_unlock(pthread_spinlock_t *lock)
{
	pthread_spinlock_t s;

	if (!lock) {
		return EINVAL;
	}
	s = (pthread_spinlock_t)PTW_LOAD_HANDLE(lock);
	if (s == NULL) {
		return EINVAL;
	}
	if (s == PTHREAD_SPINLOCK_INITIALIZER) {
		return EPERM;
	}
	return InterlockedExchange(&s->locked, 0) == 1 ? 0 : EPERM;
}

/* ---------------------------------------------------------------------------
 * Barriers
 * ------------------------------------------------------------------------ */

int PTW32_CDECL pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count)
{
	struct pthread_barrier_t_ *b;

	(void)attr;
	if (!barrier || count == 0 || count > (unsigned int)LONG_MAX) {
		return EINVAL;
	}

	b = (struct pthread_barrier_t_ *)calloc(1, sizeof(*b));
	if (!b) {
		return ENOMEM;
	}
	if (!InitializeSynchronizationBarrier(&b->barrier, (LONG)count, -1)) {
		free(b);
		return ENOMEM;
	}
	*barrier = b;
	return 0;
}

int PTW32_CDECL pthread_barrier_destroy(pthread_barrier_t *barrier)
{
	struct pthread_barrier_t_ *b;

	if (!barrier || !*barrier) {
		return EINVAL;
	}
	b = *barrier;
	*barrier = NULL;
	/* Waits for threads still leaving the barrier. */
	DeleteSynchronizationBarrier(&b->barrier);
	free(b);
	return 0;
}

int PTW32_CDECL pthread_barrier_wait(pthread_barrier_t *barrier)
{
	if (!barrier || !*barrier) {
		return EINVAL;
	}
	return EnterSynchronizationBarrier(&(*barrier)->barrier, 0) ? PTHREAD_BARRIER_SERIAL_THREAD : 0;
}

int PTW32_CDECL pthread_barrierattr_init(pthread_barrierattr_t *attr)
{
	struct pthread_barrierattr_t_ *a;

	if (!attr) {
		return EINVAL;
	}
	a = (struct pthread_barrierattr_t_ *)calloc(1, sizeof(*a));
	if (!a) {
		return ENOMEM;
	}
	a->pshared = PTHREAD_PROCESS_PRIVATE;
	*attr = a;
	return 0;
}

int PTW32_CDECL pthread_barrierattr_destroy(pthread_barrierattr_t *attr)
{
	if (!attr || !*attr) {
		return EINVAL;
	}
	free(*attr);
	*attr = NULL;
	return 0;
}

int PTW32_CDECL pthread_barrierattr_getpshared(const pthread_barrierattr_t *attr, int *pshared)
{
	if (!attr || !*attr || !pshared) {
		return EINVAL;
	}
	*pshared = (*attr)->pshared;
	return 0;
}

int PTW32_CDECL pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int pshared)
{
	if (!attr || !*attr || (pshared != PTHREAD_PROCESS_SHARED && pshared != PTHREAD_PROCESS_PRIVATE)) {
		return EINVAL;
	}
	(*attr)->pshared = PTHREAD_PROCESS_PRIVATE;
	return pshared == PTHREAD_PROCESS_SHARED ? ENOSYS : 0;
}

/* ---------------------------------------------------------------------------
 * Semaphores (return -1 and set errno, as POSIX requires)
 * ------------------------------------------------------------------------ */

static int sem_fail(int err)
{
	errno = err;
	return -1;
}

int __cdecl sem_init(sem_t *sem, int pshared, unsigned int value)
{
	struct sem_t_ *s;

	if (!sem) {
		return sem_fail(EINVAL);
	}
	if (pshared != 0) {
		return sem_fail(EPERM);
	}
	if (value > (unsigned int)SEM_VALUE_MAX) {
		return sem_fail(EINVAL);
	}

	s = (struct sem_t_ *)calloc(1, sizeof(*s));
	if (!s) {
		return sem_fail(ENOMEM);
	}

	s->sem = CreateSemaphoreW(NULL, (LONG)value, SEM_VALUE_MAX, NULL);
	if (!s->sem) {
		free(s);
		return sem_fail(ENOSPC);
	}
	s->value = (LONG)value;
	*sem = s;
	return 0;
}

int __cdecl sem_destroy(sem_t *sem)
{
	struct sem_t_ *s;

	if (!sem || !*sem) {
		return sem_fail(EINVAL);
	}
	s = *sem;
	*sem = NULL;
	CloseHandle(s->sem);
	free(s);
	return 0;
}

static int sem_wait_ms(sem_t *sem, DWORD ms)
{
	struct sem_t_ *s;
	int r;

	if (!sem || !*sem) {
		return sem_fail(EINVAL);
	}
	s = *sem;

	r = ptw_cancelable_wait(s->sem, ms); /* cancellation point */
	if (r != 0) {
		return sem_fail(r);
	}
	InterlockedDecrement(&s->value);
	return 0;
}

int __cdecl sem_wait(sem_t *sem)
{
	return sem_wait_ms(sem, INFINITE);
}

int __cdecl sem_timedwait(sem_t *sem, const struct timespec *abstime)
{
	if (!abstime) {
		return sem_fail(EINVAL);
	}
	return sem_wait_ms(sem, ptw_abstime_to_ms(abstime));
}

int __cdecl sem_trywait(sem_t *sem)
{
	struct sem_t_ *s;
	DWORD status;

	if (!sem || !*sem) {
		return sem_fail(EINVAL);
	}
	s = *sem;

	status = WaitForSingleObject(s->sem, 0);
	if (status == WAIT_OBJECT_0) {
		InterlockedDecrement(&s->value);
		return 0;
	}
	return sem_fail(status == WAIT_TIMEOUT ? EAGAIN : EINVAL);
}

int __cdecl sem_post_multiple(sem_t *sem, int count)
{
	struct sem_t_ *s;

	if (!sem || !*sem || count <= 0) {
		return sem_fail(EINVAL);
	}
	s = *sem;

	if (ptw_load_long_acquire(&s->value) > SEM_VALUE_MAX - count) {
		return sem_fail(ERANGE);
	}

	InterlockedExchangeAdd(&s->value, count);
	if (!ReleaseSemaphore(s->sem, count, NULL)) {
		InterlockedExchangeAdd(&s->value, -count);
		return sem_fail(GetLastError() == ERROR_TOO_MANY_POSTS ? ERANGE : EINVAL);
	}
	return 0;
}

int __cdecl sem_post(sem_t *sem)
{
	return sem_post_multiple(sem, 1);
}

int __cdecl sem_getvalue(sem_t *sem, int *sval)
{
	LONG v;

	if (!sem || !*sem || !sval) {
		return sem_fail(EINVAL);
	}
	v = ptw_load_long_acquire(&(*sem)->value);
	*sval = v < 0 ? 0 : (int)v;
	return 0;
}

int __cdecl sem_open(const char *name, int oflag, mode_t mode, unsigned int value)
{
	(void)name;
	(void)oflag;
	(void)mode;
	(void)value;
	return sem_fail(ENOSYS);
}

int __cdecl sem_close(sem_t *sem)
{
	(void)sem;
	return sem_fail(ENOSYS);
}

int __cdecl sem_unlink(const char *name)
{
	(void)name;
	return sem_fail(ENOSYS);
}
