#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <util/c99defs.h>
#include <util/crc32.h>

static void crc32_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* Standard CRC-32 check value */
	assert_int_equal(calc_crc32(0, "123456789", 9), 0xcbf43926);
	assert_int_equal(calc_crc32(0, "", 0), 0);

	/* Incremental calculation matches a single pass */
	uint32_t crc = calc_crc32(0, "1234", 4);
	assert_int_equal(calc_crc32(crc, "56789", 5), 0xcbf43926);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(crc32_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
