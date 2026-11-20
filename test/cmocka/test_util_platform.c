#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <sys/stat.h>
#include <math.h>

#include "obs-test.h"

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <wchar.h>

#include <util/dstr.h>
#include <obs.h>
#include <util/platform.h>

static void write_text(const char *path, const char *text)
{
	assert_true(os_quick_write_utf8_file(path, text, strlen(text), false));
}

static void file_io_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* UTF-8 round trip, with and without a BOM */
	const char *text = "Gr\xc3\xbc\xc3\x9f"
			   "e";
	assert_true(os_quick_write_utf8_file("utf8.txt", text, strlen(text), true));
	char *read = os_quick_read_utf8_file("utf8.txt");
	assert_string_equal(read, text);
	bfree(read);
	assert_int_equal(os_get_file_size("utf8.txt"), 3 + strlen(text));
	assert_int_equal(os_get_file_size("missing.txt"), -1);

	FILE *f = os_fopen("utf8.txt", "rb");
	assert_non_null(f);
	assert_int_equal(os_fgetsize(f), 3 + strlen(text));
	assert_int_equal(os_fseeki64(f, 3, SEEK_SET), 0);
	assert_int_equal(os_ftelli64(f), 3);
	rewind(f);
	char *utf8 = NULL;
	assert_int_equal(os_fread_utf8(f, &utf8), strlen(text));
	assert_string_equal(utf8, text);
	bfree(utf8);
	rewind(f);
	char *mbs = NULL;
	assert_true(os_fread_mbs(f, &mbs) > 0);
	bfree(mbs);
	fclose(f);

	/* safe write keeps a backup of the old content */
	assert_true(os_quick_write_utf8_file_safe("utf8.txt", "new", 3, false, "tmp", "bak"));
	assert_true(os_file_exists("utf8.txt.bak"));
	assert_true(os_quick_write_utf8_file_safe("utf8.txt", "newer", 5, false, ".tmp", NULL));
	assert_false(os_quick_write_utf8_file_safe("utf8.txt", "x", 1, false, NULL, NULL));
	assert_false(os_quick_write_utf8_file_safe("utf8.txt", "x", 1, false, "", NULL));
	read = os_quick_read_utf8_file("utf8.txt");
	assert_string_equal(read, "newer");
	bfree(read);

	/* multibyte helpers */
	assert_true(os_quick_write_mbs_file("mbs.txt", "plain", 5));
	read = os_quick_read_mbs_file("mbs.txt");
	assert_string_equal(read, "plain");
	bfree(read);

	/* empty and missing files */
	write_text("empty.txt", "");
	read = os_quick_read_utf8_file("empty.txt");
	assert_true(!read || !*read);
	bfree(read);
	assert_null(os_quick_read_utf8_file("missing.txt"));
	assert_null(os_quick_read_mbs_file("missing.txt"));
	assert_false(os_quick_write_utf8_file("no-such-dir/x.txt", "x", 1, false));
	assert_false(os_quick_write_mbs_file("no-such-dir/x.txt", "x", 1));

	wchar_t *wpath = NULL;
	os_utf8_to_wcs_ptr("mbs.txt", 0, &wpath);
	f = os_wfopen(wpath, "rb");
	assert_non_null(f);
	fclose(f);
	bfree(wpath);
	assert_null(os_fopen(NULL, "rb"));

	struct stat st;
	assert_int_equal(os_stat("mbs.txt", &st), 0);
	assert_int_equal(st.st_size, 5);
	assert_int_not_equal(os_stat("missing.txt", &st), 0);

	os_unlink("utf8.txt");
	os_unlink("utf8.txt.bak");
	os_unlink("mbs.txt");
	os_unlink("empty.txt");
}

