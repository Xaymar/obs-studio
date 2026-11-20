#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"
#include "null-fail.h"

/* obs.c: resetting video when the graphics device cannot create what libobs needs. Each export fails for its
 * first few calls, so the failure lands at different points of the setup; afterwards video must still come up,
 * and shutting down must free everything. */

static struct obs_video_info make_ovi(enum video_format format)
{
	struct obs_video_info ovi = {
		.graphics_module = NULL_GRAPHICS_MODULE,
		.fps_num = 30,
		.fps_den = 1,
		.base_width = 320,
		.base_height = 180,
		.output_width = 320,
		.output_height = 180,
		.output_format = format,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	return ovi;
}

static void reset_failures_test(void **state)
{
	UNUSED_PARAMETER(state);

	static const char *exports[] = {
		"device_create",
		"device_vertexshader_create",
		"device_pixelshader_create",
		"device_texture_create",
		"device_stagesurface_create",
		"device_samplerstate_create",
		"device_vertexbuffer_create",
		"device_zstencil_create",
	};
	static const long counts[] = {1, 3, 12, 40};
	static const enum video_format formats[] = {VIDEO_FORMAT_NV12, VIDEO_FORMAT_I444, VIDEO_FORMAT_RGBA};

	long allocs_before = bnum_allocs();
	for (size_t e = 0; e < sizeof(exports) / sizeof(exports[0]); e++) {
		for (size_t c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
			enum video_format format = formats[(e + c) % 3];
			assert_true(obs_startup("en-US", NULL, NULL));

			struct obs_video_info ovi = make_ovi(format);
			null_fail_next(exports[e], counts[c]);
			int ret = obs_reset_video(&ovi);
			null_reset_failures();
			print_message("%s x%ld: %d\n", exports[e], counts[c], ret);

			/* a later reset works */
			ovi = make_ovi(format);
			if (ret != OBS_VIDEO_SUCCESS)
				assert_int_equal(obs_reset_video(&ovi), OBS_VIDEO_SUCCESS);
			uint32_t frames = obs_get_total_frames();
			for (int i = 0; i < 200 && obs_get_total_frames() < frames + 2; i++)
				os_sleep_ms(5);

			obs_shutdown();
			assert_int_equal(bnum_allocs(), allocs_before);
		}
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(reset_failures_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
