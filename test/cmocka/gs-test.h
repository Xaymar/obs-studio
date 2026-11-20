/* Graphics test fixture: a gs context on the libobs-null module. Include after <cmocka.h> and "obs-test.h". */

#pragma once

#include <graphics/graphics.h>
#include <util/platform.h>

#ifndef NULL_GRAPHICS_MODULE
#error NULL_GRAPHICS_MODULE must be defined
#endif

typedef long (*gs_test_call_count_fn)(const char *name);
typedef void (*gs_test_reset_fn)(void);
typedef long (*gs_test_live_fn)(void);

static graphics_t *gs_test_graphics;
static void *gs_test_module;
static gs_test_call_count_fn gs_test_calls;
static gs_test_reset_fn gs_test_reset_calls;
static gs_test_live_fn gs_test_live;

static int gs_test_setup(void **state)
{
	obs_test_setup(state);
	if (gs_create(&gs_test_graphics, NULL_GRAPHICS_MODULE, 0) != GS_SUCCESS) {
		return -1;
	}
	gs_test_module = os_dlopen(NULL_GRAPHICS_MODULE);
	gs_test_calls = (gs_test_call_count_fn)os_dlsym(gs_test_module, "null_gs_get_call_count");
	gs_test_reset_calls = (gs_test_reset_fn)os_dlsym(gs_test_module, "null_gs_reset_call_counts");
	gs_test_live = (gs_test_live_fn)os_dlsym(gs_test_module, "null_gs_get_live_objects");
	gs_enter_context(gs_test_graphics);
	gs_test_reset_calls();
	return 0;
}

/* Fails the test if it left device objects or bmalloc() allocations behind. */
static int gs_test_teardown(void **state)
{
	gs_leave_context();
	gs_destroy(gs_test_graphics);
	gs_test_graphics = NULL;
	long live = gs_test_live();
	os_dlclose(gs_test_module);
	if (live) {
		print_error("%ld device object(s) leaked\n", live);
	}
	int ret = obs_test_teardown(state);
	return live ? -1 : ret;
}

#define gs_unit_test(f) cmocka_unit_test_setup_teardown(f, gs_test_setup, gs_test_teardown)