static void conversion_test(void **state)
{
	UNUSED_PARAMETER(state);

	const char *utf8 = "\xe2\x82\xac uro";
	wchar_t wide[16];
	assert_int_equal(os_utf8_to_wcs(utf8, 0, wide, 16), 5);
	assert_true(wcscmp(wide, L"€ uro") == 0);
	assert_int_equal(os_utf8_to_wcs(utf8, 0, NULL, 0), 5);

	char back[16];
	assert_int_equal(os_wcs_to_utf8(wide, 0, back, 16), strlen(utf8));
	assert_string_equal(back, utf8);
	assert_int_equal(os_wcs_to_utf8(wide, 0, NULL, 0), strlen(utf8));

	wchar_t *wptr = NULL;
	char *ptr = NULL;
	assert_int_equal(os_utf8_to_wcs_ptr(utf8, 0, &wptr), 5);
	assert_int_equal(os_wcs_to_utf8_ptr(wptr, 0, &ptr), strlen(utf8));
	assert_string_equal(ptr, utf8);
	bfree(wptr);
	bfree(ptr);

	/* a BOM is skipped */
	assert_int_equal(os_utf8_to_wcs("\xEF\xBB\xBF"
					"ab",
					0, wide, 16),
			 2);

	/* NULL input */
	assert_int_equal(os_utf8_to_wcs_ptr(NULL, 0, &wptr), 0);
	assert_null(wptr);
	assert_int_equal(os_wcs_to_utf8_ptr(NULL, 0, &ptr), 0);
	assert_null(ptr);

	assert_int_equal(os_mbs_to_wcs("abc", 0, wide, 16), 3);
	assert_int_equal(os_wcs_to_mbs(L"abc", 0, back, 16), 3);
	assert_string_equal(back, "abc");
	assert_int_equal(os_mbs_to_wcs_ptr("abc", 0, &wptr), 3);
	assert_int_equal(os_wcs_to_mbs_ptr(wptr, 0, &ptr), 3);
	bfree(wptr);
	bfree(ptr);
	assert_int_equal(os_utf8_to_mbs_ptr("abc", 0, &ptr), 3);
	bfree(ptr);
	assert_int_equal(os_mbs_to_utf8_ptr("abc", 0, &ptr), 3);
	bfree(ptr);
	assert_int_equal(os_mbs_to_wcs_ptr(NULL, 0, &wptr), 0);
	assert_int_equal(os_wcs_to_mbs_ptr(NULL, 0, &ptr), 0);
	assert_int_equal(os_utf8_to_mbs_ptr(NULL, 0, &ptr), 0);
	assert_int_equal(os_mbs_to_utf8_ptr(NULL, 0, &ptr), 0);

	/* number formatting is locale independent */
	assert_true(os_strtod("1.5") == 1.5);
	assert_true(os_strtod("1e3") == 1000.0);
	char num[64];
	assert_true(os_dtostr(1.25, num, sizeof(num)) > 0);
	assert_string_equal(num, "1.25");
	os_dtostr(3.0, num, sizeof(num));
	assert_string_equal(num, "3.0"); /* always keeps a decimal point */
	os_dtostr(-0.5, num, sizeof(num));
	assert_string_equal(num, "-0.5");
}

