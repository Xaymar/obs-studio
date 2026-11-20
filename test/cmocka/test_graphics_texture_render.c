#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"
#include "gs-test.h"

#include <graphics/vec4.h>

/* texture-render.c on libobs-null */

static void texrender_test(void **state)
{
	UNUSED_PARAMETER(state);

	gs_texrender_t *tr = gs_texrender_create(GS_BGRA, GS_Z24_S8);
	assert_non_null(tr);
	assert_int_equal(gs_texrender_get_format(tr), GS_BGRA);
	assert_null(gs_texrender_get_texture(tr));

	/* render into it: the previous target comes back afterwards */
	gs_texture_t *outer = gs_texture_create(2, 2, GS_BGRA, 1, NULL, GS_RENDER_TARGET);
	gs_set_render_target(outer, NULL);
	assert_true(gs_texrender_begin(tr, 4, 4));
	gs_texture_t *target = gs_texrender_get_texture(tr);
	assert_ptr_equal(gs_get_render_target(), target);
	struct gs_rect vp;
	gs_get_viewport(&vp);
	assert_int_equal(vp.cx, 4);
	struct vec4 c;
	vec4_set(&c, 0.0f, 0.0f, 1.0f, 1.0f);
	gs_clear(GS_CLEAR_COLOR, &c, 1.0f, 0);
	gs_texrender_end(tr);
	assert_ptr_equal(gs_get_render_target(), outer);

	/* a rendered texrender refuses to begin until it is reset */
	assert_false(gs_texrender_begin(tr, 4, 4));
	gs_texrender_reset(tr);
	assert_true(gs_texrender_begin_with_color_space(tr, 4, 4, GS_CS_709_EXTENDED));
	gs_texrender_end(tr);
	assert_ptr_equal(gs_texrender_get_texture(tr), target); /* same size: same target */

	/* a new size makes a new target */
	gs_texrender_reset(tr);
	assert_true(gs_texrender_begin(tr, 8, 2));
	gs_texrender_end(tr);
	assert_int_equal(gs_texture_get_width(gs_texrender_get_texture(tr)), 8);

	gs_texrender_reset(tr);
	assert_false(gs_texrender_begin(tr, 0, 4));
	assert_false(gs_texrender_begin(tr, 4, 0));

	gs_set_render_target(NULL, NULL);
	gs_texture_destroy(outer);
	gs_texrender_destroy(tr);

	/* without a depth buffer */
	tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	assert_true(gs_texrender_begin(tr, 2, 2));
	assert_null(gs_get_zstencil_target());
	gs_texrender_end(tr);
	gs_texrender_destroy(tr);

	/* an unusable format cannot make a target */
	tr = gs_texrender_create(GS_UNKNOWN, GS_ZS_NONE);
	assert_false(gs_texrender_begin(tr, 2, 2));
	gs_texrender_destroy(tr);

	/* NULL is safe */
	assert_false(gs_texrender_begin(NULL, 2, 2));
	gs_texrender_end(NULL);
	gs_texrender_reset(NULL);
	gs_texrender_destroy(NULL);
	assert_null(gs_texrender_get_texture(NULL));
	assert_int_equal(gs_texrender_get_format(NULL), GS_UNKNOWN); /* regression: dereferenced NULL */
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		gs_unit_test(texrender_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
