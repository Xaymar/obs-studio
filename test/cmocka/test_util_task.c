#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <util/platform.h>
#include <util/task.h>
#include <util/threading.h>

/* util/task.c: one worker thread per queue, tasks run in order, os_task_queue_wait() blocks until the queue has
 * run everything queued before it */

#define PRODUCERS 4
#define PER_PRODUCER 2000

struct order_data {
	os_task_queue_t *tq;
	pthread_mutex_t mutex;
	long last[PRODUCERS]; /* protected by mutex */
	long ran;             /* protected by mutex */
	bool out_of_order;    /* protected by mutex */
	bool outside;         /* protected by mutex */
};

struct order_task {
	struct order_data *od;
	int producer;
	long seq;
};

static struct order_task tasks[PRODUCERS][PER_PRODUCER];

static void order_task_run(void *param)
{
	struct order_task *t = param;
	struct order_data *od = t->od;
	pthread_mutex_lock(&od->mutex);
	/* tasks from one producer run in the order it queued them */
	if (t->seq != od->last[t->producer] + 1)
		od->out_of_order = true;
	od->last[t->producer] = t->seq;
	if (!os_task_queue_inside(od->tq))
		od->outside = true;
	od->ran++;
	pthread_mutex_unlock(&od->mutex);
}

static void *producer_thread(void *param)
{
	struct order_task *mine = param;
	for (int i = 0; i < PER_PRODUCER; i++) {
		if (!os_task_queue_queue_task(mine[i].od->tq, order_task_run, &mine[i]))
			return (void *)1;
	}
	return NULL;
}

static void order_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct order_data od = {0};
	od.tq = os_task_queue_create();
	assert_non_null(od.tq);
	assert_int_equal(pthread_mutex_init(&od.mutex, NULL), 0);
	for (int p = 0; p < PRODUCERS; p++) {
		od.last[p] = -1;
		for (int i = 0; i < PER_PRODUCER; i++)
			tasks[p][i] = (struct order_task){&od, p, i};
	}

	pthread_t producers[PRODUCERS];
	for (int p = 0; p < PRODUCERS; p++)
		assert_int_equal(pthread_create(&producers[p], NULL, producer_thread, tasks[p]), 0);
	for (int p = 0; p < PRODUCERS; p++) {
		void *ret = NULL;
		pthread_join(producers[p], &ret);
		assert_null(ret);
	}

	/* everything queued before the wait has run when it returns */
	os_task_queue_wait(od.tq);
	pthread_mutex_lock(&od.mutex);
	assert_int_equal(od.ran, PRODUCERS * PER_PRODUCER);
	assert_false(od.out_of_order);
	assert_false(od.outside);
	pthread_mutex_unlock(&od.mutex);
	assert_false(os_task_queue_inside(od.tq));

	os_task_queue_destroy(od.tq);
	pthread_mutex_destroy(&od.mutex);
}

struct block_data {
	os_event_t *go;
	volatile long ran;
};

static void block_task(void *param)
{
	struct block_data *bd = param;
	os_event_wait(bd->go);
	os_atomic_inc_long(&bd->ran);
}

static void count_task(void *param)
{
	struct block_data *bd = param;
	os_atomic_inc_long(&bd->ran);
}

static void *signal_later(void *param)
{
	struct block_data *bd = param;
	/* shortcut: gives the main thread time to enter os_task_queue_wait(); a slower machine only makes the test
	 * pass for the wrong reason (the tasks already ran), never fail */
	os_sleep_ms(50);
	os_event_signal(bd->go);
	return NULL;
}

static void wait_test(void **state)
{
	UNUSED_PARAMETER(state);

	os_task_queue_t *tq = os_task_queue_create();
	assert_non_null(tq);

	/* nothing queued: the wait returns at once, and no task ran during it */
	assert_false(os_task_queue_wait(tq));

	/* tasks that run while waiting are reported */
	struct block_data bd = {0};
	assert_int_equal(os_event_init(&bd.go, OS_EVENT_TYPE_MANUAL), 0);
	assert_true(os_task_queue_queue_task(tq, block_task, &bd));
	assert_true(os_task_queue_queue_task(tq, count_task, &bd));
	pthread_t t;
	assert_int_equal(pthread_create(&t, NULL, signal_later, &bd), 0);
	bool processed = os_task_queue_wait(tq);
	assert_int_equal(os_atomic_load_long(&bd.ran), 2);
	pthread_join(t, NULL);
	if (!processed)
		print_message("note: the tasks ran before os_task_queue_wait() started\n");

	/* destroying runs what is still queued before the thread stops */
	os_event_reset(bd.go);
	assert_true(os_task_queue_queue_task(tq, block_task, &bd));
	assert_true(os_task_queue_queue_task(tq, count_task, &bd));
	assert_true(os_task_queue_queue_task(tq, count_task, &bd));
	os_event_signal(bd.go);
	os_task_queue_destroy(tq);
	assert_int_equal(os_atomic_load_long(&bd.ran), 5);
	os_event_destroy(bd.go);

	/* NULL */
	assert_false(os_task_queue_queue_task(NULL, count_task, NULL));
	assert_false(os_task_queue_wait(NULL));
	os_task_queue_destroy(NULL);
}

struct inside_data {
	os_task_queue_t *self;
	os_task_queue_t *other;
	volatile long result;
};

static void inside_task(void *param)
{
	struct inside_data *id = param;
	/* each queue knows its own thread */
	long r = os_task_queue_inside(id->self) && !os_task_queue_inside(id->other) ? 1 : -1;
	os_atomic_set_long(&id->result, r);
}

static void inside_test(void **state)
{
	UNUSED_PARAMETER(state);

	os_task_queue_t *a = os_task_queue_create();
	os_task_queue_t *b = os_task_queue_create();
	assert_non_null(a);
	assert_non_null(b);

	struct inside_data id = {a, b, 0};
	assert_true(os_task_queue_queue_task(a, inside_task, &id));
	os_task_queue_wait(a);
	assert_int_equal(os_atomic_load_long(&id.result), 1);

	struct inside_data id2 = {b, a, 0};
	assert_true(os_task_queue_queue_task(b, inside_task, &id2));
	os_task_queue_wait(b);
	assert_int_equal(os_atomic_load_long(&id2.result), 1);

	assert_false(os_task_queue_inside(a));
	/* Regression: os_task_queue_inside(NULL) dereferenced NULL */
	assert_false(os_task_queue_inside(NULL));

	os_task_queue_destroy(a);
	os_task_queue_destroy(b);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(order_test),
		obs_unit_test(wait_test),
		obs_unit_test(inside_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