static void path_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_string_equal(os_get_path_extension("dir.d/file.tar.gz"), ".gz");
	assert_null(os_get_path_extension("dir.d/file"));
	assert_null(os_get_path_extension("dir.d\\file"));
	assert_null(os_get_path_extension(NULL)); /* regression: crashed */

	char cwd[512];
	assert_non_null(os_getcwd(cwd, sizeof(cwd)));

	char abs[512];
	assert_true(os_get_abs_path(".", abs, sizeof(abs)) > 0);
	char *abs_ptr = os_get_abs_path_ptr(".");
	assert_non_null(abs_ptr);
	bfree(abs_ptr);

	char config[512];
	assert_true(os_get_config_path(config, sizeof(config), "obs-test") > 0);
	char *config_ptr = os_get_config_path_ptr("obs-test");
	assert_non_null(config_ptr);
	bfree(config_ptr);
	assert_true(os_get_program_data_path(config, sizeof(config), "obs-test") > 0);
	config_ptr = os_get_program_data_path_ptr("obs-test");
	assert_non_null(config_ptr);
	bfree(config_ptr);
	char *exe = os_get_executable_path_ptr("x");
	assert_non_null(exe);
	bfree(exe);

	/* leftovers from an interrupted run */
	const char *names[] = {"sub/one.txt",    "sub/two.log",    "sub/renamed.txt", "sub/copy.txt",
			       "sub/target.txt", "sub/backup.txt", "sub/fresh.txt",   "sub/from.txt"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		os_unlink(names[i]);
	os_rmdir("sub/a/b");
	os_rmdir("sub/a");
	os_rmdir("sub");

	assert_int_equal(os_mkdir("sub"), MKDIR_SUCCESS);
	assert_int_equal(os_mkdir("sub"), MKDIR_EXISTS);
	assert_int_equal(os_mkdirs("sub/a/b"), MKDIR_SUCCESS);
	assert_int_equal(os_mkdirs("sub/a/b"), MKDIR_EXISTS);
	assert_int_equal(os_chdir("sub"), 0);
	assert_int_equal(os_chdir(cwd), 0);

	write_text("sub/one.txt", "1");
	write_text("sub/two.log", "22");
	assert_int_equal(os_copyfile("sub/one.txt", "sub/copy.txt"), 0);
	assert_int_not_equal(os_copyfile("sub/one.txt", "sub/copy.txt"), 0); /* target exists */
	assert_int_not_equal(os_copyfile("sub/missing.txt", "sub/x.txt"), 0);
	assert_int_equal(os_rename("sub/copy.txt", "sub/renamed.txt"), 0);
	assert_int_not_equal(os_rename("sub/missing.txt", "sub/x.txt"), 0);

	/* directory listing */
	os_dir_t *dir = os_opendir("sub");
	assert_non_null(dir);
	int files = 0, dirs = 0;
	struct os_dirent *ent;
	while ((ent = os_readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;
		if (ent->directory)
			dirs++;
		else
			files++;
	}
	os_closedir(dir);
	assert_int_equal(files, 3);
	assert_int_equal(dirs, 1);
	assert_null(os_opendir("missing-dir"));
	assert_null(os_readdir(NULL));
	os_closedir(NULL);

	os_glob_t *glob;
	assert_int_equal(os_glob("sub/*.txt", 0, &glob), 0);
	assert_int_equal(glob->gl_pathc, 2);
	os_globfree(glob);
	assert_int_equal(os_glob("sub/*", 0, &glob), 0);
	bool found_dir = false;
	for (size_t i = 0; i < glob->gl_pathc; i++)
		found_dir |= glob->gl_pathv[i].directory;
	assert_true(found_dir);
	os_globfree(glob);
	assert_int_not_equal(os_glob("sub/*.none", 0, &glob), 0);
	os_globfree(NULL);

	/* safe replace with and without a backup */
	write_text("sub/target.txt", "old");
	write_text("sub/from.txt", "new");
	assert_int_equal(os_safe_replace("sub/target.txt", "sub/from.txt", "sub/backup.txt"), 0);
	char *text = os_quick_read_utf8_file("sub/backup.txt");
	assert_string_equal(text, "old");
	bfree(text);
	write_text("sub/from.txt", "newer");
	assert_int_equal(os_safe_replace("sub/target.txt", "sub/from.txt", NULL), 0);
	write_text("sub/from.txt", "fresh");
	assert_int_equal(os_safe_replace("sub/fresh.txt", "sub/from.txt", NULL), 0);
	assert_int_not_equal(os_safe_replace("sub/target.txt", "sub/missing.txt", NULL), 0);

	assert_true(os_get_free_space("sub") > 0);
	assert_true(os_get_free_disk_space("sub") > 0);

	/* os_rmdir() below only succeeds if this removed every file */
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		os_unlink(names[i]);
	assert_int_not_equal(os_unlink("sub/missing.txt"), 0);
	assert_int_equal(os_rmdir("sub/a/b"), 0);
	assert_int_equal(os_rmdir("sub/a"), 0);
	assert_int_equal(os_rmdir("sub"), 0);
	assert_int_not_equal(os_rmdir("sub"), 0);
}

static void module_test(void **state)
{
	UNUSED_PARAMETER(state);

	void *kernel = os_dlopen("kernel32");
	assert_non_null(kernel);
	assert_non_null(os_dlsym(kernel, "GetTickCount"));
	assert_null(os_dlsym(kernel, "NoSuchFunction"));
	os_dlclose(kernel);

	assert_null(os_dlopen("no-such-module"));
	assert_null(os_dlopen("no-such-dir/no-such-module"));
	assert_null(os_dlopen(NULL));
	os_dlclose(NULL);

	assert_false(os_is_obs_plugin("no-such-plugin.dll"));
	char *exe = os_get_executable_path_ptr("test_util_platform.exe");
	assert_false(os_is_obs_plugin(exe));
	bfree(exe);
}

static void system_test(void **state)
{
	UNUSED_PARAMETER(state);

	uint64_t start = os_gettime_ns();
	os_sleep_ms(1);
	assert_true(os_gettime_ns() > start);
	assert_true(os_sleepto_ns(os_gettime_ns() + 1000000));
	assert_false(os_sleepto_ns(start));
	assert_true(os_sleepto_ns_fast(os_gettime_ns() + 1000000));
	assert_false(os_sleepto_ns_fast(start));

	struct timespec ts;
	assert_non_null(os_nstime_to_timespec(1500000000ULL, &ts));
	/* converts an os_gettime_ns() timestamp to wall-clock time */
	assert_true(ts.tv_nsec >= 0 && ts.tv_nsec < 1000000000);

	assert_true(os_get_physical_cores() > 0);
	assert_true(os_get_logical_cores() >= os_get_physical_cores());
	assert_true(os_get_sys_total_size() > 0);
	assert_true(os_get_sys_free_size() > 0);

	os_proc_memory_usage_t usage;
	assert_true(os_get_proc_memory_usage(&usage));
	assert_true(usage.resident_size > 0);
	assert_true(os_get_proc_resident_size() > 0);
	assert_true(os_get_proc_virtual_size() > 0);

	os_cpu_usage_info_t *cpu = os_cpu_usage_info_start();
	assert_non_null(cpu);
	/* Regression: queries within the same clock tick as start divided by zero (NaN) */
	for (int i = 0; i < 1000; i++) {
		double usage = os_cpu_usage_info_query(cpu);
		assert_false(isnan(usage));
		assert_true(usage >= 0.0);
	}
	os_sleep_ms(20);
	assert_true(os_cpu_usage_info_query(cpu) >= 0.0);
	os_cpu_usage_info_destroy(cpu);
	assert_true(os_cpu_usage_info_query(NULL) == 0.0);
	os_cpu_usage_info_destroy(NULL);

	os_performance_token_t *perf = os_request_high_performance("test");
	os_end_high_performance(perf);

	os_inhibit_t *inhibit = os_inhibit_sleep_create("test");
	assert_non_null(inhibit);
	/* returns whether the state changed */
	assert_true(os_inhibit_sleep_set_active(inhibit, true));
	assert_false(os_inhibit_sleep_set_active(inhibit, true));
	assert_true(os_inhibit_sleep_set_active(inhibit, false));
	assert_false(os_inhibit_sleep_set_active(inhibit, false));
	assert_true(os_inhibit_sleep_set_active(inhibit, true)); /* destroy deactivates */
	os_inhibit_sleep_destroy(inhibit);
	assert_false(os_inhibit_sleep_set_active(NULL, true));
	os_inhibit_sleep_destroy(NULL);

	os_get_emulation_status();

	char *uuid = os_generate_uuid();
	assert_int_equal(strlen(uuid), UUID_STR_LENGTH);
	char *uuid2 = os_generate_uuid();
	assert_string_not_equal(uuid, uuid2);
	bfree(uuid);
	bfree(uuid2);
}

/* Needs a started (but video-less) libobs: it reads the video info for %FPS, %CRES, %ORES and %VF */
static void formatted_filename_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* Regression: obs_get_video_info(), and so this function, crashed before obs_startup() */
	struct obs_video_info ovi;
	assert_false(obs_get_video_info(&ovi));
	char *early = os_generate_formatted_filename("mkv", true, "%VF");
	assert_string_equal(early, "None.mkv");
	bfree(early);

	assert_true(obs_startup("en-US", NULL, NULL));

	char *name = os_generate_formatted_filename("mkv", true, "%CCYY-%MM-%DD %hh-%mm-%ss %% %a %b");
	assert_non_null(name);
	assert_non_null(strstr(name, ".mkv"));
	assert_null(strchr(name, '%') == name ? NULL : strstr(name, "%CCYY"));
	bfree(name);

	/* without spaces, spaces become underscores; %s is the Unix time; unknown specifiers are dropped */
	name = os_generate_formatted_filename("txt", false, "a b %s %q");
	assert_non_null(name);
	assert_null(strchr(name, ' '));
	assert_null(strchr(name, '%'));
	assert_true(strncmp(name, "a_b_", 4) == 0);
	bfree(name);

	/* Regression: without video the info used to be uninitialized, and %VF could strcpy() from NULL */
	name = os_generate_formatted_filename(NULL, true, "%FPS %CRES %ORES %VF");
	assert_string_equal(name, "0 0x0 0x0 None");
	bfree(name);

	obs_shutdown();
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(file_io_test), obs_unit_test(conversion_test), obs_unit_test(path_test),
		obs_unit_test(module_test),  obs_unit_test(system_test),     obs_unit_test(formatted_filename_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
