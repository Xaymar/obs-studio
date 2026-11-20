#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <wchar.h>

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/lexer.h>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static void check_heap(void)
{
#if defined(_MSC_VER) && defined(_DEBUG)
	assert_true(_CrtCheckMemory());
#endif
}

static void compare_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_int_equal(astrcmpi("abc", "ABC"), 0);
	assert_int_equal(astrcmpi("abc", "abd"), -1);
	assert_int_equal(astrcmpi("abd", "ABC"), 1);
	assert_int_equal(astrcmpi(NULL, ""), 0);
	assert_int_equal(astrcmpi("", NULL), 0);
	assert_int_equal(astrcmpi("a", NULL), 1);

	assert_int_equal(wstrcmpi(L"abc", L"ABC"), 0);
	assert_int_equal(wstrcmpi(L"abc", L"abd"), -1);
	assert_int_equal(wstrcmpi(L"abd", L"ABC"), 1);
	assert_int_equal(wstrcmpi(NULL, NULL), 0);

	assert_int_equal(astrcmp_n("abcx", "abcy", 3), 0);
	assert_int_equal(astrcmp_n("abcx", "abcy", 4), -1);
	assert_int_equal(astrcmp_n("b", "a", 1), 1);
	assert_int_equal(astrcmp_n("a", "b", 0), 0);
	assert_int_equal(astrcmp_n(NULL, NULL, 2), 0);
	assert_int_equal(astrcmp_n("ab", "ab", 5), 0);

	assert_int_equal(wstrcmp_n(L"abcx", L"abcy", 3), 0);
	assert_int_equal(wstrcmp_n(L"abcx", L"abcy", 4), -1);
	assert_int_equal(wstrcmp_n(L"b", L"a", 1), 1);
	assert_int_equal(wstrcmp_n(L"a", L"b", 0), 0);
	assert_int_equal(wstrcmp_n(NULL, NULL, 2), 0);

	assert_int_equal(astrcmpi_n("ABCx", "abcy", 3), 0);
	assert_int_equal(astrcmpi_n("abcx", "ABCY", 4), -1);
	assert_int_equal(astrcmpi_n("B", "a", 1), 1);
	assert_int_equal(astrcmpi_n("a", "b", 0), 0);
	assert_int_equal(astrcmpi_n(NULL, NULL, 1), 0);

	assert_int_equal(wstrcmpi_n(L"ABCx", L"abcy", 3), 0);
	assert_int_equal(wstrcmpi_n(L"abcx", L"ABCY", 4), -1);
	assert_int_equal(wstrcmpi_n(L"B", L"a", 1), 1);
	assert_int_equal(wstrcmpi_n(L"a", L"b", 0), 0);
	assert_int_equal(wstrcmpi_n(NULL, NULL, 1), 0);
}

static void search_test(void **state)
{
	UNUSED_PARAMETER(state);

	const char *hay = "Hello World";
	assert_ptr_equal(astrstri(hay, "WORLD"), hay + 6);
	assert_null(astrstri(hay, "planet"));
	assert_null(astrstri(NULL, "a"));
	assert_null(astrstri(hay, NULL));

	const wchar_t *whay = L"Hello World";
	assert_ptr_equal(wstrstri(whay, L"world"), whay + 6);
	assert_null(wstrstri(whay, L"planet"));
	assert_null(wstrstri(NULL, L"a"));
	assert_null(wstrstri(whay, NULL));
}

static void depad_test(void **state)
{
	UNUSED_PARAMETER(state);

	char a[] = " \t\r\nvalue \n";
	assert_string_equal(strdepad(a), "value");
	char b[] = "value";
	assert_string_equal(strdepad(b), "value");
	char c[] = "   ";
	assert_string_equal(strdepad(c), "");
	char d[] = "";
	assert_string_equal(strdepad(d), "");
	assert_null(strdepad(NULL));

	wchar_t wa[] = L" \tvalue \r\n";
	assert_true(wcscmp(wcsdepad(wa), L"value") == 0);
	wchar_t wb[] = L"value";
	assert_true(wcscmp(wcsdepad(wb), L"value") == 0);
	wchar_t wc[] = L"  ";
	assert_true(wcscmp(wcsdepad(wc), L"") == 0);
	wchar_t wd[] = L"";
	assert_true(wcscmp(wcsdepad(wd), L"") == 0);
	assert_null(wcsdepad(NULL));
}

