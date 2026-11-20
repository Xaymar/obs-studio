#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <math.h>
#include <string.h>

#include <obs-data.h>
#include <graphics/quat.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/platform.h>

/* the autoselect API is deprecated but still part of obs-data.c */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(disable : 4996)
#endif

static void values_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "s", "text");
	obs_data_set_int(data, "i", -42);
	obs_data_set_double(data, "d", 2.5);
	obs_data_set_bool(data, "b", true);
	obs_data_t *child = obs_data_create();
	obs_data_set_int(child, "nested", 7);
	obs_data_set_obj(data, "o", child);
	obs_data_array_t *arr = obs_data_array_create();
	obs_data_array_push_back(arr, child);
	obs_data_set_array(data, "a", arr);

	assert_string_equal(obs_data_get_string(data, "s"), "text");
	assert_int_equal(obs_data_get_int(data, "i"), -42);
	assert_true(obs_data_get_double(data, "d") == 2.5);
	assert_true(obs_data_get_bool(data, "b"));
	/* numbers convert both ways */
	assert_true(obs_data_get_double(data, "i") == -42.0);
	assert_int_equal(obs_data_get_int(data, "d"), 2);

	obs_data_t *got = obs_data_get_obj(data, "o");
	assert_int_equal(obs_data_get_int(got, "nested"), 7);
	obs_data_release(got);
	obs_data_array_t *got_arr = obs_data_get_array(data, "a");
	assert_int_equal(obs_data_array_count(got_arr), 1);
	obs_data_array_release(got_arr);

	/* missing values and wrong types */
	assert_string_equal(obs_data_get_string(data, "missing"), "");
	assert_int_equal(obs_data_get_int(data, "s"), 0);
	assert_false(obs_data_get_bool(data, "missing"));
	assert_null(obs_data_get_obj(data, "missing"));
	assert_null(obs_data_get_array(data, "s"));

	/* replacing a value with one of a different type */
	obs_data_set_string(data, "i", "now a string");
	assert_string_equal(obs_data_get_string(data, "i"), "now a string");
	obs_data_set_string(data, "s", NULL);
	obs_data_set_obj(data, "o", NULL);
	obs_data_set_array(data, "a", NULL);

	obs_data_erase(data, "b");
	assert_false(obs_data_has_user_value(data, "b"));
	obs_data_erase(data, "never-existed");

	obs_data_t *copy = obs_data_create();
	obs_data_apply(copy, data);
	assert_true(obs_data_get_double(copy, "d") == 2.5);
	obs_data_apply(copy, NULL);
	obs_data_apply(NULL, data);
	obs_data_apply(copy, copy);
	obs_data_clear(copy);
	assert_false(obs_data_has_user_value(copy, "d"));

	obs_data_addref(copy);
	obs_data_release(copy);
	obs_data_release(copy);
	obs_data_array_release(arr);
	obs_data_release(child);
	obs_data_release(data);

	/* NULL is safe */
	obs_data_addref(NULL);
	obs_data_release(NULL);
	assert_string_equal(obs_data_get_string(NULL, "x"), "");
	obs_data_set_int(NULL, "x", 1);
	/* Regression: a NULL name crashed in strlen() */
	data = obs_data_create();
	obs_data_set_int(data, NULL, 1);
	obs_data_set_default_string(data, NULL, "x");
	assert_int_equal(obs_data_get_int(data, NULL), 0);
	assert_false(obs_data_has_user_value(data, NULL));
	assert_null(obs_data_item_byname(data, NULL));
	obs_data_erase(data, NULL);
	assert_null(obs_data_first(data));
	obs_data_release(data);
}

