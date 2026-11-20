/*
 * Threads, pthread_self(), joining, detaching, exit and cancellation.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. See COPYING.LIB.
 */

#include "internal.h"

DWORD ptw_self_tls = TLS_OUT_OF_INDEXES;

/* Thread descriptors are never freed while the library is loaded; they are
 * recycled through this list. That keeps stale pthread_t values safe to
 * inspect (the reuse counter in pthread_t.x no longer matches), which is the
 * same guarantee pthreads-win32 gave. */
static SRWLOCK reuse_lock = SRWLOCK_INIT;
static ptw_thread *free_list = NULL;
static volatile LONG64 seq_counter = 0;

static INIT_ONCE tls_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK tls_init_cb(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
	(void)once;
	(void)param;
	(void)ctx;
	ptw_self_tls = TlsAlloc();
	return ptw_self_tls != TLS_OUT_OF_INDEXES;
}

static BOOL ensure_tls(void)
{
	return InitOnceExecuteOnce(&tls_once, tls_init_cb, NULL, NULL) && ptw_self_tls != TLS_OUT_OF_INDEXES;
}

static ptw_thread *thread_alloc(void)
{
	ptw_thread *t;
	unsigned int x = 1;

	AcquireSRWLockExclusive(&reuse_lock);
	t = free_list;
	if (t) {
		free_list = t->next_free;
		x = t->handle.x; /* already bumped when it was recycled */
	}
	ReleaseSRWLockExclusive(&reuse_lock);

	if (!t) {
		t = (ptw_thread *)calloc(1, sizeof(*t));
		if (!t) {
			return NULL;
		}
	} else {
		/* Keep the identity fields stable while we reset the rest; a
		 * concurrent lookup with a stale handle only reads handle.x. */
		memset(&t->seq, 0, sizeof(*t) - offsetof(ptw_thread, seq));
	}

	t->seq = (unsigned __int64)InterlockedIncrement64(&seq_counter);
	InitializeSRWLock(&t->cancel_lock);
	t->cancel_state = PTHREAD_CANCEL_ENABLE;
	t->cancel_type = PTHREAD_CANCEL_DEFERRED;
	t->sched_priority = THREAD_PRIORITY_NORMAL;
	t->state = PTW_THREAD_RUNNING;

	AcquireSRWLockExclusive(&reuse_lock);
	t->handle.p = t;
	t->handle.x = x;
	ReleaseSRWLockExclusive(&reuse_lock);
	return t;
}

static void thread_recycle(ptw_thread *t)
{
	if (t->win_handle) {
		CloseHandle(t->win_handle);
		t->win_handle = NULL;
	}
	if (t->cancel_event) {
		CloseHandle(t->cancel_event);
		t->cancel_event = NULL;
	}

	AcquireSRWLockExclusive(&reuse_lock);
	t->handle.x++;
	if (t->handle.x == 0) {
		t->handle.x = 1;
	}
	t->state = PTW_THREAD_EXITED;
	t->next_free = free_list;
	free_list = t;
	ReleaseSRWLockExclusive(&reuse_lock);
}

static void thread_unref(ptw_thread *t)
{
	if (InterlockedDecrement(&t->refs) == 0) {
		thread_recycle(t);
	}
}

ptw_thread *ptw_thread_lookup(pthread_t t)
{
	ptw_thread *p = (ptw_thread *)t.p;
	BOOL ok;

	if (!p) {
		return NULL;
	}

	AcquireSRWLockShared(&reuse_lock);
	ok = p->handle.p == p && p->handle.x == t.x && p->state != PTW_THREAD_EXITED;
	ReleaseSRWLockShared(&reuse_lock);
	return ok ? p : NULL;
}

/* A thread that has finished running but has not been joined yet is still a
 * valid target for pthread_join(). */
static ptw_thread *thread_lookup_joinable(pthread_t t)
{
	ptw_thread *p = (ptw_thread *)t.p;
	BOOL ok;

	if (!p) {
		return NULL;
	}

	AcquireSRWLockShared(&reuse_lock);
	ok = p->handle.p == p && p->handle.x == t.x && p->win_handle != NULL;
	ReleaseSRWLockShared(&reuse_lock);
	return ok ? p : NULL;
}

/* Maps a POSIX priority in [IDLE, TIME_CRITICAL] onto a value
 * SetThreadPriority() accepts, as pthreads-win32 did. */