static void strlist_test(void **state)
{
	UNUSED_PARAMETER(state);

	char **list = strlist_split("a,,b,c", ',', false);
	assert_string_equal(list[0], "a");
	assert_string_equal(list[1], "b");
	assert_string_equal(list[2], "c");
	assert_null(list[3]);
	strlist_free(list);

	list = strlist_split("a,,b,", ',', true);
	assert_string_equal(list[0], "a");
	assert_string_equal(list[1], "");
	assert_string_equal(list[2], "b");
	assert_string_equal(list[3], "");
	assert_null(list[4]);
	strlist_free(list);

	list = strlist_split("a,", ',', false);
	assert_string_equal(list[0], "a");
	assert_null(list[1]);
	strlist_free(list);

	assert_null(strlist_split(NULL, ',', true));
}

static void copy_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr str = {0};
	struct dstr other = {0};

	dstr_copy(&str, "hello");
	assert_string_equal(str.array, "hello");
	assert_int_equal(str.len, 5);
	dstr_copy(&str, "");
	assert_null(str.array);
	dstr_copy(&str, "x");
	dstr_copy(&str, NULL);
	assert_null(str.array);

	struct strref ref = {"hello world", 5};
	dstr_init_copy_strref(&str, &ref);
	assert_string_equal(str.array, "hello");
	dstr_copy_strref(&str, &ref);
	assert_string_equal(str.array, "hello");

	dstr_ncopy(&str, "abcdef", 3);
	assert_string_equal(str.array, "abc");
	dstr_ncopy(&str, "abc", 0);
	assert_null(str.array);

	dstr_init_copy(&other, "abcdef");
	dstr_ncopy_dstr(&str, &other, 4);
	assert_string_equal(str.array, "abcd");
	dstr_ncopy_dstr(&str, &other, 100);
	assert_string_equal(str.array, "abcdef");
	dstr_ncopy_dstr(&str, &other, 0);
	assert_null(str.array);

	dstr_copy_dstr(&str, &other);
	assert_string_equal(str.array, "abcdef");
	struct dstr empty = {0};
	dstr_copy_dstr(&str, &empty);
	assert_null(str.array);

	dstr_init_copy_dstr(&str, &other);
	assert_string_equal(str.array, "abcdef");
	dstr_free(&str);
	dstr_init_copy_dstr(&str, &empty);
	assert_null(str.array);

	struct dstr moved;
	dstr_init_move(&moved, &other);
	assert_null(other.array);
	assert_string_equal(moved.array, "abcdef");
	dstr_move(&str, &moved);
	assert_string_equal(str.array, "abcdef");
	assert_null(moved.array);

	dstr_move_array(&str, bstrdup("owned"));
	assert_string_equal(str.array, "owned");
	assert_int_equal(str.len, 5);
	dstr_free(&str);
	dstr_init_move_array(&str, bstrdup("owned2"));
	assert_string_equal(str.array, "owned2");
	dstr_free(&str);

	struct dstr arr[2];
	dstr_init_copy(&arr[0], "a");
	dstr_init_copy(&arr[1], "b");
	dstr_array_free(arr, 2);
	assert_null(arr[0].array);
	assert_null(arr[1].array);
}

static void cat_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr str = {0};
	struct dstr other = {0};

	dstr_cat(&str, "ab");
	dstr_cat(&str, "");
	dstr_cat(&str, NULL);
	dstr_cat_ch(&str, 'c');
	assert_string_equal(str.array, "abc");

	dstr_init_copy(&other, "def");
	dstr_cat_dstr(&str, &other);
	struct dstr empty = {0};
	dstr_cat_dstr(&str, &empty);
	assert_string_equal(str.array, "abcdef");

	struct strref ref = {"ghijk", 2};
	dstr_cat_strref(&str, &ref);
	assert_string_equal(str.array, "abcdefgh");

	dstr_ncat(&str, "ijkl", 2);
	dstr_ncat(&str, "zz", 0);
	dstr_ncat(&str, "", 3);
	dstr_ncat(&str, NULL, 3);
	assert_string_equal(str.array, "abcdefghij");

	dstr_ncat_dstr(&str, &other, 1);
	dstr_ncat_dstr(&str, &other, 100);
	dstr_ncat_dstr(&str, &other, 0);
	dstr_ncat_dstr(&str, &empty, 1);
	assert_string_equal(str.array, "abcdefghijddef");
	assert_int_equal(str.len, 14);

	dstr_free(&str);
	dstr_free(&other);
}

