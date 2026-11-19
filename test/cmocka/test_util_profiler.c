#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <util/platform.h>
#include <util/profiler.h>

static const char *root_name = "root";
static const char *child_name = "child";
static const char *other_root = "other";

static bool count_entries(void *context, profiler_snapshot_entry_t *entry)
{
	size_t *count = context;
	(*count)++;
	assert_non_null(profiler_snapshot_entry_name(entry));
	return true;
}

static bool find_root(void *context, profiler_snapshot_entry_t *entry)
{
	profiler_snapshot_entry_t **out = context;
	if (strcmp(profiler_snapshot_entry_name(entry), root_name) == 0) {
		*out = entry;
		return false;
	}
	return true;
}

static bool keep_root_only(void *data, const char *name, bool *remove)
{
	UNUSED_PARAMETER(data);
	*remove = strcmp(name, root_name) != 0;
	return true;
}

static void profiler_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* calls before profiler_start() are ignored */
	profile_start(root_name);
	profile_end(root_name);

	/* a call while stopped turns profiling off for this thread until it is re-enabled */
	profiler_start();
	profile_reenable_thread();
	profile_register_root(root_name, 1000000);
	for (int i = 0; i < 5; i++) {
		profile_start(root_name);
		profile_start(child_name);
		os_sleep_ms(1);
		profile_end(child_name);
		profile_end(root_name);
	}
	profile_start(other_root);
	profile_end(other_root);

	/* mismatched end is reported and ignored */
	profile_start(root_name);
	profile_end(child_name);
	profile_end(root_name);
	profile_reenable_thread();

	profiler_snapshot_t *snap = profile_snapshot_create();
	assert_non_null(snap);
	assert_true(profiler_snapshot_num_roots(snap) >= 1);

	size_t roots = 0;
	profiler_snapshot_enumerate_roots(snap, count_entries, &roots);
	assert_int_equal(roots, profiler_snapshot_num_roots(snap));

	profiler_snapshot_entry_t *root = NULL;
	profiler_snapshot_enumerate_roots(snap, find_root, &root);
	assert_non_null(root);
	size_t children = 0;
	profiler_snapshot_enumerate_children(root, count_entries, &children);
	assert_int_equal(children, profiler_snapshot_num_children(root));

	assert_int_equal(profiler_snapshot_num_children(root), 1);
	assert_true(profiler_snapshot_entry_overall_count(root) >= 5);
	assert_true(profiler_snapshot_entry_max_time(root) >= profiler_snapshot_entry_min_time(root));
	assert_non_null(profiler_snapshot_entry_times(root));
	assert_int_equal(profiler_snapshot_entry_expected_time_between_calls(root),
			 1000); /* registered in ns, reported in us */
	assert_non_null(profiler_snapshot_entry_times_between_calls(root));
	assert_true(profiler_snapshot_entry_overall_between_calls_count(root) >= 4);
	assert_true(profiler_snapshot_entry_max_time_between_calls(root) >=
		    profiler_snapshot_entry_min_time_between_calls(root));

	profiler_print(snap);
	profiler_print_time_between_calls(snap);
	assert_true(profiler_snapshot_dump_csv(snap, "profile.csv"));
	assert_true(os_get_file_size("profile.csv") > 0);
	assert_true(profiler_snapshot_dump_csv_gz(snap, "profile.csv.gz"));
	assert_true(os_get_file_size("profile.csv.gz") > 0);
	assert_false(profiler_snapshot_dump_csv(snap, "no-such-dir/profile.csv"));
	assert_false(profiler_snapshot_dump_csv_gz(snap, "no-such-dir/profile.csv.gz"));

	profiler_snapshot_filter_roots(snap, keep_root_only, NULL);
	assert_int_equal(profiler_snapshot_num_roots(snap), 1);

	profile_snapshot_free(snap);
	profiler_stop();

	/* stopped: these are ignored, and a snapshot still works */
	profile_start(root_name);
	profile_end(root_name);
	snap = profile_snapshot_create();
	profiler_print(snap);
	profile_snapshot_free(snap);

	profiler_free();

	/* Regression: profiler_free() destroyed the static root mutex, so any later profiler call crashed */
	snap = profile_snapshot_create();
	profile_snapshot_free(snap);
	profiler_start();
	profile_start(root_name);
	profile_end(root_name);
	profiler_stop();
	profiler_free();

	os_unlink("profile.csv");
	os_unlink("profile.csv.gz");
}

static void name_store_test(void **state)
{
	UNUSED_PARAMETER(state);

	profiler_name_store_t *store = profiler_name_store_create();
	const char *a = profile_store_name(store, "name %d", 1);
	const char *b = profile_store_name(store, "name %d", 2);
	assert_string_equal(a, "name 1");
	assert_string_equal(b, "name 2");
	profiler_name_store_free(store);
	profiler_name_store_free(NULL);
}

static void null_test(void **state)
{
	UNUSED_PARAMETER(state);

	profiler_print(NULL);
	profiler_print_time_between_calls(NULL);
	profile_snapshot_free(NULL);
	assert_int_equal(profiler_snapshot_num_roots(NULL), 0);
	assert_int_equal(profiler_snapshot_num_children(NULL), 0);
	profiler_snapshot_enumerate_roots(NULL, count_entries, NULL);
	profiler_snapshot_enumerate_children(NULL, count_entries, NULL);
	profiler_snapshot_filter_roots(NULL, keep_root_only, NULL); /* regression: crashed */
	assert_null(profiler_snapshot_entry_name(NULL));
	assert_null(profiler_snapshot_entry_times(NULL));
	assert_null(profiler_snapshot_entry_times_between_calls(NULL));
	assert_int_equal(profiler_snapshot_entry_min_time(NULL), 0);
	assert_int_equal(profiler_snapshot_entry_max_time(NULL), 0);
	assert_int_equal(profiler_snapshot_entry_overall_count(NULL), 0);
	assert_int_equal(profiler_snapshot_entry_expected_time_between_calls(NULL), 0);
	assert_int_equal(profiler_snapshot_entry_min_time_between_calls(NULL), 0);
	assert_int_equal(profiler_snapshot_entry_max_time_between_calls(NULL), 0);
	assert_int_equal(profiler_snapshot_entry_overall_between_calls_count(NULL), 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(profiler_test),
		obs_unit_test(name_store_test),
		obs_unit_test(null_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
