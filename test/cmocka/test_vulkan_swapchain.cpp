#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <future>
#include <thread>

#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include "vulkan-subsystem.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

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
static gs_swapchain_t *(*swapchain_create)(gs_device_t *, const struct gs_init_data *);
static void (*swapchain_destroy)(gs_swapchain_t *);
static void (*resize)(gs_device_t *, uint32_t, uint32_t);
static void (*load_swapchain)(gs_device_t *, gs_swapchain_t *);
static bool (*present_ready)(gs_device_t *);
static void (*present)(gs_device_t *);
static enum gs_color_space (*get_color_space)(gs_device_t *);
static void (*update_color_space)(gs_device_t *);
static void (*clear)(gs_device_t *, uint32_t, const struct vec4 *, float, uint8_t);
static uint32_t (*get_width)(const gs_device_t *);

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
		  load(messages, "obs_vulkan_validation_messages") & load(swapchain_create, "device_swapchain_create") &
		  load(swapchain_destroy, "gs_swapchain_destroy") & load(resize, "device_resize") &
		  load(load_swapchain, "device_load_swapchain") & load(present_ready, "device_is_present_ready") &
		  load(present, "device_present") & load(get_color_space, "device_get_color_space") &
		  load(update_color_space, "device_update_color_space") & load(clear, "device_clear") &
		  load(get_width, "device_get_width");
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

static const uint32_t size = 128;

#ifdef _WIN32
// Hidden (never shown) popup window, so the client area is exactly size x size.
static void *create_window()
{
	static ATOM cls = 0;
	if (!cls) {
		WNDCLASSW wc = {};
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"obs_vulkan_test";
		cls = RegisterClassW(&wc);
	}
	HWND hwnd = CreateWindowExW(0, L"obs_vulkan_test", L"", WS_POPUP, 0, 0, size, size, nullptr, nullptr,
				    GetModuleHandleW(nullptr), nullptr);
	if (!hwnd) {
		skip();
	}
	return hwnd;
}

static void destroy_window(void *w)
{
	DestroyWindow((HWND)w);
}
#else
static void *create_window()
{
	skip(); // shortcut: only Win32 surfaces are implemented and tested
	return nullptr;
}

static void destroy_window(void *) {}
#endif

static gs_swapchain_t *create_swap(gs_device *device, void *window)
{
	gs_init_data data = {};
#ifdef _WIN32
	data.window.hwnd = window;
#endif
	data.cx = size;
	data.cy = size;
	data.num_backbuffers = 2;
	data.format = GS_BGRA;
	data.zsformat = GS_ZS_NONE;
	gs_swapchain_t *swap = swapchain_create(device, &data);
	assert_non_null(swap);
	return swap;
}

// Presents `n` cleared frames on the loaded swapchain; not-ready frames are retried (FIFO may hold every image).
static void present_frames(gs_device *device, uint32_t n)
{
	within_5s([&] {
		for (uint32_t done = 0; done < n;) {
			if (!present_ready(device)) {
				os_sleep_ms(1);
				continue;
			}
			struct vec4 color;
			vec4_set(&color, 0.0f, 0.5f, 1.0f, 1.0f);
			clear(device, GS_CLEAR_COLOR, &color, 1.0f, 0);
			const uint64_t before = device->graphics.next_value;
			present(device);
			assert_true(device->graphics.next_value > before); // the frame was submitted
			assert_int_equal(device->swap->image, UINT32_MAX); // and the image handed back
			++done;
		}
	});
}

static void test_present_after_clear_succeeds(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);
	assert_int_equal(get_width(device), size);
	assert_int_equal(get_color_space(device), GS_CS_SRGB);
	update_color_space(device);
	assert_int_equal(get_color_space(device), GS_CS_SRGB);

	// More frames than images: every image and semaphore is reused.
	present_frames(device, 10);

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

