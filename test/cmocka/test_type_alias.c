#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Test encoder and output types                                             */

static const char *encoder_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "New Encoder";
}

static const char *output_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "New Output";
}

static void *encoder_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(encoder);
	return bzalloc(1);
}

static void *output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(output);
	return bzalloc(1);
}

static void dummy_destroy(void *data)
{
	bfree(data);
}

static bool dummy_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received_packet)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(frame);
	UNUSED_PARAMETER(packet);
	*received_packet = false;
	return true;
}

static size_t dummy_frame_size(void *data)
{
	UNUSED_PARAMETER(data);
	return 1024;
}

static bool output_start(void *data)
{
	UNUSED_PARAMETER(data);
	return false;
}

static void output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(ts);
}

static void output_packet(void *data, struct encoder_packet *packet)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(packet);
}

static void new_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "new-key", 1);
}

static obs_properties_t *new_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "new-key", "New key", 0, 100, 1);
	return props;
}

static struct obs_encoder_info video_info = {
	.id = "com.example.test.video",
	.type = OBS_ENCODER_VIDEO,
	.codec = "h264",
	.caps = OBS_ENCODER_CAP_DYN_BITRATE,
	.get_name = encoder_name,
	.create = encoder_create,
	.destroy = dummy_destroy,
	.encode = dummy_encode,
	.get_defaults = new_defaults,
	.get_properties = new_properties,
};

static struct obs_encoder_info audio_info = {
	.id = "com.example.test.audio",
	.type = OBS_ENCODER_AUDIO,
	.codec = "aac",
	.get_name = encoder_name,
	.create = encoder_create,
	.destroy = dummy_destroy,
	.encode = dummy_encode,
	.get_frame_size = dummy_frame_size,
};

static struct obs_encoder_info late_info = {
	.id = "com.example.test.late",
	.type = OBS_ENCODER_VIDEO,
	.codec = "hevc",
	.get_name = encoder_name,
	.create = encoder_create,
	.destroy = dummy_destroy,
	.encode = dummy_encode,
};

static struct obs_output_info out_info = {
	.id = "com.example.test.output",
	.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED,
	.encoded_video_codecs = "h264;hevc",
	.encoded_audio_codecs = "aac;opus",
	.get_name = output_name,
	.create = output_create,
	.destroy = dummy_destroy,
	.start = output_start,
	.stop = output_stop,
	.encoded_packet = output_packet,
	.get_defaults = new_defaults,
	.get_properties = new_properties,
};

/* ------------------------------------------------------------------------- */
/* Migration callbacks                                                       */

struct migrate_log {
	int calls;
	char order[16];
	char last_old_id[64];
};

/* Renames old_key to new-key, like rate_control -> rate-control. */
static void migrate_rename(void *param, const char *old_id, obs_data_t *settings)
{
	struct migrate_log *log = param;
	log->calls++;
	snprintf(log->last_old_id, sizeof(log->last_old_id), "%s", old_id);

	if (obs_data_has_user_value(settings, "old_key")) {
		obs_data_set_int(settings, "new-key", obs_data_get_int(settings, "old_key"));
		obs_data_erase(settings, "old_key");
	}
}

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

	obs_register_encoder(&video_info);
	obs_register_encoder(&audio_info);
	obs_register_output(&out_info);
	return 0;
}

