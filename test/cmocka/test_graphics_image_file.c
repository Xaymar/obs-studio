#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <math.h>

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

	/* Regression: the EXIF orientation was read only from "Orientation" frame metadata, which this FFmpeg no longer
	 * sets (it exports a display matrix instead), so rotated JPEGs loaded unrotated.
	 *
	 * Every file stores the same 3x2 image (make-images.py) with a different EXIF orientation; the decoded image
	 * must be that image as displayed: src(sx, sy) for each output pixel (x, y). */
	static const uint8_t colors[6][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}, {0, 255, 255}, {255, 0, 255}};
	const int w = 3, h = 2;
	for (int o = 1; o <= 8; o++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/orient%d.jpg", TEST_DATA_DIR, o);
		enum gs_color_format format;
		uint32_t cx = 0, cy = 0;
		uint8_t *data = gs_create_texture_file_data(path, &format, &cx, &cy);
		if (!data)
			fail_msg("orient%d.jpg did not decode", o);
		const bool swapped = o >= 5;
		assert_int_equal(cx, swapped ? h : w);
		assert_int_equal(cy, swapped ? w : h);
		assert_true(format == GS_RGBA || format == GS_BGRA || format == GS_BGRX);
		const bool bgr = format != GS_RGBA;

		for (int y = 0; y < (int)cy; y++) {
			for (int x = 0; x < (int)cx; x++) {
				int sx, sy;
				switch (o) {
				case 2: sx = w - 1 - x; sy = y; break;
				case 3: sx = w - 1 - x; sy = h - 1 - y; break;
				case 4: sx = x; sy = h - 1 - y; break;
				case 5: sx = y; sy = x; break;
				case 6: sx = y; sy = h - 1 - x; break;
				case 7: sx = w - 1 - y; sy = h - 1 - x; break;
				case 8: sx = w - 1 - y; sy = x; break;
				default: sx = x; sy = y; break;
				}
				/* JPEG subsamples the chroma of this tiny image away; luma survives, and the six colors
				 * have distinct luma, so each pixel must be closest to the expected color's */
				const uint8_t *px = data + (y * cx + x) * 4;
				float luma = 0.299f * px[bgr ? 2 : 0] + 0.587f * px[1] + 0.114f * px[bgr ? 0 : 2];
				int nearest = 0;
				float best = 1e9f;
				for (int i = 0; i < 6; i++) {
					float l = 0.299f * colors[i][0] + 0.587f * colors[i][1] + 0.114f * colors[i][2];
					if (fabsf(l - luma) < best) {
						best = fabsf(l - luma);
						nearest = i;
					}
				}
				if (nearest != sy * w + sx)
					fail_msg("orient%d.jpg: pixel (%d, %d) looks like source pixel %d, not %d", o, x, y,
						 nearest, sy * w + sx);
			}
		}
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

	/* local palettes, transparency, cropped frames, disposal to previous, interlacing, two loops */
	gs_image_file_ex_t complex;
	gs_image_file_ex_init(&complex, DATA("complex.gif"), GS_IMAGE_ALPHA_PREMULTIPLY);
	assert_true(complex.loaded);
	assert_true(complex.is_animated_gif);
	assert_int_equal(complex.cx, 16);
	assert_int_equal(complex.cy, 12);
	gs_image_file_ex_init_texture(&complex);
	for (int i = 0; i < 12; i++) {
		gs_image_file_ex_tick(&complex, 30000000);
		gs_image_file_ex_update_texture(&complex);
	}
	gs_image_file_ex_free(&complex);

	/* a full 256 color palette, and the same image cut off half way */
	gs_image_file_ex_t colors;
	gs_image_file_ex_init(&colors, DATA("colors.gif"), GS_IMAGE_ALPHA_STRAIGHT);
	assert_true(colors.loaded);
	assert_int_equal(colors.cx, 32);
	gs_image_file_ex_free(&colors);
	gs_image_file_ex_t truncated;
	gs_image_file_ex_init(&truncated, DATA("truncated.gif"), GS_IMAGE_ALPHA_STRAIGHT);
	gs_image_file_ex_free(&truncated);

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
