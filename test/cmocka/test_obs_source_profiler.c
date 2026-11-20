#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

#include <util/source-profiler.h>

/* util/source-profiler.c: per-source tick, render and async frame timings, collected on the graphics thread */

#define FRAME_W 8
#define FRAME_H 4

static uint8_t pixels[FRAME_W * FRAME_H * 4];

static void output_async_frame(obs_source_t *source)
{
	struct obs_source_frame frame = {0};
	frame.data[0] = pixels;
	frame.linesize[0] = FRAME_W * 4;
	frame.width = FRAME_W;
	frame.height = FRAME_H;
	frame.format = VIDEO_FORMAT_BGRA;
	frame.timestamp = os_gettime_ns();
	frame.full_range = true;
	obs_source_output_video(source, &frame);
}

/* renders both sources for a while, feeding the async one */
static void run_frames(obs_source_t *async, int frames)
{
	for (int i = 0; i < frames; i++) {
		output_async_frame(async);
		core_wait_frames(1);
	}
}

static bool wait_result(obs_source_t *source, profiler_result_t *result)
{
	for (int i = 0; i < 200; i++) {
		if (source_profiler_fill_result(source, result))
			return true;
		core_wait_frames(1);
	}
	return false;
}

static void profiler_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *input = obs_source_create("fake_input", "input", NULL, NULL);
	obs_source_t *async = obs_source_create("fake_async", "async", NULL, NULL);
	obs_source_set_async_unbuffered(async, true);
	obs_set_output_source(0, input);
	obs_set_output_source(1, async);

	/* disabled: no results */
	profiler_result_t result;
	run_frames(async, 3);
	assert_false(source_profiler_fill_result(input, &result));
	assert_null(source_profiler_get_result(input));

	/* CPU timings */
	source_profiler_enable(true);
	run_frames(async, 10);
	assert_true(wait_result(input, &result));
	assert_true(result.tick_max >= result.tick_avg);
	assert_true(result.render_max >= result.render_avg);
	assert_true(result.render_sum >= result.render_avg);
	assert_true(result.render_avg > 0);
	assert_true(result.async_input == 0.0);

	assert_true(wait_result(async, &result));
	run_frames(async, 10);
	assert_true(source_profiler_fill_result(async, &result));
	assert_true(result.async_input > 0.0);
	assert_true(result.async_input_worst >= result.async_input_best);

	profiler_result_t *copy = source_profiler_get_result(input);
	assert_non_null(copy);
	bfree(copy);
	assert_false(source_profiler_fill_result(input, NULL));

	/* GPU timings on top; the null graphics module's timers report no data */
	source_profiler_gpu_enable(true);
	run_frames(async, 10);
	assert_true(source_profiler_fill_result(input, &result));
	source_profiler_gpu_enable(false);
	run_frames(async, 3);
	assert_true(source_profiler_fill_result(input, &result));

	/* GPU profiling needs the profiler itself */
	source_profiler_enable(false);
	source_profiler_gpu_enable(true);
	run_frames(async, 3);
	assert_false(source_profiler_fill_result(input, &result));

	/* removing a profiled source drops its entry on the graphics thread */
	source_profiler_enable(true);
	source_profiler_gpu_enable(true);
	run_frames(async, 10);
	assert_true(wait_result(async, &result));
	obs_set_output_source(1, NULL);
	obs_source_release(async);
	core_wait_frames(3);
	assert_true(source_profiler_fill_result(input, &result));

	source_profiler_gpu_enable(false);
	source_profiler_enable(false);
	core_wait_frames(3);

	obs_set_output_source(0, NULL);
	obs_source_release(input);
	core_stop();
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(profiler_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
