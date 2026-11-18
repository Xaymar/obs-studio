#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <future>
#include <thread>

#include <graphics/graphics.h>
#include <util/base.h>
#include <util/platform.h>

#include "vulkan-subsystem.hpp"

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

static void *module;
static int (*create)(gs_device_t **, uint32_t);
static void (*destroy)(gs_device_t *);
static int (*messages)(void);
static gs_texture_t *(*texture_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format, uint32_t,
				       const uint8_t **, uint32_t);
static void (*texture_destroy)(gs_texture_t *);
static bool (*texture_map)(gs_texture_t *, uint8_t **, uint32_t *);
static void (*texture_unmap)(gs_texture_t *);
static gs_stagesurf_t *(*stagesurface_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format);
static void (*stage_texture)(gs_device_t *, gs_stagesurf_t *, gs_texture_t *);
static bool (*stagesurface_map)(gs_stagesurf_t *, uint8_t **, uint32_t *);
static void (*stagesurface_unmap)(gs_stagesurf_t *);
static void (*stagesurface_destroy)(gs_stagesurf_t *);
static void (*flush)(gs_device_t *);

template<typename T> static bool load(T &fn, const char *name)
{
	fn = (T)os_dlsym(module, name);
	if (!fn) {
		print_message("missing export: %s\n", name);
	}
	return fn != nullptr;
}

static int setup(void **)
{
#ifdef _WIN32
	_putenv_s("OBS_VULKAN_VALIDATION", "1");
#else
	setenv("OBS_VULKAN_VALIDATION", "1", 1);
#endif
	module = os_dlopen(OBS_VULKAN_MODULE);
	if (!module) {
		return -1;
	}
	bool ok = load(create, "device_create") & load(destroy, "device_destroy") &
		  load(messages, "obs_vulkan_validation_messages") & load(texture_create, "device_texture_create") &
		  load(texture_destroy, "gs_texture_destroy") & load(texture_map, "gs_texture_map") &
		  load(texture_unmap, "gs_texture_unmap") & load(stagesurface_create, "device_stagesurface_create") &
		  load(stage_texture, "device_stage_texture") & load(stagesurface_map, "gs_stagesurface_map") &
		  load(stagesurface_unmap, "gs_stagesurface_unmap") &
		  load(stagesurface_destroy, "gs_stagesurface_destroy") & load(flush, "device_flush");
	return ok ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

static gs_device *create_or_skip()
{
	gs_device_t *device = nullptr;
	int ret = create(&device, 0);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_SUCCESS);
	return device;
}

// Destroys the device; with the validation layer active, no message may have been reported.
static void destroy_and_check(gs_device *device)
{
	destroy(device);
	int n = messages();
	if (n < 0) {
		print_message("validation layer missing, not checked\n");
	} else {
		assert_int_equal(n, 0);
	}
}

// Runs fn on this thread; aborts the process when it takes longer than 5 s (a deadlock would hang ctest).
template<typename F> static void within_5s(F fn)
{
	std::promise<void> done;
	std::thread watchdog([f = done.get_future()]() mutable {
		if (f.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
			fprintf(stderr, "watchdog: timed out after 5 s\n");
			fflush(stderr);
			_Exit(2);
		}
	});
	fn();
	done.set_value();
	watchdog.join();
}

static void set_fail_submit(bool on)
{
#ifdef _WIN32
	_putenv_s("OBS_VULKAN_TEST_FAIL_SUBMIT", on ? "1" : "");
#else
	if (on) {
		setenv("OBS_VULKAN_TEST_FAIL_SUBMIT", "1", 1);
	} else {
		unsetenv("OBS_VULKAN_TEST_FAIL_SUBMIT");
	}
#endif
}

// Stages `tex` into a new stage surface and compares it with `want` (tightly packed, 4 bytes per pixel).
static void assert_readback(gs_device *device, gs_texture_t *tex, enum gs_color_format fmt, uint32_t w, uint32_t h,
			    const uint8_t *want)
{
	gs_stagesurf_t *stage = stagesurface_create(device, w, h, fmt);
	assert_non_null(stage);
	stage_texture(device, stage, tex);
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	bool mapped = false;
	within_5s([&] { mapped = stagesurface_map(stage, &data, &linesize); });
	assert_true(mapped);
	for (uint32_t y = 0; y < h; ++y) {
		assert_memory_equal(data + y * linesize, want + y * w * 4, w * 4);
	}
	stagesurface_unmap(stage);
	stagesurface_destroy(stage);
}

