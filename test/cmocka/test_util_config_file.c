#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <stdio.h>
#include <string.h>

#include <util/bmem.h>
#include <util/config-file.h>
#include <util/platform.h>

static char *read_file(const char *path)
{
	char *text = os_quick_read_utf8_file(path);
	assert_non_null(text);
	return text;
}

static void parse_test(void **state)
{
	UNUSED_PARAMETER(state);

	config_t *config = NULL;
	assert_int_equal(config_open_string(&config, "junk before sections\n"
						     "[General]\n"
						     "# comment\n"
						     "Name=Value with spaces\n"
						     "Escaped=a\\\\b\\nc\\rd\\x\n"
						     "Empty=\n"
						     "  Indented=1\n"
						     "NoEquals\n"
						     "Int=-42\n"
						     "Hex=0x1F\n"
						     "Bool=true\n"
						     "BoolNum=1\n"
						     "Double=2.5\n"
						     "\n"
						     "[Second]\n"
						     "Key=x\n"
						     "[Broken\n"
						     "Hidden=1\n"
						     "[Third]\n"
						     "Key=y\n"
						     "[AtEnd"),
			 CONFIG_SUCCESS);

	/* Regression: a header without ']' ("[Broken") used to create a section */
	assert_int_equal(config_num_sections(config), 3);
	assert_non_null(config_get_section(config, 0));
	assert_string_equal(config_get_section(config, 2), "Third");
	assert_null(config_get_section(config, 3));
	assert_null(config_get_string(config, "Broken", "Hidden"));
	assert_null(config_get_string(config, "Second", "Hidden"));
	assert_string_equal(config_get_string(config, "Third", "Key"), "y");

	assert_string_equal(config_get_string(config, "General", "Name"), "Value with spaces");
	assert_string_equal(config_get_string(config, "General", "Escaped"), "a\\b\nc\rd\\x");
	assert_string_equal(config_get_string(config, "General", "Empty"), "");
	assert_string_equal(config_get_string(config, "General", "Indented"), "1");
	assert_null(config_get_string(config, "General", "NoEquals"));
	assert_null(config_get_string(config, "Missing", "Name"));
	assert_int_equal(config_get_int(config, "General", "Int"), -42);
	assert_int_equal(config_get_int(config, "General", "Hex"), 0x1f);
	assert_int_equal(config_get_uint(config, "General", "Hex"), 0x1f);
	assert_int_equal(config_get_uint(config, "General", "BoolNum"), 1);
	assert_true(config_get_bool(config, "General", "Bool"));
	assert_true(config_get_bool(config, "General", "BoolNum"));
	assert_false(config_get_bool(config, "General", "Name"));
	assert_true(config_get_double(config, "General", "Double") == 2.5);
	assert_int_equal(config_get_int(config, "General", "Missing"), 0);
	assert_int_equal(config_get_uint(config, "General", "Missing"), 0);
	assert_false(config_get_bool(config, "General", "Missing"));
	assert_true(config_get_double(config, "General", "Missing") == 0.0);
	assert_int_equal(config_get_int(config, "General", "Empty"), 0);
	assert_int_equal(config_get_uint(config, "General", "Empty"), 0);

	/* no file to save to */
	assert_int_equal(config_save(config), CONFIG_ERROR);
	config_close(config);

	assert_int_equal(config_open_string(NULL, "x"), CONFIG_ERROR);
	assert_int_equal(config_open(NULL, "x", CONFIG_OPEN_EXISTING), CONFIG_ERROR);
	config_close(NULL);
	assert_int_equal(config_save(NULL), CONFIG_ERROR);
}

