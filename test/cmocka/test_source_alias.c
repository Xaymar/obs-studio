#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Test source types                                                         */

static const char *name_new(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "New Source";
}

static const char *name_versioned(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Versioned Source";
}

static void *dummy_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(source);
	return bzalloc(1);
}

static void dummy_destroy(void *data)
{
	bfree(data);
}

static uint32_t dummy_size(void *data)
{
	UNUSED_PARAMETER(data);
	return 0;
}

static void new_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "new_key", 1);
}

static struct obs_source_info new_info = {
	.id = "alias_test_new",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = name_new,
	.create = dummy_create,
	.destroy = dummy_destroy,
	.get_width = dummy_size,
	.get_height = dummy_size,
	.get_defaults = new_defaults,
};

static struct obs_source_info versioned_v1 = {
	.id = "alias_test_versioned",
	.version = 1,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = name_versioned,
	.create = dummy_create,
	.destroy = dummy_destroy,
	.get_width = dummy_size,
	.get_height = dummy_size,
};

static struct obs_source_info versioned_v2 = {
	.id = "alias_test_versioned",
	.version = 2,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = name_versioned,
	.create = dummy_create,
	.destroy = dummy_destroy,
	.get_width = dummy_size,
	.get_height = dummy_size,
};

/* ------------------------------------------------------------------------- */
/* Migration callbacks                                                       */

struct migrate_log {
	int calls;
	char order[16];
	char last_old_id[64];
};

/* Renames old_key to new_key, like capture_cursor -> cursor. */
static void migrate_rename(void *param, const char *old_id, obs_data_t *settings)
{
	struct migrate_log *log = param;
	log->calls++;
	snprintf(log->last_old_id, sizeof(log->last_old_id), "%s", old_id);

	if (obs_data_has_user_value(settings, "old_key")) {
		obs_data_set_int(settings, "new_key", obs_data_get_int(settings, "old_key"));
		obs_data_erase(settings, "old_key");
	}
}

/* Appends a marker, so chained migrations show their order. */
static void migrate_mark_a(void *param, const char *old_id, obs_data_t *settings)
{
	UNUSED_PARAMETER(old_id);
	UNUSED_PARAMETER(settings);
	strcat(((struct migrate_log *)param)->order, "a");
}

static void migrate_mark_b(void *param, const char *old_id, obs_data_t *settings)
{
	UNUSED_PARAMETER(old_id);
	UNUSED_PARAMETER(settings);
	strcat(((struct migrate_log *)param)->order, "b");
}

/* ------------------------------------------------------------------------- */
/* Fixture                                                                   */

static long allocs_before;

static int setup(void **state)
{
	UNUSED_PARAMETER(state);
	allocs_before = bnum_allocs();
	if (!obs_startup("en-US", NULL, NULL)) {
		return -1;
	}

	obs_register_source(&new_info);
	obs_register_source(&versioned_v1);
	obs_register_source(&versioned_v2);
	return 0;
}