static void test_texture_with_initial_data_reads_back_exactly(void **)
{
	gs_device *device = create_or_skip();
	uint8_t pixels[4 * 4 * 4];
	for (uint32_t i = 0; i < sizeof(pixels); ++i) {
		pixels[i] = (uint8_t)(i * 4 + 1); // distinct per pixel and channel
	}
	const uint8_t *data[] = {pixels};
	gs_texture_t *tex = texture_create(device, 4, 4, GS_RGBA, 1, data, 0);
	assert_non_null(tex);

	assert_readback(device, tex, GS_RGBA, 4, 4, pixels);

	texture_destroy(tex);
	destroy_and_check(device);
}

static int warnings;
static void count_warnings(int lvl, const char *msg, va_list args, void *)
{
	if (lvl <= LOG_WARNING) {
		++warnings;
		vfprintf(stderr, msg, args);
		fputc('\n', stderr);
	}
}

static void test_texture_create_without_data_still_works(void **)
{
	gs_device *device = create_or_skip();
	log_handler_t old_handler;
	void *old_param;
	base_get_log_handler(&old_handler, &old_param);
	warnings = 0;
	base_set_log_handler(count_warnings, nullptr);

	gs_texture_t *tex = texture_create(device, 16, 16, GS_RGBA, 1, nullptr, 0);
	gs_texture_t *mips = texture_create(device, 16, 16, GS_BGRA, 0, nullptr, GS_BUILD_MIPMAPS);
	flush(device);

	base_set_log_handler(old_handler, old_param);
	assert_non_null(tex);
	assert_non_null(mips);
	assert_int_equal(warnings, 0);

	texture_destroy(mips);
	texture_destroy(tex);
	destroy_and_check(device);
}

static void test_mip_chain_generated_by_blit(void **)
{
	void (*stage_mip)(gs_device_t *, gs_stagesurf_t *, gs_texture_t *, uint32_t);
	if (!load(stage_mip, "obs_vulkan_test_stage_mip")) {
		fail();
	}

	gs_device *device = create_or_skip();
	uint8_t pixels[8 * 8 * 4];
	for (uint32_t y = 0; y < 8; ++y) {
		for (uint32_t x = 0; x < 8; ++x) {
			uint8_t v = (x + y) % 2 ? 255 : 0;
			uint8_t *p = pixels + (y * 8 + x) * 4;
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	}
	const uint8_t *data[] = {pixels};
	gs_texture_t *tex = texture_create(device, 8, 8, GS_BGRA, 1, data, GS_BUILD_MIPMAPS);
	assert_non_null(tex);
	assert_int_equal(tex->levels, 4);

	gs_stagesurf_t *stage = stagesurface_create(device, 1, 1, GS_BGRA);
	assert_non_null(stage);
	stage_mip(device, stage, tex, 3);
	uint8_t *out = nullptr;
	uint32_t linesize = 0;
	bool mapped = false;
	within_5s([&] { mapped = stagesurface_map(stage, &out, &linesize); });
	assert_true(mapped);
	for (int c = 0; c < 3; ++c) {
		if (out[c] < 126 || out[c] > 129) {
			fail_msg("channel %d is %u, expected mid-grey", c, out[c]);
		}
	}
	assert_int_equal(out[3], 255);
	stagesurface_unmap(stage);

	stagesurface_destroy(stage);
	texture_destroy(tex);
	destroy_and_check(device);
}

static void write_pattern(uint8_t *ptr, uint32_t linesize, uint32_t w, uint32_t h, uint8_t seed, uint8_t *want)
{
	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < w * 4; ++x) {
			uint8_t v = (uint8_t)(seed + y * 37 + x * 3);
			ptr[y * linesize + x] = v;
			want[y * w * 4 + x] = v;
		}
	}
}