static int teardown(void **state)
{
	UNUSED_PARAMETER(state);
	obs_shutdown();

	/* Aliases, migrated settings and the alias tables are all freed. */
	if (bnum_allocs() != allocs_before) {
		fprintf(stderr, "leaked %ld allocations\n", bnum_allocs() - allocs_before);
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* Without a core                                                            */

static void no_core(void **state)
{
	UNUSED_PARAMETER(state);

	assert_false(obs_register_encoder_alias("old", "com.example.test.video", NULL, NULL));
	assert_false(obs_register_output_alias("old", "com.example.test.output", NULL, NULL));
	assert_null(obs_encoder_alias_resolve("old"));
	assert_null(obs_output_alias_resolve("old"));
}

/* ------------------------------------------------------------------------- */
/* Encoders                                                                  */

static void encoder_register_rejects_invalid(void **state)
{
	UNUSED_PARAMETER(state);

	assert_false(obs_register_encoder_alias(NULL, "com.example.test.video", NULL, NULL));
	assert_false(obs_register_encoder_alias("x", NULL, NULL, NULL));
	assert_false(obs_register_encoder_alias("", "com.example.test.video", NULL, NULL));
	assert_false(obs_register_encoder_alias("x", "", NULL, NULL));
	assert_false(obs_register_encoder_alias("same", "same", NULL, NULL));

	/* A real encoder type can't be shadowed by an alias. */
	assert_false(obs_register_encoder_alias("com.example.test.video", "com.example.test.audio", NULL, NULL));

	/* Each old ID can only be an alias once. */
	assert_true(obs_register_encoder_alias("reg_dup", "com.example.test.video", NULL, NULL));
	assert_false(obs_register_encoder_alias("reg_dup", "com.example.test.audio", NULL, NULL));

	/* Encoder and output aliases live in separate tables. */
	assert_true(obs_register_output_alias("reg_dup", "com.example.test.output", NULL, NULL));
}

static void encoder_create_through_alias(void **state)
{
	UNUSED_PARAMETER(state);
	struct migrate_log log = {0};

	assert_true(obs_register_encoder_alias("old_video", "com.example.test.video", migrate_rename, &log));

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "old_key", 42);

	obs_encoder_t *encoder = obs_video_encoder_create("old_video", "enc", settings, NULL);
	assert_non_null(encoder);
	assert_string_equal(obs_encoder_get_id(encoder), "com.example.test.video");
	assert_string_equal(obs_encoder_get_codec(encoder), "h264");

	assert_int_equal(log.calls, 1);
	assert_string_equal(log.last_old_id, "old_video");

	obs_data_t *got = obs_encoder_get_settings(encoder);
	assert_int_equal(obs_data_get_int(got, "new-key"), 42);
	assert_false(obs_data_has_user_value(got, "old_key"));
	obs_data_release(got);

	/* The caller's settings are not modified by the migration. */
	assert_int_equal(obs_data_get_int(settings, "old_key"), 42);
	assert_false(obs_data_has_user_value(settings, "new-key"));
	obs_data_release(settings);

	/* The migration runs once per created encoder, also without settings. */
	obs_encoder_t *second = obs_video_encoder_create("old_video", "enc2", NULL, NULL);
	assert_non_null(second);
	assert_int_equal(log.calls, 2);
	got = obs_encoder_get_settings(second);
	assert_int_equal(obs_data_get_int(got, "new-key"), 1);
	obs_data_release(got);

	obs_encoder_release(second);
	obs_encoder_release(encoder);
}

static void encoder_audio_through_alias(void **state)
{
	UNUSED_PARAMETER(state);

	assert_true(obs_register_encoder_alias("old_audio", "com.example.test.audio", NULL, NULL));

	obs_encoder_t *encoder = obs_audio_encoder_create("old_audio", "aenc", NULL, 0, NULL);
	assert_non_null(encoder);
	assert_string_equal(obs_encoder_get_id(encoder), "com.example.test.audio");
	obs_encoder_release(encoder);

	/* The target's type still has to match the requested type. */
	assert_null(obs_video_encoder_create("old_audio", "venc", NULL, NULL));
}

static void encoder_queries_follow_alias(void **state)
{
	UNUSED_PARAMETER(state);

	assert_true(obs_register_encoder_alias("query_old", "com.example.test.video", NULL, NULL));

	assert_string_equal(obs_encoder_alias_resolve("query_old"), "com.example.test.video");
	assert_string_equal(obs_encoder_alias_resolve("com.example.test.video"), "com.example.test.video");
	assert_null(obs_encoder_alias_resolve("unknown"));
	assert_null(obs_encoder_alias_resolve(NULL));

	assert_string_equal(obs_encoder_get_display_name("query_old"), "New Encoder");
	assert_string_equal(obs_get_encoder_codec("query_old"), "h264");
	assert_int_equal(obs_get_encoder_type("query_old"), OBS_ENCODER_VIDEO);
	assert_int_equal(obs_get_encoder_caps("query_old"), OBS_ENCODER_CAP_DYN_BITRATE);

	obs_data_t *defaults = obs_encoder_defaults("query_old");
	assert_non_null(defaults);
	assert_int_equal(obs_data_get_int(defaults, "new-key"), 1);
	obs_data_release(defaults);

	obs_properties_t *props = obs_get_encoder_properties("query_old");
	assert_non_null(props);
	assert_non_null(obs_properties_get(props, "new-key"));
	obs_properties_destroy(props);
}

static void encoder_chain_and_cycle(void **state)
{
	UNUSED_PARAMETER(state);
	struct migrate_log log = {0};

	/* oldest -> older -> video: migrations run oldest first. */
	assert_true(obs_register_encoder_alias("chain_oldest", "chain_older", migrate_mark_a, &log));
	assert_true(obs_register_encoder_alias("chain_older", "com.example.test.video", migrate_mark_b, &log));

	obs_encoder_t *encoder = obs_video_encoder_create("chain_oldest", "chain", NULL, NULL);
	assert_non_null(encoder);
	assert_string_equal(obs_encoder_get_id(encoder), "com.example.test.video");
	assert_string_equal(log.order, "ab");
	obs_encoder_release(encoder);

	/* A cycle or a missing target resolves to nothing, and creates a
	 * placeholder that keeps the requested ID. */
	assert_true(obs_register_encoder_alias("cycle_a", "cycle_b", NULL, NULL));
	assert_true(obs_register_encoder_alias("cycle_b", "cycle_a", NULL, NULL));
	assert_null(obs_encoder_alias_resolve("cycle_a"));

	assert_true(obs_register_encoder_alias("broken", "does_not_exist", NULL, NULL));
	assert_null(obs_encoder_alias_resolve("broken"));

	encoder = obs_video_encoder_create("broken", "broken", NULL, NULL);
	assert_non_null(encoder);
	assert_string_equal(obs_encoder_get_id(encoder), "broken");
	obs_encoder_release(encoder);
}

static void encoder_target_registered_after_alias(void **state)
{
	UNUSED_PARAMETER(state);

	assert_true(obs_register_encoder_alias("late_old", "com.example.test.late", NULL, NULL));
	assert_null(obs_encoder_alias_resolve("late_old"));

	obs_register_encoder(&late_info);
	assert_string_equal(obs_encoder_alias_resolve("late_old"), "com.example.test.late");
}

static void encoder_real_type_wins(void **state)
{
	UNUSED_PARAMETER(state);

	/* An alias registered first is shadowed once a real type takes its ID. */
	assert_true(obs_register_encoder_alias("com.example.test.late", "com.example.test.video", NULL, NULL));
	assert_string_equal(obs_encoder_alias_resolve("com.example.test.late"), "com.example.test.video");

	obs_register_encoder(&late_info);
	assert_string_equal(obs_encoder_alias_resolve("com.example.test.late"), "com.example.test.late");
}

/* ------------------------------------------------------------------------- */
/* Outputs                                                                   */

static void output_register_rejects_invalid(void **state)
{
	UNUSED_PARAMETER(state);

	assert_false(obs_register_output_alias(NULL, "com.example.test.output", NULL, NULL));
	assert_false(obs_register_output_alias("x", NULL, NULL, NULL));
	assert_false(obs_register_output_alias("same", "same", NULL, NULL));
	assert_false(obs_register_output_alias("com.example.test.output", "x", NULL, NULL));

	assert_true(obs_register_output_alias("reg_dup", "com.example.test.output", NULL, NULL));
	assert_false(obs_register_output_alias("reg_dup", "x", NULL, NULL));
}

static void output_create_through_alias(void **state)
{
	UNUSED_PARAMETER(state);
	struct migrate_log log = {0};

	assert_true(obs_register_output_alias("old_output", "com.example.test.output", migrate_rename, &log));

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "old_key", 7);

	obs_output_t *output = obs_output_create("old_output", "out", settings, NULL);
	assert_non_null(output);
	assert_string_equal(obs_output_get_id(output), "com.example.test.output");
	assert_int_equal(log.calls, 1);
	assert_string_equal(log.last_old_id, "old_output");

	obs_data_t *got = obs_output_get_settings(output);
	assert_int_equal(obs_data_get_int(got, "new-key"), 7);
	assert_false(obs_data_has_user_value(got, "old_key"));
	obs_data_release(got);

	assert_int_equal(obs_data_get_int(settings, "old_key"), 7);
	obs_data_release(settings);
	obs_output_release(output);

	/* A broken alias creates a placeholder that keeps the requested ID. */
	assert_true(obs_register_output_alias("broken", "does_not_exist", NULL, NULL));
	output = obs_output_create("broken", "broken", NULL, NULL);
	assert_non_null(output);
	assert_string_equal(obs_output_get_id(output), "broken");
	obs_output_release(output);
}

