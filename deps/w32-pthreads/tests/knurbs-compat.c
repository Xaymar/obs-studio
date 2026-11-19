/*
 * Knurbs compatibility checks for w32-pthreads.dll.
 *
 * Exercises the behaviors OBS Studio, Knurbs and third-party plugins rely
 * on, so the native implementation can be compared against the original
 * pthreads-win32 build. Every check prints one line; the exit code is the
 * number of failed checks. Lines starting with "info" or "perf" are not
 * checks, they record values to compare between the two builds.
 *
 * Build: <cc> -D__CLEANUP_C -I.. knurbs-compat.c -lw32-pthreads
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

static int failures = 0;

#define CHECK(name, cond)                                                              \
	do {                                                                           \
		int ok_ = (cond);                                                      \
		printf("%s %s\n", ok_ ? "pass" : "FAIL", name);                        \
		if (!ok_)                                                              \
			failures++;                                                    \
	} while (0)

#define INFO(fmt, ...) printf("info " fmt "\n", __VA_ARGS__)

static double now_ms(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER c;
	if (!freq.QuadPart) {
		QueryPerformanceFrequency(&freq);
	}
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void abstime_in(struct timespec *ts, long ms)
{
	FILETIME ft;
	ULARGE_INTEGER u;
	GetSystemTimePreciseAsFileTime(&ft);
	u.LowPart = ft.dwLowDateTime;
	u.HighPart = ft.dwHighDateTime;
	u.QuadPart -= 116444736000000000ULL;
	u.QuadPart += (unsigned __int64)ms * 10000ULL;
	ts->tv_sec = (time_t)(u.QuadPart / 10000000ULL);
	ts->tv_nsec = (long)((u.QuadPart % 10000000ULL) * 100);
}

/* ------------------------------------------------------------------------ */

static void test_sizes(void)
{
	CHECK("sizeof(pthread_t) == 2 pointers", sizeof(pthread_t) == 2 * sizeof(void *));
	CHECK("sizeof(pthread_mutex_t) == pointer", sizeof(pthread_mutex_t) == sizeof(void *));
	CHECK("sizeof(pthread_cond_t) == pointer", sizeof(pthread_cond_t) == sizeof(void *));
	CHECK("sizeof(pthread_once_t) unchanged", sizeof(pthread_once_t) == (sizeof(void *) == 8 ? 24 : 16));
	CHECK("sizeof(sem_t) == pointer", sizeof(sem_t) == sizeof(void *));
}

/* ------------------------------------------------------------------------ */

static void test_mutex_basics(void)
{
	pthread_mutex_t zeroed;
	pthread_mutex_t st = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_t st2 = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_t m;
	int r;

	memset(&zeroed, 0, sizeof(zeroed));
	CHECK("lock on zeroed mutex returns EINVAL", pthread_mutex_lock(&zeroed) == EINVAL);

	CHECK("static initializer lock", pthread_mutex_lock(&st) == 0);
	CHECK("static initializer unlock", pthread_mutex_unlock(&st) == 0);
	CHECK("static initializer destroy after use", pthread_mutex_destroy(&st) == 0);
	CHECK("destroy of untouched static initializer", pthread_mutex_destroy(&st2) == 0);

	CHECK("init", pthread_mutex_init(&m, NULL) == 0);
	r = pthread_mutex_unlock(&m);
	INFO("unlock of unlocked normal mutex returns %d", r);
	CHECK("lock after stray unlock still works", pthread_mutex_lock(&m) == 0);
	CHECK("trylock own normal mutex is EBUSY", pthread_mutex_trylock(&m) == EBUSY);
	CHECK("destroy locked mutex is EBUSY", pthread_mutex_destroy(&m) == EBUSY);
	CHECK("unlock", pthread_mutex_unlock(&m) == 0);
	CHECK("destroy", pthread_mutex_destroy(&m) == 0);
	CHECK("destroyed mutex handle is NULL", m == NULL);
}

static pthread_mutex_t xthread_mutex;
static void *xthread_unlock(void *arg)
{
	(void)arg;
	return (void *)(size_t)pthread_mutex_unlock(&xthread_mutex);
}

