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

int PTW32_CDECL pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
	struct pthread_cond_t_ *c;
	struct pthread_mutex_t_ *m;
	pthread_mutex_t mh;
	ptw_thread *self;
	DWORD tid, ms, err = 0;
	LONG depth;
	BOOL ok;
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

	InterlockedIncrement(&c->waiters);
	ptw_set_waiting_cv(self, &c->cv);
	if (self && self->cancel_state == PTHREAD_CANCEL_ENABLE && ptw_load_long_acquire(&self->cancel_pending)) {
		ok = TRUE; /* acted on by ptw_testcancel() below */
	} else {
		ok = SleepConditionVariableSRW(&c->cv, &m->lock, ms, 0);
		if (!ok) {
			err = GetLastError();
		}
	}
	ptw_set_waiting_cv(self, NULL);
	InterlockedDecrement(&c->waiters);

	m->owner = tid;
	m->depth = depth;

	/* Cancellation point: the mutex is held again, as POSIX requires for
	 * cleanup handlers. */
	ptw_testcancel(self);

	if (!ok) {
		return err == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
	}
	return 0;
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
