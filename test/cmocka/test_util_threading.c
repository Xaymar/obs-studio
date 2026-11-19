#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include <util/platform.h>
#include <util/threading.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* The pthread and os_* threading behaviour libobs relies on, through the public API only, so these keep passing
 * when w32-pthreads is replaced by another implementation. Waits that would hang on a broken implementation use
 * timeouts, and nothing depends on sleeps for correctness except where noted. */

#define THREADS 8

/* ------------------------------------------------------------------------- */
/* pthread */

struct mutex_counter {
	pthread_mutex_t mutex;
	long value; /* protected by mutex, deliberately not atomic */
	int loops;
};

static void *mutex_counter_thread(void *param)
{
	struct mutex_counter *mc = param;
	for (int i = 0; i < mc->loops; i++) {
		pthread_mutex_lock(&mc->mutex);
		long v = mc->value;
		mc->value = v + 1;
		pthread_mutex_unlock(&mc->mutex);
	}
	return param;
}

static void *trylock_thread(void *param)
{
	pthread_mutex_t *mutex = param;
	int ret = pthread_mutex_trylock(mutex);
	if (ret == 0)
		pthread_mutex_unlock(mutex);
	return (void *)(intptr_t)ret;
}

static void mutex_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct mutex_counter mc = {.loops = 20000};
	assert_int_equal(pthread_mutex_init(&mc.mutex, NULL), 0);

	pthread_t threads[THREADS];
	for (int i = 0; i < THREADS; i++)
		assert_int_equal(pthread_create(&threads[i], NULL, mutex_counter_thread, &mc), 0);
	for (int i = 0; i < THREADS; i++) {
		void *ret = NULL;
		assert_int_equal(pthread_join(threads[i], &ret), 0);
		/* the thread's return value comes back through pthread_join() */
		assert_ptr_equal(ret, &mc);
	}
	assert_int_equal(mc.value, THREADS * mc.loops);

	/* trylock fails from another thread while the mutex is held, and succeeds once it is free */
	pthread_mutex_lock(&mc.mutex);
	pthread_t t;
	void *ret = NULL;
	assert_int_equal(pthread_create(&t, NULL, trylock_thread, &mc.mutex), 0);
	pthread_join(t, &ret);
	assert_int_equal((intptr_t)ret, EBUSY);
	pthread_mutex_unlock(&mc.mutex);
	assert_int_equal(pthread_create(&t, NULL, trylock_thread, &mc.mutex), 0);
	pthread_join(t, &ret);
	assert_int_equal((intptr_t)ret, 0);

	assert_int_equal(pthread_mutex_destroy(&mc.mutex), 0);

	/* libobs initializes static and zeroed mutexes this way */
	pthread_mutex_t init_value;
	pthread_mutex_init_value(&init_value);
	pthread_mutex_init_value(NULL);
	assert_int_equal(pthread_mutex_lock(&init_value), 0);
	assert_int_equal(pthread_mutex_unlock(&init_value), 0);
	pthread_mutex_destroy(&init_value);
}

static void recursive_mutex_test(void **state)
{
	UNUSED_PARAMETER(state);

	pthread_mutex_t mutex;
	assert_int_equal(pthread_mutex_init_recursive(&mutex), 0);

	/* the owner can lock it again; another thread cannot until every lock is released */
	assert_int_equal(pthread_mutex_lock(&mutex), 0);
	assert_int_equal(pthread_mutex_lock(&mutex), 0);
	assert_int_equal(pthread_mutex_trylock(&mutex), 0);

	pthread_t t;
	void *ret = NULL;
	assert_int_equal(pthread_create(&t, NULL, trylock_thread, &mutex), 0);
	pthread_join(t, &ret);
	assert_int_equal((intptr_t)ret, EBUSY);

	pthread_mutex_unlock(&mutex);
	pthread_mutex_unlock(&mutex);
	assert_int_equal(pthread_create(&t, NULL, trylock_thread, &mutex), 0);
	pthread_join(t, &ret);
	assert_int_equal((intptr_t)ret, EBUSY);

	pthread_mutex_unlock(&mutex);
	assert_int_equal(pthread_create(&t, NULL, trylock_thread, &mutex), 0);
	pthread_join(t, &ret);
	assert_int_equal((intptr_t)ret, 0);

	pthread_mutex_destroy(&mutex);
}