// Review Focus 2: a 0x0 (minimized) size has no swapchain; present is a no-op and nothing is submitted.
static void test_resize_to_zero_then_present_is_noop(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);
	present_frames(device, 1);

	resize(device, 0, 0);
	within_5s([&] {
		for (int i = 0; i < 3; ++i) {
			assert_false(present_ready(device));
			struct vec4 color;
			vec4_set(&color, 1.0f, 0.0f, 0.0f, 1.0f);
			clear(device, GS_CLEAR_COLOR, &color, 1.0f, 0);
			const uint64_t before = device->graphics.next_value;
			present(device);
			assert_int_equal(device->graphics.next_value, before);
		}
	});

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

static void test_resize_back_recovers(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);

	resize(device, 0, 0);
	assert_false(present_ready(device));
	resize(device, size, size);
	present_frames(device, 4);
	assert_int_equal(get_width(device), size);

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

// Review Focus 5: a swapchain that is not ready is skipped without stalling the others.
static void test_two_swapchains_present_independently(void **)
{
	void *wa = create_window();
	void *wb = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *a = create_swap(device, wa);
	gs_swapchain_t *b = create_swap(device, wb);
	a->test_not_ready = true;

	within_5s([&] {
		for (int frame = 0; frame < 4; ++frame) {
			load_swapchain(device, a);
			assert_false(present_ready(device));
			const uint64_t before = device->graphics.next_value;
			present(device);
			assert_int_equal(device->graphics.next_value, before);

			load_swapchain(device, b);
			present_frames(device, 1);
		}
	});

	// Ready again: it presents too.
	a->test_not_ready = false;
	load_swapchain(device, a);
	present_frames(device, 2);

	swapchain_destroy(a);
	swapchain_destroy(b);
	destroy_and_check(device);
	destroy_window(wa);
	destroy_window(wb);
}

// Test hook OBS_VULKAN_TEST_FAIL_SUBMIT, read once at device creation; toggled here on the live device.
static void set_fail_submit(gs_device *device, bool on)
{
	device->frames.test_fail_submit = on;
}

// Carried finding 1: a failed render submit never signals the present semaphore, so present must be skipped
// (never wait on it), the acquired image must not leak, and later frames must present normally.
static void test_present_after_failed_submit_skips_and_recovers(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);

	for (int i = 0; i < 3; ++i) {
		within_5s([&] {
			while (!present_ready(device)) {
				os_sleep_ms(1);
			}
		});
		struct vec4 color;
		vec4_set(&color, 1.0f, 1.0f, 0.0f, 1.0f);
		clear(device, GS_CLEAR_COLOR, &color, 1.0f, 0);
		set_fail_submit(device, true);
		within_5s([&] { present(device); });
		set_fail_submit(device, false);
		assert_int_equal(swap->image, UINT32_MAX);
		assert_null(device->acquire_wait);
	}
	present_frames(device, 4);

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

// A swapchain that keeps reporting VK_SUBOPTIMAL_KHR (test hook) at an unchanged size is not rebuilt every frame (a
// rebuild idles the queue); once the window's size differs from the swapchain's, it is rebuilt to the new size.
static void test_persistent_suboptimal_rebuilds_only_on_size_change(void **)
{
	void *window = create_window();
	gs_device *device = create_or_skip();
	gs_swapchain_t *swap = create_swap(device, window);
	load_swapchain(device, swap);
	present_frames(device, 1);

	swap->test_suboptimal = true;
	const uint32_t builds = swap->builds;
	present_frames(device, 5);
	assert_int_equal(swap->builds, builds);
	assert_false(swap->recreate);

#ifdef _WIN32
	SetWindowPos((HWND)window, nullptr, 0, 0, size / 2, size / 2, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	present_frames(device, 2);
	assert_true(swap->builds > builds);
	assert_int_equal(get_width(device), size / 2);
#endif

	swapchain_destroy(swap);
	destroy_and_check(device);
	destroy_window(window);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_present_after_clear_succeeds),
		cmocka_unit_test(test_resize_to_zero_then_present_is_noop),
		cmocka_unit_test(test_resize_back_recovers),
		cmocka_unit_test(test_two_swapchains_present_independently),
		cmocka_unit_test(test_present_after_failed_submit_skips_and_recovers),
		cmocka_unit_test(test_persistent_suboptimal_rebuilds_only_on_size_change),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
