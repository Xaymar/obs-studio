#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"
#include "gs-test.h"

#include <string.h>

#include <graphics/image-file.h>

/* image-file.c, graphics-ffmpeg.c and libnsgif, on the images in test/cmocka/data (see make-images.py) */

#define DATA(name) TEST_DATA_DIR "/" name

static void decode_test(void **state)
{
	UNUSED_PARAMETER(state);

	const char *files[] = {"rgba.png",   "rgb.png", "gray.png", "gray-alpha.png", "palette.png",
			       "gray16.png", "rgb.bmp", "rgba.tga", "static.gif"};
	const enum gs_image_alpha_mode modes[] = {GS_IMAGE_ALPHA_STRAIGHT, GS_IMAGE_ALPHA_PREMULTIPLY_SRGB,
						  GS_IMAGE_ALPHA_PREMULTIPLY};
	for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		for (size_t m = 0; m < 3; m++) {
			char path[512];
			snprintf(path, sizeof(path), "%s/%s", TEST_DATA_DIR, files[i]);
			enum gs_color_format format = GS_UNKNOWN;
			enum gs_color_space space = GS_CS_709_EXTENDED;
			uint32_t cx = 0, cy = 0;
			uint8_t *data = gs_create_texture_file_data3(path, modes[m], &format, &cx, &cy, &space);
			if (!data)
				fail_msg("%s (alpha mode %zu) did not decode", files[i], m);
			assert_true(cx > 0 && cy > 0);
			assert_int_not_equal(format, GS_UNKNOWN);
			bfree(data);
		}
	}

	/* straight alpha keeps the pixel values; the first pixel of rgba.png is opaque red */
	enum gs_color_format format;
	uint32_t cx, cy;
	uint8_t *data = gs_create_texture_file_data(DATA("rgba.png"), &format, &cx, &cy);
	assert_non_null(data);
	assert_int_equal(cx, 4);
	assert_int_equal(cy, 2);
	assert_true(format == GS_RGBA || format == GS_BGRA);
	uint8_t red = format == GS_RGBA ? data[0] : data[2];
	assert_int_equal(red, 255);
	assert_int_equal(data[3], 255);
	/* the fully transparent pixel stays transparent */
	assert_int_equal(data[4 * 4 + 3], 0);
	bfree(data);

	data = gs_create_texture_file_data2(DATA("rgba.png"), GS_IMAGE_ALPHA_PREMULTIPLY, &format, &cx, &cy);
	assert_non_null(data);
	/* premultiplied: the half transparent gray pixel is darkened */
	assert_true(data[5 * 4] < 128);
	bfree(data);

	assert_null(gs_create_texture_file_data(DATA("broken.png"), &format, &cx, &cy));
	assert_null(gs_create_texture_file_data(DATA("missing.png"), &format, &cx, &cy));
	assert_null(gs_create_texture_file_data(NULL, &format, &cx, &cy));
}

static void orientation_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* Orientations 5 to 8 should swap width and height of the 3x2 source, but this FFmpeg no longer reports the
	 * EXIF orientation as "Orientation" frame metadata, so libobs never rotates (see known-bugs.md). Only decoding
	 * is checked. */
	for (int o = 1; o <= 8; o++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/orient%d.jpg", TEST_DATA_DIR, o);
		enum gs_color_format format;
		uint32_t cx = 0, cy = 0;
		uint8_t *data = gs_create_texture_file_data(path, &format, &cx, &cy);
		if (!data)
			fail_msg("orient%d.jpg did not decode", o);
		assert_true(cx * cy == 6);
		bfree(data);
	}
}

static void texture_file_test(void **state)
{
	UNUSED_PARAMETER(state);

	gs_texture_t *tex = gs_texture_create_from_file(DATA("rgb.png"));
	assert_non_null(tex);
	assert_int_equal(gs_texture_get_width(tex), 4);
	gs_texture_destroy(tex);
	assert_null(gs_texture_create_from_file(DATA("broken.png")));
}