static void test_mutex_cross_thread_unlock(void)
{
	pthread_t t;
	void *res = (void *)1;

	pthread_mutex_init(&xthread_mutex, NULL);
	pthread_mutex_lock(&xthread_mutex);
	pthread_create(&t, NULL, xthread_unlock, NULL);
	pthread_join(t, &res);
	CHECK("normal mutex unlocked by another thread", res == NULL);
	CHECK("normal mutex relockable after cross-thread unlock", pthread_mutex_trylock(&xthread_mutex) == 0);
	pthread_mutex_unlock(&xthread_mutex);
	pthread_mutex_destroy(&xthread_mutex);
}

static pthread_mutex_t rec_mutex;
static void *rec_try(void *arg)
{
	(void)arg;
	int r = pthread_mutex_trylock(&rec_mutex);
	if (r == 0) {
		pthread_mutex_unlock(&rec_mutex);
	}
	return (void *)(size_t)r;
}
static void *rec_unlock(void *arg)
{
	(void)arg;
	return (void *)(size_t)pthread_mutex_unlock(&rec_mutex);
}

static void test_mutex_recursive(void)
{
	pthread_mutexattr_t a;
	pthread_t t;
	void *res;
	pthread_mutex_t st = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	CHECK("recursive init", pthread_mutex_init(&rec_mutex, &a) == 0);
	pthread_mutexattr_destroy(&a);

	CHECK("recursive lock 1", pthread_mutex_lock(&rec_mutex) == 0);
	CHECK("recursive lock 2", pthread_mutex_lock(&rec_mutex) == 0);
	CHECK("recursive trylock 3", pthread_mutex_trylock(&rec_mutex) == 0);

	pthread_create(&t, NULL, rec_try, NULL);
	pthread_join(t, &res);
	CHECK("other thread trylock is EBUSY", (int)(size_t)res == EBUSY);

	pthread_create(&t, NULL, rec_unlock, NULL);
	pthread_join(t, &res);
	CHECK("other thread unlock is EPERM", (int)(size_t)res == EPERM);

	CHECK("recursive unlock 3", pthread_mutex_unlock(&rec_mutex) == 0);
	CHECK("recursive unlock 2", pthread_mutex_unlock(&rec_mutex) == 0);
	CHECK("recursive unlock 1", pthread_mutex_unlock(&rec_mutex) == 0);
	CHECK("recursive extra unlock is EPERM", pthread_mutex_unlock(&rec_mutex) == EPERM);

	pthread_create(&t, NULL, rec_try, NULL);
	pthread_join(t, &res);
	CHECK("other thread trylock after release", res == NULL);
	CHECK("recursive destroy", pthread_mutex_destroy(&rec_mutex) == 0);

	CHECK("static recursive lock 1", pthread_mutex_lock(&st) == 0);
	CHECK("static recursive lock 2", pthread_mutex_lock(&st) == 0);
	pthread_mutex_unlock(&st);
	pthread_mutex_unlock(&st);
	CHECK("static recursive destroy", pthread_mutex_destroy(&st) == 0);
}

static void test_mutex_errorcheck(void)
{
	pthread_mutexattr_t a;
	pthread_mutex_t m;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(&m, &a);
	pthread_mutexattr_destroy(&a);

	CHECK("errorcheck unlock unlocked is EPERM", pthread_mutex_unlock(&m) == EPERM);
	CHECK("errorcheck lock", pthread_mutex_lock(&m) == 0);
	CHECK("errorcheck relock is EDEADLK", pthread_mutex_lock(&m) == EDEADLK);
	CHECK("errorcheck unlock", pthread_mutex_unlock(&m) == 0);
	pthread_mutex_destroy(&m);
}

static void test_mutex_timedlock(void)
{
	pthread_mutex_t m;
	struct timespec ts;
	double t0, el;

	pthread_mutex_init(&m, NULL);
	abstime_in(&ts, 100);
	CHECK("timedlock free mutex", pthread_mutex_timedlock(&m, &ts) == 0);
	CHECK("unlock after timedlock", pthread_mutex_unlock(&m) == 0);
	pthread_mutex_destroy(&m);
	(void)t0;
	(void)el;
}

