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
 * pthread_once_t keeps its old layout { int done; void *lock; int; int; },
 * which PTHREAD_ONCE_INIT zero-fills. `done` is set once the routine has
 * finished and `reserved1` marks a routine in progress. INIT_ONCE cannot be
 * used directly: if the routine is cancelled, POSIX requires the once
 * control to go back to its initial state so another caller runs it, and an
 * abandoned INIT_ONCE would block everyone else forever.
 *
 * All once controls share one lock and condition variable for the slow path,
 * which only runs until each control has completed.
 * ------------------------------------------------------------------------ */

static SRWLOCK once_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE once_cv = CONDITION_VARIABLE_INIT;

static void PTW32_CDECL once_cancel_cleanup(void *arg)
{
	pthread_once_t *once_control = (pthread_once_t *)arg;

	AcquireSRWLockExclusive(&once_lock);
	once_control->reserved1 = 0;
	ReleaseSRWLockExclusive(&once_lock);
	WakeAllConditionVariable(&once_cv);
}

int PTW32_CDECL pthread_once(pthread_once_t *once_control, void(PTW32_CDECL *init_routine)(void))
{
	ptw32_cleanup_t cleanup;

	if (!once_control || !init_routine) {
		return EINVAL;
	}

	if (ptw_load_long_acquire((LONG const volatile *)&once_control->done)) {
		return 0;
	}

	AcquireSRWLockExclusive(&once_lock);
	while (!once_control->done && once_control->reserved1) {
		SleepConditionVariableSRW(&once_cv, &once_lock, INFINITE, 0);
	}
	if (once_control->done) {
		ReleaseSRWLockExclusive(&once_lock);
		return 0;
	}
	once_control->reserved1 = 1;
	ReleaseSRWLockExclusive(&once_lock);

	ptw32_push_cleanup(&cleanup, once_cancel_cleanup, once_control);
	init_routine();
	(void)ptw32_pop_cleanup(0);

	AcquireSRWLockExclusive(&once_lock);
	InterlockedExchange((LONG volatile *)&once_control->done, PTW32_TRUE);
	once_control->reserved1 = 0;
	ReleaseSRWLockExclusive(&once_lock);
	WakeAllConditionVariable(&once_cv);
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
	/* Spin locks have no owner; pthreads-win32 let any thread unlock
	 * them and returned 0 even when already unlocked. */
	InterlockedExchange(&s->locked, 0);
	return 0;
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

/*
 * `value` follows pthreads-win32: when positive it is the count, when
 * negative its magnitude is the number of blocked waiters. The kernel
 * semaphore only carries the units handed directly to blocked waiters.
 * `lock` keeps the two consistent.
 */

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

	s->sem = CreateSemaphoreW(NULL, 0, SEM_VALUE_MAX, NULL);
	if (!s->sem) {
		free(s);
		return sem_fail(ENOSPC);
	}
	InitializeSRWLock(&s->lock);
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

	AcquireSRWLockExclusive(&s->lock);
	if (s->value < 0) {
		ReleaseSRWLockExclusive(&s->lock);
		return sem_fail(EBUSY);
	}
	*sem = NULL;
	ReleaseSRWLockExclusive(&s->lock);

	CloseHandle(s->sem);
	free(s);
	return 0;
}

/* Gives up a wait that timed out or was cancelled. Returns TRUE if a post
 * had already handed this waiter a unit, which it then keeps. */
static BOOL sem_abandon_wait(struct sem_t_ *s)
{
	BOOL got_unit = FALSE;

	AcquireSRWLockExclusive(&s->lock);
	if (s->value < 0) {
		s->value++; /* still counted as blocked: just leave */
	} else {
		/* A post released a unit for us after we stopped waiting. */
		WaitForSingleObject(s->sem, INFINITE);
		got_unit = TRUE;
	}
	ReleaseSRWLockExclusive(&s->lock);
	return got_unit;
}

static void PTW32_CDECL sem_cancel_cleanup(void *arg)
{
	struct sem_t_ *s = (struct sem_t_ *)arg;

	/* Cancelled: hand back a unit we may have been given. */
	if (sem_abandon_wait(s)) {
		AcquireSRWLockExclusive(&s->lock);
		if (++s->value <= 0) {
			ReleaseSemaphore(s->sem, 1, NULL);
		}
		ReleaseSRWLockExclusive(&s->lock);
	}
}

static int sem_wait_ms(sem_t *sem, DWORD ms)
{
	struct sem_t_ *s;
	ptw32_cleanup_t cleanup;
	ptw_thread *self;
	LONG v;
	int r;

	if (!sem || !*sem) {
		return sem_fail(EINVAL);
	}
	s = *sem;

	self = ptw_self();
	ptw_testcancel(self); /* cancellation point */

	AcquireSRWLockExclusive(&s->lock);
	v = --s->value;
	ReleaseSRWLockExclusive(&s->lock);
	if (v >= 0) {
		return 0;
	}

	if (ms == 0) {
		return sem_abandon_wait(s) ? 0 : sem_fail(ETIMEDOUT);
	}

	ptw32_push_cleanup(&cleanup, sem_cancel_cleanup, s);
	r = ptw_cancelable_wait(s->sem, ms);
	(void)ptw32_pop_cleanup(0);

	if (r == 0) {
		return 0;
	}
	if (sem_abandon_wait(s)) {
		return 0; /* a unit arrived as the wait ended */
	}
	return sem_fail(r);
}

int __cdecl sem_wait(sem_t *sem)
{
	return sem_wait_ms(sem, INFINITE);
}

int __cdecl sem_timedwait(sem_t *sem, const struct timespec *abstime)
{
	/* pthreads-win32 treats a NULL deadline as "wait forever". */
	return sem_wait_ms(sem, ptw_abstime_to_ms(abstime));
}

int __cdecl sem_trywait(sem_t *sem)
{
	struct sem_t_ *s;
	int r = 0;

	if (!sem || !*sem) {
		return sem_fail(EINVAL);
	}
	s = *sem;

	AcquireSRWLockExclusive(&s->lock);
	if (s->value > 0) {
		s->value--;
	} else {
		r = EAGAIN;
	}
	ReleaseSRWLockExclusive(&s->lock);
	return r ? sem_fail(r) : 0;
}

int __cdecl sem_post_multiple(sem_t *sem, int count)
{
	struct sem_t_ *s;
	LONG waiters;
	int r = 0;

	if (!sem || !*sem || count <= 0) {
		return sem_fail(EINVAL);
	}
	s = *sem;

	AcquireSRWLockExclusive(&s->lock);
	if (s->value > SEM_VALUE_MAX - count) {
		r = ERANGE;
	} else {
		waiters = s->value < 0 ? -s->value : 0;
		if (waiters > count) {
			waiters = count;
		}
		if (waiters > 0 && !ReleaseSemaphore(s->sem, waiters, NULL)) {
			r = EINVAL;
		} else {
			s->value += count;
		}
	}
	ReleaseSRWLockExclusive(&s->lock);
	return r ? sem_fail(r) : 0;
}

int __cdecl sem_post(sem_t *sem)
{
	return sem_post_multiple(sem, 1);
}

int __cdecl sem_getvalue(sem_t *sem, int *sval)
{
	struct sem_t_ *s;

	if (!sem || !*sem || !sval) {
		return sem_fail(EINVAL);
	}
	s = *sem;

	AcquireSRWLockShared(&s->lock);
	*sval = (int)s->value;
	ReleaseSRWLockShared(&s->lock);
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
