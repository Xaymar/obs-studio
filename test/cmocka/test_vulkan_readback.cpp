#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <future>
#include <thread>

#include <graphics/graphics.h>
#include <graphics/vec4.h>
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

// Set OBS_VULKAN_REQUIRE_VALIDATION=1 to make a missing validation layer fail the test.
static void require_validation()
{
	const char *rq = getenv("OBS_VULKAN_REQUIRE_VALIDATION");
	if (rq && !strcmp(rq, "1")) {
		fail_msg("validation layer required (OBS_VULKAN_REQUIRE_VALIDATION=1) but missing");
	}
}

static void *module;
static int (*create)(gs_device_t **, uint32_t);
static void (*destroy)(gs_device_t *);
static int (*messages)(void);
static gs_texture_t *(*texture_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format, uint32_t,
				       const uint8_t **, uint32_t);
static void (*texture_destroy)(gs_texture_t *);
static void (*set_render_target)(gs_device_t *, gs_texture_t *, gs_zstencil_t *);
static void (*clear)(gs_device_t *, uint32_t, const struct vec4 *, float, uint8_t);
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
		  load(texture_destroy, "gs_texture_destroy") & load(set_render_target, "device_set_render_target") &
		  load(clear, "device_clear") & load(stagesurface_create, "device_stagesurface_create") &
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
		require_validation();
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

static void clear_to(gs_device *device, gs_texture_t *tex, float r, float g, float b, float a)
{
	struct vec4 color;
	vec4_set(&color, r, g, b, a);
	set_render_target(device, tex, nullptr);
	clear(device, GS_CLEAR_COLOR, &color, 1.0f, 0);
}

static void assert_near(uint8_t got, float want)
{
	float diff = got - want * 255.0f;
	if (diff < -1.0f || diff > 1.0f) {
		fail_msg("byte %u, expected %f", got, want * 255.0f);
	}
}

static void test_clear_rgba8_then_readback_matches(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 64, 64, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	gs_stagesurf_t *stage = stagesurface_create(device, 64, 64, GS_RGBA);
	assert_non_null(tex);
	assert_non_null(stage);

	clear_to(device, tex, 0.25f, 0.5f, 0.75f, 1.0f);
	stage_texture(device, stage, tex);
	flush(device);

	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	assert_true(stagesurface_map(stage, &data, &linesize));
	assert_true(linesize >= 64 * 4);
	for (uint32_t y = 0; y < 64; ++y) {
		for (uint32_t x = 0; x < 64; ++x) {
			const uint8_t *p = data + y * linesize + x * 4;
			assert_near(p[0], 0.25f);
			assert_near(p[1], 0.5f);
			assert_near(p[2], 0.75f);
			assert_near(p[3], 1.0f);
		}
	}
	stagesurface_unmap(stage);

	stagesurface_destroy(stage);
	texture_destroy(tex);
	destroy_and_check(device);
}

// Review Focus 3: map must submit the pending copy itself, and must not wait when nothing was recorded.
static void test_map_without_flush_submits_and_returns(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 16, 16, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	gs_stagesurf_t *stage = stagesurface_create(device, 16, 16, GS_RGBA);
	gs_stagesurf_t *never_staged = stagesurface_create(device, 16, 16, GS_RGBA);
	assert_non_null(tex);
	assert_non_null(stage);
	assert_non_null(never_staged);

	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	bool mapped = false;
	within_5s([&] { mapped = stagesurface_map(never_staged, &data, &linesize); });
	assert_true(mapped);
	stagesurface_unmap(never_staged);

	clear_to(device, tex, 1.0f, 0.0f, 0.0f, 1.0f);
	stage_texture(device, stage, tex);
	within_5s([&] { mapped = stagesurface_map(stage, &data, &linesize); });
	assert_true(mapped);
	assert_near(data[0], 1.0f);
	assert_near(data[1], 0.0f);
	stagesurface_unmap(stage);

	stagesurface_destroy(never_staged);
	stagesurface_destroy(stage);
	texture_destroy(tex);
	destroy_and_check(device);
}

