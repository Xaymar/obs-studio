#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>

#include <util/bmem.h>
#include <util/deque.h>

#define expect_front(dq, literal) expect_front_n(dq, literal, sizeof(literal) - 1)

static void expect_front_n(struct deque *dq, const char *expected, size_t len)
{
	char buf[64] = {0};
	assert_int_equal(dq->size, len);
	deque_peek_front(dq, buf, len);
	assert_memory_equal(buf, expected, len);
}

static void push_pop_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct deque dq;
	deque_init(&dq);

	deque_push_back(&dq, "cd", 2);
	deque_push_front(&dq, "ab", 2); /* wraps to the end of the buffer */
	deque_push_back(&dq, "ef", 2);
	expect_front(&dq, "abcdef");

	char buf[4] = {0};
	deque_peek_back(&dq, buf, 2);
	assert_memory_equal(buf, "ef", 2);
	deque_peek_front(&dq, NULL, 2);
	deque_peek_back(&dq, NULL, 2);

	deque_pop_front(&dq, buf, 1);
	assert_int_equal(buf[0], 'a');
	deque_pop_back(&dq, buf, 1);
	assert_int_equal(buf[0], 'f');
	expect_front(&dq, "bcde");

	assert_int_equal(*(char *)deque_data(&dq, 0), 'b');
	assert_int_equal(*(char *)deque_data(&dq, 3), 'e');
	assert_null(deque_data(&dq, 4));

	deque_pop_front(&dq, NULL, 4);
	assert_int_equal(dq.size, 0);
	assert_int_equal(dq.start_pos, 0);

	deque_push_back(&dq, "xy", 2);
	deque_pop_back(&dq, NULL, 2);
	assert_int_equal(dq.size, 0);

	deque_free(&dq);
	assert_null(dq.data);
}

static void wrap_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct deque dq;
	deque_init(&dq);
	deque_reserve(&dq, 8);
	deque_reserve(&dq, 4);
	assert_int_equal(dq.capacity, 8);

	/* Move the window to the end of the buffer, then wrap a push across the end */
	deque_push_back(&dq, "012345", 6);
	deque_pop_front(&dq, NULL, 5);
	deque_push_back(&dq, "6789", 4); /* 2 bytes at the end, 2 at the start */
	expect_front(&dq, "56789");

	char buf[8] = {0};
	deque_peek_back(&dq, buf, 3); /* spans the wrap */
	assert_memory_equal(buf, "789", 3);
	assert_int_equal(*(char *)deque_data(&dq, 4), '9');

	/* pop_back across the wrap point */
	deque_pop_back(&dq, buf, 3);
	assert_memory_equal(buf, "789", 3);
	expect_front(&dq, "56");

	/* pop_front across the end of the buffer */
	deque_push_back(&dq, "ab", 2);
	deque_pop_front(&dq, buf, 3);
	assert_memory_equal(buf, "56a", 3);
	expect_front(&dq, "b");

	deque_free(&dq);
}

static void front_wrap_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct deque dq;
	deque_init(&dq);
	deque_reserve(&dq, 8);

	deque_push_back(&dq, "abc", 3);
	deque_pop_front(&dq, NULL, 1);   /* start_pos 1 */
	deque_push_front(&dq, "XYZ", 3); /* 1 byte before start, 2 wrap to the end */
	expect_front(&dq, "XYZbc");

	deque_push_front(&dq, "W", 1); /* plain move of start_pos */
	expect_front(&dq, "WXYZbc");

	/* Growing while wrapped reorders the data */
	deque_push_back(&dq, "defghi", 6);
	expect_front(&dq, "WXYZbcdefghi");

	deque_free(&dq);
}

static void zero_and_place_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct deque dq;
	deque_init(&dq);

	deque_push_front_zero(&dq, 2);
	deque_push_back_zero(&dq, 2);
	expect_front(&dq, "\0\0\0\0");

	deque_place(&dq, 1, "ab", 2);
	expect_front(&dq, "\0ab\0");
	deque_place(&dq, 3, "cdef", 4); /* grows */
	expect_front(&dq, "\0abcdef");

	deque_upsize(&dq, 4); /* smaller: no-op */
	deque_upsize(&dq, 9);
	assert_int_equal(dq.size, 9);
	assert_int_equal(*(char *)deque_data(&dq, 8), 0);

	deque_free(&dq);

	/* zero pushes and upsize across the wrap point */
	deque_init(&dq);
	deque_reserve(&dq, 8);
	deque_push_back(&dq, "012345", 6);
	deque_pop_front(&dq, NULL, 5);
	deque_push_back_zero(&dq, 4);
	expect_front(&dq, "5\0\0\0\0");
	deque_pop_back(&dq, NULL, 4);
	deque_upsize(&dq, 5);
	expect_front(&dq, "5\0\0\0\0");

	deque_pop_front(&dq, NULL, 1);
	deque_push_front_zero(&dq, 1);
	deque_pop_front(&dq, NULL, 2);
	deque_push_front_zero(&dq, 4);
	assert_int_equal(dq.size, 7);

	/* place across the end of the buffer */
	deque_free(&dq);
	deque_init(&dq);
	deque_reserve(&dq, 8);
	deque_push_back(&dq, "012345", 6);
	deque_pop_front(&dq, NULL, 5);
	deque_push_back(&dq, "abc", 3);
	deque_place(&dq, 1, "XYZ", 3);
	expect_front(&dq, "5XYZ");

	deque_free(&dq);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(push_pop_test),
		cmocka_unit_test(wrap_test),
		cmocka_unit_test(front_wrap_test),
		cmocka_unit_test(zero_and_place_test),
	};

	int ret = cmocka_run_group_tests(tests, NULL, NULL);
	return ret ? ret : (bnum_allocs() != 0);
}