static void defaults_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_data_t *data = obs_data_create();
	obs_data_t *child = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();

	obs_data_set_default_string(data, "s", "default");
	obs_data_set_default_int(data, "i", 5);
	obs_data_set_default_double(data, "d", 1.5);
	obs_data_set_default_bool(data, "b", true);
	obs_data_set_default_obj(data, "o", child);
	obs_data_set_default_array(data, "a", arr);

	assert_string_equal(obs_data_get_string(data, "s"), "default");
	assert_int_equal(obs_data_get_default_int(data, "i"), 5);
	assert_true(obs_data_get_default_double(data, "d") == 1.5);
	assert_true(obs_data_get_default_bool(data, "b"));
	assert_string_equal(obs_data_get_default_string(data, "s"), "default");
	obs_data_t *o = obs_data_get_default_obj(data, "o");
	assert_ptr_equal(o, child);
	obs_data_release(o);
	obs_data_array_t *a = obs_data_get_default_array(data, "a");
	assert_ptr_equal(a, arr);
	obs_data_array_release(a);
	assert_true(obs_data_has_default_value(data, "s"));
	assert_false(obs_data_has_user_value(data, "s"));

	/* a user value wins, unsetting it brings the default back */
	obs_data_set_string(data, "s", "user");
	assert_string_equal(obs_data_get_string(data, "s"), "user");
	obs_data_unset_user_value(data, "s");
	assert_string_equal(obs_data_get_string(data, "s"), "default");
	obs_data_unset_default_value(data, "s");
	assert_false(obs_data_has_default_value(data, "s"));

	/* autoselect values */
	obs_data_set_autoselect_string(data, "as", "auto");
	obs_data_set_autoselect_int(data, "ai", 9);
	obs_data_set_autoselect_double(data, "ad", 9.5);
	obs_data_set_autoselect_bool(data, "ab", true);
	obs_data_set_autoselect_obj(data, "ao", child);
	obs_data_set_autoselect_array(data, "aa", arr);
	assert_string_equal(obs_data_get_autoselect_string(data, "as"), "auto");
	assert_int_equal(obs_data_get_autoselect_int(data, "ai"), 9);
	assert_true(obs_data_get_autoselect_double(data, "ad") == 9.5);
	assert_true(obs_data_get_autoselect_bool(data, "ab"));
	o = obs_data_get_autoselect_obj(data, "ao");
	obs_data_release(o);
	a = obs_data_get_autoselect_array(data, "aa");
	obs_data_array_release(a);
	assert_true(obs_data_has_autoselect_value(data, "as"));
	obs_data_unset_autoselect_value(data, "as");
	assert_false(obs_data_has_autoselect_value(data, "as"));

	/* the defaults as their own object */
	obs_data_t *defaults = obs_data_get_defaults(data);
	assert_int_equal(obs_data_get_int(defaults, "i"), 5);
	obs_data_release(defaults);

	obs_data_array_release(arr);
	obs_data_release(child);
	obs_data_release(data);
}