// Review Focus 4: destroying a texture the recorded frame references is deferred until that frame signals.
static void test_destroy_texture_after_draw_is_safe(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *rt = texture_create(device, 16, 16, GS_BGRA, 1, nullptr, GS_RENDER_TARGET);
	gs_texture_t *tex = texture_create(device, 16, 16, GS_BGRA, 1, nullptr, GS_RENDER_TARGET);
	gs_stagesurf_t *stage = stagesurface_create(device, 16, 16, GS_BGRA);
	assert_non_null(rt);
	assert_non_null(tex);
	assert_non_null(stage);

	clear_to(device, tex, 0.0f, 1.0f, 0.0f, 1.0f);
	stage_texture(device, stage, tex);
	const uint32_t slot = device->frames.index;
	texture_destroy(tex);
	stagesurface_destroy(stage);
	assert_int_equal(device->frames.frames[slot].deferred.size(), 2);

	flush(device);
	assert_int_equal(device->frames.frames[slot].deferred.size(), 2); // GPU may still be running it

	// Cycle the ring until the slot is begun again: that waits for its timeline value, then runs the deferred work.
	for (uint32_t i = 0; i < vk_frame_ring::count; ++i) {
		clear_to(device, rt, 0.0f, 0.0f, 1.0f, 1.0f);
		flush(device);
	}
	assert_int_equal(device->frames.frames[slot].deferred.size(), 0);

	texture_destroy(rt);
	destroy_and_check(device);
}

// More frames than ring slots: each slot is reused and every readback sees its own frame's clear.
static void test_ring_slot_reuse_reads_each_frame(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 8, 8, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	gs_stagesurf_t *stage = stagesurface_create(device, 8, 8, GS_RGBA);
	assert_non_null(tex);
	assert_non_null(stage);

	for (uint32_t i = 0; i < 2 * vk_frame_ring::count; ++i) {
		const float v = (float)i / 8.0f;
		clear_to(device, tex, v, 1.0f - v, 0.0f, 1.0f);
		stage_texture(device, stage, tex);
		flush(device);
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		assert_true(stagesurface_map(stage, &data, &linesize));
		assert_near(data[0], v);
		assert_near(data[1], 1.0f - v);
		stagesurface_unmap(stage);
	}

	stagesurface_destroy(stage);
	texture_destroy(tex);
	destroy_and_check(device);
}

static void test_unsupported_format_returns_null(void **)
{
	gs_device *device = create_or_skip();
	assert_null(texture_create(device, 16, 16, GS_UNKNOWN, 1, nullptr, 0));
	assert_null(texture_create(device, 16, 16, GS_DXT1, 1, nullptr, 0));
	assert_null(stagesurface_create(device, 16, 16, GS_UNKNOWN));
	destroy_and_check(device);
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

// A copy in a failed submission never ran: map must report failure, never hang, and later frames must still work.
static void test_map_after_failed_submit_returns_false_without_hanging(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 16, 16, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	gs_stagesurf_t *stage = stagesurface_create(device, 16, 16, GS_RGBA);
	assert_non_null(tex);
	assert_non_null(stage);

	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	bool mapped = true;

	// Nothing submitted afterwards: map must not wait for a value no one signals.
	clear_to(device, tex, 1.0f, 0.0f, 0.0f, 1.0f);
	stage_texture(device, stage, tex);
	set_fail_submit(true);
	flush(device);
	set_fail_submit(false);
	within_5s([&] { mapped = stagesurface_map(stage, &data, &linesize); });
	assert_false(mapped);

	// Other work recorded in between: map submits it, but the lost copy must still not read as success.
	clear_to(device, tex, 1.0f, 0.0f, 0.0f, 1.0f);
	stage_texture(device, stage, tex);
	set_fail_submit(true);
	flush(device);
	set_fail_submit(false);
	clear_to(device, tex, 0.0f, 1.0f, 0.0f, 1.0f);
	mapped = true;
	within_5s([&] { mapped = stagesurface_map(stage, &data, &linesize); });
	assert_false(mapped);

	// A later copy works again (and its barriers are valid after the failure).
	clear_to(device, tex, 0.0f, 0.0f, 1.0f, 1.0f);
	stage_texture(device, stage, tex);
	within_5s([&] { mapped = stagesurface_map(stage, &data, &linesize); });
	assert_true(mapped);
	assert_near(data[0], 0.0f);
	assert_near(data[2], 1.0f);
	stagesurface_unmap(stage);

	stagesurface_destroy(stage);
	texture_destroy(tex);
	destroy_and_check(device);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_clear_rgba8_then_readback_matches),
		cmocka_unit_test(test_map_without_flush_submits_and_returns),
		cmocka_unit_test(test_destroy_texture_after_draw_is_safe),
		cmocka_unit_test(test_ring_slot_reuse_reads_each_frame),
		cmocka_unit_test(test_unsupported_format_returns_null),
		cmocka_unit_test(test_map_after_failed_submit_returns_false_without_hanging),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