#define CONTENTION_THREADS 8
#define CONTENTION_ITERS 200000
static pthread_mutex_t cont_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile long cont_counter = 0;
static void *contention_worker(void *arg)
{
	(void)arg;
	for (int i = 0; i < CONTENTION_ITERS; i++) {
		pthread_mutex_lock(&cont_mutex);
		cont_counter++;
		pthread_mutex_unlock(&cont_mutex);
	}
	return NULL;
}

static void test_mutex_contention(void)
{
	pthread_t t[CONTENTION_THREADS];
	double t0 = now_ms();

	for (int i = 0; i < CONTENTION_THREADS; i++) {
		pthread_create(&t[i], NULL, contention_worker, NULL);
	}
	for (int i = 0; i < CONTENTION_THREADS; i++) {
		pthread_join(t[i], NULL);
	}

	CHECK("contended counter is exact", cont_counter == (long)CONTENTION_THREADS * CONTENTION_ITERS);
	printf("perf contended lock/unlock (8 threads): %.1f ns/op\n",
	       (now_ms() - t0) * 1e6 / ((double)CONTENTION_THREADS * CONTENTION_ITERS));
}

static void test_mutex_uncontended_perf(void)
{
	pthread_mutex_t m;
	const int n = 5000000;
	double t0;

	pthread_mutex_init(&m, NULL);
	t0 = now_ms();
	for (int i = 0; i < n; i++) {
		pthread_mutex_lock(&m);
		pthread_mutex_unlock(&m);
	}
	printf("perf uncontended lock/unlock: %.1f ns/op\n", (now_ms() - t0) * 1e6 / n);
	pthread_mutex_destroy(&m);

	{
		pthread_mutexattr_t a;
		pthread_mutexattr_init(&a);
		pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&m, &a);
		pthread_mutexattr_destroy(&a);
	}
	t0 = now_ms();
	for (int i = 0; i < n; i++) {
		pthread_mutex_lock(&m);
		pthread_mutex_unlock(&m);
	}
	printf("perf uncontended recursive lock/unlock: %.1f ns/op\n", (now_ms() - t0) * 1e6 / n);
	pthread_mutex_destroy(&m);
}

/* ------------------------------------------------------------------------ */

static pthread_mutex_t cv_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int cv_ready = 0;
static int cv_woken = 0;

static void *cv_waiter(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&cv_mutex);
	while (!cv_ready) {
		pthread_cond_wait(&cv, &cv_mutex);
	}
	cv_woken++;
	pthread_mutex_unlock(&cv_mutex);
	return NULL;
}

static void test_cond(void)
{
	pthread_t t[4];
	struct timespec ts;
	double t0, el;
	int r;

	pthread_mutex_lock(&cv_mutex);
	abstime_in(&ts, 100);
	t0 = now_ms();
	r = pthread_cond_timedwait(&cv, &cv_mutex, &ts);
	el = now_ms() - t0;
	pthread_mutex_unlock(&cv_mutex);
	CHECK("cond_timedwait times out with ETIMEDOUT", r == ETIMEDOUT);
	CHECK("cond_timedwait waited about 100ms", el >= 90.0 && el < 1000.0);
	INFO("cond_timedwait 100ms took %.1f ms", el);

	pthread_mutex_lock(&cv_mutex);
	abstime_in(&ts, -1000);
	r = pthread_cond_timedwait(&cv, &cv_mutex, &ts);
	pthread_mutex_unlock(&cv_mutex);
	CHECK("cond_timedwait with past deadline is ETIMEDOUT", r == ETIMEDOUT);

	for (int i = 0; i < 4; i++) {
		pthread_create(&t[i], NULL, cv_waiter, NULL);
	}
	Sleep(50);
	pthread_mutex_lock(&cv_mutex);
	cv_ready = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&cv_mutex);
	for (int i = 0; i < 4; i++) {
		pthread_join(t[i], NULL);
	}
	CHECK("broadcast wakes all waiters", cv_woken == 4);

	CHECK("cond destroy", pthread_cond_destroy(&cv) == 0);
}

static pthread_mutex_t cvr_mutex;
static pthread_cond_t cvr_cond;
static int cvr_flag = 0;
static void *cvr_signaler(void *arg)
{
	(void)arg;
	Sleep(20);
	pthread_mutex_lock(&cvr_mutex);
	cvr_flag = 1;
	pthread_cond_signal(&cvr_cond);
	pthread_mutex_unlock(&cvr_mutex);
	return NULL;
}

