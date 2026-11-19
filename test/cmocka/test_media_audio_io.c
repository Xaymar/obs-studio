#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <math.h>
#include <string.h>

#include <obs.h>
#include <media-io/audio-io.h>
#include <media-io/audio-math.h>
#include <util/platform.h>
#include <util/threading.h>

/* audio-io.c, plus the header helpers in audio-io.h and audio-math.h */

/* the audio thread names itself through obs_get_profiler_name_store(), so libobs must be started; the leak check
 * spans startup and shutdown because the name lives in libobs' name store until then */
static int setup(void **state)
{
	obs_test_setup(state);
	return obs_startup("en-US", NULL, NULL) ? 0 : -1;
}

static int teardown(void **state)
{
	obs_shutdown();
	return obs_test_teardown(state);
}

struct source {
	volatile long calls;
	bool fail;
	float value;
};

static bool input_cb(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *new_ts, uint32_t active_mixers,
		     struct audio_output_data *mixes)
{
	struct source *src = param;
	UNUSED_PARAMETER(end_ts);
	os_atomic_inc_long(&src->calls);
	if (src->fail)
		return false;
	*new_ts = start_ts;
	for (size_t mix = 0; mix < MAX_AUDIO_MIXES; mix++) {
		if (!(active_mixers & (1 << mix)))
			continue;
		for (size_t ch = 0; ch < 2; ch++)
			for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
				mixes[mix].data[ch][i] = src->value;
	}
	return true;
}

struct sink {
	volatile long calls;
	size_t mix_idx;
	uint32_t frames;
	float first;
};

static void output_cb(void *param, size_t mix_idx, struct audio_data *data)
{
	struct sink *s = param;
	s->mix_idx = mix_idx;
	s->frames = data->frames;
	if (data->data[0])
		s->first = ((float *)data->data[0])[0];
	os_atomic_inc_long(&s->calls);
}

static void wait_calls(volatile long *calls, long count)
{
	for (int i = 0; i < 300 && os_atomic_load_long(calls) < count; i++)
		os_sleep_ms(5);
}

static struct audio_output_info make_info(struct source *src)
{
	struct audio_output_info info = {
		.name = "test",
		.samples_per_sec = 48000,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers = SPEAKERS_STEREO,
		.input_callback = input_cb,
		.input_param = src,
	};
	return info;
}

static void open_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct source src = {0};
	struct audio_output_info info = make_info(&src);
	audio_t *audio = NULL;
	assert_int_equal(audio_output_open(&audio, &info), AUDIO_OUTPUT_SUCCESS);
	assert_int_equal(audio_output_get_block_size(audio), 4);
	assert_int_equal(audio_output_get_planes(audio), 2);
	assert_int_equal(audio_output_get_channels(audio), 2);
	assert_int_equal(audio_output_get_sample_rate(audio), 48000);
	assert_string_equal(audio_output_get_info(audio)->name, "test");
	assert_false(audio_output_active(audio));
	/* the thread asks for input even without outputs */
	wait_calls(&src.calls, 2);
	assert_true(os_atomic_load_long(&src.calls) >= 2);
	audio_output_close(audio);

	/* interleaved output: one plane, the block holds every channel */
	info.format = AUDIO_FORMAT_FLOAT;
	assert_int_equal(audio_output_open(&audio, &info), AUDIO_OUTPUT_SUCCESS);
	assert_int_equal(audio_output_get_block_size(audio), 8);
	assert_int_equal(audio_output_get_planes(audio), 1);
	audio_output_close(audio);

	struct audio_output_info bad = make_info(&src);
	bad.format = AUDIO_FORMAT_UNKNOWN;
	assert_int_equal(audio_output_open(&audio, &bad), AUDIO_OUTPUT_INVALIDPARAM);
	bad = make_info(&src);
	bad.name = NULL;
	assert_int_equal(audio_output_open(&audio, &bad), AUDIO_OUTPUT_INVALIDPARAM);
	bad = make_info(&src);
	bad.samples_per_sec = 0;
	assert_int_equal(audio_output_open(&audio, &bad), AUDIO_OUTPUT_INVALIDPARAM);
	bad = make_info(&src);
	bad.speakers = SPEAKERS_UNKNOWN;
	assert_int_equal(audio_output_open(&audio, &bad), AUDIO_OUTPUT_INVALIDPARAM);

	audio_output_close(NULL);
	assert_null(audio_output_get_info(NULL));
	assert_false(audio_output_active(NULL));
	assert_false(audio_output_connect(NULL, 0, NULL, output_cb, NULL));
	audio_output_disconnect(NULL, 0, output_cb, NULL);
}

