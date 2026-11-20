#include "cmocka.h"

jmp_buf cmshim_jmp;
int cmshim_in_test;

enum { R_PASS, R_FAIL, R_SKIP };

void print_message(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vprintf(fmt, a);
	va_end(a);
	fflush(stdout);
}

void print_error(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	vfprintf(stderr, fmt, a);
	va_end(a);
}

void cmshim_fail(const char *file, int line, const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	fprintf(stdout, "[  ERROR   ] --- ");
	vfprintf(stdout, fmt, a);
	fprintf(stdout, "\n[   LINE   ] --- %s:%d\n", file, line);
	va_end(a);
	fflush(stdout);
	if (cmshim_in_test)
		longjmp(cmshim_jmp, R_FAIL);
	abort();
}

void cmshim_skip(const char *file, int line)
{
	(void)file;
	(void)line;
	if (cmshim_in_test)
		longjmp(cmshim_jmp, R_SKIP);
	exit(0);
}

int cmshim_run_group(const char *name, const struct CMUnitTest *tests, size_t n, CMFixtureFunction setup,
		     CMFixtureFunction teardown)
{
	void *gstate = NULL;
	size_t passed = 0, failed = 0, skipped = 0;
	printf("[==========] %s: Running %zu test(s).\n", name, n);
	if (setup && setup(&gstate) != 0) {
		printf("[  FAILED  ] GROUP SETUP\n");
		return (int)n;
	}
	for (size_t i = 0; i < n; i++) {
		void *state = tests[i].initial_state ? tests[i].initial_state : gstate;
		printf("[ RUN      ] %s\n", tests[i].name);
		fflush(stdout);
		int r;
		cmshim_in_test = 1;
		if ((r = setjmp(cmshim_jmp)) == 0) {
			if (tests[i].setup_func && tests[i].setup_func(&state) != 0)
				longjmp(cmshim_jmp, R_FAIL);
			tests[i].test_func(&state);
			r = R_PASS;
		}
		cmshim_in_test = 0;
		if (tests[i].teardown_func)
			tests[i].teardown_func(&state);
		if (r == R_PASS) {
			passed++;
			printf("[       OK ] %s\n", tests[i].name);
		} else if (r == R_SKIP) {
			skipped++;
			printf("[  SKIPPED ] %s\n", tests[i].name);
		} else {
			failed++;
			printf("[  FAILED  ] %s\n", tests[i].name);
		}
		fflush(stdout);
	}
	if (teardown)
		teardown(&gstate);
	printf("[==========] %s: %zu test(s) run.\n[  PASSED  ] %zu test(s).\n", name, n, passed);
	if (skipped)
		printf("[  SKIPPED ] %zu test(s).\n", skipped);
	if (failed)
		printf("[  FAILED  ] %zu test(s).\n", failed);
	return (int)failed;
}
