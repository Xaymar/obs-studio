#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <math.h>
#include <string.h>

#include <media-io/audio-resampler.h>

static void formats_test(void **state)
{
	UNUSED_PARAMETER(state);

	const enum audio_format formats[] = {AUDIO_FORMAT_U8BIT,        AUDIO_FORMAT_16BIT,
					     AUDIO_FORMAT_32BIT,        AUDIO_FORMAT_FLOAT,
					     AUDIO_FORMAT_U8BIT_PLANAR, AUDIO_FORMAT_16BIT_PLANAR,
					     AUDIO_FORMAT_32BIT_PLANAR, AUDIO_FORMAT_FLOAT_PLANAR};
	const enum speaker_layout layouts[] = {SPEAKERS_MONO,    SPEAKERS_STEREO,  SPEAKERS_2POINT1, SPEAKERS_4POINT0,
					       SPEAKERS_4POINT1, SPEAKERS_5POINT1, SPEAKERS_7POINT1};

	/* every format and layout converts to float planar stereo and back */
	for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		for (size_t l = 0; l < sizeof(layouts) / sizeof(layouts[0]); l++) {
			struct resample_info src = {48000, formats[f], layouts[l]};
			struct resample_info dst = {44100, AUDIO_FORMAT_FLOAT_PLANAR, SPEAKERS_STEREO};
			audio_resampler_t *rs = audio_resampler_create(&dst, &src);
			if (!rs)
				fail_msg("format %d layout %d to float planar stereo failed", formats[f], layouts[l]);
			audio_resampler_destroy(rs);

			rs = audio_resampler_create(&src, &dst);
			if (!rs)
				fail_msg("float planar stereo to format %d layout %d failed", formats[f], layouts[l]);
			audio_resampler_destroy(rs);
		}
	}

	audio_resampler_destroy(NULL);
}

static void resample_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* 48 kHz mono float to 24 kHz stereo float planar: the mono signal is copied to both channels */
	struct resample_info src = {48000, AUDIO_FORMAT_FLOAT, SPEAKERS_MONO};
	struct resample_info dst = {24000, AUDIO_FORMAT_FLOAT_PLANAR, SPEAKERS_STEREO};
	audio_resampler_t *rs = audio_resampler_create(&dst, &src);
	assert_non_null(rs);

	float in[4800];
	for (size_t i = 0; i < 4800; i++)
		in[i] = 0.5f;
	const uint8_t *input[MAX_AV_PLANES] = {(const uint8_t *)in};
	uint8_t *output[MAX_AV_PLANES] = {0};
	uint32_t total = 0;
	uint64_t ts_offset = 0;
	for (int block = 0; block < 4; block++) {
		uint32_t frames = 0;
		assert_true(audio_resampler_resample(rs, output, &frames, &ts_offset, input, 4800));
		total += frames;
	}
	/* 4 x 100 ms in, roughly 400 ms out at the new rate (minus the filter delay) */
	assert_true(total > 9000 && total <= 9600);

	const float *left = (const float *)output[0];
	const float *right = (const float *)output[1];
	assert_non_null(left);
	assert_non_null(right);
	assert_true(fabsf(left[100] - 0.5f) < 0.05f);
	assert_true(fabsf(right[100] - 0.5f) < 0.05f);

	audio_resampler_destroy(rs);

	uint32_t frames;
	assert_false(audio_resampler_resample(NULL, output, &frames, &ts_offset, input, 4800));
}

static void upmix_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* mono to every multi-channel layout uses the explicit upmix matrix */
	const enum speaker_layout layouts[] = {SPEAKERS_2POINT1, SPEAKERS_4POINT0, SPEAKERS_4POINT1, SPEAKERS_5POINT1,
					       SPEAKERS_7POINT1};
	for (size_t l = 0; l < sizeof(layouts) / sizeof(layouts[0]); l++) {
		struct resample_info src = {48000, AUDIO_FORMAT_FLOAT_PLANAR, SPEAKERS_MONO};
		struct resample_info dst = {48000, AUDIO_FORMAT_FLOAT_PLANAR, layouts[l]};
		audio_resampler_t *rs = audio_resampler_create(&dst, &src);
		assert_non_null(rs);

		float in[480];
		for (size_t i = 0; i < 480; i++)
			in[i] = 0.25f;
		const uint8_t *input[MAX_AV_PLANES] = {(const uint8_t *)in};
		uint8_t *output[MAX_AV_PLANES] = {0};
		uint32_t frames = 0;
		uint64_t ts_offset = 0;
		assert_true(audio_resampler_resample(rs, output, &frames, &ts_offset, input, 480));
		assert_true(frames > 0);
		audio_resampler_destroy(rs);
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(formats_test),
		obs_unit_test(resample_test),
		obs_unit_test(upmix_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
