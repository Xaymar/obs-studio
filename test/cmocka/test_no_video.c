#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>
#include <util/bmem.h>

#include <stdio.h>

/* Before obs_reset_video the main canvas has no mix. Queries on it must
 * report "no video" instead of dereferencing NULL. */

static long allocs_before;

static int setup(void **state)
{
	UNUSED_PARAMETER(state);
	allocs_before = bnum_allocs();
	return obs_startup("en-US", NULL, NULL) ? 0 : -1;
}

static int teardown(void **state)
{
	UNUSED_PARAMETER(state);
	obs_shutdown();
	if (bnum_allocs() != allocs_before) {
		fprintf(stderr, "leaked %ld allocations\n", bnum_allocs() - allocs_before);
		return -1;
	}
	return 0;
}

static void raw_callback(void *param, struct video_data *frame)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(frame);
}

static void main_canvas_queries(void **state)
{
	UNUSED_PARAMETER(state);

	assert_null(obs_get_video());
	assert_null(obs_get_main_texture());

	struct obs_video_info ovi;
	assert_false(obs_get_video_info(&ovi));

	/* No-ops without video, and must not crash. */
	obs_add_raw_video_callback(NULL, raw_callback, NULL);
	obs_remove_raw_video_callback(raw_callback, NULL);
}

static void raw_output_without_video(void **state)
{
	UNUSED_PARAMETER(state);

	/* An unknown output ID creates a raw (non-encoded) placeholder, which
	 * asks for the main video. */
	obs_output_t *output = obs_output_create("no.such.output", "placeholder", NULL, NULL);
	assert_non_null(output);
	assert_null(obs_output_video(output));
	obs_output_release(output);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(main_canvas_queries, setup, teardown),
		cmocka_unit_test_setup_teardown(raw_output_without_video, setup, teardown),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
