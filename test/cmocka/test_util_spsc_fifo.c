#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <util/platform.h>
#include <util/spsc-fifo.h>
#include <util/threading.h>

/* util/spsc-fifo.h: a lock-free single producer, single consumer float ring buffer. One slot always stays free. */

static void basic_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct spsc_fifo fifo = {0};
	assert_false(spsc_fifo_init(NULL, 8));
	assert_false(spsc_fifo_init(&fifo, 1));
	assert_false(spsc_fifo_init(&fifo, LONG_MAX));
	assert_int_equal(spsc_fifo_readable_frames(&fifo), 0);
	assert_int_equal(spsc_fifo_writable_frames(&fifo), 0);
	assert_int_equal(spsc_fifo_readable_frames(NULL), 0);
	assert_int_equal(spsc_fifo_writable_frames(NULL), 0);

	assert_true(spsc_fifo_init(&fifo, 8));
	/* already initialized */
	assert_false(spsc_fifo_init(&fifo, 8));
	assert_int_equal(spsc_fifo_readable_frames(&fifo), 0);
	assert_int_equal(spsc_fifo_writable_frames(&fifo), 7);

	float in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	float out[8] = {0};
	spsc_fifo_write(&fifo, in, 5);
	assert_int_equal(spsc_fifo_readable_frames(&fifo), 5);
	assert_int_equal(spsc_fifo_writable_frames(&fifo), 2);

	/* a write that does not fit is dropped whole */
	spsc_fifo_write(&fifo, in, 3);
	assert_int_equal(spsc_fifo_readable_frames(&fifo), 5);

	assert_int_equal(spsc_fifo_read(&fifo, out, 3), 3);
	assert_true(out[0] == 1 && out[1] == 2 && out[2] == 3);

	/* wraps around the end */
	spsc_fifo_write(&fifo, in + 5, 3);
	spsc_fifo_write(&fifo, in, 2);
	assert_int_equal(spsc_fifo_readable_frames(&fifo), 7);
	assert_int_equal(spsc_fifo_writable_frames(&fifo), 0);
	assert_int_equal(spsc_fifo_read(&fifo, out, 7), 7);
	const float expect[7] = {4, 5, 6, 7, 8, 1, 2};
	assert_memory_equal(out, expect, sizeof(expect));
	assert_int_equal(spsc_fifo_readable_frames(&fifo), 0);

	/* a short read fills the rest with silence */
	spsc_fifo_write(&fifo, in, 2);
	for (int i = 0; i < 8; i++)
		out[i] = -1.0f;
	assert_int_equal(spsc_fifo_read(&fifo, out, 5), 2);
	assert_true(out[0] == 1 && out[1] == 2 && out[2] == 0 && out[3] == 0 && out[4] == 0 && out[5] == -1);
	assert_int_equal(spsc_fifo_read(&fifo, out, 1), 0);
	assert_true(out[0] == 0);

	/* discard */
	spsc_fifo_write(&fifo, in, 6);
	assert_false(spsc_fifo_discard(&fifo, 7));
	assert_false(spsc_fifo_discard(&fifo, 0));
	assert_false(spsc_fifo_discard(&fifo, 8));
	assert_true(spsc_fifo_discard(&fifo, 4));
	assert_int_equal(spsc_fifo_read(&fifo, out, 2), 2);
	assert_true(out[0] == 5 && out[1] == 6);
	spsc_fifo_write(&fifo, in, 6);
	assert_true(spsc_fifo_discard(&fifo, 6));
	assert_int_equal(spsc_fifo_readable_frames(&fifo), 0);

	/* clear drops everything readable */
	spsc_fifo_write(&fifo, in, 4);
	spsc_fifo_clear(&fifo);
	assert_int_equal(spsc_fifo_readable_frames(&fifo), 0);
	assert_int_equal(spsc_fifo_writable_frames(&fifo), 7);

	/* invalid arguments are ignored */
	spsc_fifo_write(&fifo, NULL, 1);
	spsc_fifo_write(&fifo, in, 0);
	spsc_fifo_write(&fifo, in, 8);
	spsc_fifo_write(NULL, in, 1);
	assert_int_equal(spsc_fifo_readable_frames(&fifo), 0);
	assert_int_equal(spsc_fifo_read(&fifo, NULL, 1), 0);
	assert_int_equal(spsc_fifo_read(&fifo, out, 0), 0);
	assert_int_equal(spsc_fifo_read(&fifo, out, 8), 0);
	assert_int_equal(spsc_fifo_read(NULL, out, 1), 0);
	assert_false(spsc_fifo_discard(NULL, 1));
	spsc_fifo_clear(NULL);

	spsc_fifo_free(&fifo);
	assert_null(fifo.data);
	assert_int_equal(spsc_fifo_readable_frames(&fifo), 0);
	assert_false(spsc_fifo_discard(&fifo, 1));
	spsc_fifo_clear(&fifo);
	spsc_fifo_free(&fifo);
	spsc_fifo_free(NULL);
	/* reusable after free */
	assert_true(spsc_fifo_init(&fifo, 4));
	spsc_fifo_free(&fifo);
}