static void test_dynamic_texture_map_write_unmap_roundtrip(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 8, 4, GS_BGRA, 1, nullptr, GS_DYNAMIC);
	assert_non_null(tex);
	uint8_t want[8 * 4 * 4];
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	bool mapped = false;

	within_5s([&] { mapped = texture_map(tex, &ptr, &linesize); });
	assert_true(mapped);
	assert_int_equal(linesize, 8 * 4);
	write_pattern(ptr, linesize, 8, 4, 1, want);
	texture_unmap(tex);
	assert_readback(device, tex, GS_BGRA, 8, 4, want);

	// Unmapped copy still recorded, not submitted: map must submit it and wait for it, not hang.
	within_5s([&] { mapped = texture_map(tex, &ptr, &linesize); });
	assert_true(mapped);
	write_pattern(ptr, linesize, 8, 4, 100, want);
	texture_unmap(tex);
	const uint64_t first_copy = tex->upload_value;
	within_5s([&] { mapped = texture_map(tex, &ptr, &linesize); });
	assert_true(mapped);
	assert_true(device->graphics.next_value > first_copy); // map submitted the frame holding the copy
	write_pattern(ptr, linesize, 8, 4, 200, want);
	texture_unmap(tex);
	assert_readback(device, tex, GS_BGRA, 8, 4, want);

	texture_destroy(tex);
	destroy_and_check(device);
}

static void test_map_non_dynamic_texture_returns_false(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 8, 8, GS_RGBA, 1, nullptr, 0);
	assert_non_null(tex);
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	assert_false(texture_map(tex, &ptr, &linesize));
	texture_destroy(tex);
	destroy_and_check(device);
}

static void test_srgb_capable_format_creates_with_both_views_without_validation_errors(void **)
{
	gs_device *device = create_or_skip();
	const gs_color_format srgb[] = {GS_RGBA, GS_BGRA, GS_BGRX};
	for (gs_color_format f : srgb) {
		gs_texture_t *tex = texture_create(device, 16, 16, f, 1, nullptr, GS_RENDER_TARGET);
		assert_non_null(tex);
		assert_non_null(tex->view);
		assert_non_null(tex->srgb_view);
		texture_destroy(tex);
	}
	gs_texture_t *unorm = texture_create(device, 16, 16, GS_BGRX_UNORM, 1, nullptr, 0);
	assert_non_null(unorm);
	assert_non_null(unorm->view);
	assert_null(unorm->srgb_view);
	texture_destroy(unorm);
	destroy_and_check(device);
}

static void test_failed_submit_after_upload_does_not_hang(void **)
{
	gs_device *device = create_or_skip();
	uint8_t pixels[8 * 8 * 4] = {};
	const uint8_t *data[] = {pixels};
	gs_texture_t *tex = texture_create(device, 8, 8, GS_RGBA, 1, data, GS_BUILD_MIPMAPS);
	gs_texture_t *dyn = texture_create(device, 8, 8, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	assert_non_null(tex);
	assert_non_null(dyn);
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	bool mapped = false;
	within_5s([&] { mapped = texture_map(dyn, &ptr, &linesize); });
	assert_true(mapped);
	texture_unmap(dyn);

	set_fail_submit(true);
	flush(device);
	set_fail_submit(false);

	// The lost copy must not make the next map wait for a value nobody signals.
	within_5s([&] { mapped = texture_map(dyn, &ptr, &linesize); });
	assert_true(mapped);
	texture_unmap(dyn);
	// Unmapped copy recorded and its submission fails inside map: still no hang.
	set_fail_submit(true);
	within_5s([&] { mapped = texture_map(dyn, &ptr, &linesize); });
	set_fail_submit(false);
	assert_true(mapped);
	texture_unmap(dyn);
	within_5s([&] { flush(device); });

	texture_destroy(dyn);
	texture_destroy(tex);
	within_5s([&] { destroy_and_check(device); });
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_texture_with_initial_data_reads_back_exactly),
		cmocka_unit_test(test_texture_create_without_data_still_works),
		cmocka_unit_test(test_mip_chain_generated_by_blit),
		cmocka_unit_test(test_dynamic_texture_map_write_unmap_roundtrip),
		cmocka_unit_test(test_map_non_dynamic_texture_returns_false),
		cmocka_unit_test(test_srgb_capable_format_creates_with_both_views_without_validation_errors),
		cmocka_unit_test(test_failed_submit_after_upload_does_not_hang),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