struct cond_data {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int ready; /* protected by mutex */
	int woken; /* protected by mutex */
	int waiting; /* protected by mutex */
};

static void *cond_waiter(void *param)
{
	struct cond_data *cd = param;
	pthread_mutex_lock(&cd->mutex);
	cd->waiting++;
	while (!cd->ready)
		pthread_cond_wait(&cd->cond, &cd->mutex);
	cd->woken++;
	pthread_mutex_unlock(&cd->mutex);
	return NULL;
}

static void wait_for_waiters(struct cond_data *cd, int count)
{
	for (int i = 0; i < 5000; i++) {
		pthread_mutex_lock(&cd->mutex);
		int waiting = cd->waiting;
		pthread_mutex_unlock(&cd->mutex);
		if (waiting == count)
			return;
		os_sleep_ms(1);
	}
	fail_msg("waiters did not start");
}

static void cond_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct cond_data cd = {0};
	assert_int_equal(pthread_mutex_init(&cd.mutex, NULL), 0);
	assert_int_equal(pthread_cond_init(&cd.cond, NULL), 0);

	/* signal wakes a waiter */
	pthread_t threads[THREADS];
	assert_int_equal(pthread_create(&threads[0], NULL, cond_waiter, &cd), 0);
	wait_for_waiters(&cd, 1);
	pthread_mutex_lock(&cd.mutex);
	cd.ready = 1;
	pthread_cond_signal(&cd.cond);
	pthread_mutex_unlock(&cd.mutex);
	pthread_join(threads[0], NULL);
	assert_int_equal(cd.woken, 1);

	/* broadcast wakes all of them */
	cd.ready = 0;
	cd.woken = 0;
	cd.waiting = 0;
	for (int i = 0; i < THREADS; i++)
		assert_int_equal(pthread_create(&threads[i], NULL, cond_waiter, &cd), 0);
	wait_for_waiters(&cd, THREADS);
	pthread_mutex_lock(&cd.mutex);
	cd.ready = 1;
	pthread_cond_broadcast(&cd.cond);
	pthread_mutex_unlock(&cd.mutex);
	for (int i = 0; i < THREADS; i++)
		pthread_join(threads[i], NULL);
	assert_int_equal(cd.woken, THREADS);

	/* a timed wait without a signal times out at the absolute (realtime) deadline */
	struct timespec ts;
	assert_int_equal(timespec_get(&ts, TIME_UTC), TIME_UTC);
	ts.tv_nsec += 50 * 1000000;
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	uint64_t start = os_gettime_ns();
	pthread_mutex_lock(&cd.mutex);
	assert_int_equal(pthread_cond_timedwait(&cd.cond, &cd.mutex, &ts), ETIMEDOUT);
	pthread_mutex_unlock(&cd.mutex);
	uint64_t elapsed_ms = (os_gettime_ns() - start) / 1000000;
	assert_true(elapsed_ms >= 30);
	assert_true(elapsed_ms < 5000);

	pthread_cond_destroy(&cd.cond);
	pthread_mutex_destroy(&cd.mutex);
}

static pthread_once_t once_token = PTHREAD_ONCE_INIT;
static volatile long once_calls = 0;

static void once_init(void)
{
	/* slow, so that the other threads must wait for it to finish */
	os_sleep_ms(20);
	os_atomic_inc_long(&once_calls);
}

static void *once_thread(void *param)
{
	UNUSED_PARAMETER(param);
	pthread_once(&once_token, once_init);
	/* pthread_once() returns only after the initializer finished, in every thread */
	return (void *)(intptr_t)os_atomic_load_long(&once_calls);
}

