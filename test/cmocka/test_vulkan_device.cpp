#include <stdio.h>
#include <stdlib.h>

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

using create_fn = int (*)(gs_device_t **, uint32_t);
using destroy_fn = void (*)(gs_device_t *);

static void *module;
static create_fn create;
static destroy_fn destroy;

static int setup(void **)
{
	module = os_dlopen(OBS_VULKAN_MODULE);
	create = (create_fn)os_dlsym(module, "device_create");
	destroy = (destroy_fn)os_dlsym(module, "device_destroy");
	return module && create && destroy ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

// Creates adapter 0 or skips the test when this machine has no usable Vulkan 1.3 device.
static gs_device *create_or_skip()
{
	gs_device_t *device = nullptr;
	int ret = create(&device, 0);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_SUCCESS);
	assert_non_null(device);
	return device;
}

static void test_device_create_succeeds_on_adapter_0(void **)
{
	gs_device *device = create_or_skip();
	assert_non_null(device->dev);
	assert_non_null(device->graphics.q);
	assert_non_null(device->vma);
	destroy(device);
}

static void test_device_create_rejects_invalid_adapter(void **)
{
	gs_device_t *device = nullptr;
	int ret = create(&device, 9999);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_ERROR_FAIL);
	assert_null(device);
}

static void test_device_destroy_leaves_no_validation_errors(void **)
{
#ifdef _WIN32
	_putenv_s("OBS_VULKAN_VALIDATION", "1");
#else
	setenv("OBS_VULKAN_VALIDATION", "1", 1);
#endif
	auto messages = (int (*)(void))os_dlsym(module, "obs_vulkan_validation_messages");
	assert_non_null(messages);

	gs_device *device = create_or_skip();
	if (messages() < 0) {
		destroy(device);
		// Set OBS_VULKAN_REQUIRE_VALIDATION=1 to make a missing validation layer fail the test.
		const char *rq = getenv("OBS_VULKAN_REQUIRE_VALIDATION");
		if (rq && !strcmp(rq, "1")) {
			fail_msg("validation layer required (OBS_VULKAN_REQUIRE_VALIDATION=1) but missing");
		}
		skip(); // validation layer not installed
	}
	destroy(device);
	assert_int_equal(messages(), 0);
}

static void test_graphics_queue_priority_is_below_realtime(void **)
{
	gs_device *device = create_or_skip();
	assert_true(device->graphics.priority < 1.0f);
	destroy(device);
}

// These are optional imports, but a real device can answer them, so they must now be exported.
static void test_adapter_info_exports(void **)
{
	assert_non_null(os_dlsym(module, "device_enum_adapters"));
	auto renderer = (const char *(*)(void))os_dlsym(module, "gpu_get_renderer");
	auto driver = (const char *(*)(void))os_dlsym(module, "gpu_get_driver_version");
	assert_non_null(renderer);
	assert_non_null(driver);

	gs_device *device = create_or_skip();
	assert_true(strlen(renderer()) > 0);
	assert_true(strlen(driver()) > 0);
	destroy(device);
}

// A missing Vulkan loader must surface as GS_ERROR_NOT_SUPPORTED (so OBS can fall back), not a crash.
static void test_device_create_without_loader_is_not_supported(void **)
{
#ifdef _WIN32
	_putenv_s("OBS_VULKAN_TEST_NO_LOADER", "1");
#else
	setenv("OBS_VULKAN_TEST_NO_LOADER", "1", 1);
#endif
	gs_device_t *device = nullptr;
	int ret = create(&device, 0);
#ifdef _WIN32
	_putenv_s("OBS_VULKAN_TEST_NO_LOADER", "");
#else
	unsetenv("OBS_VULKAN_TEST_NO_LOADER");
#endif
	assert_int_equal(ret, GS_ERROR_NOT_SUPPORTED);
	assert_null(device);
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

static int hook_warnings;
static char hook_message[512];
static void count_hook_warnings(int lvl, const char *msg, va_list args, void *)
{
	char buf[512];
	vsnprintf(buf, sizeof(buf), msg, args);
	if (lvl == LOG_WARNING && strstr(buf, "test hook")) {
		++hook_warnings;
		strcpy(hook_message, buf);
	}
}

// Creates adapter 0 with the log captured; skips (after restoring the handler) without a device.
static gs_device *create_counting_hook_warnings()
{
	log_handler_t old_handler;
	void *old_param;
	base_get_log_handler(&old_handler, &old_param);
	hook_warnings = 0;
	hook_message[0] = 0;
	base_set_log_handler(count_hook_warnings, nullptr);
	gs_device_t *device = nullptr;
	int ret = create(&device, 0);
	base_set_log_handler(old_handler, old_param);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_SUCCESS);
	return device;
}

// The OBS_VULKAN_TEST_* hooks are read once at device creation: none is active (and nothing is logged) in a normal
// run, and an active one is named in a single warning.
static void test_test_hooks_off_by_default_and_logged_when_on(void **)
{
	gs_device *device = create_counting_hook_warnings();
	assert_int_equal(hook_warnings, 0);
	assert_false(device->test_no_loader);
	assert_false(device->test_refuse_realtime);
	assert_false(device->frames.test_fail_submit);
	destroy(device);

	set_env("OBS_VULKAN_TEST_FAIL_SUBMIT", "1");
	set_env("OBS_VULKAN_TEST_REFUSE_REALTIME", "1");
	device = create_counting_hook_warnings();
	set_env("OBS_VULKAN_TEST_FAIL_SUBMIT", "");
	set_env("OBS_VULKAN_TEST_REFUSE_REALTIME", "");
	assert_int_equal(hook_warnings, 1);
	assert_non_null(strstr(hook_message, "OBS_VULKAN_TEST_FAIL_SUBMIT"));
	assert_non_null(strstr(hook_message, "OBS_VULKAN_TEST_REFUSE_REALTIME"));
	assert_null(strstr(hook_message, "OBS_VULKAN_TEST_NO_LOADER"));
	assert_true(
		device->frames.test_fail_submit); // read at creation: clearing the variable afterwards changes nothing
	assert_true(device->test_refuse_realtime);
	device->frames.test_fail_submit = false;
	destroy(device);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_device_create_succeeds_on_adapter_0),
		cmocka_unit_test(test_device_create_rejects_invalid_adapter),
		cmocka_unit_test(test_device_destroy_leaves_no_validation_errors),
		cmocka_unit_test(test_graphics_queue_priority_is_below_realtime),
		cmocka_unit_test(test_adapter_info_exports),
		cmocka_unit_test(test_device_create_without_loader_is_not_supported),
		cmocka_unit_test(test_test_hooks_off_by_default_and_logged_when_on),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
