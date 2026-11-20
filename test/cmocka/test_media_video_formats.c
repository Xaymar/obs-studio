#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <math.h>
#include <string.h>

#include <media-io/frame-rate.h>
#include <media-io/video-io.h>

/* video-fourcc.c, video-matrices.c, frame-rate.h and the format helpers in video-io.h */

/* same as the private macro in video-fourcc.c */
#define MAKE_FOURCC(a, b, c, d) ((uint32_t)(((d) << 24) | ((c) << 16) | ((b) << 8) | (a)))

static void fourcc_test(void **state)
{
	UNUSED_PARAMETER(state);

	const char *uyvy[] = {"UYVY", "HDYC", "UYNV", "UYNY", "uyv1", "2vuy", "2Vuy"};
	const char *yuy2[] = {"YUY2", "Y422", "V422", "VYUY", "YUNV", "yuv2", "yuvs"};
	for (size_t i = 0; i < 7; i++) {
		assert_int_equal(video_format_from_fourcc(MAKE_FOURCC(uyvy[i][0], uyvy[i][1], uyvy[i][2], uyvy[i][3])),
				 VIDEO_FORMAT_UYVY);
		assert_int_equal(video_format_from_fourcc(MAKE_FOURCC(yuy2[i][0], yuy2[i][1], yuy2[i][2], yuy2[i][3])),
				 VIDEO_FORMAT_YUY2);
	}
	assert_int_equal(video_format_from_fourcc(MAKE_FOURCC('Y', 'V', 'Y', 'U')), VIDEO_FORMAT_YVYU);
	assert_int_equal(video_format_from_fourcc(MAKE_FOURCC('Y', '8', '0', '0')), VIDEO_FORMAT_Y800);
	assert_int_equal(video_format_from_fourcc(MAKE_FOURCC('N', 'O', 'N', 'E')), VIDEO_FORMAT_NONE);
}

static void frame_rate_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct media_frames_per_second fps = {30000, 1001};
	assert_true(media_frames_per_second_is_valid(fps));
	assert_true(fabs(media_frames_per_second_to_fps(fps) - 29.97002997) < 1e-6);
	assert_true(fabs(media_frames_per_second_to_frame_interval(fps) - 0.033366667) < 1e-6);
	struct media_frames_per_second zero = {0, 1};
	assert_false(media_frames_per_second_is_valid(zero));
	zero.numerator = 1;
	zero.denominator = 0;
	assert_false(media_frames_per_second_is_valid(zero));
}

