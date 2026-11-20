#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include <util/base.h>
#include <util/bmem.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void alloc_test(void **state)
{
	UNUSED_PARAMETER(state);

	long base = bnum_allocs();

	uint8_t *mem = bmalloc(16);
	assert_non_null(mem);
	assert_int_equal(((uintptr_t)mem) % base_get_alignment(), 0);
	assert_int_equal(bnum_allocs(), base + 1);

	memset(mem, 0xab, 16);
	mem = brealloc(mem, 64);
	assert_int_equal(mem[15], 0xab);
	assert_int_equal(bnum_allocs(), base + 1);

	/* brealloc(NULL) is an allocation */
	void *fresh = brealloc(NULL, 8);
	assert_int_equal(bnum_allocs(), base + 2);

	uint8_t *zero = bzalloc(32);
	for (size_t i = 0; i < 32; i++)
		assert_int_equal(zero[i], 0);

	char *dup = bmemdup("abc", 4);
	assert_string_equal(dup, "abc");

	bfree(mem);
	bfree(fresh);
	bfree(zero);
	bfree(dup);
	bfree(NULL);
	assert_int_equal(bnum_allocs(), base);
}

static void strdup_test(void **state)
{
	UNUSED_PARAMETER(state);

	char *s = bstrdup("text");
	assert_string_equal(s, "text");
	bfree(s);
	assert_null(bstrdup(NULL));

	s = bstrdup_n("textual", 4);
	assert_string_equal(s, "text");
	bfree(s);
	assert_null(bstrdup_n(NULL, 3));

	wchar_t *w = bwstrdup(L"wide");
	assert_true(wcscmp(w, L"wide") == 0);
	bfree(w);
	assert_null(bwstrdup(NULL));

	w = bwstrdup_n(L"wider", 4);
	assert_true(wcscmp(w, L"wide") == 0);
	bfree(w);
	assert_null(bwstrdup_n(NULL, 3));
}

#ifdef _WIN32
/* bmalloc(0) hits os_breakpoint() before bcrash(); step over the int3 so the crash handler runs. */
static LONG CALLBACK skip_breakpoint(EXCEPTION_POINTERS *info)
{
	if (info->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT)
		return EXCEPTION_CONTINUE_SEARCH;
#ifdef _M_X64
	info->ContextRecord->Rip++;
#endif
	return EXCEPTION_CONTINUE_EXECUTION;
}

static jmp_buf crash_jump;
static char crash_msg[256];

static void crash_handler(const char *format, va_list args, void *param)
{
	UNUSED_PARAMETER(param);
	vsnprintf(crash_msg, sizeof(crash_msg), format, args);
	longjmp(crash_jump, 1);
}

/* One crash per process: bcrash() refuses to run twice. */
static void zero_alloc_crashes_test(void **state)
{
	UNUSED_PARAMETER(state);

	void *veh = AddVectoredExceptionHandler(1, skip_breakpoint);
	base_set_crash_handler(crash_handler, NULL);

	if (setjmp(crash_jump) == 0) {
		bmalloc(0);
		fail_msg("bmalloc(0) returned");
	}
	assert_non_null(strstr(crash_msg, "Allocating 0 bytes"));

	RemoveVectoredExceptionHandler(veh);
}
#endif

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(alloc_test),
		cmocka_unit_test(strdup_test),
#ifdef _WIN32
		cmocka_unit_test(zero_alloc_crashes_test),
#endif
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