static void math_types_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_data_t *data = obs_data_create();
	struct vec2 v2, r2;
	struct vec3 v3, r3;
	struct vec4 v4, r4;
	struct quat q, rq;
	vec2_set(&v2, 1, 2);
	vec3_set(&v3, 1, 2, 3);
	vec4_set(&v4, 1, 2, 3, 4);
	quat_set(&q, 0, 0, 0, 1);

	obs_data_set_vec2(data, "v2", &v2);
	obs_data_set_vec3(data, "v3", &v3);
	obs_data_set_vec4(data, "v4", &v4);
	obs_data_set_quat(data, "q", &q);
	obs_data_get_vec2(data, "v2", &r2);
	obs_data_get_vec3(data, "v3", &r3);
	obs_data_get_vec4(data, "v4", &r4);
	obs_data_get_quat(data, "q", &rq);
	assert_true(r2.y == 2.0f && r3.z == 3.0f && r4.w == 4.0f && rq.w == 1.0f);

	obs_data_set_default_vec2(data, "dv2", &v2);
	obs_data_set_default_vec3(data, "dv3", &v3);
	obs_data_set_default_vec4(data, "dv4", &v4);
	obs_data_set_default_quat(data, "dq", &q);
	obs_data_get_default_vec2(data, "dv2", &r2);
	obs_data_get_default_vec3(data, "dv3", &r3);
	obs_data_get_default_vec4(data, "dv4", &r4);
	obs_data_get_default_quat(data, "dq", &rq);
	assert_true(r2.x == 1.0f && r3.y == 2.0f && r4.z == 3.0f);

	obs_data_set_autoselect_vec2(data, "av2", &v2);
	obs_data_set_autoselect_vec3(data, "av3", &v3);
	obs_data_set_autoselect_vec4(data, "av4", &v4);
	obs_data_set_autoselect_quat(data, "aq", &q);
	obs_data_get_autoselect_vec2(data, "av2", &r2);
	obs_data_get_autoselect_vec3(data, "av3", &r3);
	obs_data_get_autoselect_vec4(data, "av4", &r4);
	obs_data_get_autoselect_quat(data, "aq", &rq);

	/* a missing value leaves the destination as it was, so callers can pre-fill defaults */
	vec3_set(&r3, 7, 7, 7);
	obs_data_get_vec3(data, "missing", &r3);
	assert_true(r3.x == 7.0f);

	struct media_frames_per_second fps = {30000, 1001}, got;
	const char *option = NULL;
	obs_data_set_frames_per_second(data, "fps", fps, NULL);
	assert_true(obs_data_get_frames_per_second(data, "fps", &got, &option));
	assert_int_equal(got.numerator, 30000);
	/* an option is stored instead of the rate */
	obs_data_set_frames_per_second(data, "opt", fps, "custom");
	assert_true(obs_data_get_frames_per_second(data, "opt", &got, &option));
	assert_string_equal(option, "custom");
	obs_data_set_default_frames_per_second(data, "dfps", fps, NULL);
	assert_true(obs_data_get_default_frames_per_second(data, "dfps", &got, NULL));
	obs_data_set_autoselect_frames_per_second(data, "afps", fps, NULL);
	assert_true(obs_data_get_autoselect_frames_per_second(data, "afps", &got, NULL));
	assert_false(obs_data_get_frames_per_second(data, "missing", &got, NULL));
	obs_data_set_string(data, "badfps", "nope");
	assert_false(obs_data_get_frames_per_second(data, "badfps", &got, NULL));

	obs_data_release(data);
}

