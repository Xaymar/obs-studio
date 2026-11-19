#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

#include <util/threading.h>

/* obs.c / obs-video.c: the output texture conversion and copy for every output format, with and without GPU
 * conversion, and the colorspaces that change the conversion */

static volatile long raw_frames;
static enum video_format raw_format;

static void on_raw_video(void *param, struct video_data *frame)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(frame);
	os_atomic_inc_long(&raw_frames);
}

static bool run_format(enum video_format format, bool gpu_conversion, enum video_colorspace colorspace)
{
	struct obs_video_info ovi = {
		.graphics_module = NULL_GRAPHICS_MODULE,
		.fps_num = 60,
		.fps_den = 1,
		.base_width = 640,
		.base_height = 360,
		.output_width = 320,
		.output_height = 180,
		.output_format = format,
		.gpu_conversion = gpu_conversion,
		.colorspace = colorspace,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BILINEAR,
	};
	bool rgbx = format == VIDEO_FORMAT_RGBA || format == VIDEO_FORMAT_BGRA || format == VIDEO_FORMAT_BGRX;
	bool converted = format == VIDEO_FORMAT_I420 || format == VIDEO_FORMAT_NV12 || format == VIDEO_FORMAT_I444 ||
			 format == VIDEO_FORMAT_I010 || format == VIDEO_FORMAT_P010 || format == VIDEO_FORMAT_P216 ||
			 format == VIDEO_FORMAT_P416;
	int ret = obs_reset_video(&ovi);
	/* Regression: formats that are neither 4-byte RGB nor converted on the GPU were accepted, and the RGBA
	 * output texture was copied into their smaller frames, past the end of the buffer */
	if (!rgbx && !(converted && gpu_conversion)) {
		assert_int_equal(ret, OBS_VIDEO_INVALID_PARAM);
		return true;
	}
	if (ret != OBS_VIDEO_SUCCESS)
		return false;

	os_atomic_set_long(&raw_frames, 0);
	raw_format = format;
	obs_add_raw_video_callback(NULL, on_raw_video, NULL);
	for (int i = 0; i < 400 && os_atomic_load_long(&raw_frames) < 3; i++)
		os_sleep_ms(5);
	obs_remove_raw_video_callback(on_raw_video, NULL);
	return os_atomic_load_long(&raw_frames) >= 3;
}

static void formats_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_set_output_source(0, src);

	static const enum video_format formats[] = {
		VIDEO_FORMAT_I420, VIDEO_FORMAT_NV12, VIDEO_FORMAT_I444, VIDEO_FORMAT_I010, VIDEO_FORMAT_P010,
		VIDEO_FORMAT_P216, VIDEO_FORMAT_P416, VIDEO_FORMAT_RGBA, VIDEO_FORMAT_BGRA, VIDEO_FORMAT_BGRX,
		VIDEO_FORMAT_Y800, VIDEO_FORMAT_I412, VIDEO_FORMAT_I210, VIDEO_FORMAT_YA2L,
	};
	static const enum video_colorspace spaces[] = {VIDEO_CS_709, VIDEO_CS_601, VIDEO_CS_SRGB, VIDEO_CS_2100_PQ,
						       VIDEO_CS_2100_HLG};
	for (size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		for (int gpu = 0; gpu < 2; gpu++) {
			enum video_colorspace cs = spaces[(f + gpu) % 5];
			/* the HDR spaces need a 10-bit format */
			bool high = formats[f] == VIDEO_FORMAT_I010 || formats[f] == VIDEO_FORMAT_P010 ||
				    formats[f] == VIDEO_FORMAT_P216 || formats[f] == VIDEO_FORMAT_P416 ||
				    formats[f] == VIDEO_FORMAT_I412 || formats[f] == VIDEO_FORMAT_I210 ||
				    formats[f] == VIDEO_FORMAT_YA2L;
			if (!high && (cs == VIDEO_CS_2100_PQ || cs == VIDEO_CS_2100_HLG))
				cs = VIDEO_CS_709;
			if (!run_format(formats[f], gpu != 0, cs))
				fail_msg("format %d, gpu conversion %d, colorspace %d: no frames", formats[f], gpu, cs);
		}
	}

	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(formats_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