static int teardown(void **state)
{
	UNUSED_PARAMETER(state);
	obs_shutdown();

	/* Aliases, migrated settings and the alias table are all freed. */
	if (bnum_allocs() != allocs_before) {
		fprintf(stderr, "leaked %ld allocations\n", bnum_allocs() - allocs_before);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                     */

static void register_rejects_invalid(void **state)
{
	UNUSED_PARAMETER(state);

	assert_false(obs_register_source_alias(NULL, "alias_test_new", NULL, NULL));
	assert_false(obs_register_source_alias("x", NULL, NULL, NULL));
	assert_false(obs_register_source_alias("", "alias_test_new", NULL, NULL));
	assert_false(obs_register_source_alias("x", "", NULL, NULL));
	assert_false(obs_register_source_alias("same", "same", NULL, NULL));

	/* A real source type can't be shadowed by an alias. */
	assert_false(obs_register_source_alias("alias_test_new", "alias_test_versioned", NULL, NULL));

	/* Each old ID can only be an alias once. */
	assert_true(obs_register_source_alias("reg_dup", "alias_test_new", NULL, NULL));
	assert_false(obs_register_source_alias("reg_dup", "alias_test_versioned", NULL, NULL));
}

static void create_through_alias(void **state)
{
	UNUSED_PARAMETER(state);
	struct migrate_log log = {0};

	assert_true(obs_register_source_alias("create_old", "alias_test_new", migrate_rename, &log));

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "old_key", 42);

	obs_source_t *source = obs_source_create("create_old", "created", settings, NULL);
	assert_non_null(source);
	assert_string_equal(obs_source_get_id(source), "alias_test_new");
	assert_string_equal(obs_source_get_unversioned_id(source), "alias_test_new");

	assert_int_equal(log.calls, 1);
	assert_string_equal(log.last_old_id, "create_old");

	obs_data_t *got = obs_source_get_settings(source);
	assert_int_equal(obs_data_get_int(got, "new_key"), 42);
	assert_false(obs_data_has_user_value(got, "old_key"));
	obs_data_release(got);

	/* The caller's settings are not modified by the migration. */
	assert_int_equal(obs_data_get_int(settings, "old_key"), 42);
	assert_false(obs_data_has_user_value(settings, "new_key"));

	obs_source_release(source);
	obs_data_release(settings);
}

static void create_through_alias_without_settings(void **state)
{
	UNUSED_PARAMETER(state);
	struct migrate_log log = {0};

	assert_true(obs_register_source_alias("nosettings_old", "alias_test_new", migrate_rename, &log));

	obs_source_t *source = obs_source_create("nosettings_old", "nosettings", NULL, NULL);
	assert_non_null(source);
	assert_string_equal(obs_source_get_id(source), "alias_test_new");
	assert_int_equal(log.calls, 1);

	/* Defaults of the new type still apply. */
	obs_data_t *got = obs_source_get_settings(source);
	assert_int_equal(obs_data_get_int(got, "new_key"), 1);
	obs_data_release(got);

	obs_source_release(source);
}

static void create_without_migrate_callback(void **state)
{
	UNUSED_PARAMETER(state);

	assert_true(obs_register_source_alias("nomigrate_old", "alias_test_new", NULL, NULL));

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "kept", "yes");

	obs_source_t *source = obs_source_create("nomigrate_old", "nomigrate", settings, NULL);
	assert_non_null(source);
	assert_string_equal(obs_source_get_id(source), "alias_test_new");

	/* Unknown keys are kept, not dropped. */
	obs_data_t *got = obs_source_get_settings(source);
	assert_string_equal(obs_data_get_string(got, "kept"), "yes");
	obs_data_release(got);

	obs_source_release(source);
	obs_data_release(settings);
}

static void chain_runs_migrations_in_order(void **state)
{
	UNUSED_PARAMETER(state);
	struct migrate_log log = {0};

	assert_true(obs_register_source_alias("chain_a", "chain_b", migrate_mark_a, &log));
	assert_true(obs_register_source_alias("chain_b", "alias_test_new", migrate_mark_b, &log));

	assert_string_equal(obs_source_alias_resolve("chain_a"), "alias_test_new");

	obs_source_t *source = obs_source_create("chain_a", "chained", NULL, NULL);
	assert_non_null(source);
	assert_string_equal(obs_source_get_id(source), "alias_test_new");
	assert_string_equal(log.order, "ab");
	obs_source_release(source);

	/* Starting in the middle of the chain only runs the rest of it. */
	log.order[0] = 0;
	source = obs_source_create("chain_b", "chained2", NULL, NULL);
	assert_non_null(source);
	assert_string_equal(log.order, "b");
	obs_source_release(source);
}