static int ptw_win_priority(int prio)
{
	if (prio > THREAD_PRIORITY_IDLE && prio < THREAD_PRIORITY_LOWEST) {
		return THREAD_PRIORITY_LOWEST;
	}
	if (prio < THREAD_PRIORITY_TIME_CRITICAL && prio > THREAD_PRIORITY_HIGHEST) {
		return THREAD_PRIORITY_HIGHEST;
	}
	return prio;
}

ptw_thread *ptw_self(void)
{
	ptw_thread *s;

	if (!ensure_tls()) {
		return NULL;
	}

	s = (ptw_thread *)TlsGetValue(ptw_self_tls);
	if (s) {
		return s;
	}

	/* A thread not created by pthread_create(): give it an implicit,
	 * detached descriptor that lives until the thread exits. */
	s = thread_alloc();
	if (!s) {
		return NULL;
	}

	s->implicit = TRUE;
	s->detached = 1;
	s->refs = 1;
	s->win_id = GetCurrentThreadId();
	s->sched_priority = GetThreadPriority(GetCurrentThread());
	if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s->win_handle, 0, FALSE,
			     DUPLICATE_SAME_ACCESS)) {
		s->win_handle = NULL;
	}

	TlsSetValue(ptw_self_tls, s);
	return s;
}

/* ---------------------------------------------------------------------------
 * Cancellation
 * ------------------------------------------------------------------------ */

static HANDLE get_cancel_event(ptw_thread *t)
{
	HANDLE ev;

	AcquireSRWLockExclusive(&t->cancel_lock);
	if (!t->cancel_event) {
		t->cancel_event = CreateEventW(NULL, TRUE, t->cancel_pending ? TRUE : FALSE, NULL);
	}
	ev = t->cancel_event;
	ReleaseSRWLockExclusive(&t->cancel_lock);
	return ev;
}

void ptw_testcancel(ptw_thread *self)
{
	if (!self || self->cancel_state != PTHREAD_CANCEL_ENABLE) {
		return;
	}
	if (ptw_load_long_acquire(&self->cancel_pending) == 0) {
		return;
	}
	if (self->state != PTW_THREAD_RUNNING) {
		return;
	}

	AcquireSRWLockExclusive(&self->cancel_lock);
	self->cancel_pending = 0;
	if (self->cancel_event) {
		ResetEvent(self->cancel_event);
	}
	ReleaseSRWLockExclusive(&self->cancel_lock);

	/* Further cancellation requests are ignored while unwinding. */
	self->cancel_state = PTHREAD_CANCEL_DISABLE;
	ptw_thread_exit(self, PTHREAD_CANCELED);
}

int ptw_cancelable_wait(HANDLE h, DWORD ms)
{
	ptw_thread *self = ptw_self();
	HANDLE handles[2];
	DWORD count = 1;
	DWORD status;

	handles[0] = h;
	if (self && self->cancel_state == PTHREAD_CANCEL_ENABLE && self->state == PTW_THREAD_RUNNING) {
		HANDLE ev = get_cancel_event(self);
		if (ev) {
			handles[count++] = ev;
		}
	}

	status = WaitForMultipleObjects(count, handles, FALSE, ms);
	switch (status) {
	case WAIT_OBJECT_0:
		return 0;
	case WAIT_OBJECT_0 + 1:
		ptw_testcancel(self);
		/* Cancellation was disabled in the meantime. */
		return EINVAL;
	case WAIT_TIMEOUT:
		return ETIMEDOUT;
	default:
		return EINVAL;
	}
}

int PTW32_CDECL pthreadCancelableWait(HANDLE waitHandle)
{
	return ptw_cancelable_wait(waitHandle, INFINITE);
}

int PTW32_CDECL pthreadCancelableTimedWait(HANDLE waitHandle, DWORD timeout)
{
	return ptw_cancelable_wait(waitHandle, timeout);
}