static void insert_remove_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr str = {0};
	struct dstr other = {0};

	dstr_copy(&str, "ace");
	dstr_insert(&str, 1, "b");
	dstr_insert(&str, str.len, "f");
	dstr_insert(&str, 0, "");
	dstr_insert(&str, 0, NULL);
	assert_string_equal(str.array, "abcef");

	dstr_init_copy(&other, "d");
	dstr_insert_dstr(&str, 3, &other);
	dstr_insert_dstr(&str, str.len, &other);
	struct dstr empty = {0};
	dstr_insert_dstr(&str, 0, &empty);
	assert_string_equal(str.array, "abcdefd");

	dstr_insert_ch(&str, 0, '>');
	dstr_insert_ch(&str, str.len, '<');
	assert_string_equal(str.array, ">abcdefd<");

	dstr_remove(&str, 0, 0);
	dstr_remove(&str, 0, 1);
	dstr_remove(&str, str.len - 2, 2);
	assert_string_equal(str.array, "abcdef");
	dstr_remove(&str, 1, 2);
	assert_string_equal(str.array, "adef");
	assert_int_equal(str.len, 4);
	dstr_remove(&str, 0, str.len);
	assert_null(str.array);

	dstr_free(&other);
	check_heap();
}

/* Exercises the case where dstr_insert_ch() used to write one byte past the buffer (capacity exactly len + 2). The
 * overrun lands in _aligned_malloc's slack, so only a sanitizer build sees it fail without the fix. */
static void insert_ch_capacity_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr str = {0};
	dstr_copy(&str, "abc");
	dstr_reserve(&str, 5);
	assert_int_equal(str.capacity, 5);

	dstr_insert_ch(&str, 0, 'x');
	assert_string_equal(str.array, "xabc");
	assert_int_equal(str.len, 4);
	check_heap();

	dstr_free(&str);
}

static void printf_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr str = {0};

	dstr_printf(&str, "%d-%s", 42, "x");
	assert_string_equal(str.array, "42-x");
	dstr_catf(&str, "+%c", 'y');
	assert_string_equal(str.array, "42-x+y");
	assert_int_equal(str.len, 6);

	dstr_printf(&str, "%s", "");
	assert_null(str.array);
	dstr_catf(&str, "%s", "");
	assert_null(str.array);

	dstr_safe_printf(&str, "$1 $2 $3 $4", "a", "b", "c", "d");
	assert_string_equal(str.array, "a b c d");
	dstr_safe_printf(&str, "$1 $2", NULL, NULL, NULL, NULL);
	assert_string_equal(str.array, "$1 $2");

	dstr_free(&str);
}

static void replace_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr str = {0};

	/* shorter replacement, including a match at the very end */
	dstr_copy(&str, "aXXbXX");
	dstr_replace(&str, "XX", "y");
	assert_string_equal(str.array, "ayby");
	assert_int_equal(str.len, 4);

	/* empty replacement */
	dstr_copy(&str, "aXXbXXc");
	dstr_replace(&str, "XX", NULL);
	assert_string_equal(str.array, "abc");
	assert_int_equal(str.len, 3);

	/* shorter replacement without matches */
	dstr_copy(&str, "abc");
	dstr_replace(&str, "XX", "y");
	assert_string_equal(str.array, "abc");

	/* longer replacement */
	dstr_copy(&str, "aXbX");
	dstr_replace(&str, "X", "123");
	assert_string_equal(str.array, "a123b123");
	assert_int_equal(str.len, 8);

	/* longer replacement without matches */
	dstr_replace(&str, "Q", "123");
	assert_string_equal(str.array, "a123b123");

	/* same length */
	dstr_replace(&str, "123", "456");
	assert_string_equal(str.array, "a456b456");

	dstr_free(&str);
	dstr_replace(&str, "a", "b");
	assert_null(str.array);
	check_heap();
}

static void substr_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr str = {0};
	struct dstr out = {0};

	dstr_copy(&str, "  padded  ");
	dstr_depad(&str);
	assert_string_equal(str.array, "padded");
	assert_int_equal(str.len, 6);

	struct dstr blank = {0};
	dstr_copy(&blank, "   ");
	dstr_depad(&blank);
	assert_null(blank.array);
	dstr_depad(&blank);

	dstr_left(&out, &str, 3);
	assert_string_equal(out.array, "pad");
	dstr_mid(&out, &str, 1, 3);
	assert_string_equal(out.array, "add");
	dstr_right(&out, &str, 3);
	assert_string_equal(out.array, "ded");

	dstr_left(&str, &str, 2);
	assert_string_equal(str.array, "pa");

	dstr_free(&str);
	dstr_free(&out);
}