static void item_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_data_t *data = obs_data_create_from_json(
		"{\"s\": \"str\", \"i\": 3, \"d\": 0.5, \"b\": false, \"o\": {\"x\": 1}, \"a\": [{\"y\": 2}], \"n\": null}");
	assert_non_null(data);

	int count = 0;
	for (obs_data_item_t *item = obs_data_first(data); item; obs_data_item_next(&item)) {
		assert_non_null(obs_data_item_get_name(item));
		obs_data_item_gettype(item);
		obs_data_item_numtype(item);
		obs_data_item_has_user_value(item);
		obs_data_item_has_default_value(item);
		obs_data_item_has_autoselect_value(item);
		count++;
	}
	assert_true(count >= 6);

	obs_data_item_t *item = obs_data_item_byname(data, "i");
	assert_int_equal(obs_data_item_gettype(item), OBS_DATA_NUMBER);
	assert_int_equal(obs_data_item_numtype(item), OBS_DATA_NUM_INT);
	assert_int_equal(obs_data_item_get_int(item), 3);
	obs_data_item_set_int(&item, 4);
	obs_data_item_set_default_int(&item, 1);
	obs_data_item_set_autoselect_int(&item, 2);
	assert_int_equal(obs_data_item_get_default_int(item), 1);
	assert_int_equal(obs_data_item_get_autoselect_int(item), 2);
	obs_data_item_unset_user_value(item);
	obs_data_item_unset_default_value(item);
	obs_data_item_unset_autoselect_value(item);
	obs_data_item_release(&item);

	item = obs_data_item_byname(data, "s");
	assert_string_equal(obs_data_item_get_string(item), "str");
	obs_data_item_set_string(&item, "longer string value");
	obs_data_item_set_default_string(&item, "d");
	obs_data_item_set_autoselect_string(&item, "a");
	assert_string_equal(obs_data_item_get_default_string(item), "d");
	assert_string_equal(obs_data_item_get_autoselect_string(item), "a");
	obs_data_item_release(&item);

	item = obs_data_item_byname(data, "d");
	assert_int_equal(obs_data_item_numtype(item), OBS_DATA_NUM_DOUBLE);
	obs_data_item_set_double(&item, 1.25);
	obs_data_item_set_default_double(&item, 2.25);
	obs_data_item_set_autoselect_double(&item, 3.25);
	assert_true(obs_data_item_get_double(item) == 1.25);
	assert_true(obs_data_item_get_default_double(item) == 2.25);
	assert_true(obs_data_item_get_autoselect_double(item) == 3.25);
	obs_data_item_release(&item);

	item = obs_data_item_byname(data, "b");
	obs_data_item_set_bool(&item, true);
	obs_data_item_set_default_bool(&item, true);
	obs_data_item_set_autoselect_bool(&item, true);
	assert_true(obs_data_item_get_bool(item));
	assert_true(obs_data_item_get_default_bool(item));
	assert_true(obs_data_item_get_autoselect_bool(item));
	obs_data_item_release(&item);

	item = obs_data_item_byname(data, "o");
	obs_data_t *o = obs_data_item_get_obj(item);
	assert_int_equal(obs_data_get_int(o, "x"), 1);
	obs_data_item_set_obj(&item, o);
	obs_data_item_set_default_obj(&item, o);
	obs_data_item_set_autoselect_obj(&item, o);
	obs_data_t *d = obs_data_item_get_default_obj(item);
	obs_data_release(d);
	d = obs_data_item_get_autoselect_obj(item);
	obs_data_release(d);
	obs_data_release(o);
	obs_data_item_release(&item);

	item = obs_data_item_byname(data, "a");
	obs_data_array_t *a = obs_data_item_get_array(item);
	obs_data_item_set_array(&item, a);
	obs_data_item_set_default_array(&item, a);
	obs_data_item_set_autoselect_array(&item, a);
	obs_data_array_t *da = obs_data_item_get_default_array(item);
	obs_data_array_release(da);
	da = obs_data_item_get_autoselect_array(item);
	obs_data_array_release(da);
	obs_data_array_release(a);
	obs_data_item_release(&item);

	struct media_frames_per_second fps = {60, 1}, got;
	obs_data_set_frames_per_second(data, "fps", fps, NULL);
	item = obs_data_item_byname(data, "fps");
	obs_data_item_set_frames_per_second(&item, fps, NULL); /* an option would replace the rate */
	obs_data_item_set_default_frames_per_second(&item, fps, NULL);
	obs_data_item_set_autoselect_frames_per_second(&item, fps, NULL);
	assert_true(obs_data_item_get_frames_per_second(item, &got, NULL));
	assert_true(obs_data_item_get_default_frames_per_second(item, &got, NULL));
	assert_true(obs_data_item_get_autoselect_frames_per_second(item, &got, NULL));
	obs_data_item_release(&item);

	/* Regression: remove dropped only the caller's reference, the parent's leaked */
	item = obs_data_item_byname(data, "s");
	obs_data_item_remove(&item);
	assert_null(item);
	assert_null(obs_data_item_byname(data, "s"));
	assert_null(obs_data_item_byname(data, "missing"));

	/* NULL items */
	obs_data_item_release(NULL);
	item = NULL;
	obs_data_item_release(&item);
	assert_false(obs_data_item_next(&item));
	assert_null(obs_data_item_get_name(NULL));
	assert_int_equal(obs_data_item_get_int(NULL), 0);

	obs_data_release(data);
}