static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static volatile long readers_inside = 0;
static volatile long max_readers_inside = 0;
static volatile bool writer_inside = false;
static volatile bool rwlock_violation = false;

static void *rwlock_thread(void *param)
{
	bool writer = param != NULL;
	for (int i = 0; i < 2000; i++) {
		if (writer) {
			pthread_rwlock_wrlock(&rwlock);
			if (os_atomic_set_bool(&writer_inside, true) || os_atomic_load_long(&readers_inside))
				os_atomic_store_bool(&rwlock_violation, true);
			os_atomic_store_bool(&writer_inside, false);
		} else {
			pthread_rwlock_rdlock(&rwlock);
			long inside = os_atomic_inc_long(&readers_inside);
			if (os_atomic_load_bool(&writer_inside))
				os_atomic_store_bool(&rwlock_violation, true);
			long max = os_atomic_load_long(&max_readers_inside);
			while (inside > max && !os_atomic_compare_exchange_long(&max_readers_inside, &max, inside))
				;
			os_atomic_dec_long(&readers_inside);
		}
		pthread_rwlock_unlock(&rwlock);
	}
	return NULL;
}

static THREAD_LOCAL int thread_local_value = 0;

static void *thread_local_thread(void *param)
{
	/* each thread starts with its own zeroed copy */
	int start = thread_local_value;
	thread_local_value = (int)(intptr_t)param;
	os_sleep_ms(5);
	return (void *)(intptr_t)(start == 0 && thread_local_value == (int)(intptr_t)param);
}

static void *self_thread(void *param)
{
	pthread_t *out = param;
	*out = pthread_self();
	return NULL;
}

static void *detached_thread(void *param)
{
	os_event_signal(param);
	return NULL;
}

static void pthread_misc_test(void **state)
{
	UNUSED_PARAMETER(state);

	pthread_t threads[THREADS];
	for (int i = 0; i < THREADS; i++)
		assert_int_equal(pthread_create(&threads[i], NULL, once_thread, NULL), 0);
	for (int i = 0; i < THREADS; i++) {
		void *ret = NULL;
		pthread_join(threads[i], &ret);
		assert_int_equal((intptr_t)ret, 1);
	}
	pthread_once(&once_token, once_init);
	assert_int_equal(os_atomic_load_long(&once_calls), 1);

	/* readers share the lock, writers have it alone */
	for (int i = 0; i < THREADS; i++)
		assert_int_equal(pthread_create(&threads[i], NULL, rwlock_thread, i < 2 ? (void *)1 : NULL), 0);
	for (int i = 0; i < THREADS; i++)
		pthread_join(threads[i], NULL);
	assert_false(rwlock_violation);

	thread_local_value = 99;
	for (int i = 0; i < THREADS; i++)
		assert_int_equal(pthread_create(&threads[i], NULL, thread_local_thread, (void *)(intptr_t)(i + 1)), 0);
	for (int i = 0; i < THREADS; i++) {
		void *ret = NULL;
		pthread_join(threads[i], &ret);
		assert_int_equal((intptr_t)ret, 1);
	}
	assert_int_equal(thread_local_value, 99);

	/* pthread_self() identifies the thread pthread_create() returned */
	pthread_t self = pthread_self();
	pthread_t seen;
	assert_int_equal(pthread_create(&threads[0], NULL, self_thread, &seen), 0);
	pthread_join(threads[0], NULL);
	assert_true(pthread_equal(seen, threads[0]));
	assert_false(pthread_equal(seen, self));
	assert_true(pthread_equal(self, pthread_self()));

	/* a detached thread runs to completion on its own */
	os_event_t *done;
	assert_int_equal(os_event_init(&done, OS_EVENT_TYPE_MANUAL), 0);
	pthread_t detached;
	assert_int_equal(pthread_create(&detached, NULL, detached_thread, done), 0);
	assert_int_equal(pthread_detach(detached), 0);
	assert_int_equal(os_event_timedwait(done, 5000), 0);
	os_event_destroy(done);
}

