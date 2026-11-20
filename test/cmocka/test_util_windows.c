#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <stdio.h>
#include <string.h>

#include <obs.h>
#include <util/dstr.h>
#include <util/windows/device-enum.h>
#include <util/windows/win-version.h>
#include <util/windows/window-helpers.h>

/* util/windows/: window-helpers.c (window capture's window list and search), device-enum.c and win-version.c */

#define TEST_CLASS "KnurbsWindowHelpersTest"
#define TEST_TITLE "Knurbs test: #window"
#define TEST_EXE "test_util_windows.exe"
/* title:class:exe with '#' and ':' encoded */
#define TEST_ENCODED "Knurbs test#3A #22window:" TEST_CLASS ":" TEST_EXE

static HWND test_window;
static PROCESS_INFORMATION helper;
static HANDLE helper_ready, helper_quit;

/* the window helpers skip windows of their own process, so the test window lives in a child process: this
 * executable started with --window. It shows the window and pumps messages until told to quit. */
static int window_process(void)
{
	HANDLE ready = OpenEventA(EVENT_MODIFY_STATE, FALSE, "Local\\KnurbsWindowHelpersReady");
	HANDLE quit = OpenEventA(SYNCHRONIZE, FALSE, "Local\\KnurbsWindowHelpersQuit");
	if (!ready || !quit)
		return 1;

	WNDCLASSA wc = {0};
	wc.lpfnWndProc = DefWindowProcA;
	wc.hInstance = GetModuleHandle(NULL);
	wc.lpszClassName = TEST_CLASS;
	if (!RegisterClassA(&wc))
		return 1;

	/* visible, but placed off-screen and never activated */
	HWND window = CreateWindowExA(WS_EX_NOACTIVATE, TEST_CLASS, TEST_TITLE, WS_OVERLAPPEDWINDOW, -3000, -3000,
				      200, 100, NULL, NULL, wc.hInstance, NULL);
	if (!window)
		return 1;
	ShowWindow(window, SW_SHOWNOACTIVATE);
	SetEvent(ready);

	while (MsgWaitForMultipleObjects(1, &quit, FALSE, 30000, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
		MSG msg;
		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
	}

	DestroyWindow(window);
	return 0;
}

static int window_setup(void **state)
{
	helper_ready = CreateEventA(NULL, TRUE, FALSE, "Local\\KnurbsWindowHelpersReady");
	helper_quit = CreateEventA(NULL, TRUE, FALSE, "Local\\KnurbsWindowHelpersQuit");
	if (!helper_ready || !helper_quit)
		return -1;

	char path[MAX_PATH];
	char cmd[MAX_PATH + 16];
	GetModuleFileNameA(NULL, path, MAX_PATH);
	snprintf(cmd, sizeof(cmd), "\"%s\" --window", path);
	STARTUPINFOA si = {sizeof(si)};
	if (!CreateProcessA(path, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &helper))
		return -1;
	if (WaitForSingleObject(helper_ready, 10000) != WAIT_OBJECT_0)
		return -1;

	test_window = FindWindowA(TEST_CLASS, NULL);
	if (!test_window)
		return -1;
	return obs_test_setup(state);
}

static int window_teardown(void **state)
{
	SetEvent(helper_quit);
	WaitForSingleObject(helper.hProcess, 10000);
	CloseHandle(helper.hProcess);
	CloseHandle(helper.hThread);
	CloseHandle(helper_ready);
	CloseHandle(helper_quit);
	return obs_test_teardown(state);
}

/* minimizes or restores the helper's window and waits until it is in that state */
static void show_test_window(int cmd)
{
	bool minimize = cmd == SW_SHOWMINNOACTIVE;
	ShowWindowAsync(test_window, cmd);
	for (int i = 0; i < 1000 && !IsIconic(test_window) != !minimize; i++)
		Sleep(2);
	assert_true(!IsIconic(test_window) == !minimize);
}

static void window_info_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct dstr str = {0};
	assert_true(ms_get_window_exe(&str, test_window));
	assert_string_equal(str.array, TEST_EXE);
	dstr_free(&str);
	ms_get_window_title(&str, test_window);
	assert_string_equal(str.array, TEST_TITLE);
	dstr_free(&str);
	ms_get_window_class(&str, test_window);
	assert_string_equal(str.array, TEST_CLASS);
	dstr_free(&str);

	assert_false(ms_is_uwp_window(test_window));
	assert_null(ms_get_uwp_actual_window(test_window));

	/* windows of the calling process are skipped */
	HWND own = CreateWindowExA(0, "STATIC", "own", 0, 0, 0, 1, 1, NULL, NULL, GetModuleHandle(NULL), NULL);
	assert_non_null(own);
	assert_false(ms_get_window_exe(&str, own));
	DestroyWindow(own);

	/* a window that is gone has an unknown executable */
	HWND gone = own;
	assert_true(ms_get_window_exe(&str, gone));
	assert_string_equal(str.array, "unknown");
	dstr_free(&str);
	ms_get_window_title(&str, gone);
	assert_true(dstr_is_empty(&str));
	ms_get_window_class(&str, gone);
	assert_true(dstr_is_empty(&str));
	assert_false(ms_is_uwp_window(gone));
	dstr_free(&str);
}