static void test_cond_recursive_mutex(void)
{
	pthread_mutexattr_t a;
	pthread_t t;
	int r = 0;

	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&cvr_mutex, &a);
	pthread_mutexattr_destroy(&a);
	pthread_cond_init(&cvr_cond, NULL);

	pthread_mutex_lock(&cvr_mutex);
	pthread_create(&t, NULL, cvr_signaler, NULL);
	while (!cvr_flag && r == 0) {
		r = pthread_cond_wait(&cvr_cond, &cvr_mutex);
	}
	CHECK("cond_wait with recursive mutex", r == 0 && cvr_flag);
	CHECK("recursive mutex still owned after cond_wait", pthread_mutex_unlock(&cvr_mutex) == 0);
	pthread_join(t, NULL);
	pthread_cond_destroy(&cvr_cond);
	pthread_mutex_destroy(&cvr_mutex);
}

/* ------------------------------------------------------------------------ */

static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static pthread_barrier_t rw_barrier;
static void *rw_reader(void *arg)
{
	(void)arg;
	int r = pthread_rwlock_rdlock(&rw);
	pthread_barrier_wait(&rw_barrier); /* both readers inside at once */
	pthread_barrier_wait(&rw_barrier); /* main checked trywrlock */
	pthread_rwlock_unlock(&rw);
	return (void *)(size_t)r;
}

static void test_rwlock(void)
{
	pthread_t a, b;
	void *ra, *rb;

	pthread_barrier_init(&rw_barrier, NULL, 3);
	pthread_create(&a, NULL, rw_reader, NULL);
	pthread_create(&b, NULL, rw_reader, NULL);
	pthread_barrier_wait(&rw_barrier);
	CHECK("trywrlock while readers hold it is EBUSY", pthread_rwlock_trywrlock(&rw) == EBUSY);
	CHECK("tryrdlock while readers hold it", pthread_rwlock_tryrdlock(&rw) == 0);
	pthread_rwlock_unlock(&rw);
	pthread_barrier_wait(&rw_barrier);
	pthread_join(a, &ra);
	pthread_join(b, &rb);
	CHECK("two readers held the lock together", ra == NULL && rb == NULL);
	pthread_barrier_destroy(&rw_barrier);

	CHECK("wrlock", pthread_rwlock_wrlock(&rw) == 0);
	CHECK("tryrdlock while writer holds it is EBUSY", pthread_rwlock_tryrdlock(&rw) == EBUSY);
	CHECK("unlock writer", pthread_rwlock_unlock(&rw) == 0);
	CHECK("rwlock destroy", pthread_rwlock_destroy(&rw) == 0);
}

/* ------------------------------------------------------------------------ */

static pthread_once_t once_token = PTHREAD_ONCE_INIT;
static volatile LONG once_count = 0;
static void once_init(void)
{
	InterlockedIncrement(&once_count);
	Sleep(20);
}
static void *once_worker(void *arg)
{
	(void)arg;
	pthread_once(&once_token, once_init);
	return (void *)(size_t)once_count;
}

static pthread_once_t once_template = PTHREAD_ONCE_INIT;
static int copied_once_count = 0;
static void copied_once_init(void)
{
	copied_once_count++;
}

static void test_once(void)
{
	pthread_t t[16];
	int all_saw_init = 1;
	pthread_once_t copy;

	for (int i = 0; i < 16; i++) {
		pthread_create(&t[i], NULL, once_worker, NULL);
	}
	for (int i = 0; i < 16; i++) {
		void *r;
		pthread_join(t[i], &r);
		if ((LONG)(size_t)r != 1) {
			all_saw_init = 0;
		}
	}
	CHECK("once runs exactly once across 16 threads", once_count == 1);
	CHECK("every thread returned after init finished", all_saw_init);

	/* libobs copies a PTHREAD_ONCE_INIT token by value. */
	copy = once_template;
	pthread_once(&copy, copied_once_init);
	pthread_once(&copy, copied_once_init);
	CHECK("once token copied by value works", copied_once_count == 1);
}

