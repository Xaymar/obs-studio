#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <util/bmem.h>
#include <util/darray.h>

typedef DARRAY(int) int_array_t;

static void expect_ints(const int_array_t *arr, const int *values, size_t num)
{
	assert_int_equal(arr->num, num);
	for (size_t i = 0; i < num; i++)
		assert_int_equal(arr->array[i], values[i]);
}

static void push_insert_test(void **state)
{
	UNUSED_PARAMETER(state);

	int_array_t arr;
	da_init(arr);

	int v = 1;
	assert_int_equal(da_push_back(arr, &v), 0);
	*(int *)da_push_back_new(arr) = 3;
	v = 2;
	da_insert(arr, 1, &v);
	v = 4;
	da_insert(arr, arr.num, &v);
	*(int *)da_insert_new(arr, 0) = 0;
	*(int *)da_insert_new(arr, arr.num) = 5;
	expect_ints(&arr, (int[]){0, 1, 2, 3, 4, 5}, 6);
	assert_int_equal(*(int *)da_end(arr), 5);

	int more[] = {6, 7};
	assert_int_equal(da_push_back_array(arr, more, 2), 6);
	assert_int_equal(da_push_back_array(arr, NULL, 2), 8);
	assert_int_equal(da_push_back_array(arr, more, 0), 8);
	assert_int_equal(darray_push_back_array(sizeof(int), NULL, more, 2), 0);

	int mid[] = {-1, -2};
	da_insert_array(arr, 1, mid, 2);
	expect_ints(&arr, (int[]){0, -1, -2, 1, 2, 3, 4, 5, 6, 7}, 10);

	int_array_t other;
	da_init(other);
	da_push_back_array(other, more, 2);
	da_insert_da(arr, 0, other);
	da_push_back_da(arr, other);
	expect_ints(&arr, (int[]){6, 7, 0, -1, -2, 1, 2, 3, 4, 5, 6, 7, 6, 7}, 14);
	assert_int_equal(da_alloc_size(arr), 14 * sizeof(int));

	da_free(other);
	da_free(arr);
	assert_null(arr.array);
}

static void erase_test(void **state)
{
	UNUSED_PARAMETER(state);

	int_array_t arr;
	da_init(arr);
	da_push_back_array(arr, ((int[]){0, 1, 2, 3, 4, 5, 6, 7}), 8);

	da_erase(arr, 0);
	int seven = 7;
	da_erase_item(arr, &seven);
	int missing = 42;
	da_erase_item(arr, &missing);
	expect_ints(&arr, (int[]){1, 2, 3, 4, 5, 6}, 6);

	da_erase_range(arr, 1, 2); /* single element */
	da_erase_range(arr, 1, 3); /* middle */
	expect_ints(&arr, (int[]){1, 5, 6}, 3);
	da_erase_range(arr, 1, 3); /* tail, nothing to move */
	expect_ints(&arr, (int[]){1}, 1);

	da_push_back_array(arr, ((int[]){2, 3}), 2);
	da_pop_front(arr);
	da_pop_back(arr);
	expect_ints(&arr, (int[]){2}, 1);
	da_erase(arr, 0);
	assert_int_equal(arr.num, 0);

	da_push_back_array(arr, ((int[]){1, 2}), 2);
	da_erase_range(arr, 0, 2); /* everything */
	assert_int_equal(arr.num, 0);

	da_free(arr);
}

static void find_move_swap_test(void **state)
{
	UNUSED_PARAMETER(state);

	int_array_t arr;
	da_init(arr);
	da_push_back_array(arr, ((int[]){0, 1, 2, 3, 4}), 5);

	int needle = 3;
	assert_int_equal(da_find(arr, &needle, 0), 3);
	assert_int_equal(da_find(arr, &needle, 4), DARRAY_INVALID);

	da_move_item(arr, 4, 1);
	expect_ints(&arr, (int[]){0, 4, 1, 2, 3}, 5);
	da_move_item(arr, 1, 4);
	expect_ints(&arr, (int[]){0, 1, 2, 3, 4}, 5);
	da_move_item(arr, 2, 2);

	da_swap(arr, 0, 4);
	da_swap(arr, 1, 1);
	expect_ints(&arr, (int[]){4, 1, 2, 3, 0}, 5);

	da_free(arr);
}

static void resize_copy_test(void **state)
{
	UNUSED_PARAMETER(state);

	int_array_t arr, copy;
	da_init(arr);
	da_init(copy);

	da_reserve(arr, 0);
	da_reserve(arr, 4);
	assert_int_equal(arr.capacity, 4);
	da_push_back_array(arr, ((int[]){1, 2}), 2);
	da_reserve(arr, 2);
	da_reserve(arr, 8);
	expect_ints(&arr, (int[]){1, 2}, 2);

	da_resize(arr, 4);
	expect_ints(&arr, (int[]){1, 2, 0, 0}, 4);
	da_resize(arr, 4);
	da_resize(arr, 1);
	expect_ints(&arr, (int[]){1}, 1);
	da_resize(arr, 0);
	assert_int_equal(arr.num, 0);

	da_push_back_array(arr, ((int[]){5, 6, 7}), 3);
	da_copy(copy, arr);
	expect_ints(&copy, (int[]){5, 6, 7}, 3);
	da_copy_array(copy, ((int[]){8, 9}), 2);
	expect_ints(&copy, (int[]){8, 9}, 2);

	int_array_t empty;
	da_init(empty);
	da_copy(copy, empty);
	assert_null(copy.array);

	da_move(copy, arr);
	assert_null(arr.array);
	expect_ints(&copy, (int[]){5, 6, 7}, 3);

	da_clear(copy);
	assert_int_equal(copy.num, 0);

	da_push_back_array(arr, ((int[]){1}), 1);
	da_push_back_array(copy, ((int[]){2, 3}), 2);
	da_join(arr, copy);
	assert_null(copy.array);
	expect_ints(&arr, (int[]){1, 2, 3}, 3);

	da_free(arr);
	da_free(copy);
}

/* Regression test: da_split() copied the whole array into the first half and dropped a one-element second half. */
static void split_test(void **state)
{
	UNUSED_PARAMETER(state);

	int_array_t src, a, b;
	da_init(src);
	da_init(a);
	da_init(b);
	da_push_back_array(src, ((int[]){1, 2, 3}), 3);

	da_split(a, b, src, 1);
	expect_ints(&a, (int[]){1}, 1);
	expect_ints(&b, (int[]){2, 3}, 2);

	da_split(a, b, src, 2);
	expect_ints(&a, (int[]){1, 2}, 2);
	expect_ints(&b, (int[]){3}, 1);

	da_split(a, b, src, 0);
	assert_int_equal(a.num, 0);
	expect_ints(&b, (int[]){1, 2, 3}, 3);

	da_split(a, b, src, 3);
	expect_ints(&a, (int[]){1, 2, 3}, 3);
	assert_int_equal(b.num, 0);

	int_array_t empty;
	da_init(empty);
	da_split(a, b, empty, 0);
	assert_int_equal(a.num, 0);
	assert_int_equal(b.num, 0);

	da_free(src);
	da_free(a);
	da_free(b);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(push_insert_test), cmocka_unit_test(erase_test), cmocka_unit_test(find_move_swap_test),
		cmocka_unit_test(resize_copy_test), cmocka_unit_test(split_test),
	};

	int ret = cmocka_run_group_tests(tests, NULL, NULL);
	return ret ? ret : (bnum_allocs() != 0);
}