/* ------------------------------------------------------------------------- */
/* os_event, os_sem */

struct event_waiter {
	os_event_t *event;
	os_sem_t *started;
	volatile long *woken;
};

static void *event_waiter_thread(void *param)
{
	struct event_waiter *ew = param;
	os_sem_post(ew->started);
	if (os_event_wait(ew->event) == 0)
		os_atomic_inc_long(ew->woken);
	return NULL;
}

static long wait_long_value(volatile long *value, long expected)
{
	for (int i = 0; i < 5000 && os_atomic_load_long(value) != expected; i++)
		os_sleep_ms(1);
	return os_atomic_load_long(value);
}

static void event_test(void **state)
{
	UNUSED_PARAMETER(state);

	os_event_t *ev;

	/* auto: a signal before the wait is kept, a wait consumes it */
	assert_int_equal(os_event_init(&ev, OS_EVENT_TYPE_AUTO), 0);
	assert_int_equal(os_event_try(ev), EAGAIN);
	assert_int_equal(os_event_timedwait(ev, 10), ETIMEDOUT);
	assert_int_equal(os_event_signal(ev), 0);
	assert_int_equal(os_event_signal(ev), 0); /* signals do not count up */
	assert_int_equal(os_event_try(ev), 0);
	assert_int_equal(os_event_try(ev), EAGAIN);
	assert_int_equal(os_event_signal(ev), 0);
	assert_int_equal(os_event_timedwait(ev, 1000), 0);
	assert_int_equal(os_event_try(ev), EAGAIN);
	os_event_signal(ev);
	os_event_reset(ev);
	assert_int_equal(os_event_try(ev), EAGAIN);
	assert_int_equal(os_event_signal(ev), 0);
	assert_int_equal(os_event_wait(ev), 0);

	/* auto: each signal releases one waiter */
	os_sem_t *started;
	assert_int_equal(os_sem_init(&started, 0), 0);
	volatile long woken = 0;
	struct event_waiter ew = {ev, started, &woken};
	pthread_t threads[THREADS];
	for (int i = 0; i < 3; i++)
		assert_int_equal(pthread_create(&threads[i], NULL, event_waiter_thread, &ew), 0);
	for (int i = 0; i < 3; i++)
		os_sem_wait(started);
	for (long i = 1; i <= 3; i++) {
		os_event_signal(ev);
		assert_int_equal(wait_long_value(&woken, i), i);
		/* shortcut: the remaining waiters get a moment to wake by mistake; a broken auto event fails here */
		os_sleep_ms(10);
		assert_int_equal(os_atomic_load_long(&woken), i);
	}
	for (int i = 0; i < 3; i++)
		pthread_join(threads[i], NULL);
	os_event_destroy(ev);

	/* manual: stays signaled for every waiter until reset */
	assert_int_equal(os_event_init(&ev, OS_EVENT_TYPE_MANUAL), 0);
	ew.event = ev;
	woken = 0;
	for (int i = 0; i < THREADS; i++)
		assert_int_equal(pthread_create(&threads[i], NULL, event_waiter_thread, &ew), 0);
	for (int i = 0; i < THREADS; i++)
		os_sem_wait(started);
	assert_int_equal(os_event_signal(ev), 0);
	for (int i = 0; i < THREADS; i++)
		pthread_join(threads[i], NULL);
	assert_int_equal(woken, THREADS);
	assert_int_equal(os_event_try(ev), 0);
	assert_int_equal(os_event_wait(ev), 0);
	assert_int_equal(os_event_timedwait(ev, 0), 0);
	os_event_reset(ev);
	assert_int_equal(os_event_try(ev), EAGAIN);
	assert_int_equal(os_event_timedwait(ev, 10), ETIMEDOUT);
	os_event_destroy(ev);
	os_sem_destroy(started);

	/* NULL */
	assert_int_equal(os_event_wait(NULL), EINVAL);
	assert_int_equal(os_event_timedwait(NULL, 0), EINVAL);
	assert_int_equal(os_event_try(NULL), EINVAL);
	assert_int_equal(os_event_signal(NULL), EINVAL);
	os_event_reset(NULL);
	os_event_destroy(NULL);
}