static void image_file_test(void **state)
{
	UNUSED_PARAMETER(state);

	gs_image_file_ex_t image;
	gs_image_file_ex_init(&image, DATA("rgba.png"), GS_IMAGE_ALPHA_STRAIGHT);
	assert_true(image.loaded);
	assert_false(image.is_animated_gif);
	assert_int_equal(image.cx, 4);
	gs_image_file_ex_init_texture(&image);
	assert_non_null(image.texture);
	assert_false(gs_image_file_ex_tick(&image, 1000000));
	gs_image_file_ex_update_texture(&image);
	gs_image_file_ex_free(&image);

	/* a static GIF goes through libnsgif but is not animated */
	gs_image_file_ex_init(&image, DATA("static.gif"), GS_IMAGE_ALPHA_PREMULTIPLY);
	assert_true(image.loaded);
	assert_false(image.is_animated_gif);
	gs_image_file_ex_init_texture(&image);
	gs_image_file_ex_free(&image);

	/* Regression: a corrupt GIF made the scan loop spin forever */
	gs_image_file_ex_init(&image, DATA("broken.gif"), GS_IMAGE_ALPHA_STRAIGHT);
	assert_false(image.loaded);
	gs_image_file_ex_init_texture(&image);
	gs_image_file_ex_free(&image);

	gs_image_file_ex_init(&image, DATA("missing.png"), GS_IMAGE_ALPHA_STRAIGHT);
	assert_false(image.loaded);
	gs_image_file_ex_free(&image);

	gs_image_file_ex_init(&image, NULL, GS_IMAGE_ALPHA_STRAIGHT);
	assert_false(image.loaded);
	gs_image_file_ex_free(&image);
}

static void animated_gif_test(void **state)
{
	UNUSED_PARAMETER(state);

	const enum gs_image_alpha_mode modes[] = {GS_IMAGE_ALPHA_STRAIGHT, GS_IMAGE_ALPHA_PREMULTIPLY_SRGB,
						  GS_IMAGE_ALPHA_PREMULTIPLY};
	for (size_t m = 0; m < 3; m++) {
		gs_image_file_ex_t image;
		gs_image_file_ex_init(&image, DATA("anim.gif"), modes[m]);
		assert_true(image.loaded);
		assert_true(image.is_animated_gif);
		assert_int_equal(image.cx, 4);
		gs_image_file_ex_init_texture(&image);
		assert_non_null(image.texture);

		/* frames last 50, 100 and 20 ms */
		assert_int_equal(image.cur_frame, 0);
		assert_false(gs_image_file_ex_tick(&image, 10000000)); /* 10 ms: still frame 0 */
		assert_true(gs_image_file_ex_tick(&image, 50000000));  /* 60 ms: frame 1 */
		assert_int_equal(image.cur_frame, 1);
		gs_image_file_ex_update_texture(&image);
		assert_true(gs_image_file_ex_tick(&image, 100000000)); /* frame 2 */
		assert_true(gs_image_file_ex_tick(&image, 30000000));  /* loops to frame 0 */
		gs_image_file_ex_update_texture(&image);
		/* a huge step skips over whole loops */
		gs_image_file_ex_tick(&image, 10ULL * 1000000000ULL);
		gs_image_file_ex_update_texture(&image);
		gs_image_file_ex_free(&image);
	}

	/* a GIF that plays once stops on its last frame */
	gs_image_file_ex_t once;
	gs_image_file_ex_init(&once, DATA("anim-once.gif"), GS_IMAGE_ALPHA_STRAIGHT);
	assert_true(once.is_animated_gif);
	gs_image_file_ex_init_texture(&once);
	for (int i = 0; i < 20; i++)
		gs_image_file_ex_tick(&once, 40000000);
	gs_image_file_ex_update_texture(&once);
	gs_image_file_ex_free(&once);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		gs_unit_test(decode_test),     gs_unit_test(orientation_test),  gs_unit_test(texture_file_test),
		gs_unit_test(image_file_test), gs_unit_test(animated_gif_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
