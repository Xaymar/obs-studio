/*
 * Condition variables on top of CONDITION_VARIABLE + SRWLOCK.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. See COPYING.LIB.
 */

#include "internal.h"

static int cond_alloc(pthread_cond_t *out)
{
	struct pthread_cond_t_ *c = (struct pthread_cond_t_ *)calloc(1, sizeof(*c));

	if (!c) {
		return ENOMEM;
	}
	InitializeConditionVariable(&c->cv);
	*out = c;
	return 0;
}

static int cond_get(pthread_cond_t *cond, struct pthread_cond_t_ **out)
{
	pthread_cond_t c;
	int r = 0;

	if (!cond) {
		return EINVAL;
	}

	c = (pthread_cond_t)PTW_LOAD_HANDLE(cond);
	if (c == NULL) {
		return EINVAL;
	}

	if (c == PTHREAD_COND_INITIALIZER) {
		AcquireSRWLockExclusive(&ptw_static_init_lock);
		c = *cond;
		if (c == PTHREAD_COND_INITIALIZER) {
			pthread_cond_t created;
			r = cond_alloc(&created);
			if (r == 0) {
				PTW_STORE_HANDLE(cond, created);
				c = created;
			}
		} else if (c == NULL) {
			r = EINVAL;
		}
		ReleaseSRWLockExclusive(&ptw_static_init_lock);
	}

	if (r == 0) {
		*out = c;
	}
	return r;
}

int PTW32_CDECL pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
	if (!cond) {
		return EINVAL;
	}
	if (attr && *attr && (*attr)->pshared == PTHREAD_PROCESS_SHARED) {
		return ENOSYS;
	}
	return cond_alloc(cond);
}

int PTW32_CDECL pthread_cond_destroy(pthread_cond_t *cond)
{
	pthread_cond_t c;

	if (!cond) {
		return EINVAL;
	}

	c = (pthread_cond_t)PTW_LOAD_HANDLE(cond);
	if (c == NULL) {
		return EINVAL;
	}

	if (c == PTHREAD_COND_INITIALIZER) {
		int r = 0;
		AcquireSRWLockExclusive(&ptw_static_init_lock);
		if (*cond == PTHREAD_COND_INITIALIZER) {
			*cond = NULL;
		} else {
			r = EBUSY;
		}
		ReleaseSRWLockExclusive(&ptw_static_init_lock);
		return r;
	}

	if (ptw_load_long_acquire(&c->waiters) > 0) {
		return EBUSY;
	}

	PTW_STORE_HANDLE(cond, NULL);
	free(c);
	return 0;
}

/* Takes one wakeup token, if any is available. */
static BOOL take_token(struct pthread_cond_t_ *c)
{
	for (;;) {
		LONG k = ptw_load_long_acquire(&c->wakeups);
		if (k <= 0) {
			return FALSE;
		}
		if (InterlockedCompareExchange(&c->wakeups, k - 1, k) == k) {
			return TRUE;
		}
	}
}

/* Tokens beyond the number of waiters belong to nobody (POSIX drops
 * signals that find no waiter), so drop them. */
static void clamp_tokens(struct pthread_cond_t_ *c)
{
	for (;;) {
		LONG w = ptw_load_long_acquire(&c->waiters);
		LONG k = ptw_load_long_acquire(&c->wakeups);
		if (w < 0) {
			w = 0;
		}
		if (k <= w) {
			return;
		}
		if (InterlockedCompareExchange(&c->wakeups, w, k) == k) {
			return;
		}
	}
}

static BOOL cancel_requested(ptw_thread *self)
{
	return self && self->cancel_state == PTHREAD_CANCEL_ENABLE && self->state == PTW_THREAD_RUNNING &&
	       ptw_load_long_acquire(&self->cancel_pending);
}