struct sem_queue {
	os_sem_t *items;
	pthread_mutex_t mutex;
	long produced; /* protected by mutex */
	long consumed; /* protected by mutex */
	int per_thread;
};

static void *sem_producer(void *param)
{
	struct sem_queue *q = param;
	for (int i = 0; i < q->per_thread; i++) {
		pthread_mutex_lock(&q->mutex);
		q->produced++;
		pthread_mutex_unlock(&q->mutex);
		os_sem_post(q->items);
	}
	return NULL;
}

static void *sem_consumer(void *param)
{
	struct sem_queue *q = param;
	for (int i = 0; i < q->per_thread; i++) {
		if (os_sem_wait(q->items) != 0)
			return (void *)1;
		pthread_mutex_lock(&q->mutex);
		/* a wait returns only for a post that happened */
		bool ok = q->consumed < q->produced;
		q->consumed++;
		pthread_mutex_unlock(&q->mutex);
		if (!ok)
			return (void *)1;
	}
	return NULL;
}

static void sem_test(void **state)
{
	UNUSED_PARAMETER(state);

	os_sem_t *sem;
	assert_int_equal(os_sem_init(&sem, 2), 0);
	/* the initial count is available right away */
	assert_int_equal(os_sem_wait(sem), 0);
	assert_int_equal(os_sem_wait(sem), 0);
	assert_int_equal(os_sem_post(sem), 0);
	assert_int_equal(os_sem_post(sem), 0);
	assert_int_equal(os_sem_wait(sem), 0);
	assert_int_equal(os_sem_wait(sem), 0);
	os_sem_destroy(sem);

	struct sem_queue q = {.per_thread = 5000};
	assert_int_equal(os_sem_init(&q.items, 0), 0);
	assert_int_equal(pthread_mutex_init(&q.mutex, NULL), 0);
	pthread_t producers[THREADS / 2], consumers[THREADS / 2];
	for (int i = 0; i < THREADS / 2; i++) {
		assert_int_equal(pthread_create(&consumers[i], NULL, sem_consumer, &q), 0);
		assert_int_equal(pthread_create(&producers[i], NULL, sem_producer, &q), 0);
	}
	for (int i = 0; i < THREADS / 2; i++) {
		void *ret = NULL;
		pthread_join(producers[i], NULL);
		pthread_join(consumers[i], &ret);
		assert_null(ret);
	}
	assert_int_equal(q.produced, (THREADS / 2) * q.per_thread);
	assert_int_equal(q.consumed, q.produced);
	pthread_mutex_destroy(&q.mutex);
	os_sem_destroy(q.items);

	assert_int_equal(os_sem_post(NULL), -1);
	assert_int_equal(os_sem_wait(NULL), -1);
	os_sem_destroy(NULL);
}

/* ------------------------------------------------------------------------- */
/* os_atomic */