static void cycle_and_broken_chain(void **state)
{
	UNUSED_PARAMETER(state);
	struct migrate_log log = {0};

	assert_true(obs_register_source_alias("cycle_a", "cycle_b", migrate_rename, &log));
	assert_true(obs_register_source_alias("cycle_b", "cycle_a", migrate_rename, &log));
	assert_null(obs_source_alias_resolve("cycle_a"));

	assert_true(obs_register_source_alias("broken_old", "does_not_exist", migrate_rename, &log));
	assert_null(obs_source_alias_resolve("broken_old"));

	/* An unresolvable alias behaves like any unknown ID: a placeholder source
	 * that keeps its ID and settings, and no migration runs. */
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "old_key", 7);
	obs_source_t *source = obs_source_create("cycle_a", "cycle", settings, NULL);
	assert_non_null(source);
	assert_string_equal(obs_source_get_id(source), "cycle_a");
	assert_int_equal(log.calls, 0);

	obs_data_t *got = obs_source_get_settings(source);
	assert_int_equal(obs_data_get_int(got, "old_key"), 7);
	obs_data_release(got);

	obs_source_release(source);
	obs_data_release(settings);
}

static void unversioned_target_uses_latest(void **state)
{
	UNUSED_PARAMETER(state);

	assert_true(obs_register_source_alias("ver_old", "alias_test_versioned", NULL, NULL));
	assert_true(obs_register_source_alias("ver_old_exact", "alias_test_versioned_v1", NULL, NULL));

	assert_string_equal(obs_source_alias_resolve("ver_old"), "alias_test_versioned_v2");
	assert_string_equal(obs_source_alias_resolve("ver_old_exact"), "alias_test_versioned_v1");

	obs_source_t *source = obs_source_create("ver_old", "versioned", NULL, NULL);
	assert_non_null(source);
	assert_string_equal(obs_source_get_id(source), "alias_test_versioned_v2");
	assert_string_equal(obs_source_get_unversioned_id(source), "alias_test_versioned");
	obs_source_release(source);
}

static void real_type_wins_over_alias(void **state)
{
	UNUSED_PARAMETER(state);

	static struct obs_source_info late_info = {
		.id = "late_type",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO,
		.get_name = name_versioned,
		.create = dummy_create,
		.destroy = dummy_destroy,
		.get_width = dummy_size,
		.get_height = dummy_size,
	};

	/* The alias is registered first (another module, loading in parallel),
	 * then a real type with the same ID appears. */
	assert_true(obs_register_source_alias("late_type", "alias_test_new", NULL, NULL));
	assert_string_equal(obs_source_alias_resolve("late_type"), "alias_test_new");

	obs_register_source(&late_info);
	assert_string_equal(obs_source_alias_resolve("late_type"), "late_type");

	obs_source_t *source = obs_source_create("late_type", "late", NULL, NULL);
	assert_non_null(source);
	assert_string_equal(obs_source_get_id(source), "late_type");
	obs_source_release(source);
}

static void target_registered_after_alias(void **state)
{
	UNUSED_PARAMETER(state);

	static struct obs_source_info later_target = {
		.id = "later_target",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO,
		.get_name = name_versioned,
		.create = dummy_create,
		.destroy = dummy_destroy,
		.get_width = dummy_size,
		.get_height = dummy_size,
	};

	assert_true(obs_register_source_alias("early_alias", "later_target", NULL, NULL));
	assert_null(obs_source_alias_resolve("early_alias"));

	obs_register_source(&later_target);
	assert_string_equal(obs_source_alias_resolve("early_alias"), "later_target");
}

static void resolve_plain_and_unknown(void **state)
{
	UNUSED_PARAMETER(state);

	assert_string_equal(obs_source_alias_resolve("alias_test_new"), "alias_test_new");
	assert_null(obs_source_alias_resolve("never_registered"));
	assert_null(obs_source_alias_resolve(NULL));
}

static void type_queries_follow_alias(void **state)
{
	UNUSED_PARAMETER(state);

	assert_true(obs_register_source_alias("query_old", "alias_test_new", NULL, NULL));

	assert_string_equal(obs_source_get_display_name("query_old"), "New Source");
	assert_int_equal(obs_get_source_output_flags("query_old"), OBS_SOURCE_VIDEO);

	obs_data_t *defaults = obs_get_source_defaults("query_old");
	assert_non_null(defaults);
	assert_int_equal(obs_data_get_int(defaults, "new_key"), 1);
	obs_data_release(defaults);
}

