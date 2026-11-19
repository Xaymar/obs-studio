#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

#include <media-io/video-frame.h>

#define FRAME_W 8
#define FRAME_H 4

static uint8_t pixels[FRAME_W * FRAME_H * 4];

static void make_frame(struct obs_source_frame *frame, enum video_format format, uint64_t ts)
{
	memset(frame, 0, sizeof(*frame));
	frame->data[0] = pixels;
	frame->linesize[0] = FRAME_W * 4;
	frame->width = FRAME_W;
	frame->height = FRAME_H;
	frame->format = format;
	frame->timestamp = ts;
	video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_FULL, format, frame->color_matrix,
					       frame->color_range_min, frame->color_range_max);
	frame->full_range = true;
}

/* waits until the source has picked up an async frame of the given width */
static bool wait_async_width(obs_source_t *source, uint32_t width)
{
	for (int i = 0; i < 200; i++) {
		if (obs_source_get_width(source) == width)
			return true;
		os_sleep_ms(5);
	}
	return false;
}

static void async_video_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *src = obs_source_create("fake_async", "async", NULL, NULL);
	obs_source_t *filter = obs_source_create("fake_async_filter", "filter", NULL, NULL);
	obs_source_filter_add(src, filter);
	/* without a frame the size is 0 */
	assert_int_equal(obs_source_get_width(src), 0);

	obs_set_output_source(0, src);
	obs_source_set_async_unbuffered(src, true);
	assert_true(obs_source_async_unbuffered(src));
	obs_source_set_async_decoupled(src, true);
	assert_true(obs_source_async_decoupled(src));
	obs_source_set_async_decoupled(src, false);

	struct obs_source_frame frame;
	make_frame(&frame, VIDEO_FORMAT_BGRA, os_gettime_ns());
	obs_source_output_video(src, &frame);
	assert_true(wait_async_width(src, FRAME_W));
	assert_int_equal(obs_source_get_height(src), FRAME_H);
	assert_true(fake_of(filter)->filter_frames > 0);

	/* rotation swaps the reported size */
	obs_source_set_async_rotation(src, 90);
	assert_int_equal(obs_source_get_width(src), FRAME_H);
	assert_int_equal(obs_source_get_height(src), FRAME_W);
	obs_source_set_async_rotation(src, 0);

	/* other formats go through the conversion shaders */
	static const enum video_format formats[] = {VIDEO_FORMAT_RGBA, VIDEO_FORMAT_BGRX, VIDEO_FORMAT_Y800,
						    VIDEO_FORMAT_YUY2, VIDEO_FORMAT_UYVY, VIDEO_FORMAT_I420,
						    VIDEO_FORMAT_NV12, VIDEO_FORMAT_I444};
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		make_frame(&frame, formats[i], os_gettime_ns());
		struct video_frame planes;
		video_frame_init(&planes, formats[i], FRAME_W, FRAME_H);
		for (size_t p = 0; p < MAX_AV_PLANES; p++) {
			frame.data[p] = planes.data[p];
			frame.linesize[p] = planes.linesize[p];
		}
		obs_source_output_video(src, &frame);
		core_wait_frames(2);
		video_frame_free(&planes);
	}

	/* deinterlacing renders through the deinterlace effects */
	static const enum obs_deinterlace_mode modes[] = {
		OBS_DEINTERLACE_MODE_DISCARD,  OBS_DEINTERLACE_MODE_RETRO,    OBS_DEINTERLACE_MODE_BLEND,
		OBS_DEINTERLACE_MODE_BLEND_2X, OBS_DEINTERLACE_MODE_LINEAR,   OBS_DEINTERLACE_MODE_LINEAR_2X,
		OBS_DEINTERLACE_MODE_YADIF,    OBS_DEINTERLACE_MODE_YADIF_2X,
	};
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		obs_source_set_deinterlace_mode(src, modes[i]);
		assert_int_equal(obs_source_get_deinterlace_mode(src), modes[i]);
		for (int f = 0; f < 3; f++) {
			make_frame(&frame, VIDEO_FORMAT_BGRA, os_gettime_ns());
			obs_source_output_video(src, &frame);
			core_wait_frames(1);
		}
	}
	obs_source_set_deinterlace_mode(src, OBS_DEINTERLACE_MODE_DISABLE);

	/* preloading shows a frame on request */
	make_frame(&frame, VIDEO_FORMAT_BGRA, os_gettime_ns());
	frame.width = FRAME_W / 2;
	obs_source_preload_video(src, &frame);
	obs_source_show_preloaded_video(src);
	assert_true(wait_async_width(src, FRAME_W / 2));

	/* setting a frame directly */
	make_frame(&frame, VIDEO_FORMAT_BGRA, os_gettime_ns());
	obs_source_set_video_frame(src, &frame);
	assert_true(wait_async_width(src, FRAME_W));

	/* a NULL frame clears the source */
	obs_source_output_video(src, NULL);
	assert_true(wait_async_width(src, 0));

	obs_set_output_source(0, NULL);
	obs_source_filter_remove(src, filter);
	obs_source_release(filter);
	obs_source_release(src);
	core_stop();
}