int PTW32_CDECL pthread_cancel(pthread_t thread)
{
	ptw_thread *t = ptw_thread_lookup(thread);
	ptw_thread *self;

	if (!t) {
		return ESRCH;
	}

	AcquireSRWLockExclusive(&t->cancel_lock);
	InterlockedExchange(&t->cancel_pending, 1);
	if (t->cancel_event) {
		SetEvent(t->cancel_event);
	}
	/* Wake a condition variable wait so the target can notice; POSIX
	 * allows spurious wakeups, so this is safe for other waiters. */
	if (t->waiting_cv) {
		WakeAllConditionVariable(t->waiting_cv);
	}
	ReleaseSRWLockExclusive(&t->cancel_lock);

	/* The target may have registered its wait but not gone to sleep yet,
	 * in which case the wake above is lost. Retry briefly; the window is
	 * a few instructions wide, so this rarely loops more than once. */
	for (int i = 0; i < 20; i++) {
		BOOL still_waiting;

		Sleep(i == 0 ? 0 : 1);
		AcquireSRWLockExclusive(&t->cancel_lock);
		still_waiting = t->waiting_cv != NULL && t->cancel_pending;
		if (still_waiting) {
			WakeAllConditionVariable(t->waiting_cv);
		}
		ReleaseSRWLockExclusive(&t->cancel_lock);
		if (!still_waiting) {
			break;
		}
	}

	/* Asynchronous cancellation is treated as deferred, except for a
	 * thread cancelling itself, where both are equivalent. */
	self = ptw_self();
	if (self == t && t->cancel_type == PTHREAD_CANCEL_ASYNCHRONOUS) {
		ptw_testcancel(self);
	}

	return 0;
}

int PTW32_CDECL pthread_setcancelstate(int state, int *oldstate)
{
	ptw_thread *self;

	if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE) {
		return EINVAL;
	}

	self = ptw_self();
	if (!self) {
		return EINVAL;
	}

	if (oldstate) {
		*oldstate = self->cancel_state;
	}
	self->cancel_state = state;

	if (state == PTHREAD_CANCEL_ENABLE && self->cancel_type == PTHREAD_CANCEL_ASYNCHRONOUS) {
		ptw_testcancel(self);
	}
	return 0;
}

int PTW32_CDECL pthread_setcanceltype(int type, int *oldtype)
{
	ptw_thread *self;

	if (type != PTHREAD_CANCEL_DEFERRED && type != PTHREAD_CANCEL_ASYNCHRONOUS) {
		return EINVAL;
	}

	self = ptw_self();
	if (!self) {
		return EINVAL;
	}

	if (oldtype) {
		*oldtype = self->cancel_type;
	}
	self->cancel_type = type;

	if (type == PTHREAD_CANCEL_ASYNCHRONOUS) {
		ptw_testcancel(self);
	}
	return 0;
}

void PTW32_CDECL pthread_testcancel(void)
{
	ptw_testcancel(ptw_self());
}

/* ---------------------------------------------------------------------------
 * Cleanup handlers (used by the pthread_cleanup_push/pop macros)
 * ------------------------------------------------------------------------ */

void PTW32_CDECL ptw32_push_cleanup(ptw32_cleanup_t *cleanup, ptw32_cleanup_callback_t routine, void *arg)
{
	ptw_thread *self = ptw_self();

	cleanup->routine = routine;
	cleanup->arg = arg;
	if (self) {
		cleanup->prev = self->cleanup_top;
		self->cleanup_top = cleanup;
	} else {
		cleanup->prev = NULL;
	}
}

ptw32_cleanup_t *PTW32_CDECL ptw32_pop_cleanup(int execute)
{
	ptw_thread *self = ptw_self();
	ptw32_cleanup_t *top;

	if (!self) {
		return NULL;
	}

	top = self->cleanup_top;
	if (top) {
		self->cleanup_top = top->prev;
		if (execute && top->routine) {
			top->routine(top->arg);
		}
	}
	return top;
}

/* ---------------------------------------------------------------------------
 * Creation, exit, join, detach
 * ------------------------------------------------------------------------ */

static void thread_finish(ptw_thread *self)
{
	ptw_run_key_destructors();
	TlsSetValue(ptw_self_tls, NULL);

	AcquireSRWLockExclusive(&self->cancel_lock);
	self->waiting_cv = NULL;
	ReleaseSRWLockExclusive(&self->cancel_lock);

	InterlockedExchange(&self->state, PTW_THREAD_EXITING);
}

static unsigned __stdcall thread_start(void *param)
{
	ptw_thread *const self = (ptw_thread *)param;

	TlsSetValue(ptw_self_tls, self);

	if (setjmp(self->exit_jmp) == 0) {
		self->has_exit_jmp = TRUE;
		self->result = self->start(self->arg);
	}
	/* Normal return, or pthread_exit()/cancellation unwound to here. */
	self->has_exit_jmp = FALSE;

	thread_finish(self);
	thread_unref(self);
	return 0;
}