static void output_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* out of range samples are clamped unless the input allows clipping */
	struct source src = {.value = 2.0f};
	struct audio_output_info info = make_info(&src);
	audio_t *audio = NULL;
	assert_int_equal(audio_output_open(&audio, &info), AUDIO_OUTPUT_SUCCESS);

	struct sink clamped = {0}, clipping = {0}, resampled = {0}, other_mix = {0};
	assert_true(audio_output_connect(audio, 0, NULL, output_cb, &clamped));
	assert_false(audio_output_connect(audio, 0, NULL, output_cb, &clamped)); /* already connected */
	assert_false(audio_output_connect(audio, MAX_AUDIO_MIXES, NULL, output_cb, &clamped));

	struct audio_convert_info allow = {0, AUDIO_FORMAT_UNKNOWN, SPEAKERS_UNKNOWN, true};
	assert_true(audio_output_connect(audio, 0, &allow, output_cb, &clipping));

	struct audio_convert_info to_mono = {24000, AUDIO_FORMAT_FLOAT, SPEAKERS_MONO, false};
	assert_true(audio_output_connect(audio, 0, &to_mono, output_cb, &resampled));

	assert_true(audio_output_connect(audio, 3, NULL, output_cb, &other_mix));
	assert_true(audio_output_active(audio));

	wait_calls(&clamped.calls, 3);
	wait_calls(&clipping.calls, 3);
	wait_calls(&resampled.calls, 3);
	wait_calls(&other_mix.calls, 3);
	assert_true(clamped.first == 1.0f);
	assert_int_equal(clamped.frames, AUDIO_OUTPUT_FRAMES);
	assert_true(clipping.first == 2.0f);
	assert_true(resampled.frames > 0 && resampled.frames <= AUDIO_OUTPUT_FRAMES);
	assert_int_equal(other_mix.mix_idx, 3);

	/* NaN becomes silence */
	src.value = NAN;
	long seen = os_atomic_load_long(&clamped.calls);
	wait_calls(&clamped.calls, seen + 3);
	assert_true(clamped.first == 0.0f);

	/* a failing input callback produces no output */
	src.fail = true;
	os_sleep_ms(50);
	seen = os_atomic_load_long(&clamped.calls);
	os_sleep_ms(60);
	assert_int_equal(os_atomic_load_long(&clamped.calls), seen);

	audio_output_disconnect(audio, 0, output_cb, &clamped);
	audio_output_disconnect(audio, 0, output_cb, &clamped); /* not connected anymore */
	audio_output_disconnect(audio, MAX_AUDIO_MIXES, output_cb, &clamped);
	audio_output_disconnect(audio, 0, output_cb, &clipping);
	audio_output_disconnect(audio, 3, output_cb, &other_mix);
	/* resampled stays connected: close frees it */
	audio_output_close(audio);
}

static void helper_test(void **state)
{
	UNUSED_PARAMETER(state);

	const uint32_t channels[] = {0, 1, 2, 3, 4, 5, 6, 0, 8};
	for (int s = SPEAKERS_UNKNOWN; s <= SPEAKERS_7POINT1; s++)
		assert_int_equal(get_audio_channels((enum speaker_layout)s), channels[s]);

	const size_t bytes[] = {0, 1, 2, 4, 4, 1, 2, 4, 4};
	for (int f = AUDIO_FORMAT_UNKNOWN; f <= AUDIO_FORMAT_FLOAT_PLANAR; f++) {
		assert_int_equal(get_audio_bytes_per_channel((enum audio_format)f), bytes[f]);
		assert_int_equal(is_audio_planar((enum audio_format)f), f >= AUDIO_FORMAT_U8BIT_PLANAR);
	}
	assert_int_equal(get_audio_bytes_per_channel((enum audio_format)99), 0);
	assert_false(is_audio_planar((enum audio_format)99));

	assert_int_equal(get_audio_planes(AUDIO_FORMAT_FLOAT_PLANAR, SPEAKERS_5POINT1), 6);
	assert_int_equal(get_audio_planes(AUDIO_FORMAT_FLOAT, SPEAKERS_5POINT1), 1);
	assert_int_equal(get_audio_size(AUDIO_FORMAT_16BIT, SPEAKERS_STEREO, 10), 40);
	assert_int_equal(get_audio_size(AUDIO_FORMAT_16BIT_PLANAR, SPEAKERS_STEREO, 10), 20);
	assert_int_equal(get_total_audio_size(AUDIO_FORMAT_16BIT_PLANAR, SPEAKERS_STEREO, 10), 40);
	assert_int_equal(audio_frames_to_ns(48000, 48000), 1000000000ULL);
	assert_int_equal(ns_to_audio_frames(48000, 500000000ULL), 24000);

	assert_true(isinf(mul_to_db(0.0f)) && mul_to_db(0.0f) < 0);
	assert_true(fabsf(mul_to_db(1.0f)) < 1e-6f);
	assert_true(fabsf(mul_to_db(0.5f) + 6.0206f) < 1e-3f);
	assert_true(fabsf(db_to_mul(0.0f) - 1.0f) < 1e-6f);
	assert_true(db_to_mul(-INFINITY) == 0.0f);
	assert_true(fabsf(db_to_mul(-6.0206f) - 0.5f) < 1e-4f);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(open_test, setup, teardown),
		cmocka_unit_test_setup_teardown(output_test, setup, teardown),
		obs_unit_test(helper_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
