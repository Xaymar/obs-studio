#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <util/config-file.h>
#include <util/platform.h>
#include <util/bmem.h>
#include <util/dstr.h>

static char *temp_path(const char *name)
{
	char *dir = os_get_abs_path_ptr(".");
	struct dstr path = {0};
	dstr_printf(&path, "%s/%s", dir, name);
	bfree(dir);
	return path.array;
}

/* Regression: saving a config with no sections failed (fwrite of 0 bytes),
 * so config_save_safe left an empty .tmp file and reported an error. */
static void save_empty_config(void **state)
{
	UNUSED_PARAMETER(state);

	char *path = temp_path("test_config_file_empty.ini");
	char *tmp = temp_path("test_config_file_empty.ini.tmp");
	os_unlink(path);
	os_unlink(tmp);

	config_t *config = NULL;
	assert_int_equal(config_open(&config, path, CONFIG_OPEN_ALWAYS), CONFIG_SUCCESS);
	assert_int_equal(config_save(config), CONFIG_SUCCESS);
	assert_int_equal(config_save_safe(config, "tmp", NULL), CONFIG_SUCCESS);
	assert_false(os_file_exists(tmp));
	assert_true(os_file_exists(path));
	config_close(config);

	os_unlink(path);
	bfree(path);
	bfree(tmp);
}

static void save_and_reload(void **state)
{
	UNUSED_PARAMETER(state);

	char *path = temp_path("test_config_file_values.ini");
	os_unlink(path);

	config_t *config = NULL;
	assert_int_equal(config_open(&config, path, CONFIG_OPEN_ALWAYS), CONFIG_SUCCESS);
	config_set_string(config, "General", "Name", "line1\nline2");
	config_set_int(config, "General", "Count", 42);
	assert_int_equal(config_save_safe(config, "tmp", NULL), CONFIG_SUCCESS);
	config_close(config);

	config = NULL;
	assert_int_equal(config_open(&config, path, CONFIG_OPEN_EXISTING), CONFIG_SUCCESS);
	assert_string_equal(config_get_string(config, "General", "Name"), "line1\nline2");
	assert_int_equal(config_get_int(config, "General", "Count"), 42);
	config_close(config);

	os_unlink(path);
	bfree(path);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(save_empty_config),
		cmocka_unit_test(save_and_reload),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