static void atomic_single_test(void **state)
{
	UNUSED_PARAMETER(state);

	volatile long l = 5;
	assert_int_equal(os_atomic_inc_long(&l), 6); /* the new value */
	assert_int_equal(os_atomic_dec_long(&l), 5);
	assert_int_equal(os_atomic_set_long(&l, 9), 5); /* the old value */
	assert_int_equal(os_atomic_exchange_long(&l, 3), 9);
	os_atomic_store_long(&l, 4);
	assert_int_equal(os_atomic_load_long(&l), 4);
	assert_true(os_atomic_compare_swap_long(&l, 4, 7));
	assert_false(os_atomic_compare_swap_long(&l, 4, 8));
	assert_int_equal(l, 7);
	long expected = 6;
	/* a failed compare exchange stores the current value in the expected value */
	assert_false(os_atomic_compare_exchange_long(&l, &expected, 1));
	assert_int_equal(expected, 7);
	assert_true(os_atomic_compare_exchange_long(&l, &expected, 1));
	assert_int_equal(l, 1);
	assert_int_equal(expected, 7);

	int a = 1, b = 2;
	void *volatile p = &a;
	assert_ptr_equal(os_atomic_load_ptr(&p), &a);
	assert_ptr_equal(os_atomic_set_ptr(&p, &b), &a);
	void *old = &a;
	assert_false(os_atomic_compare_exchange_ptr(&p, &old, NULL));
	assert_ptr_equal(old, &b);
	assert_true(os_atomic_compare_exchange_ptr(&p, &old, NULL));
	assert_null(os_atomic_load_ptr(&p));

	volatile bool flag = false;
	assert_false(os_atomic_set_bool(&flag, true));
	assert_true(os_atomic_exchange_bool(&flag, false));
	assert_false(os_atomic_load_bool(&flag));
	os_atomic_store_bool(&flag, true);
	assert_true(os_atomic_load_bool(&flag));
}

struct atomic_data {
	volatile long counter;
	volatile long cas_counter;
	volatile bool lock;
	long protected_value; /* protected by lock */
	int loops;
};

static void *atomic_thread(void *param)
{
	struct atomic_data *ad = param;
	for (int i = 0; i < ad->loops; i++) {
		os_atomic_inc_long(&ad->counter);
		os_atomic_inc_long(&ad->counter);
		os_atomic_dec_long(&ad->counter);

		long cur = os_atomic_load_long(&ad->cas_counter);
		while (!os_atomic_compare_exchange_long(&ad->cas_counter, &cur, cur + 2))
			;

		/* a spinlock: the exchange must order the unprotected accesses around it */
		while (os_atomic_exchange_bool(&ad->lock, true))
			;
		ad->protected_value++;
		os_atomic_store_bool(&ad->lock, false);
	}
	return NULL;
}

static void atomic_threads_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct atomic_data ad = {.loops = 50000};
	pthread_t threads[THREADS];
	for (int i = 0; i < THREADS; i++)
		assert_int_equal(pthread_create(&threads[i], NULL, atomic_thread, &ad), 0);
	for (int i = 0; i < THREADS; i++)
		pthread_join(threads[i], NULL);

	assert_int_equal(ad.counter, THREADS * ad.loops);
	assert_int_equal(ad.cas_counter, 2 * THREADS * ad.loops);
	assert_int_equal(ad.protected_value, THREADS * ad.loops);
}

/* ------------------------------------------------------------------------- */
/* os_set_thread_name */

static void *named_thread(void *param)
{
	os_set_thread_name("knurbs-test-thread");
#ifdef _WIN32
	PWSTR desc = NULL;
	if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc))) {
		bool ok = wcscmp(desc, L"knurbs-test-thread") == 0;
		LocalFree(desc);
		*(int *)param = ok ? 1 : -1;
		return NULL;
	}
#endif
	*(int *)param = 1;
	return NULL;
}

static void thread_name_test(void **state)
{
	UNUSED_PARAMETER(state);

	int result = 0;
	pthread_t t;
	assert_int_equal(pthread_create(&t, NULL, named_thread, &result), 0);
	pthread_join(t, NULL);
	assert_int_equal(result, 1);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(mutex_test),        obs_unit_test(recursive_mutex_test), obs_unit_test(cond_test),
		obs_unit_test(pthread_misc_test), obs_unit_test(event_test),           obs_unit_test(sem_test),
		obs_unit_test(atomic_single_test), obs_unit_test(atomic_threads_test), obs_unit_test(thread_name_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
