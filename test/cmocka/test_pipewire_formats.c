/*
 * Plane layout of async PipeWire frames (shared/pipewire/formats.c).
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <string.h>

#include "formats.h"

#include <spa/buffer/buffer.h>

static uint8_t memory[4][64 * 64 * 4];

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data datas[4];
	struct spa_chunk chunks[4];
};

static void init_buffer(struct test_buffer *t, uint32_t n_datas)
{
	memset(t, 0, sizeof(*t));
	t->buffer.n_datas = n_datas;
	t->buffer.datas = t->datas;
	for (uint32_t i = 0; i < 4; i++) {
		t->datas[i].data = memory[i];
		t->datas[i].maxsize = sizeof(memory[i]);
		t->datas[i].chunk = &t->chunks[i];
	}
}

static struct obs_source_frame frame_of(enum video_format format, uint32_t w, uint32_t h)
{
	struct obs_source_frame f = {0};
	f.format = format;
	f.width = w;
	f.height = h;
	return f;
}

/* The old code set only plane 0, with width * 2 bytes per row for NV12. */
static void nv12_two_datas_uses_both_strides(void **state)
{
	(void)state;
	struct test_buffer t;
	init_buffer(&t, 2);
	t.chunks[0].stride = 64;
	t.chunks[1].stride = 64;
	t.chunks[1].offset = 16;

	struct obs_source_frame f = frame_of(VIDEO_FORMAT_NV12, 40, 32);
	assert_true(obs_pw_frame_set_planes(&f, &t.buffer));
	assert_ptr_equal(f.data[0], memory[0]);
	assert_ptr_equal(f.data[1], memory[1] + 16);
	assert_int_equal(f.linesize[0], 64);
	assert_int_equal(f.linesize[1], 64);
}

static void nv12_one_data_places_chroma_after_luma(void **state)
{
	(void)state;
	struct test_buffer t;
	init_buffer(&t, 1);
	t.chunks[0].stride = 48;

	struct obs_source_frame f = frame_of(VIDEO_FORMAT_NV12, 40, 32);
	assert_true(obs_pw_frame_set_planes(&f, &t.buffer));
	assert_ptr_equal(f.data[1], memory[0] + 48 * 32);
	assert_int_equal(f.linesize[1], 48);
}

static void i420_one_data_has_three_planes(void **state)
{
	(void)state;
	struct test_buffer t;
	init_buffer(&t, 1);

	struct obs_source_frame f = frame_of(VIDEO_FORMAT_I420, 32, 16);
	assert_true(obs_pw_frame_set_planes(&f, &t.buffer));
	assert_int_equal(f.linesize[0], 32);
	assert_int_equal(f.linesize[1], 16);
	assert_int_equal(f.linesize[2], 16);
	assert_ptr_equal(f.data[1], memory[0] + 32 * 16);
	assert_ptr_equal(f.data[2], memory[0] + 32 * 16 + 16 * 8);
}

static void packed_falls_back_without_stride(void **state)
{
	(void)state;
	struct test_buffer t;
	init_buffer(&t, 1);
	t.chunks[0].offset = 8;

	struct obs_source_frame f = frame_of(VIDEO_FORMAT_YUY2, 31, 4);
	assert_true(obs_pw_frame_set_planes(&f, &t.buffer));
	assert_ptr_equal(f.data[0], memory[0] + 8);
	assert_int_equal(f.linesize[0], 64);
}

static void too_small_buffer_is_refused(void **state)
{
	(void)state;
	struct test_buffer t;
	init_buffer(&t, 1);
	t.datas[0].maxsize = 100;

	struct obs_source_frame f = frame_of(VIDEO_FORMAT_BGRA, 16, 16);
	assert_false(obs_pw_frame_set_planes(&f, &t.buffer));
}

static void unknown_format_is_refused(void **state)
{
	(void)state;
	struct test_buffer t;
	init_buffer(&t, 1);

	struct obs_source_frame f = frame_of(VIDEO_FORMAT_P010, 16, 16);
	assert_false(obs_pw_frame_set_planes(&f, &t.buffer));
}

static void table_has_camera_formats(void **state)
{
	(void)state;
	struct obs_pw_video_format fmt;
	assert_true(obs_pw_video_format_from_spa_format(SPA_VIDEO_FORMAT_UYVY, &fmt));
	assert_int_equal(fmt.video_format, VIDEO_FORMAT_UYVY);
	assert_true(obs_pw_video_format_from_spa_format(SPA_VIDEO_FORMAT_I420, &fmt));
	assert_int_equal(fmt.video_format, VIDEO_FORMAT_I420);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(nv12_two_datas_uses_both_strides),
		cmocka_unit_test(nv12_one_data_places_chroma_after_luma),
		cmocka_unit_test(i420_one_data_has_three_planes),
		cmocka_unit_test(packed_falls_back_without_stride),
		cmocka_unit_test(too_small_buffer_is_refused),
		cmocka_unit_test(unknown_format_is_refused),
		cmocka_unit_test(table_has_camera_formats),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
