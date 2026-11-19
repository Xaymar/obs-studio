#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

#include <util/dstr.h>

#ifndef FAKE_MODULE_PATH
#error FAKE_MODULE_PATH must be defined
#endif

typedef int (*count_fn)(void);

static int count(obs_module_t *module, const char *name)
{
	count_fn fn = (count_fn)os_dlsym(obs_get_module_lib(module), name);
	assert_non_null(fn);
	return fn();
}

static int enumerated;
static void on_module(void *param, obs_module_t *module)
{
	UNUSED_PARAMETER(param);
	if (module == obs_get_module("fake-module"))
		enumerated++;
}

static void open_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	/* a file that is not a module */
	obs_module_t *bad = NULL;
	assert_int_equal(obs_open_module(&bad, "no-such-module.dll", NULL), MODULE_FAILED_TO_OPEN);
	assert_int_equal(obs_open_module(&bad, NULL, NULL), MODULE_ERROR);

	obs_module_t *module = NULL;
	assert_int_equal(obs_open_module(&module, FAKE_MODULE_PATH, FAKE_MODULE_DATA), MODULE_SUCCESS);
	assert_non_null(module);
	assert_string_equal(obs_get_module_file_name(module), "fake-module.dll");
	assert_string_equal(obs_get_module_binary_path(module), FAKE_MODULE_PATH);
	assert_string_equal(obs_get_module_data_path(module), FAKE_MODULE_DATA);
	assert_int_equal(count(module, "fake_module_loads"), 0);

	assert_true(obs_init_module(module));
	assert_int_equal(count(module, "fake_module_loads"), 1);
	/* a second init does nothing */
	assert_true(obs_init_module(module));
	assert_int_equal(count(module, "fake_module_loads"), 1);
	assert_string_equal(obs_get_module_name(module), "Fake module");
	assert_string_equal(obs_get_module_description(module), "A module for the libobs tests");
	assert_string_equal(obs_get_module_author(module), "Knurbs tests");
	assert_ptr_equal(obs_get_module("fake-module"), module);
	assert_null(obs_get_module("missing"));

	/* the source type it registered belongs to it */
	assert_string_equal(obs_source_get_display_name("fake_module_source"), "Fake module source");
	obs_source_t *src = obs_source_create("fake_module_source", "m", NULL, NULL);
	assert_ptr_equal(obs_source_get_module("fake_module_source"), module);
	assert_int_equal(obs_source_load_state("fake_module_source"), OBS_MODULE_ENABLED);
	obs_source_release(src);
	/* Regression: a source the module tried to register but that was rejected was still listed as its own */
	assert_null(obs_source_get_display_name("fake_module_invalid_source"));
	assert_null(obs_source_get_module("fake_module_invalid_source"));
	assert_int_equal(obs_source_load_state("fake_module_invalid_source"), OBS_MODULE_MISSING);

	/* locale and data files */
	const char *out = NULL;
	assert_true(obs_module_get_locale_string(module, "FakeModule.Hello", &out));
	assert_string_equal(out, "Hello");
	assert_false(obs_module_get_locale_string(module, "missing", &out));
	assert_string_equal(obs_module_get_locale_text(module, "missing"), "missing");
	char *file = obs_find_module_file(module, "file.txt");
	assert_non_null(file);
	assert_true(os_file_exists(file));
	bfree(file);
	assert_null(obs_find_module_file(module, "missing.txt"));

	enumerated = 0;
	obs_enum_modules(on_module, NULL);
	assert_int_equal(enumerated, 1);

	core_stop();
}

static void load_all_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	/* plugins found in a directory load with obs_load_plugins() */
	struct dstr bin = {0};
	dstr_copy(&bin, FAKE_MODULE_PATH);
	char *slash = strrchr(bin.array, '/');
	assert_non_null(slash);
	dstr_resize(&bin, slash - bin.array);

	/* only the fake module: the directory holds the test programs too */
	obs_add_safe_module("fake-module");
	struct obs_runtime_module_info info = {
		.path_info = {.binary = bin.array, .data = FAKE_MODULE_DATA},
		.type = OBS_MODULE_TYPE_PLUGIN,
		.name = "tests",
	};
	struct obs_module_failure_info mfi = {0};
	obs_load_plugins(&info, &mfi);
	assert_int_equal(mfi.count, 0);
	obs_module_failure_info_free(&mfi);
	dstr_free(&bin);
	obs_post_load_modules();
	obs_module_t *module = obs_get_module("fake-module");
	assert_non_null(module);
	/* libobs does not close module libraries, so the counts carry over from open_test, whose
	 * shutdown unloaded the module once */
	assert_int_equal(count(module, "fake_module_loads"), 2);
	assert_int_equal(count(module, "fake_module_post_loads"), 1);
	assert_int_equal(count(module, "fake_module_unloads"), 1);
	obs_log_loaded_modules();

	core_stop();
}

struct found_module {
	char name[64];
	char bin[512];
	char data[512];
	int count;
};

static void on_found(void *param, const struct obs_module_info2 *info)
{
	struct found_module *found = param;
	if (strcmp(info->name, "com.example.plugin") != 0 && strstr(info->bin_path, "com.example") == NULL)
		return;
	snprintf(found->name, sizeof(found->name), "%s", info->name);
	snprintf(found->bin, sizeof(found->bin), "%s", info->bin_path);
	snprintf(found->data, sizeof(found->data), "%s", info->data_path);
	found->count++;
}