__declspec(noreturn) void ptw_thread_exit(ptw_thread *self, void *value)
{
	InterlockedExchange(&self->state, PTW_THREAD_EXITING);

	while (self->cleanup_top) {
		ptw32_cleanup_t *top = self->cleanup_top;
		self->cleanup_top = top->prev;
		if (top->routine) {
			top->routine(top->arg);
		}
	}

	self->result = value;

	if (self->has_exit_jmp) {
		longjmp(self->exit_jmp, 1);
	}

	/* Implicit thread: there is no trampoline to return to. */
	thread_finish(self);
	thread_unref(self);
	/* Like pthreads-win32, make the value visible as the exit code. */
	_endthreadex((unsigned)(size_t)value);
	/* Not reached, but some compilers cannot tell. */
	ExitThread(0);
}

void PTW32_CDECL pthread_exit(void *value_ptr)
{
	ptw_thread *self = ptw_self();

	if (!self) {
		_endthreadex(0);
	}
	ptw_thread_exit(self, value_ptr);
}

int PTW32_CDECL pthread_create(pthread_t *tid, const pthread_attr_t *attr, void *(PTW32_CDECL *start)(void *),
			       void *arg)
{
	const struct pthread_attr_t_ *a = NULL;
	ptw_thread *t;
	int priority = THREAD_PRIORITY_NORMAL;
	unsigned stack = 0;
	BOOL detached = FALSE;
	uintptr_t h;
	unsigned id = 0;

	if (!tid || !start) {
		return EINVAL;
	}

	if (!ensure_tls()) {
		return EAGAIN;
	}

	if (attr && *attr) {
		a = *attr;
		stack = (unsigned)a->stacksize;
		detached = a->detachstate == PTHREAD_CREATE_DETACHED;
		priority = a->param.sched_priority;
		if (a->inheritsched == PTHREAD_INHERIT_SCHED) {
			ptw_thread *parent = ptw_self();
			priority = parent ? parent->sched_priority : GetThreadPriority(GetCurrentThread());
		}
	}

	t = thread_alloc();
	if (!t) {
		return EAGAIN;
	}

	t->start = start;
	t->arg = arg;
	t->detached = detached ? 1 : 0;
	t->refs = detached ? 1 : 2;
	t->sched_priority = priority;

	h = _beginthreadex(NULL, stack, thread_start, t, CREATE_SUSPENDED, &id);
	if (!h) {
		t->refs = 0;
		thread_recycle(t);
		return EAGAIN;
	}

	t->win_handle = (HANDLE)h;
	t->win_id = id;
	if (priority != THREAD_PRIORITY_NORMAL) {
		SetThreadPriority(t->win_handle, ptw_win_priority(priority));
	}

	*tid = t->handle;
	ResumeThread(t->win_handle);
	return 0;
}

int PTW32_CDECL pthread_join(pthread_t thread, void **value_ptr)
{
	ptw_thread *t = thread_lookup_joinable(thread);
	ptw_thread *self;
	int r;

	if (!t) {
		return ESRCH;
	}
	self = ptw_self();
	if (self == t) {
		return EDEADLK;
	}

	if (ptw_load_long_acquire(&t->detached)) {
		return EINVAL;
	}

	r = ptw_cancelable_wait(t->win_handle, INFINITE);
	if (r != 0) {
		return ESRCH;
	}

	/* Claim the join; a concurrent detach or join loses. */
	if (InterlockedCompareExchange(&t->detached, 1, 0) != 0) {
		return EINVAL;
	}

	if (value_ptr) {
		*value_ptr = t->result;
	}

	thread_unref(t);
	return 0;
}

int PTW32_CDECL pthread_detach(pthread_t thread)
{
	ptw_thread *t = thread_lookup_joinable(thread);

	if (!t) {
		return ESRCH;
	}
	if (InterlockedCompareExchange(&t->detached, 1, 0) != 0) {
		return EINVAL;
	}

	thread_unref(t);
	return 0;
}

pthread_t PTW32_CDECL pthread_self(void)
{
	ptw_thread *self = ptw_self();
	pthread_t none = {NULL, 0};

	return self ? self->handle : none;
}

int PTW32_CDECL pthread_equal(pthread_t t1, pthread_t t2)
{
	return t1.p == t2.p && t1.x == t2.x;
}