static void output_queries_follow_alias(void **state)
{
	UNUSED_PARAMETER(state);
	struct migrate_log log = {0};

	assert_true(obs_register_output_alias("query_oldest", "query_old", migrate_mark_a, &log));
	assert_true(obs_register_output_alias("query_old", "com.example.test.output", migrate_mark_b, &log));

	assert_string_equal(obs_output_alias_resolve("query_oldest"), "com.example.test.output");
	assert_string_equal(obs_output_alias_resolve("com.example.test.output"), "com.example.test.output");
	assert_null(obs_output_alias_resolve("unknown"));
	assert_null(obs_output_alias_resolve(NULL));

	assert_string_equal(obs_output_get_display_name("query_oldest"), "New Output");
	assert_int_equal(obs_get_output_flags("query_oldest"), OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED);
	assert_string_equal(obs_get_output_supported_video_codecs("query_oldest"), "h264;hevc");
	assert_string_equal(obs_get_output_supported_audio_codecs("query_oldest"), "aac;opus");

	obs_data_t *defaults = obs_output_defaults("query_oldest");
	assert_non_null(defaults);
	assert_int_equal(obs_data_get_int(defaults, "new-key"), 1);
	obs_data_release(defaults);

	obs_properties_t *props = obs_get_output_properties("query_oldest");
	assert_non_null(props);
	assert_non_null(obs_properties_get(props, "new-key"));
	obs_properties_destroy(props);

	/* Queries don't run migrations. */
	assert_string_equal(log.order, "");

	obs_output_t *output = obs_output_create("query_oldest", "chained", NULL, NULL);
	assert_non_null(output);
	assert_string_equal(log.order, "ab");
	obs_output_release(output);
}

