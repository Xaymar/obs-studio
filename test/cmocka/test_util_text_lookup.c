#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <stdio.h>
#include <string.h>

#include <util/bmem.h>
#include <util/platform.h>
#include <util/text-lookup.h>

static void write_file(const char *path, const char *text)
{
	FILE *f = os_fopen(path, "wb");
	assert_non_null(f);
	fwrite(text, 1, strlen(text), f);
	fclose(f);
}

static void lookup_test(void **state)
{
	UNUSED_PARAMETER(state);

	write_file("en.ini", "\xEF\xBB\xBF# comment line\r\n"
			     "Plain=Hello\r\n"
			     "Quoted=\"Hello \\\"World\\\"\\n\\tTab\\r\"\n"
			     "Spaced=\"x y\" # trailing comment\n"
			     "Around = x\n"
			     "\t  Indented\t=  \"y y\"\r\n"
			     "Blank = \n"
			     "CrLfNoValue\r\n"
			     "Unterminated=\"open\n"
			     "\n"
			     "NoValue\n"
			     "NoEquals \"value\"\n"
			     "Override=first\n"
			     "Override=second\n"
			     "Last=end");

	lookup_t *lookup = text_lookup_create("en.ini");
	assert_non_null(lookup);

	const char *out = NULL;
	assert_true(text_lookup_getstr(lookup, "Plain", &out));
	assert_string_equal(out, "Hello");
	assert_true(text_lookup_getstr(lookup, "Quoted", &out));
	assert_string_equal(out, "Hello \"World\"\n\tTab\r");
	assert_true(text_lookup_getstr(lookup, "Spaced", &out));
	assert_string_equal(out, "x y");
	/* Regression: whitespace around '=' used to be the value */
	assert_true(text_lookup_getstr(lookup, "Around", &out));
	assert_string_equal(out, "x");
	assert_true(text_lookup_getstr(lookup, "Indented", &out));
	assert_string_equal(out, "y y");
	assert_false(text_lookup_getstr(lookup, "Blank", &out));
	assert_false(text_lookup_getstr(lookup, "CrLfNoValue", &out));
	assert_true(text_lookup_getstr(lookup, "Unterminated", &out));
	assert_string_equal(out, "open");
	/* without '=' the first token after the name is the value */
	assert_true(text_lookup_getstr(lookup, "NoEquals", &out));
	assert_string_equal(out, "value");
	assert_true(text_lookup_getstr(lookup, "Override", &out));
	assert_string_equal(out, "second");
	assert_true(text_lookup_getstr(lookup, "Last", &out));
	assert_string_equal(out, "end");
	assert_false(text_lookup_getstr(lookup, "NoValue", &out));
	assert_false(text_lookup_getstr(lookup, "Missing", &out));

	/* a second file overrides and extends */
	write_file("extra.ini", "Plain=Hi\nExtra=1\n");
	assert_true(text_lookup_add(lookup, "extra.ini"));
	assert_true(text_lookup_getstr(lookup, "Plain", &out));
	assert_string_equal(out, "Hi");
	assert_true(text_lookup_getstr(lookup, "Extra", &out));

	assert_false(text_lookup_add(lookup, "missing.ini"));

	/* an empty file adds nothing */
	write_file("empty.ini", "");
	assert_false(text_lookup_add(lookup, "empty.ini"));

	text_lookup_destroy(lookup);
	text_lookup_destroy(NULL);
	assert_false(text_lookup_getstr(NULL, "Plain", &out));

	os_unlink("en.ini");
	os_unlink("extra.ini");
	os_unlink("empty.ini");
}

static void create_fail_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_null(text_lookup_create("does-not-exist.ini"));

	/* a lookup without items */
	write_file("comments.ini", "# only comments\n");
	lookup_t *lookup = text_lookup_create("comments.ini");
	assert_non_null(lookup);
	const char *out;
	assert_false(text_lookup_getstr(lookup, "x", &out));
	text_lookup_destroy(lookup);
	os_unlink("comments.ini");
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(lookup_test),
		obs_unit_test(create_fail_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
