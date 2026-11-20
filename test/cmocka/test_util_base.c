#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>

#include <util/base.h>
#include <util/bmem.h>

static int last_level;
static char last_msg[256];
static void *last_param;

static void capture_log(int level, const char *format, va_list args, void *param)
{
	last_level = level;
	last_param = param;
	vsnprintf(last_msg, sizeof(last_msg), format, args);
}

static void log_handler_test(void **state)
{
	UNUSED_PARAMETER(state);

	log_handler_t old_handler;
	void *old_param;
	base_get_log_handler(&old_handler, &old_param);
	assert_non_null(old_handler);
	base_get_log_handler(NULL, NULL);

	int tag;
	base_set_log_handler(capture_log, &tag);
	log_handler_t handler;
	void *param;
	base_get_log_handler(&handler, &param);
	assert_ptr_equal(handler, capture_log);
	assert_ptr_equal(param, &tag);

	blog(LOG_WARNING, "value %d", 7);
	assert_int_equal(last_level, LOG_WARNING);
	assert_string_equal(last_msg, "value 7");
	assert_ptr_equal(last_param, &tag);

	/* NULL restores the default handler, which prints every level */
	base_set_log_handler(NULL, NULL);
	base_get_log_handler(&handler, NULL);
	assert_ptr_equal(handler, old_handler);
	blog(LOG_DEBUG, "default handler: debug");
	blog(LOG_INFO, "default handler: info");
	blog(LOG_WARNING, "default handler: warning");
	blog(LOG_ERROR, "default handler: error");
	blog(12345, "default handler: unknown level is dropped");
}

static jmp_buf crash_jump;
static char crash_msg[256];
static void *crash_param;

static void crash_handler(const char *format, va_list args, void *param)
{
	crash_param = param;
	vsnprintf(crash_msg, sizeof(crash_msg), format, args);
	longjmp(crash_jump, 1);
}

static void other_crash_handler(const char *format, va_list args, void *param)
{
	UNUSED_PARAMETER(format);
	UNUSED_PARAMETER(args);
	UNUSED_PARAMETER(param);
	fail_msg("replaced the first crash handler");
}

/* bcrash() can only run once per process (a second call exits with "Crashed in the crash handler"). */
static void crash_handler_test(void **state)
{
	UNUSED_PARAMETER(state);

	int tag;
	base_set_crash_handler(crash_handler, &tag);

	/* Only the first handler sticks */
	base_set_log_handler(capture_log, NULL);
	base_set_crash_handler(other_crash_handler, NULL);
	assert_int_equal(last_level, LOG_WARNING);
	base_set_log_handler(NULL, NULL);

	if (setjmp(crash_jump) == 0) {
		bcrash("crash %s", "now");
		fail_msg("bcrash returned");
	}
	assert_string_equal(crash_msg, "crash now");
	assert_ptr_equal(crash_param, &tag);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(log_handler_test),
		cmocka_unit_test(crash_handler_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
