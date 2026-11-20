/* Shared helpers for libobs cmocka tests. Include after <cmocka.h>. */

#pragma once

#include <util/bmem.h>

static long obs_test_allocs_before;

static int obs_test_setup(void **state)
{
	(void)state;
	obs_test_allocs_before = bnum_allocs();
	return 0;
}

/* Fails the test if it left bmalloc() allocations behind. */
static int obs_test_teardown(void **state)
{
	(void)state;
	long leaked = bnum_allocs() - obs_test_allocs_before;
	if (leaked) {
		print_error("%ld bmalloc allocation(s) leaked\n", leaked);
	}
	return leaked ? -1 : 0;
}

#define obs_unit_test(f) cmocka_unit_test_setup_teardown(f, obs_test_setup, obs_test_teardown)