/* a buffered source deinterlacing a steady 60 fps stream, with a timestamp jump and both field orders */
static void deinterlace_buffered_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *src = obs_source_create("fake_async", "deint", NULL, NULL);
	obs_set_output_source(0, src);

	static const enum obs_deinterlace_mode modes[] = {
		OBS_DEINTERLACE_MODE_BLEND_2X, OBS_DEINTERLACE_MODE_LINEAR_2X, OBS_DEINTERLACE_MODE_YADIF_2X,
		OBS_DEINTERLACE_MODE_YADIF,    OBS_DEINTERLACE_MODE_DISCARD,
	};
	const uint64_t interval = 1000000000ULL / 60;
	uint64_t ts = os_gettime_ns();
	struct obs_source_frame frame;
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		obs_source_set_deinterlace_mode(src, modes[i]);
		obs_source_set_deinterlace_field_order(src, i % 2 ? OBS_DEINTERLACE_FIELD_ORDER_BOTTOM
								  : OBS_DEINTERLACE_FIELD_ORDER_TOP);
		for (int f = 0; f < 12; f++) {
			/* a jump of a second halfway through */
			if (f == 6)
				ts += 1000000000ULL;
			make_frame(&frame, f % 3 ? VIDEO_FORMAT_BGRA : VIDEO_FORMAT_RGBA, ts);
			obs_source_output_video(src, &frame);
			ts += interval;
			if (f % 2)
				core_wait_frames(1);
		}
		core_wait_frames(3);
		assert_int_equal(obs_source_get_width(src), FRAME_W);
	}

	/* turning it off again goes back to plain frames */
	obs_source_set_deinterlace_mode(src, OBS_DEINTERLACE_MODE_DISABLE);
	make_frame(&frame, VIDEO_FORMAT_BGRA, ts);
	obs_source_output_video(src, &frame);
	core_wait_frames(3);

	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

/* every async video format through obs_source_output_video2(), with both ranges and every transfer function, on an
 * SDR and on an HDR canvas; then preloading and setting frames the same way */
static void async_formats_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *src = obs_source_create("fake_async", "formats", NULL, NULL);
	obs_source_set_async_unbuffered(src, true);
	obs_set_output_source(0, src);

	static const uint8_t trcs[] = {VIDEO_TRC_DEFAULT, VIDEO_TRC_SRGB, VIDEO_TRC_PQ, VIDEO_TRC_HLG};
	for (int f = VIDEO_FORMAT_I420; f <= VIDEO_FORMAT_R10L; f++) {
		for (int variant = 0; variant < 2; variant++) {
			struct video_frame planes;
			video_frame_init(&planes, (enum video_format)f, FRAME_W, FRAME_H);
			struct obs_source_frame2 frame = {0};
			for (size_t p = 0; p < MAX_AV_PLANES; p++) {
				frame.data[p] = planes.data[p];
				frame.linesize[p] = planes.linesize[p];
			}
			frame.width = FRAME_W;
			frame.height = FRAME_H;
			frame.format = (enum video_format)f;
			frame.range = variant ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
			frame.trc = trcs[(f + variant) % 4];
			frame.flip = variant != 0;
			frame.timestamp = os_gettime_ns();
			enum video_colorspace cs = frame.trc == VIDEO_TRC_PQ	? VIDEO_CS_2100_PQ
						   : frame.trc == VIDEO_TRC_HLG ? VIDEO_CS_2100_HLG
										: VIDEO_CS_709;
			video_format_get_parameters_for_format(cs, frame.range, frame.format, frame.color_matrix,
							       frame.color_range_min, frame.color_range_max);
			obs_source_output_video2(src, &frame);
			core_wait_frames(2);
			video_frame_free(&planes);
		}
	}

	/* preloading and setting frames */
	struct video_frame planes;
	video_frame_init(&planes, VIDEO_FORMAT_I420, FRAME_W, FRAME_H);
	struct obs_source_frame2 frame = {0};
	for (size_t p = 0; p < MAX_AV_PLANES; p++) {
		frame.data[p] = planes.data[p];
		frame.linesize[p] = planes.linesize[p];
	}
	frame.width = FRAME_W;
	frame.height = FRAME_H;
	frame.format = VIDEO_FORMAT_I420;
	frame.range = VIDEO_RANGE_PARTIAL;
	video_format_get_parameters_for_format(VIDEO_CS_601, frame.range, frame.format, frame.color_matrix,
					       frame.color_range_min, frame.color_range_max);
	obs_source_preload_video2(src, &frame);
	obs_source_show_preloaded_video(src);
	core_wait_frames(2);
	obs_source_set_video_frame2(src, &frame);
	core_wait_frames(2);
	obs_source_output_video2(src, NULL);
	video_frame_free(&planes);

	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

