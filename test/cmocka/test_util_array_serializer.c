#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <util/array-serializer.h>

static void write_seek_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct serializer s;
	struct array_output_data data;
	array_output_serializer_init(&s, &data);

	s_write(&s, "abcdef", 6);
	assert_int_equal(data.bytes.num, 6);
	assert_int_equal(serializer_get_pos(&s), 6);

	/* overwrite inside the array */
	assert_int_equal(serializer_seek(&s, 1, SERIALIZE_SEEK_START), 1);
	s_write(&s, "XY", 2);
	assert_memory_equal(data.bytes.array, "aXYdef", 6);
	/* Regression: get_pos reported the array size, not the write position */
	assert_int_equal(serializer_get_pos(&s), 3);

	/* overwrite across the end grows the array */
	assert_int_equal(serializer_seek(&s, 1, SERIALIZE_SEEK_CURRENT), 4);
	s_write(&s, "1234", 4);
	assert_int_equal(data.bytes.num, 8);
	assert_memory_equal(data.bytes.array, "aXYd1234", 8);

	/* Regression: SEEK_END used to count backwards for a positive offset; it now works like fseek() */
	assert_int_equal(serializer_seek(&s, -2, SERIALIZE_SEEK_END), 6);
	assert_int_equal(serializer_get_pos(&s), 6);
	s_write(&s, "!", 1);
	assert_memory_equal(data.bytes.array, "aXYd12!4", 8);

	assert_int_equal(serializer_seek(&s, 100, SERIALIZE_SEEK_START), -1);
	assert_int_equal(serializer_seek(&s, 1, SERIALIZE_SEEK_END), -1);
	assert_int_equal(serializer_seek(&s, -9, SERIALIZE_SEEK_END), -1);
	assert_int_equal(serializer_seek(&s, 0, SERIALIZE_SEEK_END), 8);

	array_output_serializer_reset(&data);
	assert_int_equal(data.bytes.num, 0);
	assert_int_equal(data.cur_pos, 0);

	/* nothing to read back */
	uint8_t byte;
	assert_int_equal(s_read(&s, &byte, 1), 0);

	array_output_serializer_free(&data);
}

static void typed_write_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct serializer s;
	struct array_output_data data;
	array_output_serializer_init(&s, &data);

	s_w8(&s, 0x01);
	s_wl16(&s, 0x0302);
	s_wl24(&s, 0x060504);
	s_wl32(&s, 0x0a090807);
	s_wl64(&s, 0x1211100f0e0d0c0bULL);
	s_wb16(&s, 0x1314);
	s_wb24(&s, 0x151617);
	s_wb32(&s, 0x18191a1b);
	s_wb64(&s, 0x1c1d1e1f20212223ULL);

	uint8_t expected[35];
	for (int i = 0; i < 35; i++)
		expected[i] = (uint8_t)(i + 1);
	assert_int_equal(data.bytes.num, 35);
	assert_memory_equal(data.bytes.array, expected, 35);

	array_output_serializer_reset(&data);
	s_wlf(&s, 1.0f);
	s_wbf(&s, 1.0f);
	s_wld(&s, 1.0);
	s_wbd(&s, 1.0);
	const uint8_t floats[] = {0x00, 0x00, 0x80, 0x3f, 0x3f, 0x80, 0x00, 0x00, 0, 0, 0, 0,
				  0,    0,    0xf0, 0x3f, 0x3f, 0xf0, 0,    0,    0, 0, 0, 0};
	assert_int_equal(data.bytes.num, sizeof(floats));
	assert_memory_equal(data.bytes.array, floats, sizeof(floats));

	array_output_serializer_reset(&data);
	char buf[] = "xyz";
	assert_int_equal(serialize(&s, buf, 3), 3);
	assert_memory_equal(data.bytes.array, "xyz", 3);

	array_output_serializer_free(&data);
}

static void null_serializer_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* a serializer without callbacks does nothing */
	struct serializer s = {0};
	uint8_t byte = 0;
	assert_int_equal(s_read(&s, &byte, 1), 0);
	assert_int_equal(s_write(&s, &byte, 1), 0);
	assert_int_equal(serialize(&s, &byte, 1), 0);
	assert_int_equal(serializer_seek(&s, 0, SERIALIZE_SEEK_START), -1);
	assert_int_equal(serializer_get_pos(&s), -1);
	assert_int_equal(s_read(NULL, &byte, 1), 0);
	assert_int_equal(s_write(NULL, &byte, 1), 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(write_seek_test),
		obs_unit_test(typed_write_test),
		obs_unit_test(null_serializer_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