/* ------------------------------------------------------------------------ */

static void *ret_arg_plus_one(void *arg)
{
	return (void *)((size_t)arg + 1);
}

static void *exit_with_value(void *arg)
{
	(void)arg;
	pthread_exit((void *)(size_t)1234);
	return NULL;
}

static void test_threads(void)
{
	pthread_t t, t2;
	void *res = NULL;
	int r;
	pthread_attr_t attr;
	int detachstate = -1;

	CHECK("create", pthread_create(&t, NULL, ret_arg_plus_one, (void *)41) == 0);
	CHECK("join", pthread_join(t, &res) == 0);
	CHECK("join returns thread result", (size_t)res == 42);
	r = pthread_join(t, &res);
	INFO("second join of same thread returns %d", r);
	CHECK("second join fails", r != 0);
	CHECK("kill(0) on joined thread is ESRCH", pthread_kill(t, 0) == ESRCH);

	CHECK("pthread_exit value", pthread_create(&t, NULL, exit_with_value, NULL) == 0 &&
					    pthread_join(t, &res) == 0 && (size_t)res == 1234);

	pthread_create(&t, NULL, ret_arg_plus_one, NULL);
	CHECK("detach", pthread_detach(t) == 0);
	Sleep(50);
	r = pthread_join(t, NULL);
	INFO("join of detached, finished thread returns %d", r);
	CHECK("join of detached thread fails", r != 0);

	pthread_attr_init(&attr);
	pthread_attr_getdetachstate(&attr, &detachstate);
	CHECK("attr default is joinable", detachstate == PTHREAD_CREATE_JOINABLE);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	CHECK("create detached via attr", pthread_create(&t2, &attr, ret_arg_plus_one, NULL) == 0);
	pthread_attr_destroy(&attr);
	Sleep(50);
	CHECK("join of attr-detached thread fails", pthread_join(t2, NULL) != 0);

	t = pthread_self();
	CHECK("self equals self", pthread_equal(t, pthread_self()));
	CHECK("getw32threadid_np matches", pthread_getw32threadid_np(t) == GetCurrentThreadId());
	CHECK("getw32threadhandle_np non-null", pthread_getw32threadhandle_np(t) != NULL);
	CHECK("join self is EDEADLK", pthread_join(t, NULL) == EDEADLK);
	CHECK("num_processors_np >= 1", pthread_num_processors_np() >= 1);
}

static void test_thread_reuse(void)
{
	DWORD before = 0, after = 0;
	int ok = 1;
	pthread_t first, t;
	unsigned __int64 seq_first;

	pthread_create(&first, NULL, ret_arg_plus_one, NULL);
	seq_first = pthread_getunique_np(first);
	pthread_join(first, NULL);

	GetProcessHandleCount(GetCurrentProcess(), &before);
	for (int i = 0; i < 2000; i++) {
		void *res;
		if (pthread_create(&t, NULL, ret_arg_plus_one, (void *)(size_t)i) != 0 || pthread_join(t, &res) != 0 ||
		    (size_t)res != (size_t)i + 1) {
			ok = 0;
			break;
		}
	}
	GetProcessHandleCount(GetCurrentProcess(), &after);
	CHECK("2000 create/join cycles", ok);
	CHECK("no handle leak over 2000 threads", after <= before + 16);
	INFO("handles before %lu after %lu", (unsigned long)before, (unsigned long)after);
	CHECK("stale pthread_t not equal to recycled one", !pthread_equal(first, t));
	CHECK("getunique_np increases", pthread_getunique_np(t) > seq_first);
}

/* ------------------------------------------------------------------------ */

static pthread_key_t key;
static volatile LONG dtor_calls = 0;
static void key_dtor(void *v)
{
	if ((size_t)v == 0xABC) {
		InterlockedIncrement(&dtor_calls);
	}
}
static void *key_worker(void *arg)
{
	(void)arg;
	pthread_setspecific(key, (void *)0xABC);
	return pthread_getspecific(key);
}
static DWORD WINAPI native_key_worker(LPVOID arg)
{
	pthread_t a, b;
	(void)arg;
	pthread_setspecific(key, (void *)0xABC);
	a = pthread_self();
	b = pthread_self();
	return pthread_equal(a, b) ? 0 : 1;
}

