#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>

#include "nvenc-migrate.h"

/* Settings saved by the pre-31.0 NVENC encoders only hold what the user
 * changed. Migration must give the current encoder the same configuration,
 * including the old defaults the user never touched. */

static void untouched_settings_get_old_defaults(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *settings = obs_data_create();

	nvenc_migrate_legacy_settings(settings, NVENC_LEGACY_H264, 1);

	assert_int_equal(obs_data_get_int(settings, "bitrate"), 2500);
	assert_int_equal(obs_data_get_int(settings, "max_bitrate"), 5000);
	assert_int_equal(obs_data_get_int(settings, "keyint_sec"), 0);
	assert_int_equal(obs_data_get_int(settings, "cqp"), 20);
	assert_string_equal(obs_data_get_string(settings, "rate_control"), "CBR");
	assert_string_equal(obs_data_get_string(settings, "preset"), "p5");
	assert_string_equal(obs_data_get_string(settings, "multipass"), "qres");
	assert_string_equal(obs_data_get_string(settings, "tune"), "hq");
	assert_string_equal(obs_data_get_string(settings, "profile"), "high");
	assert_true(obs_data_get_bool(settings, "adaptive_quantization"));
	assert_int_equal(obs_data_get_int(settings, "bf"), 2);
	assert_false(obs_data_get_bool(settings, "repeat_headers"));

	/* Every value is explicit, so the new encoder's defaults don't apply. */
	assert_true(obs_data_has_user_value(settings, "bitrate"));
	assert_true(obs_data_has_user_value(settings, "preset"));
	assert_true(obs_data_has_user_value(settings, "adaptive_quantization"));
	assert_false(obs_data_has_user_value(settings, "device"));

	obs_data_release(settings);
}

static void hevc_and_av1_default_to_main(void **state)
{
	UNUSED_PARAMETER(state);

	obs_data_t *settings = obs_data_create();
	nvenc_migrate_legacy_settings(settings, NVENC_LEGACY_HEVC, 1);
	assert_string_equal(obs_data_get_string(settings, "profile"), "main");
	obs_data_release(settings);

	settings = obs_data_create();
	nvenc_migrate_legacy_settings(settings, NVENC_LEGACY_AV1, 1);
	assert_string_equal(obs_data_get_string(settings, "profile"), "main");
	obs_data_release(settings);
}

static void user_values_are_kept_and_renamed(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", 8000);
	obs_data_set_string(settings, "rate_control", "VBR");
	obs_data_set_string(settings, "profile", "baseline");
	obs_data_set_string(settings, "preset2", "p7");
	obs_data_set_bool(settings, "psycho_aq", false);

	nvenc_migrate_legacy_settings(settings, NVENC_LEGACY_H264, 1);

	assert_int_equal(obs_data_get_int(settings, "bitrate"), 8000);
	assert_string_equal(obs_data_get_string(settings, "rate_control"), "VBR");
	assert_string_equal(obs_data_get_string(settings, "profile"), "baseline");
	assert_string_equal(obs_data_get_string(settings, "preset"), "p7");
	assert_false(obs_data_get_bool(settings, "adaptive_quantization"));

	/* The old keys are gone. */
	assert_false(obs_data_has_user_value(settings, "preset2"));
	assert_false(obs_data_has_user_value(settings, "psycho_aq"));

	obs_data_release(settings);
}

static void gpu_index_needs_several_devices(void **state)
{
	UNUSED_PARAMETER(state);

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "gpu", 1);
	nvenc_migrate_legacy_settings(settings, NVENC_LEGACY_H264, 2);
	assert_int_equal(obs_data_get_int(settings, "device"), 1);
	assert_false(obs_data_has_user_value(settings, "gpu"));
	obs_data_release(settings);

	/* With one GPU the old index is dropped: "device" keeps meaning "the
	 * GPU OBS renders on". */
	settings = obs_data_create();
	obs_data_set_int(settings, "gpu", 1);
	nvenc_migrate_legacy_settings(settings, NVENC_LEGACY_H264, 1);
	assert_false(obs_data_has_user_value(settings, "device"));
	assert_false(obs_data_has_user_value(settings, "gpu"));
	obs_data_release(settings);
}

static void null_settings(void **state)
{
	UNUSED_PARAMETER(state);
	nvenc_migrate_legacy_settings(NULL, NVENC_LEGACY_H264, 1);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(untouched_settings_get_old_defaults),
		cmocka_unit_test(hevc_and_av1_default_to_main),
		cmocka_unit_test(user_values_are_kept_and_renamed),
		cmocka_unit_test(gpu_index_needs_several_devices),
		cmocka_unit_test(null_settings),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
