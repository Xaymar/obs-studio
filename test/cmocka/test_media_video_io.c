#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <obs.h>
#include <media-io/video-frame.h>
#include <media-io/video-io.h>
#include <util/platform.h>
#include <util/threading.h>

/* video_output_create_with_frame_rate_divisor() and the texture encoder counters are not exported; they are
 * covered through the encoder tests instead. */

/* the video thread names itself through obs_get_profiler_name_store(), so libobs must be started */
static int setup(void **state)
{
	/* measured around startup and shutdown: the thread names live in libobs' name store until shutdown */
	obs_test_setup(state);
	return obs_startup("en-US", NULL, NULL) ? 0 : -1;
}

static int teardown(void **state)
{
	obs_shutdown();
	return obs_test_teardown(state);
}

struct received {
	volatile long frames;
	uint8_t first_byte;
	uint32_t linesize;
	uint64_t timestamp;
};

static void on_frame(void *param, struct video_data *frame)
{
	struct received *r = param;
	r->first_byte = frame->data[0][0];
	r->linesize = frame->linesize[0];
	r->timestamp = frame->timestamp;
	os_atomic_inc_long(&r->frames);
}

static void wait_frames(struct received *r, long count)
{
	for (int i = 0; i < 200 && os_atomic_load_long(&r->frames) < count; i++)
		os_sleep_ms(5);
}

static struct video_output_info make_info(void)
{
	struct video_output_info info = {
		.name = "test",
		.format = VIDEO_FORMAT_Y800,
		.fps_num = 30,
		.fps_den = 1,
		.width = 64,
		.height = 32,
		.cache_size = 4,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_FULL,
	};
	return info;
}

static void open_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct video_output_info info = make_info();
	video_t *video = NULL;
	assert_int_equal(video_output_open(&video, &info), VIDEO_OUTPUT_SUCCESS);
	assert_int_equal(video_output_get_format(video), VIDEO_FORMAT_Y800);
	assert_int_equal(video_output_get_width(video), 64);
	assert_int_equal(video_output_get_height(video), 32);
	assert_true(video_output_get_frame_rate(video) == 30.0);
	assert_int_equal(video_output_get_frame_time(video), 33333333);
	assert_string_equal(video_output_get_info(video)->name, "test");
	assert_false(video_output_active(video));
	assert_false(video_output_stopped(video));
	video_output_stop(video);
	assert_true(video_output_stopped(video));
	video_output_stop(video);
	video_output_close(video);

	struct video_output_info bad = make_info();
	bad.width = 0;
	assert_int_equal(video_output_open(&video, &bad), VIDEO_OUTPUT_INVALIDPARAM);
	bad = make_info();
	bad.fps_den = 0;
	assert_int_equal(video_output_open(&video, &bad), VIDEO_OUTPUT_INVALIDPARAM);
	/* Regression: an output without a frame cache was accepted and could never lock a frame */
	bad = make_info();
	bad.cache_size = 0;
	assert_int_equal(video_output_open(&video, &bad), VIDEO_OUTPUT_INVALIDPARAM);

	/* the cache is capped */
	struct video_output_info big = make_info();
	big.cache_size = 1000;
	assert_int_equal(video_output_open(&video, &big), VIDEO_OUTPUT_SUCCESS);
	assert_true(video_output_get_info(video)->cache_size < 1000);
	video_output_close(video);

	/* NULL is safe everywhere it is checked */
	video_output_close(NULL);
	video_output_stop(NULL);
	assert_true(video_output_stopped(NULL));
	assert_false(video_output_active(NULL));
	assert_null(video_output_get_info(NULL));
	assert_int_equal(video_output_get_frame_time(NULL), 0);
	assert_int_equal(video_output_get_format(NULL), VIDEO_FORMAT_NONE);
	assert_int_equal(video_output_get_width(NULL), 0);
	assert_int_equal(video_output_get_height(NULL), 0);
	assert_true(video_output_get_frame_rate(NULL) == 0.0);
	/* Regression: these two dereferenced NULL */
	assert_int_equal(video_output_get_skipped_frames(NULL), 0);
	assert_int_equal(video_output_get_total_frames(NULL), 0);
	struct video_frame frame;
	assert_false(video_output_lock_frame(NULL, &frame, 1, 0));
	video_output_unlock_frame(NULL);
	/* Regression: connect dereferenced NULL before checking it */
	assert_false(video_output_connect(NULL, NULL, on_frame, NULL));
	assert_false(video_output_disconnect2(NULL, on_frame, NULL));
}