static void test_keys(void)
{
	pthread_t t;
	void *res;
	HANDLE h;
	DWORD code = 99;

	CHECK("key create", pthread_key_create(&key, key_dtor) == 0);
	CHECK("getspecific default NULL", pthread_getspecific(key) == NULL);
	pthread_create(&t, NULL, key_worker, NULL);
	pthread_join(t, &res);
	CHECK("getspecific returns set value", (size_t)res == 0xABC);
	CHECK("destructor ran for pthread thread", dtor_calls == 1);

	h = CreateThread(NULL, 0, native_key_worker, NULL, 0, NULL);
	WaitForSingleObject(h, INFINITE);
	GetExitCodeThread(h, &code);
	CloseHandle(h);
	CHECK("pthread_self stable in non-pthread thread", code == 0);
	CHECK("destructor ran for non-pthread thread", dtor_calls == 2);

	CHECK("key delete", pthread_key_delete(key) == 0);
}

/* ------------------------------------------------------------------------ */

static char cleanup_order[8];
static int cleanup_pos = 0;
static void cleanup_push_char(void *c)
{
	cleanup_order[cleanup_pos++] = (char)(size_t)c;
}
static void *cleanup_worker(void *arg)
{
	(void)arg;
	pthread_cleanup_push(cleanup_push_char, (void *)'a');
	pthread_cleanup_push(cleanup_push_char, (void *)'b');
	pthread_cleanup_push(cleanup_push_char, (void *)'x');
	pthread_cleanup_pop(0); /* not executed */
	pthread_exit((void *)7);
	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	return NULL;
}

static void test_cleanup(void)
{
	pthread_t t;
	void *res;
	pthread_create(&t, NULL, cleanup_worker, NULL);
	pthread_join(t, &res);
	cleanup_order[cleanup_pos] = 0;
	CHECK("pthread_exit runs cleanup handlers LIFO", strcmp(cleanup_order, "ba") == 0 && (size_t)res == 7);
}

/* ------------------------------------------------------------------------ */

static void *cancel_spin(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_testcancel();
		Sleep(1);
	}
	return NULL;
}

static pthread_mutex_t cc_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cc_cond = PTHREAD_COND_INITIALIZER;
static volatile LONG cc_cleanup_had_mutex = 0;
static void cc_cleanup(void *arg)
{
	(void)arg;
	/* POSIX: the mutex is re-acquired before cleanup handlers run. */
	if (pthread_mutex_unlock(&cc_mutex) == 0) {
		InterlockedExchange(&cc_cleanup_had_mutex, 1);
	}
}
static void *cancel_in_cond(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&cc_mutex);
	pthread_cleanup_push(cc_cleanup, NULL);
	for (;;) {
		pthread_cond_wait(&cc_cond, &cc_mutex);
	}
	pthread_cleanup_pop(1);
	return NULL;
}

static sem_t cs_sem;
static void *cancel_in_sem(void *arg)
{
	(void)arg;
	sem_wait(&cs_sem);
	return NULL;
}

static void *cancel_in_delay(void *arg)
{
	struct timespec ts = {10, 0};
	(void)arg;
	pthread_delay_np(&ts);
	return NULL;
}

static void *cancel_in_join(void *arg)
{
	pthread_t *target = (pthread_t *)arg;
	pthread_join(*target, NULL);
	return NULL;
}

static volatile LONG disabled_reached_end = 0;
static void *cancel_disabled(void *arg)
{
	int old;
	(void)arg;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
	Sleep(100);
	pthread_testcancel(); /* ignored while disabled */
	InterlockedExchange(&disabled_reached_end, 1);
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old);
	pthread_testcancel(); /* acted on now */
	return (void *)1;
}

static int join_within(pthread_t t, double ms, void **res)
{
	/* pthread_join has no timeout; poll the native handle first. */
	HANDLE h = pthread_getw32threadhandle_np(t);
	if (WaitForSingleObject(h, (DWORD)ms) != WAIT_OBJECT_0) {
		return 0;
	}
	return pthread_join(t, res) == 0;
}