static void matrix_test(void **state)
{
	UNUSED_PARAMETER(state);

	float matrix[16], min[3], max[3];

	/* BT.709 partial range: Y' = 16..235, the luma row of the inverse matrix scales by 255/219 */
	assert_true(video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, matrix, min, max));
	assert_true(fabsf(min[0] - 16.0f / 255.0f) < 1e-5f);
	assert_true(fabsf(max[0] - 235.0f / 255.0f) < 1e-5f);
	assert_true(fabsf(matrix[0] - 255.0f / 219.0f) < 1e-4f);

	assert_true(video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_FULL, matrix, min, max));
	assert_true(fabsf(min[0]) < 1e-6f);
	assert_true(fabsf(max[0] - 1.0f) < 1e-6f);
	assert_true(fabsf(matrix[0] - 1.0f) < 1e-4f);

	/* default and sRGB map to 709, HLG to PQ */
	float m709[16], other[16];
	video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, m709, min, max);
	assert_true(video_format_get_parameters(VIDEO_CS_DEFAULT, VIDEO_RANGE_PARTIAL, other, min, max));
	assert_memory_equal(m709, other, sizeof(m709));
	assert_true(video_format_get_parameters(VIDEO_CS_SRGB, VIDEO_RANGE_PARTIAL, other, min, max));
	assert_memory_equal(m709, other, sizeof(m709));

	float pq[16];
	assert_true(video_format_get_parameters(VIDEO_CS_2100_PQ, VIDEO_RANGE_PARTIAL, pq, min, max));
	assert_true(video_format_get_parameters(VIDEO_CS_2100_HLG, VIDEO_RANGE_PARTIAL, other, min, max));
	assert_memory_equal(pq, other, sizeof(pq));
	assert_true(video_format_get_parameters(VIDEO_CS_601, VIDEO_RANGE_PARTIAL, other, min, max));
	assert_memory_not_equal(m709, other, sizeof(m709));

	/* range limits scale with the bit depth of the format */
	const enum video_format formats[] = {VIDEO_FORMAT_NV12, VIDEO_FORMAT_I010, VIDEO_FORMAT_P010, VIDEO_FORMAT_I210,
					     VIDEO_FORMAT_V210, VIDEO_FORMAT_R10L, VIDEO_FORMAT_I412, VIDEO_FORMAT_YA2L,
					     VIDEO_FORMAT_P216, VIDEO_FORMAT_P416};
	const float expected_min[] = {16.0f / 255.0f,     64.0f / 1023.0f,   64.0f / 1023.0f,  64.0f / 1023.0f,
				      64.0f / 1023.0f,    64.0f / 1023.0f,   256.0f / 4095.0f, 256.0f / 4095.0f,
				      4096.0f / 65535.0f, 4096.0f / 65535.0f};
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		assert_true(video_format_get_parameters_for_format(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, formats[i],
								   matrix, min, max));
		if (fabsf(min[0] - expected_min[i]) > 1e-5f)
			fail_msg("format %d: min %f, expected %f", formats[i], min[0], expected_min[i]);
	}

	/* an unknown color space fails */
	assert_false(video_format_get_parameters((enum video_colorspace)99, VIDEO_RANGE_PARTIAL, matrix, min, max));
}

static void helper_test(void **state)
{
	UNUSED_PARAMETER(state);

	for (int f = VIDEO_FORMAT_NONE; f <= VIDEO_FORMAT_R10L; f++) {
		assert_non_null(get_video_format_name((enum video_format)f));
		format_is_yuv((enum video_format)f);
		assert_non_null(get_video_range_name((enum video_format)f, VIDEO_RANGE_DEFAULT));
	}
	assert_true(format_is_yuv(VIDEO_FORMAT_NV12));
	assert_false(format_is_yuv(VIDEO_FORMAT_BGRA));
	assert_string_equal(get_video_format_name(VIDEO_FORMAT_NV12), "NV12");
	assert_string_equal(get_video_format_name((enum video_format)999), "None");

	for (int cs = VIDEO_CS_DEFAULT; cs <= VIDEO_CS_2100_HLG; cs++)
		assert_non_null(get_video_colorspace_name((enum video_colorspace)cs));

	/* the default range is partial for YUV and full for RGB; an explicit range is kept */
	assert_int_equal(resolve_video_range(VIDEO_FORMAT_BGRA, VIDEO_RANGE_DEFAULT), VIDEO_RANGE_FULL);
	assert_int_equal(resolve_video_range(VIDEO_FORMAT_BGRA, VIDEO_RANGE_PARTIAL), VIDEO_RANGE_PARTIAL);
	assert_int_equal(resolve_video_range(VIDEO_FORMAT_NV12, VIDEO_RANGE_DEFAULT), VIDEO_RANGE_PARTIAL);
	assert_int_equal(resolve_video_range(VIDEO_FORMAT_NV12, VIDEO_RANGE_FULL), VIDEO_RANGE_FULL);
	assert_string_equal(get_video_range_name(VIDEO_FORMAT_NV12, VIDEO_RANGE_FULL), "Full");
	assert_string_equal(get_video_range_name(VIDEO_FORMAT_NV12, VIDEO_RANGE_PARTIAL), "Partial");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(fourcc_test),
		obs_unit_test(frame_rate_test),
		obs_unit_test(matrix_test),
		obs_unit_test(helper_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