static void frames_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct video_output_info info = make_info();
	video_t *video = NULL;
	assert_int_equal(video_output_open(&video, &info), VIDEO_OUTPUT_SUCCESS);

	struct received raw = {0};
	assert_true(video_output_connect(video, NULL, on_frame, &raw));
	assert_false(video_output_connect(video, NULL, on_frame, &raw)); /* already connected */
	assert_false(video_output_connect(video, NULL, NULL, &raw));
	assert_true(video_output_active(video));

	/* a converted input gets its own scaled frame */
	struct received scaled = {0};
	struct video_scale_info conv = {VIDEO_FORMAT_Y800, 32, 16, VIDEO_RANGE_FULL, VIDEO_CS_709};
	assert_true(video_output_connect(video, &conv, on_frame, &scaled));

	/* half the frame rate */
	struct received half = {0};
	assert_false(video_output_connect2(video, NULL, 0, on_frame, &half));
	assert_true(video_output_connect2(video, NULL, 2, on_frame, &half));

	/* a conversion the scaler cannot do is refused */
	struct received refused = {0};
	struct video_scale_info bad = {VIDEO_FORMAT_AYUV, 0, 0, VIDEO_RANGE_FULL, VIDEO_CS_709};
	assert_false(video_output_connect(video, &bad, on_frame, &refused));

	for (int i = 0; i < 4; i++) {
		struct video_frame frame;
		assert_true(video_output_lock_frame(video, &frame, 1, 1000 + i));
		memset(frame.data[0], 0x40 + i, 64 * 32);
		video_output_unlock_frame(video);
		wait_frames(&raw, i + 1);
	}

	assert_int_equal(os_atomic_load_long(&raw.frames), 4);
	assert_int_equal(raw.first_byte, 0x43);
	assert_int_equal(raw.linesize, 64);
	assert_int_equal(raw.timestamp, 1003);
	wait_frames(&scaled, 4);
	assert_int_equal(scaled.linesize, 32);
	assert_int_equal(scaled.first_byte, 0x43);
	assert_int_equal(os_atomic_load_long(&half.frames), 2);
	assert_true(video_output_get_total_frames(video) >= 4);

	/* a frame shown twice reaches the callbacks twice */
	struct video_frame frame;
	assert_true(video_output_lock_frame(video, &frame, 2, 5000));
	video_output_unlock_frame(video);
	wait_frames(&raw, 6);
	assert_int_equal(os_atomic_load_long(&raw.frames), 6);

	assert_true(video_output_disconnect2(video, on_frame, &half));
	assert_false(video_output_disconnect2(video, on_frame, &half));
	video_output_disconnect(video, on_frame, &scaled);
	video_output_disconnect(video, on_frame, &raw);
	assert_false(video_output_active(video));

	video_output_close(video);
}

static void skipped_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* stop the thread so frames pile up, then fill the cache */
	struct video_output_info info = make_info();
	info.cache_size = 2;
	video_t *video = NULL;
	assert_int_equal(video_output_open(&video, &info), VIDEO_OUTPUT_SUCCESS);
	struct received raw = {0};
	assert_true(video_output_connect(video, NULL, on_frame, &raw));
	video_output_stop(video);

	struct video_frame frame;
	assert_true(video_output_lock_frame(video, &frame, 1, 0));
	video_output_unlock_frame(video);
	assert_true(video_output_lock_frame(video, &frame, 1, 1));
	video_output_unlock_frame(video);
	/* no frame free: the newest frame is repeated and counted as skipped */
	assert_false(video_output_lock_frame(video, &frame, 1, 2));

	video_output_disconnect(video, on_frame, &raw);
	video_output_close(video);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(open_test, setup, teardown),
		cmocka_unit_test_setup_teardown(frames_test, setup, teardown),
		cmocka_unit_test_setup_teardown(skipped_test, setup, teardown),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