static void window_strings_test(void **state)
{
	UNUSED_PARAMETER(state);

	char *class, *title, *exe;
	ms_build_window_strings(TEST_ENCODED, &class, &title, &exe);
	assert_string_equal(class, TEST_CLASS);
	assert_string_equal(title, TEST_TITLE);
	assert_string_equal(exe, TEST_EXE);
	bfree(class);
	bfree(title);
	bfree(exe);

	/* fewer than three parts, or nothing */
	ms_build_window_strings("title:class", &class, &title, &exe);
	assert_null(class);
	assert_null(title);
	assert_null(exe);
	ms_build_window_strings(NULL, &class, &title, &exe);
	assert_null(class);
}

static const char *rejected_title;

static bool reject_test_window(const char *title, const char *window_class, const char *exe)
{
	UNUSED_PARAMETER(window_class);
	UNUSED_PARAMETER(exe);
	return strcmp(title, rejected_title) != 0;
}

static bool list_has(obs_property_t *p, const char *value, size_t *idx)
{
	for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
		if (strcmp(obs_property_list_item_string(p, i), value) == 0) {
			if (idx)
				*idx = i;
			return true;
		}
	}
	return false;
}

static void window_list_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "window", "Window", OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);

	ms_fill_window_list(p, INCLUDE_MINIMIZED, NULL);
	size_t idx = 0;
	assert_true(list_has(p, TEST_ENCODED, &idx));
	assert_string_equal(obs_property_list_item_name(p, idx), "[" TEST_EXE "]: " TEST_TITLE);

	/* the callback can drop windows */
	obs_property_list_clear(p);
	rejected_title = TEST_TITLE;
	ms_fill_window_list(p, EXCLUDE_MINIMIZED, reject_test_window);
	assert_false(list_has(p, TEST_ENCODED, NULL));

	/* minimized windows are only listed with INCLUDE_MINIMIZED */
	show_test_window(SW_SHOWMINNOACTIVE);
	obs_property_list_clear(p);
	ms_fill_window_list(p, EXCLUDE_MINIMIZED, NULL);
	assert_false(list_has(p, TEST_ENCODED, NULL));
	obs_property_list_clear(p);
	ms_fill_window_list(p, INCLUDE_MINIMIZED, NULL);
	assert_true(list_has(p, TEST_ENCODED, NULL));
	show_test_window(SW_SHOWNOACTIVATE);

	/* a saved window that is not in the list is kept as a disabled entry */
	obs_data_t *settings = obs_data_create();
	assert_false(ms_check_window_property_setting(props, p, settings, "window", 0));
	obs_data_set_string(settings, "window", TEST_ENCODED);
	assert_false(ms_check_window_property_setting(props, p, settings, "window", 0));
	obs_data_set_string(settings, "window", "Old#3A title:OldClass:old.exe");
	assert_true(ms_check_window_property_setting(props, p, settings, "window", 0));
	assert_string_equal(obs_property_list_item_string(p, 0), "Old#3A title:OldClass:old.exe");
	assert_string_equal(obs_property_list_item_name(p, 0), "[old.exe]: Old: title");
	assert_true(obs_property_list_item_disabled(p, 0));
	obs_data_release(settings);

	obs_properties_destroy(props);
}

