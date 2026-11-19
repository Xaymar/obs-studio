/* Failure injection in the libobs-null graphics module (see null-graphics.h). Include after <cmocka.h>. */

#pragma once

#include <util/platform.h>

typedef bool (*null_fail_next_fn)(const char *name, long count);
typedef void (*null_reset_failures_fn)(void);

/* kept loaded: the failure counts live in the module, and unloading it would reset them */
static void *null_fail_module;

static void *null_fail_sym(const char *name)
{
	if (!null_fail_module)
		null_fail_module = os_dlopen(NULL_GRAPHICS_MODULE);
	void *sym = os_dlsym(null_fail_module, name);
	assert_non_null(sym);
	return sym;
}

/* the next `count` calls of the named null graphics export fail */
static void null_fail_next(const char *name, long count)
{
	null_fail_next_fn fail_fn = (null_fail_next_fn)null_fail_sym("null_gs_fail_next");
	assert_true(fail_fn(name, count));
}

static void null_reset_failures(void)
{
	null_reset_failures_fn reset = (null_reset_failures_fn)null_fail_sym("null_gs_reset_failures");
	reset();
}
