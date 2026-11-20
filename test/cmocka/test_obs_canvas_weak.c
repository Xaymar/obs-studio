#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>

static int setup(void **state)
{
	UNUSED_PARAMETER(state);
	return obs_startup("en-US", NULL, NULL) ? 0 : -1;
}

static int teardown(void **state)
{
	UNUSED_PARAMETER(state);
	obs_shutdown();
	return 0;
}

/* Regression test: obs_weak_canvas_references_canvas() dereferenced a struct that no longer existed. */
static void weak_canvas_references_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* Canvases without a video mix need no graphics */
	obs_canvas_t *canvas = obs_canvas_create_private("weak-test", NULL, 0);
	obs_canvas_t *other = obs_canvas_create_private("weak-test-other", NULL, 0);
	assert_non_null(canvas);
	assert_non_null(other);

	obs_weak_canvas_t *weak = obs_canvas_get_weak_canvas(canvas);
	assert_non_null(weak);

	assert_true(obs_weak_canvas_references_canvas(weak, canvas));
	assert_false(obs_weak_canvas_references_canvas(weak, other));
	assert_false(obs_weak_canvas_references_canvas(weak, NULL));
	assert_false(obs_weak_canvas_references_canvas(NULL, canvas));

	obs_canvas_t *strong = obs_weak_canvas_get_canvas(weak);
	assert_ptr_equal(strong, canvas);
	obs_canvas_release(strong);

	obs_canvas_release(canvas);
	assert_null(obs_weak_canvas_get_canvas(weak));

	obs_weak_canvas_release(weak);
	obs_canvas_release(other);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(weak_canvas_references_test, setup, teardown),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