static char *abs_path(const char *path)
{
	char *abs = os_get_abs_path_ptr(path);
	assert_non_null(abs);
	for (char *c = abs; *c; c++) {
		if (*c == '\\')
			*c = '/';
	}
	return abs;
}

/* Regression: a module in a folder with dots in its name (plugins/com.example.plugin/...) was looked up as
 * "com.example", the folder name cut at its last dot, so its binary was not found. Dots in parent folders must
 * not matter either. */
static void dotted_paths_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* per-module folders under a dotted parent: <root>/plugins/com.example.plugin/bin/64bit/com.example.plugin.dll */
	assert_true(os_mkdirs("dotted.root/plugins/com.example.plugin/bin/64bit") != MKDIR_ERROR);
	assert_true(os_mkdirs("dotted.root/plugins/com.example.plugin/data") != MKDIR_ERROR);
	/* left from an earlier run */
	os_unlink("dotted.root/plugins/com.example.plugin/bin/64bit/com.example.plugin.dll");
	os_unlink("dotted.root/flat/com.example.plugin.dll");
	assert_int_equal(os_copyfile(FAKE_MODULE_PATH,
				     "dotted.root/plugins/com.example.plugin/bin/64bit/com.example.plugin.dll"),
			 0);
	/* the flat layout: <root>/flat/com.example.plugin.dll, data in <root>/flat-data/com.example.plugin */
	assert_true(os_mkdirs("dotted.root/flat") != MKDIR_ERROR);
	assert_true(os_mkdirs("dotted.root/flat-data/com.example.plugin") != MKDIR_ERROR);
	assert_int_equal(os_copyfile(FAKE_MODULE_PATH, "dotted.root/flat/com.example.plugin.dll"), 0);

	/* allocated before core_start(), which counts allocations from there */
	char *root = abs_path("dotted.root");
	struct dstr bin = {0}, data = {0}, want_bin = {0}, want_data = {0};
	dstr_reserve(&bin, 512);
	dstr_reserve(&data, 512);
	dstr_reserve(&want_bin, 512);
	dstr_reserve(&want_data, 512);
	core_start();

	/* folders */
	dstr_printf(&bin, "%s/plugins/%%module%%/bin/64bit", root);
	dstr_printf(&data, "%s/plugins/%%module%%/data", root);
	obs_add_module_path(bin.array, data.array);
	struct found_module found = {0};
	obs_find_modules2(on_found, &found);
	assert_int_equal(found.count, 1);
	assert_string_equal(found.name, "com.example.plugin");
	dstr_printf(&want_bin, "%s/plugins/com.example.plugin/bin/64bit/com.example.plugin.dll", root);
	dstr_printf(&want_data, "%s/plugins/com.example.plugin/data", root);
	assert_string_equal(found.bin, want_bin.array);
	assert_string_equal(found.data, want_data.array);

	/* and loading them the way the frontend does */
	obs_add_safe_module("com.example.plugin");
	struct obs_runtime_module_info info = {
		.path_info = {.binary = bin.array, .data = data.array},
		.type = OBS_MODULE_TYPE_PLUGIN,
		.name = "dotted",
	};
	struct obs_module_failure_info mfi = {0};
	obs_load_plugins(&info, &mfi);
	assert_int_equal(mfi.count, 0);
	obs_module_failure_info_free(&mfi);
	obs_module_t *module = obs_get_module("com.example.plugin");
	assert_non_null(module);
	assert_string_equal(obs_get_module_file_name(module), "com.example.plugin.dll");
	assert_string_equal(obs_get_module_binary_path(module), want_bin.array);
	assert_string_equal(obs_get_module_data_path(module), want_data.array);

	core_stop();
	core_start();

	/* flat */
	dstr_printf(&bin, "%s/flat", root);
	dstr_printf(&data, "%s/flat-data/%%module%%", root);
	obs_add_module_path(bin.array, data.array);
	memset(&found, 0, sizeof(found));
	obs_find_modules2(on_found, &found);
	assert_int_equal(found.count, 1);
	assert_string_equal(found.name, "com.example.plugin");
	dstr_printf(&want_bin, "%s/flat/com.example.plugin.dll", root);
	dstr_printf(&want_data, "%s/flat-data/com.example.plugin", root);
	assert_string_equal(found.bin, want_bin.array);
	assert_string_equal(found.data, want_data.array);

	obs_add_safe_module("com.example.plugin");
	info.path_info.binary = bin.array;
	info.path_info.data = data.array;
	obs_load_plugins(&info, &mfi);
	assert_int_equal(mfi.count, 0);
	obs_module_failure_info_free(&mfi);
	module = obs_get_module("com.example.plugin");
	assert_non_null(module);
	assert_string_equal(obs_get_module_binary_path(module), want_bin.array);
	assert_string_equal(obs_get_module_data_path(module), want_data.array);

	core_stop();
	dstr_free(&bin);
	dstr_free(&data);
	dstr_free(&want_bin);
	dstr_free(&want_data);
	bfree(root);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(open_test),
		cmocka_unit_test(load_all_test),
		cmocka_unit_test(dotted_paths_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