static void test_cancel(void)
{
	pthread_t t, j;
	void *res = NULL;

	pthread_create(&t, NULL, cancel_spin, NULL);
	Sleep(20);
	CHECK("cancel running thread", pthread_cancel(t) == 0);
	CHECK("deferred cancel at testcancel", join_within(t, 2000, &res) && res == PTHREAD_CANCELED);

	pthread_create(&t, NULL, cancel_in_cond, NULL);
	Sleep(50);
	pthread_cancel(t);
	res = NULL;
	CHECK("cancel wakes cond_wait", join_within(t, 2000, &res) && res == PTHREAD_CANCELED);
	CHECK("cleanup handler holds the mutex after cancelled cond_wait", cc_cleanup_had_mutex == 1);

	sem_init(&cs_sem, 0, 0);
	pthread_create(&t, NULL, cancel_in_sem, NULL);
	Sleep(50);
	pthread_cancel(t);
	res = NULL;
	CHECK("cancel wakes sem_wait", join_within(t, 2000, &res) && res == PTHREAD_CANCELED);
	sem_destroy(&cs_sem);

	pthread_create(&t, NULL, cancel_in_delay, NULL);
	Sleep(50);
	pthread_cancel(t);
	res = NULL;
	CHECK("cancel wakes pthread_delay_np", join_within(t, 2000, &res) && res == PTHREAD_CANCELED);

	pthread_create(&t, NULL, cancel_spin, NULL);
	pthread_create(&j, NULL, cancel_in_join, &t);
	Sleep(50);
	pthread_cancel(j);
	res = NULL;
	CHECK("cancel wakes pthread_join", join_within(j, 2000, &res) && res == PTHREAD_CANCELED);
	pthread_cancel(t);
	pthread_join(t, NULL);

	pthread_create(&t, NULL, cancel_disabled, NULL);
	Sleep(20);
	pthread_cancel(t);
	res = NULL;
	CHECK("cancel waits while disabled, then acts",
	      join_within(t, 2000, &res) && res == PTHREAD_CANCELED && disabled_reached_end == 1);
}

/* ------------------------------------------------------------------------ */

static void test_sem(void)
{
	sem_t s;
	struct timespec ts;
	int v = -1;
	double t0, el;
	int r;

	CHECK("sem_init", sem_init(&s, 0, 2) == 0);
	sem_getvalue(&s, &v);
	CHECK("sem_getvalue initial", v == 2);
	CHECK("sem_wait 1", sem_wait(&s) == 0);
	CHECK("sem_trywait 2", sem_trywait(&s) == 0);
	errno = 0;
	CHECK("sem_trywait empty is EAGAIN", sem_trywait(&s) == -1 && errno == EAGAIN);
	abstime_in(&ts, 100);
	t0 = now_ms();
	errno = 0;
	r = sem_timedwait(&s, &ts);
	el = now_ms() - t0;
	CHECK("sem_timedwait times out with ETIMEDOUT", r == -1 && errno == ETIMEDOUT);
	CHECK("sem_timedwait waited about 100ms", el >= 90.0 && el < 1000.0);
	CHECK("sem_post_multiple", sem_post_multiple(&s, 3) == 0);
	sem_getvalue(&s, &v);
	CHECK("sem_getvalue after post_multiple", v == 3);
	errno = 0;
	CHECK("sem_init pshared is EPERM", sem_init(&s, 1, 0) == -1 && errno == EPERM);
	CHECK("sem_destroy", sem_destroy(&s) == 0);
	errno = 0;
	CHECK("sem_open is ENOSYS", sem_open("x", 0, 0, 0) == -1 && errno == ENOSYS);
}

/* ------------------------------------------------------------------------ */

static pthread_barrier_t bar;
static volatile LONG serial_count = 0;
static void *barrier_worker(void *arg)
{
	(void)arg;
	for (int round = 0; round < 3; round++) {
		if (pthread_barrier_wait(&bar) == PTHREAD_BARRIER_SERIAL_THREAD) {
			InterlockedIncrement(&serial_count);
		}
	}
	return NULL;
}

static pthread_spinlock_t spin;
static volatile long spin_counter = 0;
static void *spin_worker(void *arg)
{
	(void)arg;
	for (int i = 0; i < 100000; i++) {
		pthread_spin_lock(&spin);
		spin_counter++;
		pthread_spin_unlock(&spin);
	}
	return NULL;
}