/* Regression: repeated keys and sections were added to the hash twice */
static void duplicate_test(void **state)
{
	UNUSED_PARAMETER(state);

	config_t *config = NULL;
	assert_int_equal(config_open_string(&config, "[A]\n"
						     "Key=first\n"
						     "Only=1\n"
						     "Key=second\n"
						     "[B]\n"
						     "X=1\n"
						     "[A]\n"
						     "Later=2\n"
						     "Key=third\n"),
			 CONFIG_SUCCESS);

	assert_int_equal(config_num_sections(config), 2);
	assert_string_equal(config_get_section(config, 0), "A");
	assert_string_equal(config_get_section(config, 1), "B");
	assert_string_equal(config_get_string(config, "A", "Key"), "third");
	assert_string_equal(config_get_string(config, "A", "Only"), "1");
	assert_string_equal(config_get_string(config, "A", "Later"), "2");
	assert_string_equal(config_get_string(config, "B", "X"), "1");

	config_set_string(config, "A", "Key", "set");
	assert_true(config_remove_value(config, "A", "Key"));
	assert_null(config_get_string(config, "A", "Key"));
	config_close(config);
}

static void set_save_test(void **state)
{
	UNUSED_PARAMETER(state);

	config_t *config = config_create("created.ini");
	assert_non_null(config);

	config_set_string(config, "S", "Str", "multi\nline\\path\r");
	config_set_string(config, "S", "Null", NULL);
	config_set_int(config, "S", "Int", -5);
	config_set_uint(config, "S", "Uint", 18446744073709551615ULL);
	config_set_bool(config, "S", "True", true);
	config_set_bool(config, "S", "False", false);
	config_set_double(config, "S", "Double", 0.25);
	config_set_string(config, "Other", "Key", "1");
	config_set_string(config, "S", "Str", "replaced\nline\\path\r");

	assert_true(config_remove_value(config, "Other", "Key"));
	assert_false(config_remove_value(config, "Other", "Key"));
	assert_false(config_remove_value(config, "Nope", "Key"));

	assert_int_equal(config_save(config), CONFIG_SUCCESS);
	config_close(config);

	assert_int_equal(config_open(&config, "created.ini", CONFIG_OPEN_EXISTING), CONFIG_SUCCESS);
	assert_string_equal(config_get_string(config, "S", "Str"), "replaced\nline\\path\r");
	assert_string_equal(config_get_string(config, "S", "Null"), "");
	assert_int_equal(config_get_int(config, "S", "Int"), -5);
	assert_true(config_get_uint(config, "S", "Uint") == 18446744073709551615ULL);
	assert_true(config_get_bool(config, "S", "True"));
	assert_false(config_get_bool(config, "S", "False"));
	assert_true(config_get_double(config, "S", "Double") == 0.25);

	/* safe save with and without a backup */
	assert_int_equal(config_save_safe(config, "tmp", "bak"), CONFIG_SUCCESS);
	assert_true(os_file_exists("created.ini.bak"));
	assert_int_equal(config_save_safe(config, ".tmp", NULL), CONFIG_SUCCESS);
	assert_int_equal(config_save_safe(config, NULL, NULL), CONFIG_ERROR);
	assert_int_equal(config_save_safe(config, "", NULL), CONFIG_ERROR);
	config_close(config);

	char *text = read_file("created.ini");
	assert_non_null(strstr(text, "Str=replaced\\nline\\\\path\\r"));
	bfree(text);

	os_unlink("created.ini");
	os_unlink("created.ini.bak");
}

/* saving fails cleanly when the file cannot be written any more */
static void save_fail_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_int_equal(os_mkdir("gone"), MKDIR_SUCCESS);
	config_t *config = config_create("gone/config.ini");
	assert_non_null(config);
	config_set_string(config, "S", "K", "V");
	assert_int_equal(os_unlink("gone/config.ini"), 0);
	assert_int_equal(os_rmdir("gone"), 0);

	assert_int_equal(config_save(config), CONFIG_FILENOTFOUND);
	assert_int_equal(config_save_safe(config, "tmp", "bak"), CONFIG_FILENOTFOUND);
	assert_false(os_file_exists("gone"));
	config_close(config);
}

