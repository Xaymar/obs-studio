#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <util/pipe.h>

static void args_test(void **state)
{
	UNUSED_PARAMETER(state);

	os_process_args_t *args = os_process_args_create("prog");
	os_process_args_add_arg(args, "plain");
	os_process_args_add_argf(args, "num=%d", 5);
	/* Regression: an empty formatted argument used to become the argv terminator */
	os_process_args_add_argf(args, "%s", "");
	os_process_args_add_arg(args, "last");

	assert_int_equal(os_process_args_get_argc(args), 5);
	char **argv = os_process_args_get_argv(args);
	assert_string_equal(argv[0], "prog");
	assert_string_equal(argv[1], "plain");
	assert_string_equal(argv[2], "num=5");
	assert_string_equal(argv[3], "");
	assert_string_equal(argv[4], "last");
	assert_null(argv[5]);

	os_process_args_destroy(args);
	os_process_args_destroy(NULL);
}

static size_t read_all(os_process_pipe_t *pp, char *buf, size_t size, bool err)
{
	size_t total = 0, got;
	while (total < size - 1 && (got = err ? os_process_pipe_read_err(pp, (uint8_t *)buf + total, size - 1 - total)
					      : os_process_pipe_read(pp, (uint8_t *)buf + total, size - 1 - total)) > 0)
		total += got;
	buf[total] = 0;
	return total;
}

static void read_pipe_test(void **state)
{
	UNUSED_PARAMETER(state);

	os_process_pipe_t *pp = os_process_pipe_create("cmd.exe /c echo out& echo err 1>&2& exit 3", "r");
	assert_non_null(pp);
	char buf[256];
	read_all(pp, buf, sizeof(buf), false);
	assert_non_null(strstr(buf, "out"));
	read_all(pp, buf, sizeof(buf), true);
	assert_non_null(strstr(buf, "err"));
	/* a read pipe cannot be written to */
	assert_int_equal(os_process_pipe_write(pp, (const uint8_t *)"x", 1), 0);
	assert_int_equal(os_process_pipe_destroy(pp), 3);
}

static void write_pipe_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* findstr echoes matching stdin lines to stdout, which is not captured for write pipes */
	os_process_args_t *args = os_process_args_create("cmd.exe");
	os_process_args_add_arg(args, "/c");
	os_process_args_add_arg(args, "findstr x >NUL");
	os_process_pipe_t *pp = os_process_pipe_create2(args, "w");
	os_process_args_destroy(args);
	assert_non_null(pp);

	assert_int_equal(os_process_pipe_write(pp, (const uint8_t *)"x\r\n", 3), 3);
	uint8_t byte;
	assert_int_equal(os_process_pipe_read(pp, &byte, 1), 0);
	assert_int_equal(os_process_pipe_destroy(pp), 0);
}

static void quoting_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* arguments with spaces, quotes, tabs and trailing backslashes survive the command line */
	os_process_args_t *args = os_process_args_create("cmd.exe");
	os_process_args_add_arg(args, "/c");
	os_process_args_add_arg(args, "echo");
	os_process_args_add_arg(args, "a b");
	os_process_args_add_arg(args, "q\"uote");
	os_process_args_add_arg(args, "back\\\\\"slash");
	os_process_args_add_arg(args, "tab\there\\");
	os_process_args_add_arg(args, "");
	os_process_pipe_t *pp = os_process_pipe_create2(args, "r");
	os_process_args_destroy(args);
	assert_non_null(pp);

	char buf[256];
	read_all(pp, buf, sizeof(buf), false);
	/* cmd's echo prints the raw command line */
	assert_non_null(strstr(buf, "\"a b\""));
	assert_non_null(strstr(buf, "q\\\"uote"));
	os_process_pipe_destroy(pp);
}

static void error_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_null(os_process_pipe_create(NULL, "r"));
	assert_null(os_process_pipe_create("cmd.exe", NULL));
	assert_null(os_process_pipe_create("cmd.exe", "x"));
	assert_null(os_process_pipe_create("no-such-program-for-obs-tests.exe", "r"));

	uint8_t byte;
	assert_int_equal(os_process_pipe_read(NULL, &byte, 1), 0);
	assert_int_equal(os_process_pipe_read_err(NULL, &byte, 1), 0);
	assert_int_equal(os_process_pipe_write(NULL, &byte, 1), 0);
	assert_int_equal(os_process_pipe_destroy(NULL), 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(args_test),    obs_unit_test(read_pipe_test), obs_unit_test(write_pipe_test),
		obs_unit_test(quoting_test), obs_unit_test(error_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