#define TOTAL 2000000
#define CAPACITY 257

struct stream {
	struct spsc_fifo fifo;
	volatile bool failed;
	volatile bool producer_done;
};

/* writes 0, 1, 2, ... in chunks of varying size, waiting for room instead of dropping */
static void *producer_thread(void *param)
{
	struct stream *s = param;
	float chunk[64];
	long next = 0;
	unsigned int size = 1;

	while (next < TOTAL) {
		size = size * 1103515245u + 12345u;
		long frames = 1 + (long)((size >> 16) % 64);
		if (frames > TOTAL - next)
			frames = TOTAL - next;

		long writable = spsc_fifo_writable_frames(&s->fifo);
		if (writable < 0 || writable > CAPACITY - 1) {
			os_atomic_store_bool(&s->failed, true);
			break;
		}
		if (writable < frames) {
			if (os_atomic_load_bool(&s->failed))
				break;
			continue;
		}

		for (long i = 0; i < frames; i++)
			chunk[i] = (float)(next + i);
		spsc_fifo_write(&s->fifo, chunk, frames);
		next += frames;
	}

	os_atomic_store_bool(&s->producer_done, true);
	return NULL;
}

/* reads everything back and checks nothing was lost, repeated or torn */
static void *consumer_thread(void *param)
{
	struct stream *s = param;
	float chunk[100];
	long expected = 0;
	unsigned int size = 7;

	while (expected < TOTAL) {
		size = size * 1103515245u + 12345u;
		long want = 1 + (long)((size >> 16) % 100);

		long readable = spsc_fifo_readable_frames(&s->fifo);
		if (readable < 0 || readable > CAPACITY - 1) {
			os_atomic_store_bool(&s->failed, true);
			break;
		}
		if (!readable) {
			if (os_atomic_load_bool(&s->failed))
				break;
			if (os_atomic_load_bool(&s->producer_done) && !spsc_fifo_readable_frames(&s->fifo))
				break;
			continue;
		}

		long got = spsc_fifo_read(&s->fifo, chunk, want);
		/* a single consumer never gets fewer frames than it saw readable */
		if (got < (readable < want ? readable : want)) {
			os_atomic_store_bool(&s->failed, true);
			break;
		}
		for (long i = 0; i < got; i++) {
			if (chunk[i] != (float)expected++) {
				os_atomic_store_bool(&s->failed, true);
				return NULL;
			}
		}
	}

	if (expected != TOTAL)
		os_atomic_store_bool(&s->failed, true);
	return NULL;
}

static void concurrent_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* floats are exact up to 2^24, well above TOTAL */
	struct stream s = {0};
	assert_true(spsc_fifo_init(&s.fifo, CAPACITY));

	pthread_t producer, consumer;
	assert_int_equal(pthread_create(&consumer, NULL, consumer_thread, &s), 0);
	assert_int_equal(pthread_create(&producer, NULL, producer_thread, &s), 0);
	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);

	assert_false(s.failed);
	assert_int_equal(spsc_fifo_readable_frames(&s.fifo), 0);
	spsc_fifo_free(&s.fifo);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(basic_test),
		obs_unit_test(concurrent_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
