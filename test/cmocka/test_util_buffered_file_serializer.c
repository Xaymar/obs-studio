#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <sys/stat.h>

#include "obs-test.h"

#include <string.h>

#include <util/buffered-file-serializer.h>
#include <util/platform.h>

static void write_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct serializer s;
	assert_true(buffered_file_serializer_init_defaults(&s, "default.bin"));
	s_write(&s, "abcdef", 6);
	assert_int_equal(s.write(s.data, "", 0), 0);
	assert_int_equal(serializer_get_pos(&s), 6);

	/* rewrite the start, then continue at the end */
	assert_int_equal(serializer_seek(&s, 0, SERIALIZE_SEEK_START), 0);
	s_write(&s, "AB", 2);
	assert_int_equal(serializer_seek(&s, 4, SERIALIZE_SEEK_CURRENT), 6);
	s_write(&s, "gh", 2);
	/* Regression: SEEK_END moved back from the current write position; it now counts from the end like fseek() */
	assert_int_equal(serializer_seek(&s, 2, SERIALIZE_SEEK_START), 2);
	assert_int_equal(serializer_seek(&s, -1, SERIALIZE_SEEK_END), 7);
	s_write(&s, "H", 1);
	assert_int_equal(serializer_seek(&s, 0, SERIALIZE_SEEK_END), 8);
	buffered_file_serializer_free(&s);

	size_t size = 0;
	char *text = os_quick_read_utf8_file("default.bin");
	assert_non_null(text);
	size = strlen(text);
	assert_int_equal(size, 8);
	assert_string_equal(text, "ABcdefgH");
	bfree(text);
	os_unlink("default.bin");
}

/* Small buffers force chunking and waiting for the I/O thread */
static void small_buffer_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct serializer s;
	assert_true(buffered_file_serializer_init(&s, "small.bin", 256, 16));

	uint8_t block[1000];
	for (size_t i = 0; i < sizeof(block); i++)
		block[i] = (uint8_t)i;
	for (int i = 0; i < 20; i++)
		assert_int_equal(s_write(&s, block, sizeof(block)), sizeof(block));
	assert_int_equal(serializer_get_pos(&s), 20000);
	buffered_file_serializer_free(&s);

	struct stat st;
	assert_int_equal(os_stat("small.bin", &st), 0);
	assert_int_equal(st.st_size, 20000);
	os_unlink("small.bin");
}

static void open_fail_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct serializer s = {0};
	assert_false(buffered_file_serializer_init_defaults(&s, "no-such-dir/out.bin"));
	buffered_file_serializer_free(&s);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(write_test),
		obs_unit_test(small_buffer_test),
		obs_unit_test(open_fail_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
