// Queue global priority: LOW by default, realtime only on opt-in (OBS_VULKAN_REALTIME=1 or the host API), and a
// refused level falls back without failing device creation. Runs under the validation layer like the other device
// tests; skips without a Vulkan 1.3 device.
#include <stdio.h>
#include <stdlib.h>

#include <string>

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
static int (*messages)(void);
static int (*set_priority)(int);

static std::string log_text;
static log_handler_t prev_handler;
static void *prev_param;

static void capture_log(int lvl, const char *fmt, va_list args, void *)
{
	char buf[4096];
	vsnprintf(buf, sizeof(buf), fmt, args);
	log_text += buf;
	log_text += "\n";
	(void)lvl;
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value);
#else
	if (*value) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
#endif
}

static int setup(void **)
{
	module = os_dlopen(OBS_VULKAN_MODULE);
	create = (create_fn)os_dlsym(module, "device_create");
	destroy = (destroy_fn)os_dlsym(module, "device_destroy");
	messages = (int (*)(void))os_dlsym(module, "obs_vulkan_validation_messages");
	set_priority = (int (*)(int))os_dlsym(module, "obs_vulkan_set_global_priority");
	base_get_log_handler(&prev_handler, &prev_param);
	return module && create && destroy && messages ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

// Every test starts from the default: no environment opt-in, host level LOW, log captured from here on.
static int reset(void **)
{
	set_env("OBS_VULKAN_REALTIME", "");
	set_env("OBS_VULKAN_TEST_REFUSE_REALTIME", "");
	if (set_priority) {
		set_priority(VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR);
	}
	log_text.clear();
	base_set_log_handler(capture_log, nullptr);
	return 0;
}

static int restore(void **)
{
	reset(nullptr);
	base_set_log_handler(prev_handler, prev_param);
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
	assert_non_null(device);
	return device;
}

// Destroys the device and checks the validation layer reported nothing (fails if it is required but missing).
static void destroy_checked(gs_device *device)
{
	destroy(device);
	const int n = messages();
	if (n < 0) {
		const char *rq = getenv("OBS_VULKAN_REQUIRE_VALIDATION");
		if (rq && !strcmp(rq, "1")) {
			fail_msg("validation layer required (OBS_VULKAN_REQUIRE_VALIDATION=1) but missing");
		}
		return;
	}
	assert_int_equal(n, 0);
}

static bool logged(const char *text)
{
	return log_text.find(text) != std::string::npos;
}

// The "<label> queue:" log line names the requested and the granted level, each with its number.
static void check_queue_line(const char *label, const vk_queue &q)
{
	const size_t at = log_text.find(std::string(label) + " queue: family");
	assert_true(at != std::string::npos);
	const std::string line = log_text.substr(at, log_text.find('\n', at) - at);
	const size_t req = line.find("requested "), gr = line.find(", granted ");
	assert_true(req != std::string::npos && gr != std::string::npos);
	const std::string want_req = "(" + std::to_string((int)q.requested) + ")";
	const std::string want_gr = "(" + std::to_string((int)q.global_priority) + ")";
	assert_true(line.substr(req, gr - req).find(want_req) != std::string::npos);
	assert_true(line.substr(gr).find(want_gr) != std::string::npos);
}

static void test_granted_priority_is_logged(void **)
{
	gs_device *device = create_or_skip();
	print_message("%s", log_text.c_str());
	assert_int_equal(device->graphics.requested, VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR);
	assert_true(logged("requested low (128)"));
	check_queue_line("graphics", device->graphics);
	if (device->transfer.q) {
		check_queue_line("transfer", device->transfer);
	}
	destroy_checked(device);
}

static void test_realtime_is_not_requested_by_default(void **)
{
	gs_device *device = create_or_skip();
	assert_int_equal(device->graphics.requested, VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR);
	// A family that offers LOW must be granted LOW (128); otherwise no request at all (driver default, 0.5).
	const char *offered_key = "offered by the graphics family: ";
	const size_t at = log_text.find(offered_key);
	assert_true(at != std::string::npos);
	const std::string offered = log_text.substr(at, log_text.find('\n', at) - at);
	if (offered.find("low") != std::string::npos) {
		assert_int_equal(device->graphics.global_priority, 128);
		assert_int_equal(device->graphics.create_info_priority, 128); // really chained to vkCreateDevice
		assert_true(device->graphics.priority == 0.25f);
		assert_true(logged("create-info priority 128"));
	} else {
		assert_int_equal(device->graphics.global_priority, 0);
		assert_int_equal(device->graphics.create_info_priority, 0);
		assert_true(device->graphics.priority == 0.5f);
	}
	assert_false(logged("requested realtime"));
	destroy_checked(device);
}

static void test_realtime_requested_when_env_obs_vulkan_realtime_is_1(void **)
{
	set_env("OBS_VULKAN_REALTIME", "1");
	gs_device *device = create_or_skip();
	print_message("%s", log_text.c_str());
	assert_int_equal(device->graphics.requested, VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR);
	assert_true(logged("requested realtime (1024)"));
	if (device->graphics.global_priority == VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR) {
		assert_true(device->graphics.priority == 1.0f);
		assert_true(logged("granted realtime (1024)"));
	} else {
		// Refused or not offered: some lower level was granted, and the log says which.
		assert_true(device->graphics.global_priority < VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR);
	}
	check_queue_line("graphics", device->graphics);
	destroy_checked(device);
}

static void test_host_api_sets_the_requested_level(void **)
{
	assert_non_null(set_priority);
	assert_int_equal(set_priority(7), 0); // not a VkQueueGlobalPriorityKHR value: ignored
	assert_int_equal(set_priority(VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR), 1);
	gs_device *device = create_or_skip();
	assert_int_equal(device->graphics.requested, VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR);
	assert_true(device->graphics.global_priority <= VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR);
	assert_true(logged("requested medium (256)"));
	destroy_checked(device);
}

static void test_driver_refusal_falls_back_without_error(void **)
{
	set_env("OBS_VULKAN_REALTIME", "1");
	set_env("OBS_VULKAN_TEST_REFUSE_REALTIME", "1");
	gs_device *device = create_or_skip(); // device_create still succeeds
	print_message("%s", log_text.c_str());
	assert_non_null(device->dev);
	assert_int_equal(device->graphics.requested, VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR);
	assert_true(device->graphics.global_priority < VK_QUEUE_GLOBAL_PRIORITY_REALTIME_KHR);
	assert_true(device->graphics.priority < 1.0f);
	assert_true(logged("global priority realtime (1024) refused"));
	check_queue_line("graphics", device->graphics);
	destroy_checked(device);
}

static void check_transfer(gs_device *device)
{
	if (!device->transfer.q) {
		destroy_checked(device);
		skip(); // single queue family
	}
	assert_int_equal(device->transfer.requested, VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR);
	assert_true(device->transfer.global_priority <= VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR);
	assert_true(device->transfer.priority <= device->graphics.priority);
	assert_true(logged("transfer queue: family"));
	destroy_checked(device);
}

static void test_transfer_queue_priority_is_not_above_graphics(void **)
{
	check_transfer(create_or_skip());
	log_text.clear();
	set_env("OBS_VULKAN_REALTIME", "1"); // an opt-in raises graphics only
	check_transfer(create_or_skip());
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(test_granted_priority_is_logged, reset, restore),
		cmocka_unit_test_setup_teardown(test_realtime_is_not_requested_by_default, reset, restore),
		cmocka_unit_test_setup_teardown(test_realtime_requested_when_env_obs_vulkan_realtime_is_1, reset,
						restore),
		cmocka_unit_test_setup_teardown(test_host_api_sets_the_requested_level, reset, restore),
		cmocka_unit_test_setup_teardown(test_driver_refusal_falls_back_without_error, reset, restore),
		cmocka_unit_test_setup_teardown(test_transfer_queue_priority_is_not_above_graphics, reset, restore),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