/* ------------------------------------------------------------------------- */
/* Threads                                                                   */

#define THREADS 8
#define PER_THREAD 64

static void *register_worker(void *param)
{
	int t = (int)(intptr_t)param;
	char id[64];

	for (int i = 0; i < PER_THREAD; i++) {
		snprintf(id, sizeof(id), "parallel_%d_%d", t, i);
		if (!obs_register_encoder_alias(id, "com.example.test.video", NULL, NULL)) {
			return (void *)1;
		}
		if (!obs_register_output_alias(id, "com.example.test.output", NULL, NULL)) {
			return (void *)1;
		}
		if (!obs_encoder_alias_resolve(id) || !obs_output_alias_resolve(id)) {
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
			assert_string_equal(obs_encoder_alias_resolve(id), "com.example.test.video");
			assert_string_equal(obs_output_alias_resolve(id), "com.example.test.output");
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(no_core),
		cmocka_unit_test_setup_teardown(encoder_register_rejects_invalid, setup, teardown),
		cmocka_unit_test_setup_teardown(encoder_create_through_alias, setup, teardown),
		cmocka_unit_test_setup_teardown(encoder_audio_through_alias, setup, teardown),
		cmocka_unit_test_setup_teardown(encoder_queries_follow_alias, setup, teardown),
		cmocka_unit_test_setup_teardown(encoder_chain_and_cycle, setup, teardown),
		cmocka_unit_test_setup_teardown(encoder_target_registered_after_alias, setup, teardown),
		cmocka_unit_test_setup_teardown(encoder_real_type_wins, setup, teardown),
		cmocka_unit_test_setup_teardown(output_register_rejects_invalid, setup, teardown),
		cmocka_unit_test_setup_teardown(output_create_through_alias, setup, teardown),
		cmocka_unit_test_setup_teardown(output_queries_follow_alias, setup, teardown),
		cmocka_unit_test_setup_teardown(parallel_registration, setup, teardown),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
