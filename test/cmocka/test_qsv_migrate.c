#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>

#include "qsv-migrate.h"

/* Settings saved by the deprecated v1 QSV H.264 encoder had one quantizer per
 * frame type. The current encoder has one for all of them. */

static void p_frame_quantizer_becomes_cqp(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "__ver", 1);
	obs_data_set_int(settings, "qpi", 18);
	obs_data_set_int(settings, "qpp", 20);
	obs_data_set_int(settings, "qpb", 22);
	obs_data_set_string(settings, "rate_control", "CQP");

	qsv_migrate_v1_settings(settings);

	assert_int_equal(obs_data_get_int(settings, "cqp"), 20);
	assert_string_equal(obs_data_get_string(settings, "rate_control"), "CQP");

	/* The old keys are gone. */
	assert_false(obs_data_has_user_value(settings, "qpi"));
	assert_false(obs_data_has_user_value(settings, "qpp"));
	assert_false(obs_data_has_user_value(settings, "qpb"));
	assert_false(obs_data_has_user_value(settings, "__ver"));

	obs_data_release(settings);
}

static void untouched_quantizers_stay_untouched(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "qpi", 18);
	obs_data_set_int(settings, "bitrate", 8000);

	qsv_migrate_v1_settings(settings);

	/* Only qpp carries over; the current encoder's default cqp applies. */
	assert_false(obs_data_has_user_value(settings, "cqp"));
	assert_false(obs_data_has_user_value(settings, "qpi"));
	assert_int_equal(obs_data_get_int(settings, "bitrate"), 8000);

	obs_data_release(settings);
}

static void existing_cqp_is_replaced_by_qpp(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *settings = obs_data_create();
	/* v1 ignored cqp, so a stray value must not win over what v1 used. */
	obs_data_set_int(settings, "cqp", 30);
	obs_data_set_int(settings, "qpp", 25);

	qsv_migrate_v1_settings(settings);

	assert_int_equal(obs_data_get_int(settings, "cqp"), 25);

	obs_data_release(settings);
}

static void stray_cqp_falls_back_to_default(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "cqp", 30);

	qsv_migrate_v1_settings(settings);

	assert_false(obs_data_has_user_value(settings, "cqp"));

	obs_data_release(settings);
}

static void null_settings(void **state)
{
	UNUSED_PARAMETER(state);
	qsv_migrate_v1_settings(NULL);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(p_frame_quantizer_becomes_cqp),
		cmocka_unit_test(untouched_quantizers_stay_untouched),
		cmocka_unit_test(existing_cqp_is_replaced_by_qpp),
		cmocka_unit_test(stray_cqp_falls_back_to_default),
		cmocka_unit_test(null_settings),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