static void array_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_data_array_t *arr = obs_data_array_create();
	obs_data_t *items[3];
	for (int i = 0; i < 3; i++) {
		items[i] = obs_data_create();
		obs_data_set_int(items[i], "n", i);
	}
	assert_int_equal(obs_data_array_push_back(arr, items[0]), 0);
	assert_int_equal(obs_data_array_push_back(arr, items[2]), 1);
	obs_data_array_insert(arr, 1, items[1]);
	assert_int_equal(obs_data_array_count(arr), 3);
	for (size_t i = 0; i < 3; i++) {
		obs_data_t *item = obs_data_array_item(arr, i);
		assert_int_equal(obs_data_get_int(item, "n"), (long long)i);
		obs_data_release(item);
	}
	assert_null(obs_data_array_item(arr, 10));

	obs_data_array_t *other = obs_data_array_create();
	obs_data_array_push_back_array(other, arr);
	assert_int_equal(obs_data_array_count(other), 3);
	obs_data_array_erase(other, 0);
	assert_int_equal(obs_data_array_count(other), 2);

	int sum = 0;
	obs_data_array_enum(arr, (void (*)(obs_data_t *, void *))NULL, NULL);
	obs_data_array_enum(NULL, NULL, &sum);

	obs_data_array_addref(arr);
	obs_data_array_release(arr);
	obs_data_array_release(other);
	obs_data_array_release(arr);
	for (int i = 0; i < 3; i++)
		obs_data_release(items[i]);

	assert_int_equal(obs_data_array_count(NULL), 0);
	assert_int_equal(obs_data_array_push_back(NULL, NULL), 0);
	obs_data_array_release(NULL);
	obs_data_array_addref(NULL);
}

static void json_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "name", "value");
	obs_data_set_int(data, "int", 1);
	obs_data_set_double(data, "dbl", 0.25);
	obs_data_set_bool(data, "flag", true);
	obs_data_set_default_int(data, "def", 5);
	obs_data_t *child = obs_data_create();
	obs_data_set_string(child, "k", "v");
	obs_data_set_obj(data, "child", child);
	obs_data_array_t *arr = obs_data_array_create();
	obs_data_array_push_back(arr, child);
	obs_data_set_array(data, "list", arr);

	const char *json = obs_data_get_json(data);
	assert_non_null(strstr(json, "\"name\""));
	assert_null(strstr(json, "\"def\""));
	assert_non_null(strstr(obs_data_get_json_with_defaults(data), "\"def\""));
	assert_non_null(strstr(obs_data_get_json_pretty(data), "\n"));
	assert_non_null(strstr(obs_data_get_json_pretty_with_defaults(data), "\"def\""));
	assert_non_null(obs_data_get_last_json(data));

	obs_data_t *parsed = obs_data_create_from_json(obs_data_get_json(data));
	assert_string_equal(obs_data_get_string(parsed, "name"), "value");
	obs_data_release(parsed);

	/* files, plain and safe */
	assert_true(obs_data_save_json(data, "data.json"));
	parsed = obs_data_create_from_json_file("data.json");
	assert_true(obs_data_get_bool(parsed, "flag"));
	obs_data_release(parsed);
	assert_true(obs_data_save_json_safe(data, "data.json", "tmp", "bak"));
	assert_true(obs_data_save_json_pretty_safe(data, "data.json", "tmp", "bak"));
	parsed = obs_data_create_from_json_file_safe("data.json", "bak");
	assert_int_equal(obs_data_get_int(parsed, "int"), 1);
	obs_data_release(parsed);

	/* a broken file falls back to the backup */
	assert_true(os_quick_write_utf8_file("data.json", "{ broken", 8, false));
	parsed = obs_data_create_from_json_file_safe("data.json", "bak");
	assert_non_null(parsed);
	assert_string_equal(obs_data_get_string(parsed, "name"), "value");
	obs_data_release(parsed);
	parsed = obs_data_create_from_json_file_safe("data.json", NULL);
	obs_data_release(parsed);

	assert_null(obs_data_create_from_json("{ not json"));
	assert_null(obs_data_create_from_json(NULL));
	assert_null(obs_data_create_from_json_file("missing.json"));
	assert_false(obs_data_save_json(data, "no-such-dir/x.json"));
	assert_false(obs_data_save_json_safe(data, "no-such-dir/x.json", "tmp", NULL));
	assert_false(obs_data_save_json(NULL, "x.json"));

	obs_data_array_release(arr);
	obs_data_release(child);
	obs_data_release(data);
	os_unlink("data.json");
	os_unlink("data.json.bak");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(values_test), obs_unit_test(defaults_test), obs_unit_test(math_types_test),
		obs_unit_test(item_test),   obs_unit_test(array_test),    obs_unit_test(json_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