static void find_window_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_ptr_equal(ms_find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_CLASS, TEST_CLASS, TEST_TITLE, TEST_EXE),
			 test_window);
	assert_ptr_equal(ms_find_window(EXCLUDE_MINIMIZED, WINDOW_PRIORITY_TITLE, "other", TEST_TITLE, "other.exe"),
			 test_window);
	assert_ptr_equal(ms_find_window(EXCLUDE_MINIMIZED, WINDOW_PRIORITY_EXE, "other", TEST_TITLE, TEST_EXE),
			 test_window);
	/* a class match with another exe is still the best match */
	assert_ptr_equal(ms_find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_CLASS, TEST_CLASS, TEST_TITLE, "other.exe"),
			 test_window);
	/* generic classes are matched by title, UWP classes always by title */
	assert_ptr_equal(ms_find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_CLASS, "Chrome_WidgetWin_1", TEST_TITLE,
					TEST_EXE),
			 test_window);
	assert_ptr_equal(ms_find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_CLASS, "Windows.UI.Core.CoreWindow",
					TEST_TITLE, TEST_EXE),
			 test_window);
	assert_ptr_equal(ms_find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_EXE, "WinUIDesktopWin32WindowClass",
					TEST_TITLE, TEST_EXE),
			 test_window);
	assert_null(ms_find_window(INCLUDE_MINIMIZED, WINDOW_PRIORITY_CLASS, NULL, TEST_TITLE, TEST_EXE));

	assert_ptr_equal(
		ms_find_window_top_level(INCLUDE_MINIMIZED, WINDOW_PRIORITY_CLASS, TEST_CLASS, TEST_TITLE, TEST_EXE),
		test_window);
	assert_ptr_equal(ms_find_window_top_level(EXCLUDE_MINIMIZED, WINDOW_PRIORITY_TITLE, "Chrome", TEST_TITLE,
						  "other.exe"),
			 test_window);
	assert_ptr_equal(ms_find_window_top_level(INCLUDE_MINIMIZED, WINDOW_PRIORITY_CLASS,
						  "Windows.UI.Core.CoreWindow", TEST_TITLE, TEST_EXE),
			 test_window);
	assert_null(ms_find_window_top_level(INCLUDE_MINIMIZED, WINDOW_PRIORITY_CLASS, NULL, TEST_TITLE, TEST_EXE));

	/* a minimized window is skipped with EXCLUDE_MINIMIZED */
	show_test_window(SW_SHOWMINNOACTIVE);
	assert_ptr_not_equal(
		ms_find_window(EXCLUDE_MINIMIZED, WINDOW_PRIORITY_CLASS, TEST_CLASS, TEST_TITLE, TEST_EXE),
		test_window);
	assert_ptr_not_equal(
		ms_find_window_top_level(EXCLUDE_MINIMIZED, WINDOW_PRIORITY_CLASS, TEST_CLASS, TEST_TITLE, TEST_EXE),
		test_window);
	show_test_window(SW_SHOWNOACTIVATE);
}

struct luids {
	int count;
	int stop_after;
	uint32_t last_idx;
};

static bool count_luid(void *param, uint32_t idx, uint64_t luid)
{
	UNUSED_PARAMETER(luid);
	struct luids *l = param;
	l->count++;
	l->last_idx = idx;
	return l->count != l->stop_after;
}

static void device_enum_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* every machine has at least the basic render driver */
	struct luids all = {0};
	enum_graphics_device_luids(count_luid, &all);
	assert_true(all.count >= 1);
	assert_int_equal(all.last_idx, (uint32_t)all.count - 1);

	/* the callback stops the enumeration */
	struct luids one = {.stop_after = 1};
	enum_graphics_device_luids(count_luid, &one);
	assert_int_equal(one.count, 1);
}

static void win_version_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct win_version_info ver;
	get_win_ver(&ver);
	assert_true(ver.major >= 10);
	assert_true(ver.build > 0);
	uint32_t ver_int = get_win_ver_int();
	assert_int_equal(ver_int >> 8, (uint32_t)ver.major);
	assert_int_equal(ver_int & 0xFF, (uint32_t)ver.minor);

	struct win_version_info dll;
	assert_true(get_dll_ver(L"kernel32.dll", &dll));
	assert_true(dll.major >= 10);
	assert_false(get_dll_ver(L"knurbs-no-such.dll", &dll));

	assert_true(is_64_bit_windows());
	is_arm64_windows();

	struct win_version_info a = {10, 0, 19041, 1}, b = a;
	assert_int_equal(win_version_compare(&a, &b), 0);
	b.revis = 5; /* the revision does not count */
	assert_int_equal(win_version_compare(&a, &b), 0);
	b.build = 22000;
	assert_int_equal(win_version_compare(&a, &b), -1);
	assert_int_equal(win_version_compare(&b, &a), 1);
	b = a;
	b.minor = 1;
	assert_int_equal(win_version_compare(&a, &b), -1);
	assert_int_equal(win_version_compare(&b, &a), 1);
	b = a;
	b.major = 11;
	assert_int_equal(win_version_compare(&a, &b), -1);
	assert_int_equal(win_version_compare(&b, &a), 1);
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--window") == 0)
		return window_process();

	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(window_info_test, window_setup, window_teardown),
		cmocka_unit_test_setup_teardown(window_strings_test, window_setup, window_teardown),
		cmocka_unit_test_setup_teardown(window_list_test, window_setup, window_teardown),
		cmocka_unit_test_setup_teardown(find_window_test, window_setup, window_teardown),
		obs_unit_test(device_enum_test),
		obs_unit_test(win_version_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