static void query_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr str = {0};
	assert_true(dstr_is_empty(&str));
	assert_int_equal(dstr_end(&str), 0);
	assert_null(dstr_find(&str, "a"));
	assert_null(dstr_find_i(&str, "a"));

	dstr_copy(&str, "Hello");
	assert_false(dstr_is_empty(&str));
	assert_int_equal(dstr_end(&str), 'o');
	assert_ptr_equal(dstr_find(&str, "ll"), str.array + 2);
	assert_ptr_equal(dstr_find_i(&str, "LL"), str.array + 2);
	assert_int_equal(dstr_cmp(&str, "Hello"), 0);
	assert_int_equal(dstr_cmpi(&str, "hello"), 0);
	assert_int_equal(dstr_ncmp(&str, "Help", 3), 0);
	assert_int_equal(dstr_ncmpi(&str, "hELP", 3), 0);

	dstr_resize(&str, 2);
	assert_string_equal(str.array, "He");
	dstr_resize(&str, 0);
	assert_null(str.array);

	dstr_reserve(&str, 0);
	dstr_reserve(&str, 16);
	assert_int_equal(str.capacity, 16);
	dstr_free(&str);
}

static void convert_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr str = {0};

	dstr_from_wcs(&str, L"Grüße");
	assert_string_equal(str.array, "Gr\xc3\xbc\xc3\x9f"
				       "e");
	wchar_t *wide = dstr_to_wcs(&str);
	assert_true(wcscmp(wide, L"Grüße") == 0);
	bfree(wide);

	dstr_from_wcs(&str, L"");
	assert_null(str.array);

	dstr_from_mbs(&str, "plain");
	assert_string_equal(str.array, "plain");
	char *mbs = dstr_to_mbs(&str);
	assert_string_equal(mbs, "plain");
	bfree(mbs);

	/* towupper()/towlower() follow the C locale: ASCII changes case, the rest round-trips through UTF-16 */
	dstr_copy(&str, "MiXeD \xc3\xa4");
	dstr_to_upper(&str);
	assert_string_equal(str.array, "MIXED \xc3\xa4");
	dstr_to_lower(&str);
	assert_string_equal(str.array, "mixed \xc3\xa4");

	dstr_free(&str);
	dstr_to_upper(&str);
	dstr_to_lower(&str);
	assert_null(str.array);
}

/* Regression: dstr_ncopy() read len + 1 bytes, one past a string that ends at a guard page */
static void ncopy_guard_page_test(void **state)
{
	UNUSED_PARAMETER(state);

#ifdef _WIN32
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	size_t page = si.dwPageSize;
	char *pages = VirtualAlloc(NULL, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	assert_non_null(pages);
	DWORD old;
	assert_true(VirtualProtect(pages + page, page, PAGE_NOACCESS, &old));
#else
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	char *pages = mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert_true(pages != MAP_FAILED);
	assert_int_equal(mprotect(pages + page, page, PROT_NONE), 0);
#endif

	char *abc = pages + page - 3;
	memcpy(abc, "abc", 3);

	struct dstr str = {0};
	dstr_ncopy(&str, abc, 3);
	assert_string_equal(str.array, "abc");
	dstr_free(&str);

#ifdef _WIN32
	VirtualFree(pages, 0, MEM_RELEASE);
#else
	munmap(pages, page * 2);
#endif
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(compare_test),       cmocka_unit_test(search_test),
		cmocka_unit_test(depad_test),         cmocka_unit_test(strlist_test),
		cmocka_unit_test(copy_test),          cmocka_unit_test(cat_test),
		cmocka_unit_test(insert_remove_test), cmocka_unit_test(insert_ch_capacity_test),
		cmocka_unit_test(printf_test),        cmocka_unit_test(replace_test),
		cmocka_unit_test(substr_test),        cmocka_unit_test(query_test),
		cmocka_unit_test(convert_test),       cmocka_unit_test(ncopy_guard_page_test),
	};

	int ret = cmocka_run_group_tests(tests, NULL, NULL);
	return ret ? ret : (bnum_allocs() != 0);
}
