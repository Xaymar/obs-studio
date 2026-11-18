#include <fstream>
#include <string>
#include <vector>

#include <graphics/graphics.h>
#include <util/platform.h>

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

// Required (non-optional) imports of the Windows section of graphics-imports.c.
#define EXPECTED_REQUIRED_WIN 142

static void *open_module()
{
	return os_dlopen(OBS_VULKAN_MODULE);
}

static void test_module_loads(void **)
{
	void *module = open_module();
	assert_non_null(module);
	os_dlclose(module);
}

// Names of the imports of the current platform from graphics-imports.c, for the given macro prefix.
static std::vector<std::string> imports_for_platform(const std::string &key)
{
	std::ifstream file(std::string(OBS_SOURCE_DIR) + "/libobs/graphics/graphics-imports.c");
	assert_true(file.is_open());

	enum { ANY, APPLE, WIN, LINUX } section = ANY;
#if defined(__APPLE__)
	const auto here = APPLE;
#elif defined(_WIN32)
	const auto here = WIN;
#else
	const auto here = LINUX;
#endif

	std::vector<std::string> names;
	std::string line;
	while (std::getline(file, line)) {
		if (line.rfind("#ifdef __APPLE__", 0) == 0) {
			section = APPLE;
		} else if (line.rfind("#elif _WIN32", 0) == 0) {
			section = WIN;
		} else if (line.rfind("#elif defined(__linux__)", 0) == 0) {
			section = LINUX;
		} else if (line.rfind("#endif", 0) == 0) {
			section = ANY;
		}

		size_t pos = line.find(key);
		if (pos == std::string::npos || line.find_first_not_of(" 	") != pos) {
			continue;
		}
		if (section != ANY && section != here) {
			continue;
		}
		std::string name = line.substr(pos + key.size());
		names.push_back(name.substr(0, name.find(')')));
	}
	return names;
}

// Every GRAPHICS_IMPORT( (not _OPTIONAL) of the current platform must be exported.
static void test_required_exports(void **)
{
	void *module = open_module();
	assert_non_null(module);

	auto names = imports_for_platform("GRAPHICS_IMPORT(");
#ifdef _WIN32
	assert_int_equal(names.size(), EXPECTED_REQUIRED_WIN);
#else
	assert_true(names.size() > 100);
#endif
	size_t missing = 0;
	for (auto &name : names) {
		if (!os_dlsym(module, name.c_str())) {
			print_message("missing export: %s\n", name.c_str());
			++missing;
		}
	}
	assert_int_equal(missing, 0);
	os_dlclose(module);
}

// Optional imports are feature probes: exporting a stub makes libobs believe the feature works.
static void test_optional_not_exported(void **)
{
	void *module = open_module();
	assert_non_null(module);

	auto names = imports_for_platform("GRAPHICS_IMPORT_OPTIONAL(");
	assert_true(names.size() > 10);
	size_t exported = 0;
	for (auto &name : names) {
		// Implemented for real (Task 2), so exporting them is correct.
		if (name == "device_enum_adapters" || name == "gpu_get_driver_version" || name == "gpu_get_renderer") {
			continue;
		}
		if (os_dlsym(module, name.c_str())) {
			print_message("optional export must be absent: %s\n", name.c_str());
			++exported;
		}
	}
	assert_int_equal(exported, 0);
	os_dlclose(module);
}

// Exports beyond the graphics imports, allowed on purpose: the host API (documented in vulkan-subsystem.hpp) and the
// test hooks. None may be a graphics import name, so they cannot change what libobs probes.
static void test_extra_exports_allowlist(void **)
{
	void *module = open_module();
	assert_non_null(module);
	const char *extra[] = {"obs_vulkan_set_global_priority", "obs_vulkan_validation_messages",
			       "obs_vulkan_test_stage_mip", "obs_vulkan_glslang_refs"};
	auto required = imports_for_platform("GRAPHICS_IMPORT(");
	auto optional = imports_for_platform("GRAPHICS_IMPORT_OPTIONAL(");
	for (const char *name : extra) {
		assert_non_null(os_dlsym(module, name));
		for (auto *list : {&required, &optional}) {
			for (auto &import : *list) {
				assert_string_not_equal(import.c_str(), name);
			}
		}
	}
	os_dlclose(module);
}

static void test_identity(void **)
{
	void *module = open_module();
	assert_non_null(module);

	auto get_type = (int (*)(void))os_dlsym(module, "device_get_type");
	auto get_name = (const char *(*)(void))os_dlsym(module, "device_get_name");
	assert_non_null(get_type);
	assert_non_null(get_name);
	assert_int_equal(get_type(), GS_DEVICE_VULKAN);
	assert_string_equal(get_name(), "Vulkan");
	os_dlclose(module);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_module_loads),
		cmocka_unit_test(test_required_exports),
		cmocka_unit_test(test_optional_not_exported),
		cmocka_unit_test(test_extra_exports_allowlist),
		cmocka_unit_test(test_identity),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