static void load_and_save_scene_data(void **state)
{
	UNUSED_PARAMETER(state);
	struct migrate_log log = {0};

	assert_true(obs_register_source_alias("saved_old_v2", "alias_test_new", migrate_rename, &log));

	/* As written by an older build: unversioned id plus versioned_id. */
	obs_data_t *data = obs_data_create_from_json("{\"id\":\"saved_old\",\"versioned_id\":\"saved_old_v2\","
						     "\"name\":\"loaded\",\"settings\":{\"old_key\":5}}");
	assert_non_null(data);

	obs_source_t *source = obs_load_source(data);
	assert_non_null(source);
	assert_string_equal(obs_source_get_id(source), "alias_test_new");
	assert_int_equal(log.calls, 1);

	obs_data_t *got = obs_source_get_settings(source);
	assert_int_equal(obs_data_get_int(got, "new_key"), 5);
	obs_data_release(got);

	/* Saving writes the new ID, so the alias is only needed once. */
	obs_data_t *saved = obs_save_source(source);
	assert_string_equal(obs_data_get_string(saved, "id"), "alias_test_new");
	assert_string_equal(obs_data_get_string(saved, "versioned_id"), "alias_test_new");
	obs_data_t *saved_settings = obs_data_get_obj(saved, "settings");
	assert_int_equal(obs_data_get_int(saved_settings, "new_key"), 5);
	assert_false(obs_data_has_user_value(saved_settings, "old_key"));
	obs_data_release(saved_settings);
	obs_data_release(saved);

	obs_source_remove(source);
	obs_source_release(source);
	obs_data_release(data);
}

#define THREADS 8
#define PER_THREAD 64

static void *register_worker(void *param)
{
	int index = (int)(intptr_t)param;
	char id[64];
	for (int i = 0; i < PER_THREAD; i++) {
		snprintf(id, sizeof(id), "parallel_%d_%d", index, i);
		if (!obs_register_source_alias(id, "alias_test_new", NULL, NULL)) {
			return (void *)1;
		}
		/* Resolve while other threads keep registering. */
		if (!obs_source_alias_resolve(id)) {
			return (void *)1;
		}
	}
	return NULL;
}

static void parallel_registration(void **state)
{
	UNUSED_PARAMETER(state);
	pthread_t threads[THREADS];

	for (int t = 0; t < THREADS; t++) {
		assert_int_equal(pthread_create(&threads[t], NULL, register_worker, (void *)(intptr_t)t), 0);
	}

	for (int t = 0; t < THREADS; t++) {
		void *ret = NULL;
		pthread_join(threads[t], &ret);
		assert_null(ret);
	}

	char id[64];
	for (int t = 0; t < THREADS; t++) {
		for (int i = 0; i < PER_THREAD; i++) {
			snprintf(id, sizeof(id), "parallel_%d_%d", t, i);
			assert_string_equal(obs_source_alias_resolve(id), "alias_test_new");
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(register_rejects_invalid, setup, teardown),
		cmocka_unit_test_setup_teardown(create_through_alias, setup, teardown),
		cmocka_unit_test_setup_teardown(create_through_alias_without_settings, setup, teardown),
		cmocka_unit_test_setup_teardown(create_without_migrate_callback, setup, teardown),
		cmocka_unit_test_setup_teardown(chain_runs_migrations_in_order, setup, teardown),
		cmocka_unit_test_setup_teardown(cycle_and_broken_chain, setup, teardown),
		cmocka_unit_test_setup_teardown(unversioned_target_uses_latest, setup, teardown),
		cmocka_unit_test_setup_teardown(real_type_wins_over_alias, setup, teardown),
		cmocka_unit_test_setup_teardown(target_registered_after_alias, setup, teardown),
		cmocka_unit_test_setup_teardown(resolve_plain_and_unknown, setup, teardown),
		cmocka_unit_test_setup_teardown(type_queries_follow_alias, setup, teardown),
		cmocka_unit_test_setup_teardown(load_and_save_scene_data, setup, teardown),
		cmocka_unit_test_setup_teardown(parallel_registration, setup, teardown),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
