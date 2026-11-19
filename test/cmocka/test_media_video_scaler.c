#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <media-io/video-frame.h>
#include <media-io/video-scaler.h>

static const enum video_format supported[] = {
	VIDEO_FORMAT_I420, VIDEO_FORMAT_NV12, VIDEO_FORMAT_YUY2, VIDEO_FORMAT_UYVY, VIDEO_FORMAT_YVYU,
	VIDEO_FORMAT_RGBA, VIDEO_FORMAT_BGRA, VIDEO_FORMAT_BGRX, VIDEO_FORMAT_Y800, VIDEO_FORMAT_I444,
	VIDEO_FORMAT_I412, VIDEO_FORMAT_BGR3, VIDEO_FORMAT_I422, VIDEO_FORMAT_I210, VIDEO_FORMAT_I40A,
	VIDEO_FORMAT_I42A, VIDEO_FORMAT_YUVA, VIDEO_FORMAT_YA2L, VIDEO_FORMAT_I010, VIDEO_FORMAT_P010,
	VIDEO_FORMAT_P216, VIDEO_FORMAT_P416,
};

static void create_test(void **state)
{
	UNUSED_PARAMETER(state);

	for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
		struct video_scale_info src = {supported[i], 64, 32, VIDEO_RANGE_PARTIAL, VIDEO_CS_709};
		struct video_scale_info dst = {VIDEO_FORMAT_BGRA, 32, 16, VIDEO_RANGE_FULL, VIDEO_CS_SRGB};
		video_scaler_t *scaler = NULL;
		if (video_scaler_create(&scaler, &dst, &src, VIDEO_SCALE_BICUBIC) != VIDEO_SCALER_SUCCESS)
			fail_msg("format %d to BGRA failed", supported[i]);
		video_scaler_destroy(scaler);

		if (video_scaler_create(&scaler, &src, &dst, VIDEO_SCALE_POINT) != VIDEO_SCALER_SUCCESS)
			fail_msg("BGRA to format %d failed", supported[i]);
		video_scaler_destroy(scaler);
	}

	/* every scale type, color space and range */
	const enum video_scale_type types[] = {VIDEO_SCALE_DEFAULT,  VIDEO_SCALE_POINT,   VIDEO_SCALE_FAST_BILINEAR,
					       VIDEO_SCALE_BILINEAR, VIDEO_SCALE_BICUBIC, (enum video_scale_type)99};
	const enum video_colorspace spaces[] = {VIDEO_CS_DEFAULT, VIDEO_CS_601,     VIDEO_CS_709,
						VIDEO_CS_SRGB,    VIDEO_CS_2100_PQ, VIDEO_CS_2100_HLG};
	const enum video_range_type ranges[] = {VIDEO_RANGE_DEFAULT, VIDEO_RANGE_PARTIAL, VIDEO_RANGE_FULL};
	for (size_t t = 0; t < 6; t++) {
		struct video_scale_info src = {VIDEO_FORMAT_NV12, 64, 32, ranges[t % 3], spaces[t]};
		struct video_scale_info dst = {VIDEO_FORMAT_I420, 32, 16, ranges[(t + 1) % 3], spaces[(t + 2) % 6]};
		video_scaler_t *scaler = NULL;
		assert_int_equal(video_scaler_create(&scaler, &dst, &src, types[t]), VIDEO_SCALER_SUCCESS);
		video_scaler_destroy(scaler);
	}

	struct video_scale_info ok = {VIDEO_FORMAT_NV12, 64, 32, VIDEO_RANGE_PARTIAL, VIDEO_CS_709};
	struct video_scale_info bad = {VIDEO_FORMAT_AYUV, 64, 32, VIDEO_RANGE_PARTIAL, VIDEO_CS_709};
	video_scaler_t *scaler = NULL;
	assert_int_equal(video_scaler_create(&scaler, &ok, &bad, VIDEO_SCALE_DEFAULT), VIDEO_SCALER_BAD_CONVERSION);
	assert_int_equal(video_scaler_create(&scaler, &bad, &ok, VIDEO_SCALE_DEFAULT), VIDEO_SCALER_BAD_CONVERSION);
	bad.format = VIDEO_FORMAT_NONE;
	assert_int_equal(video_scaler_create(&scaler, &ok, &bad, VIDEO_SCALE_DEFAULT), VIDEO_SCALER_BAD_CONVERSION);
	assert_int_equal(video_scaler_create(NULL, &ok, &ok, VIDEO_SCALE_DEFAULT), VIDEO_SCALER_FAILED);

	video_scaler_destroy(NULL);
}

static void scale_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* a uniform gray Y800 image scales to a uniform image of the same gray */
	struct video_scale_info src = {VIDEO_FORMAT_Y800, 64, 32, VIDEO_RANGE_FULL, VIDEO_CS_709};
	struct video_scale_info dst = {VIDEO_FORMAT_Y800, 32, 16, VIDEO_RANGE_FULL, VIDEO_CS_709};
	video_scaler_t *scaler = NULL;
	assert_int_equal(video_scaler_create(&scaler, &dst, &src, VIDEO_SCALE_BILINEAR), VIDEO_SCALER_SUCCESS);

	struct video_frame in, out;
	video_frame_init(&in, VIDEO_FORMAT_Y800, 64, 32);
	video_frame_init(&out, VIDEO_FORMAT_Y800, 32, 16);
	memset(in.data[0], 0x80, 64 * 32);
	memset(out.data[0], 0, 32 * 16);

	assert_true(video_scaler_scale(scaler, out.data, out.linesize, (const uint8_t *const *)in.data, in.linesize));
	for (int i = 0; i < 32 * 16; i++)
		assert_int_equal(out.data[0][i], 0x80);

	/* a linesize that does not match the scaler's goes through its internal buffer */
	struct video_frame padded;
	video_frame_init(&padded, VIDEO_FORMAT_Y800, 48, 16);
	padded.linesize[0] = 48;
	assert_true(
		video_scaler_scale(scaler, padded.data, padded.linesize, (const uint8_t *const *)in.data, in.linesize));
	assert_int_equal(padded.data[0][0], 0x80);
	assert_int_equal(padded.data[0][48 + 31], 0x80);

	/* an output linesize smaller than the scaler's gets each row cut to it */
	uint8_t narrow[16 * 16 + 1];
	memset(narrow, 0, sizeof(narrow));
	uint8_t *narrow_planes[MAX_AV_PLANES] = {narrow};
	uint32_t narrow_linesize[MAX_AV_PLANES] = {16};
	assert_true(video_scaler_scale(scaler, narrow_planes, narrow_linesize, (const uint8_t *const *)in.data,
				       in.linesize));
	assert_int_equal(narrow[0], 0x80);
	assert_int_equal(narrow[16 * 16 - 1], 0x80);
	assert_int_equal(narrow[16 * 16], 0);

	video_frame_free(&in);
	video_frame_free(&out);
	video_frame_free(&padded);
	video_scaler_destroy(scaler);

	uint8_t *none[MAX_AV_PLANES] = {0};
	uint32_t ls[MAX_AV_PLANES] = {0};
	assert_false(video_scaler_scale(NULL, none, ls, (const uint8_t *const *)none, ls));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(create_test),
		obs_unit_test(scale_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