/* Regression test: saving a config without sections reported CONFIG_ERROR */
static void save_empty_test(void **state)
{
	UNUSED_PARAMETER(state);

	config_t *config = config_create("empty.ini");
	assert_non_null(config);
	assert_int_equal(config_save(config), CONFIG_SUCCESS);
	config_close(config);
	os_unlink("empty.ini");
}

static void open_test(void **state)
{
	UNUSED_PARAMETER(state);

	config_t *config = NULL;
	assert_int_equal(config_open(&config, "missing.ini", CONFIG_OPEN_EXISTING), CONFIG_FILENOTFOUND);
	assert_null(config);

	assert_int_equal(config_open(&config, "always.ini", CONFIG_OPEN_ALWAYS), CONFIG_SUCCESS);
	assert_non_null(config);
	assert_int_equal(config_num_sections(config), 0);
	config_close(config);
	os_unlink("always.ini");

	/* paths that cannot be opened */
	assert_null(config_create("no-such-dir/file.ini"));
	assert_int_equal(config_open(&config, "no-such-dir/file.ini", CONFIG_OPEN_ALWAYS), CONFIG_FILENOTFOUND);

	assert_int_equal(config_open_string(&config, "[S]\nK=V\n"), CONFIG_SUCCESS);
	config_close(config);
}

static void defaults_test(void **state)
{
	UNUSED_PARAMETER(state);

	FILE *f = os_fopen("defaults.ini", "wb");
	assert_non_null(f);
	fputs("[D]\nFromFile=file\n", f);
	fclose(f);

	config_t *config = NULL;
	assert_int_equal(config_open_string(&config, "[D]\nUser=user\n"), CONFIG_SUCCESS);
	assert_int_equal(config_open_defaults(config, "defaults.ini"), CONFIG_SUCCESS);
	assert_int_equal(config_open_defaults(config, "missing.ini"), CONFIG_FILENOTFOUND);
	assert_int_equal(config_open_defaults(NULL, "defaults.ini"), CONFIG_ERROR);

	assert_string_equal(config_get_string(config, "D", "FromFile"), "file");
	assert_true(config_has_default_value(config, "D", "FromFile"));
	assert_false(config_has_user_value(config, "D", "FromFile"));
	assert_true(config_has_user_value(config, "D", "User"));

	config_set_default_string(config, "D", "Str", "s");
	config_set_default_string(config, "D", "NullStr", NULL);
	config_set_default_int(config, "D", "Int", -3);
	config_set_default_uint(config, "D", "Uint", 7);
	config_set_default_bool(config, "D", "Bool", true);
	config_set_default_double(config, "D", "Double", 1.5);
	config_set_default_string(config, "D", "User", "default");

	assert_string_equal(config_get_default_string(config, "D", "Str"), "s");
	assert_string_equal(config_get_default_string(config, "D", "NullStr"), "");
	assert_int_equal(config_get_default_int(config, "D", "Int"), -3);
	assert_int_equal(config_get_default_uint(config, "D", "Uint"), 7);
	assert_true(config_get_default_bool(config, "D", "Bool"));
	assert_true(config_get_default_double(config, "D", "Double") == 1.5);
	assert_null(config_get_default_string(config, "D", "Missing"));
	assert_int_equal(config_get_default_int(config, "D", "Missing"), 0);
	assert_int_equal(config_get_default_uint(config, "D", "Missing"), 0);
	assert_false(config_get_default_bool(config, "D", "Missing"));
	assert_true(config_get_default_double(config, "D", "Missing") == 0.0);

	/* a default does not replace a user value, but fills in a missing one */
	assert_string_equal(config_get_string(config, "D", "User"), "user");
	assert_string_equal(config_get_default_string(config, "D", "User"), "default");
	assert_true(config_has_user_value(config, "D", "Str"));

	config_close(config);
	os_unlink("defaults.ini");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(parse_test), obs_unit_test(duplicate_test), obs_unit_test(save_fail_test), obs_unit_test(set_save_test), obs_unit_test(save_empty_test),
		obs_unit_test(open_test),  obs_unit_test(defaults_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