static void test_barrier_spin(void)
{
	pthread_t t[4];

	CHECK("barrier_init count 0 is EINVAL", pthread_barrier_init(&bar, NULL, 0) == EINVAL);
	pthread_barrier_init(&bar, NULL, 4);
	for (int i = 0; i < 4; i++) {
		pthread_create(&t[i], NULL, barrier_worker, NULL);
	}
	for (int i = 0; i < 4; i++) {
		pthread_join(t[i], NULL);
	}
	CHECK("one serial thread per barrier round", serial_count == 3);
	CHECK("barrier destroy", pthread_barrier_destroy(&bar) == 0);

	pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE);
	for (int i = 0; i < 4; i++) {
		pthread_create(&t[i], NULL, spin_worker, NULL);
	}
	for (int i = 0; i < 4; i++) {
		pthread_join(t[i], NULL);
	}
	CHECK("spin lock counter is exact", spin_counter == 400000);
	CHECK("spin trylock", pthread_spin_trylock(&spin) == 0);
	CHECK("spin trylock held is EBUSY", pthread_spin_trylock(&spin) == EBUSY);
	CHECK("spin destroy held is EBUSY", pthread_spin_destroy(&spin) == EBUSY);
	pthread_spin_unlock(&spin);
	CHECK("spin destroy", pthread_spin_destroy(&spin) == 0);
}

/* ------------------------------------------------------------------------ */

static void *report_priority(void *arg)
{
	(void)arg;
	return (void *)(size_t)(GetThreadPriority(GetCurrentThread()) + 100);
}

static void test_sched(void)
{
	pthread_attr_t attr;
	struct sched_param p;
	pthread_t t;
	void *res;
	int policy = -1;
	struct timespec ts = {0, 50 * 1000000};
	double t0, el;

	pthread_attr_init(&attr);
	p.sched_priority = THREAD_PRIORITY_ABOVE_NORMAL;
	CHECK("attr_setschedparam", pthread_attr_setschedparam(&attr, &p) == 0);
	pthread_create(&t, &attr, report_priority, NULL);
	pthread_join(t, &res);
	CHECK("explicit priority applied", (int)(size_t)res - 100 == THREAD_PRIORITY_ABOVE_NORMAL);
	pthread_attr_destroy(&attr);

	CHECK("getschedparam self", pthread_getschedparam(pthread_self(), &policy, &p) == 0 && policy == SCHED_OTHER);
	CHECK("priority range", sched_get_priority_min(SCHED_OTHER) == THREAD_PRIORITY_IDLE &&
					sched_get_priority_max(SCHED_OTHER) == THREAD_PRIORITY_TIME_CRITICAL);
	CHECK("sched_yield", sched_yield() == 0);

	t0 = now_ms();
	CHECK("delay_np", pthread_delay_np(&ts) == 0);
	el = now_ms() - t0;
	CHECK("delay_np waited about 50ms", el >= 45.0 && el < 1000.0);
}

/* Last on purpose: pthreads-win32 crashes here (it dereferences the NULL
 * handle), and the native build returns EINVAL instead. */
static void test_zeroed_unlock_destroy(void)
{
	pthread_mutex_t zeroed;

	memset(&zeroed, 0, sizeof(zeroed));
	CHECK("unlock on zeroed mutex returns EINVAL", pthread_mutex_unlock(&zeroed) == EINVAL);
	CHECK("destroy on zeroed mutex returns EINVAL", pthread_mutex_destroy(&zeroed) == EINVAL);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	test_sizes();
	test_mutex_basics();
	test_mutex_cross_thread_unlock();
	test_mutex_recursive();
	test_mutex_errorcheck();
	test_mutex_timedlock();
	test_mutex_contention();
	test_mutex_uncontended_perf();
	test_cond();
	test_cond_recursive_mutex();
	test_rwlock();
	test_once();
	test_threads();
	test_thread_reuse();
	test_keys();
	test_cleanup();
	test_cancel();
	test_sem();
	test_barrier_spin();
	test_sched();
	test_zeroed_unlock_destroy();

	printf("%s: %d failed check(s)\n", failures ? "FAILED" : "OK", failures);
	return failures;
}
