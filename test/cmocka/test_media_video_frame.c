#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <media-io/video-frame.h>

struct layout {
	enum video_format format;
	uint32_t linesize[4];
	uint32_t heights[4];
};

/* 10x6 frame; V210 rounds the width up to 48 pixels (regression: it used a power-of-two mask and got 16). VIDEO_FORMAT_NONE is covered below. */
static const struct layout layouts[] = {
	{VIDEO_FORMAT_I420, {10, 5, 5}, {6, 3, 3}},
	{VIDEO_FORMAT_NV12, {10, 10}, {6, 3}},
	{VIDEO_FORMAT_YVYU, {20}, {6}},
	{VIDEO_FORMAT_YUY2, {20}, {6}},
	{VIDEO_FORMAT_UYVY, {20}, {6}},
	{VIDEO_FORMAT_RGBA, {40}, {6}},
	{VIDEO_FORMAT_BGRA, {40}, {6}},
	{VIDEO_FORMAT_BGRX, {40}, {6}},
	{VIDEO_FORMAT_Y800, {10}, {6}},
	{VIDEO_FORMAT_I444, {10, 10, 10}, {6, 6, 6}},
	{VIDEO_FORMAT_BGR3, {30}, {6}},
	{VIDEO_FORMAT_I422, {10, 5, 5}, {6, 6, 6}},
	{VIDEO_FORMAT_I40A, {10, 5, 5, 10}, {6, 3, 3, 6}},
	{VIDEO_FORMAT_I42A, {10, 5, 5, 10}, {6, 6, 6, 6}},
	{VIDEO_FORMAT_YUVA, {10, 10, 10, 10}, {6, 6, 6, 6}},
	{VIDEO_FORMAT_AYUV, {40}, {6}},
	{VIDEO_FORMAT_I010, {20, 10, 10}, {6, 3, 3}},
	{VIDEO_FORMAT_P010, {20, 20}, {6, 3}},
	{VIDEO_FORMAT_I210, {20, 10, 10}, {6, 6, 6}},
	{VIDEO_FORMAT_I412, {20, 20, 20}, {6, 6, 6}},
	{VIDEO_FORMAT_YA2L, {20, 20, 20, 20}, {6, 6, 6, 6}},
	{VIDEO_FORMAT_P216, {20, 20}, {6, 6}},
	{VIDEO_FORMAT_P416, {20, 40}, {6, 6}},
	{VIDEO_FORMAT_V210, {128}, {6}},
	{VIDEO_FORMAT_R10L, {40}, {6}},
};

static void frame_test(void **state)
{
	UNUSED_PARAMETER(state);

	for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
		const struct layout *l = &layouts[i];
		struct video_frame *src = video_frame_create(l->format, 10, 6);
		struct video_frame *dst = video_frame_create(l->format, 10, 6);
		assert_non_null(src->data[0]);

		/* planes follow each other in one aligned allocation */
		for (int p = 0; p < 4; p++) {
			if (src->linesize[p] != l->linesize[p])
				fail_msg("format %d plane %d: linesize %u", l->format, p, src->linesize[p]);
			if (l->linesize[p]) {
				assert_non_null(src->data[p]);
				assert_int_equal((uintptr_t)src->data[p] % base_get_alignment(), 0);
				memset(src->data[p], (int)(0x10 + p), (size_t)l->linesize[p] * l->heights[p]);
			}
		}

		for (int p = 0; p < 4; p++) {
			if (l->linesize[p]) {
				size_t last = (size_t)l->linesize[p] * l->heights[p] - 1;
				assert_int_equal(src->data[p][0], 0x10 + p);
				assert_int_equal(src->data[p][last], 0x10 + p);
			}
		}

		video_frame_copy(dst, src, l->format, 6);
		for (int p = 0; p < 4; p++) {
			if (l->linesize[p])
				assert_memory_equal(dst->data[p], src->data[p], (size_t)l->linesize[p] * l->heights[p]);
		}

		video_frame_destroy(src);
		video_frame_destroy(dst);
	}

	/* copying between different linesizes copies row by row */
	struct video_frame narrow, wide;
	video_frame_init(&narrow, VIDEO_FORMAT_Y800, 10, 2);
	video_frame_init(&wide, VIDEO_FORMAT_Y800, 16, 2);
	memset(narrow.data[0], 7, 20);
	memset(wide.data[0], 0, 32);
	video_frame_copy(&wide, &narrow, VIDEO_FORMAT_Y800, 2);
	assert_int_equal(wide.data[0][0], 7);
	assert_int_equal(wide.data[0][9], 7);
	assert_int_equal(wide.data[0][10], 0);
	assert_int_equal(wide.data[0][16], 7);
	video_frame_free(&narrow);
	video_frame_free(&wide);

	/* Regression: no planes made video_frame_init() call bmalloc(0) and crash */
	struct video_frame none;
	video_frame_init(&none, VIDEO_FORMAT_NONE, 10, 6);
	assert_null(none.data[0]);
	video_frame_free(&none);
	video_frame_init(&none, (enum video_format)999, 10, 6);
	assert_null(none.data[0]);
	video_frame_init(&none, VIDEO_FORMAT_I420, 0, 6);
	assert_null(none.data[0]);
	video_frame_destroy(video_frame_create(VIDEO_FORMAT_NONE, 10, 6));

	video_frame_init(NULL, VIDEO_FORMAT_I420, 10, 6);
	video_frame_free(NULL);
	video_frame_destroy(NULL);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(frame_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
