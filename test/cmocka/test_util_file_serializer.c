#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <util/file-serializer.h>
#include <util/platform.h>

static void round_trip_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct serializer out;
	assert_true(file_output_serializer_init(&out, "plain.bin"));
	s_write(&out, "hello world", 11);
	assert_int_equal(serializer_get_pos(&out), 11);
	assert_int_equal(serializer_seek(&out, 0, SERIALIZE_SEEK_START), 0);
	s_write(&out, "J", 1);
	assert_int_equal(serializer_seek(&out, 0, SERIALIZE_SEEK_END), 11);
	assert_int_equal(serializer_seek(&out, -5, SERIALIZE_SEEK_CURRENT), 6);
	s_write(&out, "W", 1);
	assert_int_equal(serializer_seek(&out, -1, SERIALIZE_SEEK_START), -1);
	file_output_serializer_free(&out);

	struct serializer in;
	assert_true(file_input_serializer_init(&in, "plain.bin"));
	char buf[16] = {0};
	assert_int_equal(s_read(&in, buf, sizeof(buf)), 11);
	assert_string_equal(buf, "Jello World");
	assert_int_equal(serializer_get_pos(&in), 11);
	assert_int_equal(serializer_seek(&in, 6, SERIALIZE_SEEK_START), 6);
	assert_int_equal(serializer_seek(&in, 1, SERIALIZE_SEEK_CURRENT), 7);
	assert_int_equal(serializer_seek(&in, -2, SERIALIZE_SEEK_END), 9);
	assert_int_equal(s_read(&in, buf, 2), 2);
	assert_memory_equal(buf, "ld", 2);
	assert_int_equal(serializer_seek(&in, -1, SERIALIZE_SEEK_START), -1);
	assert_int_equal(s_write(&in, "x", 1), 0);

	/* serialize() reads from a serializer without a write function */
	assert_int_equal(serializer_seek(&in, 0, SERIALIZE_SEEK_START), 0);
	memset(buf, 0, sizeof(buf));
	assert_int_equal(serialize(&in, buf, 5), 5);
	assert_string_equal(buf, "Jello");
	assert_int_equal(serialize(NULL, buf, 5), 0);
	file_input_serializer_free(&in);

	os_unlink("plain.bin");
}

static void safe_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct serializer out;
	assert_false(file_output_serializer_init_safe(&out, "safe.bin", NULL));
	assert_false(file_output_serializer_init_safe(&out, "safe.bin", ""));
	assert_false(file_output_serializer_init_safe(&out, "no-such-dir/safe.bin", "tmp"));

	/* an existing file is only replaced once the serializer is freed */
	assert_true(file_output_serializer_init(&out, "safe.bin"));
	s_write(&out, "old", 3);
	file_output_serializer_free(&out);

	assert_true(file_output_serializer_init_safe(&out, "safe.bin", "tmp"));
	assert_true(os_file_exists("safe.bin.tmp"));
	s_write(&out, "new", 3);
	file_output_serializer_free(&out);
	assert_false(os_file_exists("safe.bin.tmp"));

	char *text = os_quick_read_utf8_file("safe.bin");
	assert_string_equal(text, "new");
	bfree(text);

	assert_true(file_output_serializer_init_safe(&out, "safe.bin", ".tmp2"));
	file_output_serializer_free(&out);

	os_unlink("safe.bin");
}

static void open_fail_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct serializer s = {0};
	assert_false(file_input_serializer_init(&s, "missing.bin"));
	file_input_serializer_free(&s);
	assert_false(file_output_serializer_init(&s, "no-such-dir/out.bin"));

	struct serializer empty = {0};
	file_output_serializer_free(&empty);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(round_trip_test),
		obs_unit_test(safe_test),
		obs_unit_test(open_fail_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