static void get_frame_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	/* a source nobody renders keeps the frame for obs_source_get_frame() after a tick */
	obs_source_t *src = obs_source_create("fake_async", "async", NULL, NULL);
	obs_set_output_source(0, src);
	struct obs_source_frame frame;
	make_frame(&frame, VIDEO_FORMAT_BGRA, os_gettime_ns());
	obs_source_output_video(src, &frame);
	assert_true(wait_async_width(src, FRAME_W));
	obs_set_output_source(0, NULL);

	struct obs_source_frame *got = obs_source_get_frame(src);
	if (got) {
		assert_int_equal(got->width, FRAME_W);
		obs_source_release_frame(src, got);
	}
	obs_source_release_frame(src, NULL);

	/* frame helpers */
	struct obs_source_frame *a = obs_source_frame_create(VIDEO_FORMAT_I420, 16, 8);
	struct obs_source_frame *b = obs_source_frame_create(VIDEO_FORMAT_I420, 16, 8);
	memset(a->data[0], 0x80, a->linesize[0] * 8);
	a->timestamp = 1234;
	obs_source_frame_copy(b, a);
	assert_int_equal(b->timestamp, 1234);
	assert_int_equal(b->data[0][5], 0x80);
	obs_source_frame_destroy(a);
	obs_source_frame_destroy(b);
	obs_source_frame_destroy(NULL);

	obs_source_release(src);
	core_stop();
}

static int captured;
static size_t captured_frames;
static void on_audio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(muted);
	captured++;
	captured_frames += audio->frames;
}

static void audio_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *src = obs_source_create("fake_async", "audio", NULL, NULL);
	obs_source_t *filter = obs_source_create("fake_async_filter", "filter", NULL, NULL);
	obs_source_filter_add(src, filter);
	obs_set_output_source(0, src);
	captured = 0;
	captured_frames = 0;
	obs_source_add_audio_capture_callback(src, on_audio, NULL);

	static float samples[2][480];
	for (int i = 0; i < 480; i++)
		samples[0][i] = samples[1][i] = (i % 48) / 48.0f - 0.5f;

	uint64_t ts = os_gettime_ns();
	for (int i = 0; i < 20; i++) {
		struct obs_source_audio audio = {
			.data = {(uint8_t *)samples[0], (uint8_t *)samples[1]},
			.frames = 480,
			.speakers = SPEAKERS_STEREO,
			.format = AUDIO_FORMAT_FLOAT_PLANAR,
			.samples_per_sec = 48000,
			.timestamp = ts + i * 10000000ULL,
		};
		obs_source_output_audio(src, &audio);
	}
	assert_true(captured >= 20);
	assert_int_equal(captured_frames, 20 * 480);
	assert_true(fake_of(filter)->filter_audios >= 20);
	assert_true(obs_source_audio_active(src));

	/* other formats and rates are resampled */
	static int16_t pcm[960];
	struct obs_source_audio audio16 = {
		.data = {(uint8_t *)pcm},
		.frames = 480,
		.speakers = SPEAKERS_MONO,
		.format = AUDIO_FORMAT_16BIT,
		.samples_per_sec = 44100,
		.timestamp = ts + 200000000ULL,
	};
	obs_source_output_audio(src, &audio16);

	/* muted and force-mono sources still report their audio */
	obs_source_set_muted(src, true);
	obs_source_set_flags(src, OBS_SOURCE_FLAG_FORCE_MONO);
	obs_source_set_balance_value(src, 0.0f);
	struct obs_source_audio audio = {
		.data = {(uint8_t *)samples[0], (uint8_t *)samples[1]},
		.frames = 480,
		.speakers = SPEAKERS_STEREO,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.samples_per_sec = 48000,
		.timestamp = ts + 210000000ULL,
	};
	int before = captured;
	obs_source_output_audio(src, &audio);
	assert_true(captured > before);

	/* the audio thread mixes it */
	os_sleep_ms(100);
	obs_source_set_audio_active(src, false);
	assert_false(obs_source_audio_active(src));
	obs_source_set_audio_active(src, true);

	obs_source_remove_audio_capture_callback(src, on_audio, NULL);
	before = captured;
	obs_source_output_audio(src, &audio);
	assert_int_equal(captured, before);

	obs_set_output_source(0, NULL);
	obs_source_filter_remove(src, filter);
	obs_source_release(filter);
	obs_source_release(src);
	core_stop();
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(async_video_test),
		cmocka_unit_test(get_frame_test),
		cmocka_unit_test(async_formats_test),
		cmocka_unit_test(deinterlace_buffered_test),
		cmocka_unit_test(audio_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
