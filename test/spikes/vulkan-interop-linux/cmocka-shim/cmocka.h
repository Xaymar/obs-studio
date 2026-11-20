/* Minimal cmocka stand-in for the Linux spike run (libcmocka-dev is not in the image). Implements only what
 * test/cmocka/test_vulkan_*.cpp use: assertions, skip/fail, unit tests with setup/teardown, group runner. */
#pragma once
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*CMUnitTestFunction)(void **state);
typedef int (*CMFixtureFunction)(void **state);
struct CMUnitTest {
	const char *name;
	CMUnitTestFunction test_func;
	CMFixtureFunction setup_func;
	CMFixtureFunction teardown_func;
	void *initial_state;
};

extern jmp_buf cmshim_jmp;
extern int cmshim_in_test;
void cmshim_fail(const char *file, int line, const char *fmt, ...);
void cmshim_skip(const char *file, int line);
void print_message(const char *fmt, ...);
void print_error(const char *fmt, ...);
int cmshim_run_group(const char *name, const struct CMUnitTest *tests, size_t n, CMFixtureFunction setup,
		     CMFixtureFunction teardown);

#define cmocka_unit_test(f) {#f, f, NULL, NULL, NULL}
#define cmocka_unit_test_setup_teardown(f, s, t) {#f, f, s, t, NULL}
#define cmocka_unit_test_setup(f, s) {#f, f, s, NULL, NULL}
#define cmocka_unit_test_teardown(f, t) {#f, f, NULL, t, NULL}
#define cmocka_run_group_tests(t, s, td) cmshim_run_group(#t, t, sizeof(t) / sizeof((t)[0]), s, td)
#define cmocka_run_group_tests_name(nm, t, s, td) cmshim_run_group(nm, t, sizeof(t) / sizeof((t)[0]), s, td)

#define skip() cmshim_skip(__FILE__, __LINE__)
#define fail() cmshim_fail(__FILE__, __LINE__, "fail()")
#define fail_msg(...) cmshim_fail(__FILE__, __LINE__, __VA_ARGS__)
#define assert_true(c) ((c) ? (void)0 : cmshim_fail(__FILE__, __LINE__, "assert_true(%s)", #c))
#define assert_false(c) (!(c) ? (void)0 : cmshim_fail(__FILE__, __LINE__, "assert_false(%s)", #c))
#define assert_non_null(p) ((p) != NULL ? (void)0 : cmshim_fail(__FILE__, __LINE__, "%s is NULL", #p))
#define assert_null(p) ((p) == NULL ? (void)0 : cmshim_fail(__FILE__, __LINE__, "%s is not NULL", #p))
#define assert_ptr_equal(a, b) \
	((const void *)(a) == (const void *)(b) ? (void)0 : cmshim_fail(__FILE__, __LINE__, "%s != %s", #a, #b))
#define assert_int_equal(a, b)                                                                                \
	do {                                                                                                  \
		intmax_t cm_a_ = (intmax_t)(a), cm_b_ = (intmax_t)(b);                                        \
		if (cm_a_ != cm_b_)                                                                           \
			cmshim_fail(__FILE__, __LINE__, "%jd != %jd (%s != %s)", cm_a_, cm_b_, #a, #b);       \
	} while (0)
#define assert_int_not_equal(a, b)                                                                            \
	do {                                                                                                  \
		if ((intmax_t)(a) == (intmax_t)(b))                                                           \
			cmshim_fail(__FILE__, __LINE__, "%s == %s", #a, #b);                                  \
	} while (0)
#define assert_string_equal(a, b)                                                                             \
	(strcmp((a), (b)) == 0 ? (void)0 : cmshim_fail(__FILE__, __LINE__, "\"%s\" != \"%s\"", (a), (b)))
#define assert_string_not_equal(a, b)                                                                         \
	(strcmp((a), (b)) != 0 ? (void)0 : cmshim_fail(__FILE__, __LINE__, "\"%s\" == \"%s\"", (a), (b)))
#define assert_memory_equal(a, b, n) \
	(memcmp((a), (b), (n)) == 0 ? (void)0 : cmshim_fail(__FILE__, __LINE__, "memory differs: %s %s", #a, #b))
#define assert_in_range(v, lo, hi)                                                                            \
	do {                                                                                                  \
		intmax_t cm_v_ = (intmax_t)(v);                                                               \
		if (cm_v_ < (intmax_t)(lo) || cm_v_ > (intmax_t)(hi))                                         \
			cmshim_fail(__FILE__, __LINE__, "%jd not in [%s, %s]", cm_v_, #lo, #hi);              \
	} while (0)

#ifdef __cplusplus
}
#endif