int PTW32_CDECL pthread_kill(pthread_t thread, int sig)
{
	if (!ptw_thread_lookup(thread)) {
		return ESRCH;
	}
	return sig == 0 ? 0 : EINVAL;
}

/* Called on DLL_THREAD_DETACH for threads that were never created by
 * pthread_create() but picked up an implicit descriptor. */
void ptw_thread_detach(void)
{
	ptw_thread *self;

	if (ptw_self_tls == TLS_OUT_OF_INDEXES) {
		return;
	}

	self = (ptw_thread *)TlsGetValue(ptw_self_tls);
	if (!self || !self->implicit) {
		return;
	}

	thread_finish(self);
	thread_unref(self);
}

/* ---------------------------------------------------------------------------
 * Scheduling and non-portable extras that need the descriptor
 * ------------------------------------------------------------------------ */

int PTW32_CDECL pthread_setschedparam(pthread_t thread, int policy, const struct sched_param *param)
{
	ptw_thread *t = ptw_thread_lookup(thread);
	int prio;

	if (!t) {
		return ESRCH;
	}
	if (policy < SCHED_MIN || policy > SCHED_MAX || !param) {
		return EINVAL;
	}
	if (policy != SCHED_OTHER) {
		return ENOTSUP;
	}

	prio = param->sched_priority;
	if (prio < THREAD_PRIORITY_IDLE || prio > THREAD_PRIORITY_TIME_CRITICAL) {
		return EINVAL;
	}

	if (!t->win_handle || !SetThreadPriority(t->win_handle, ptw_win_priority(prio))) {
		return EINVAL;
	}

	/* Remember what was asked for, as pthreads-win32 did. */
	t->sched_priority = param->sched_priority;
	return 0;
}

int PTW32_CDECL pthread_getschedparam(pthread_t thread, int *policy, struct sched_param *param)
{
	ptw_thread *t = ptw_thread_lookup(thread);

	if (!t) {
		return ESRCH;
	}
	if (!policy || !param) {
		return EINVAL;
	}

	*policy = SCHED_OTHER;
	param->sched_priority = t->sched_priority;
	return 0;
}

unsigned __int64 PTW32_CDECL pthread_getunique_np(pthread_t thread)
{
	ptw_thread *t = (ptw_thread *)thread.p;
	return t ? t->seq : 0;
}

HANDLE PTW32_CDECL pthread_getw32threadhandle_np(pthread_t thread)
{
	ptw_thread *t = (ptw_thread *)thread.p;
	return t ? t->win_handle : NULL;
}

DWORD PTW32_CDECL pthread_getw32threadid_np(pthread_t thread)
{
	ptw_thread *t = (ptw_thread *)thread.p;
	return t ? t->win_id : 0;
}

int PTW32_CDECL pthread_delay_np(struct timespec *interval)
{
	unsigned __int64 ms;
	ptw_thread *self;

	if (!interval || interval->tv_sec < 0 || interval->tv_nsec < 0) {
		return EINVAL;
	}

	self = ptw_self();
	if (interval->tv_sec == 0 && interval->tv_nsec == 0) {
		ptw_testcancel(self);
		Sleep(0);
		ptw_testcancel(self);
		return 0;
	}

	ms = (unsigned __int64)interval->tv_sec * 1000ULL +
	     ((unsigned __int64)interval->tv_nsec + 999999ULL) / 1000000ULL;
	if (ms >= INFINITE) {
		return EINVAL;
	}

	if (!self) {
		return ENOMEM;
	}

	if (self->cancel_state == PTHREAD_CANCEL_ENABLE) {
		HANDLE ev = get_cancel_event(self);
		if (ev) {
			DWORD status = WaitForSingleObject(ev, (DWORD)ms);
			if (status == WAIT_OBJECT_0) {
				ptw_testcancel(self);
				return ESRCH;
			}
			return status == WAIT_TIMEOUT ? 0 : EINVAL;
		}
	}

	Sleep((DWORD)ms);
	return 0;
}

/* Used by cond.c to make condition waits wakeable by pthread_cancel(). */
void ptw_set_waiting_cv(ptw_thread *self, CONDITION_VARIABLE *cv)
{
	if (!self) {
		return;
	}
	AcquireSRWLockExclusive(&self->cancel_lock);
	self->waiting_cv = cv;
	ReleaseSRWLockExclusive(&self->cancel_lock);
}
