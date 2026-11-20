#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>

/* Full path of libobs-null, set by CMake; os_dlopen() does not search PATH */
#ifndef NULL_GRAPHICS_MODULE
#error NULL_GRAPHICS_MODULE must be defined
#endif

typedef long (*call_count_fn)(const char *name);
typedef long (*live_objects_fn)(void);

static void wait_for_frames(uint32_t count)
{
	/* 60 fps: 5 frames take ~83 ms; allow generous slack for loaded CI machines */
	for (int i = 0; i < 500 && obs_get_total_frames() < count; i++)
		os_sleep_ms(10);
}

static void video_null_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_true(obs_startup("en-US", NULL, NULL));

	struct obs_video_info ovi = {
		.graphics_module = NULL_GRAPHICS_MODULE,
		.fps_num = 60,
		.fps_den = 1,
		.base_width = 640,
		.base_height = 360,
		.output_width = 640,
		.output_height = 360,
		.output_format = VIDEO_FORMAT_NV12,
		.adapter = 0,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	assert_int_equal(obs_reset_video(&ovi), OBS_VIDEO_SUCCESS);

	void *module = os_dlopen(NULL_GRAPHICS_MODULE);
	assert_non_null(module);
	call_count_fn call_count = (call_count_fn)os_dlsym(module, "null_gs_get_call_count");
	live_objects_fn live_objects = (live_objects_fn)os_dlsym(module, "null_gs_get_live_objects");
	assert_non_null(call_count);
	assert_non_null(live_objects);

	/* The default effects compiled through the module's shader parser */
	assert_true(call_count("device_vertexshader_create") > 0);
	assert_true(call_count("device_pixelshader_create") > 0);

	wait_for_frames(5);
	assert_true(obs_get_total_frames() >= 5);
	assert_true(call_count("device_begin_scene") > 0);

	obs_shutdown();

	/* Everything libobs created on the device was destroyed with it */
	assert_int_equal(live_objects(), 0);
	os_dlclose(module);

	assert_int_equal(bnum_allocs(), 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(video_null_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