int PTW32_CDECL pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
	struct pthread_cond_t_ *c;
	struct pthread_mutex_t_ *m;
	pthread_mutex_t mh;
	ptw_thread *self;
	DWORD tid, ms;
	LONG depth;
	BOOL cancelled = FALSE;
	int r;

	r = cond_get(cond, &c);
	if (r != 0) {
		return r;
	}

	if (!mutex) {
		return EINVAL;
	}
	mh = (pthread_mutex_t)PTW_LOAD_HANDLE(mutex);
	if (mh == NULL || mh == PTHREAD_MUTEX_INITIALIZER || mh == PTHREAD_RECURSIVE_MUTEX_INITIALIZER ||
	    mh == PTHREAD_ERRORCHECK_MUTEX_INITIALIZER) {
		return EINVAL; /* cannot be locked by us */
	}
	m = mh;

	tid = GetCurrentThreadId();
	/* Waiting with an SRWLOCK we do not hold would corrupt it. */
	if (m->owner != tid && !(m->kind == PTHREAD_MUTEX_NORMAL && m->owner != 0)) {
		return EPERM;
	}

	self = ptw_self();
	ptw_testcancel(self); /* cancellation point */

	ms = ptw_abstime_to_ms(abstime);

	depth = m->depth;
	m->owner = 0;
	m->depth = 0;

	/* CONDITION_VARIABLE may wake spuriously, and pthread_cancel() wakes
	 * every waiter to reach its target. Signals hand out tokens, and only
	 * a waiter that takes one returns success; the others go back to
	 * sleep. This keeps the "one signal wakes one waiter" behavior that
	 * pthreads-win32 had and existing code relies on. */
	InterlockedIncrement(&c->waiters);
	ptw_set_waiting_cv(self, &c->cv);
	for (;;) {
		BOOL ok;
		DWORD err = 0;

		if (cancel_requested(self)) {
			cancelled = TRUE;
			break;
		}

		ok = SleepConditionVariableSRW(&c->cv, &m->lock, ms, 0);
		if (!ok) {
			err = GetLastError();
		}

		if (take_token(c)) {
			r = 0;
			break;
		}
		if (!ok && err != ERROR_TIMEOUT) {
			r = EINVAL;
			break;
		}
		if (cancel_requested(self)) {
			cancelled = TRUE;
			break;
		}
		ms = ptw_abstime_to_ms(abstime);
		if (ms == 0) {
			r = ETIMEDOUT;
			break;
		}
	}
	ptw_set_waiting_cv(self, NULL);
	InterlockedDecrement(&c->waiters);
	clamp_tokens(c);

	/* A cancelled waiter may have been the one a signal was meant for;
	 * pass it on so the signal is not lost. */
	if (cancelled && ptw_load_long_acquire(&c->wakeups) > 0) {
		WakeConditionVariable(&c->cv);
	}

	m->owner = tid;
	m->depth = depth;

	/* Cancellation point: the mutex is held again, as POSIX requires for
	 * cleanup handlers. */
	ptw_testcancel(self);
	return r;
}

int PTW32_CDECL pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
	return pthread_cond_timedwait(cond, mutex, NULL);
}

int PTW32_CDECL pthread_cond_signal(pthread_cond_t *cond)
{
	struct pthread_cond_t_ *c;
	int r = cond_get(cond, &c);

	if (r != 0) {
		return r;
	}

	for (;;) {
		LONG w = ptw_load_long_acquire(&c->waiters);
		LONG k = ptw_load_long_acquire(&c->wakeups);
		if (k >= w) {
			return 0; /* nobody left to wake */
		}
		if (InterlockedCompareExchange(&c->wakeups, k + 1, k) == k) {
			break;
		}
	}
	WakeConditionVariable(&c->cv);
	return 0;
}

int PTW32_CDECL pthread_cond_broadcast(pthread_cond_t *cond)
{
	struct pthread_cond_t_ *c;
	int r = cond_get(cond, &c);

	if (r != 0) {
		return r;
	}

	for (;;) {
		LONG w = ptw_load_long_acquire(&c->waiters);
		LONG k = ptw_load_long_acquire(&c->wakeups);
		if (k >= w) {
			return 0;
		}
		if (InterlockedCompareExchange(&c->wakeups, w, k) == k) {
			break;
		}
	}
	WakeAllConditionVariable(&c->cv);
	return 0;
}

int PTW32_CDECL pthread_condattr_init(pthread_condattr_t *attr)
{
	struct pthread_condattr_t_ *a;

	if (!attr) {
		return EINVAL;
	}
	a = (struct pthread_condattr_t_ *)calloc(1, sizeof(*a));
	if (!a) {
		return ENOMEM;
	}
	a->pshared = PTHREAD_PROCESS_PRIVATE;
	*attr = a;
	return 0;
}

int PTW32_CDECL pthread_condattr_destroy(pthread_condattr_t *attr)
{
	if (!attr || !*attr) {
		return EINVAL;
	}
	free(*attr);
	*attr = NULL;
	return 0;
}

int PTW32_CDECL pthread_condattr_getpshared(const pthread_condattr_t *attr, int *pshared)
{
	if (!attr || !*attr || !pshared) {
		return EINVAL;
	}
	*pshared = (*attr)->pshared;
	return 0;
}

int PTW32_CDECL pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared)
{
	if (!attr || !*attr || (pshared != PTHREAD_PROCESS_SHARED && pshared != PTHREAD_PROCESS_PRIVATE)) {
		return EINVAL;
	}
	(*attr)->pshared = PTHREAD_PROCESS_PRIVATE;
	return pshared == PTHREAD_PROCESS_SHARED ? ENOSYS : 0;
}

/* pthreads-win32 used this to wake timed waits after a system clock change.
 * Native waits use relative timeouts, so there is nothing to do. */
void *PTW32_CDECL pthread_timechange_handler_np(void *arg)
{
	(void)arg;
	return NULL;
}
