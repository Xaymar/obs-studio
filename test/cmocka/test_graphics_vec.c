#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <math.h>

#include <graphics/matrix3.h>
#include <graphics/matrix4.h>
#include <graphics/plane.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

/* vec2.c, vec3.c, vec4.c and their headers */

#define EPS 1e-5f
#define assert_near(a, b) assert_true(fabsf((float)(a) - (float)(b)) < 1e-4f)

static void vec2_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct vec2 a, b, r;
	vec2_set(&a, 3.0f, -4.0f);
	vec2_set(&b, 1.0f, 2.0f);
	vec2_add(&r, &a, &b);
	assert_near(r.x, 4.0f);
	vec2_sub(&r, &a, &b);
	assert_near(r.y, -6.0f);
	vec2_mul(&r, &a, &b);
	assert_near(r.y, -8.0f);
	vec2_div(&r, &a, &b);
	assert_near(r.y, -2.0f);
	vec2_addf(&r, &a, 1.0f);
	assert_near(r.x, 4.0f);
	vec2_subf(&r, &a, 1.0f);
	assert_near(r.x, 2.0f);
	vec2_mulf(&r, &a, 2.0f);
	assert_near(r.y, -8.0f);
	vec2_divf(&r, &a, 2.0f);
	assert_near(r.x, 1.5f);
	vec2_neg(&r, &a);
	assert_near(r.x, -3.0f);
	assert_near(vec2_dot(&a, &b), -5.0f);
	assert_near(vec2_len(&a), 5.0f);
	assert_near(vec2_dist(&a, &b), sqrtf(4.0f + 36.0f));
	vec2_minf(&r, &a, 0.0f);
	assert_near(r.x, 0.0f);
	vec2_maxf(&r, &a, 0.0f);
	assert_near(r.y, 0.0f);
	vec2_min(&r, &a, &b);
	assert_near(r.x, 1.0f);
	vec2_max(&r, &a, &b);
	assert_near(r.y, 2.0f);
	vec2_abs(&r, &a);
	assert_near(r.y, 4.0f);
	vec2_set(&r, 1.5f, -1.5f);
	vec2_floor(&r, &r);
	assert_near(r.y, -2.0f);
	vec2_set(&r, 1.5f, -1.5f);
	vec2_ceil(&r, &r);
	assert_near(r.x, 2.0f);
	vec2_norm(&r, &a);
	assert_near(vec2_len(&r), 1.0f);
	vec2_zero(&r);
	vec2_norm(&r, &r); /* zero stays zero */
	assert_near(r.x, 0.0f);
	vec2_copy(&r, &a);
	assert_true(vec2_close(&r, &a, EPS));
	vec2_set(&r, 3.1f, -4.0f);
	assert_false(vec2_close(&r, &a, EPS));
	vec2_set(&r, 3.0f, -4.1f);
	assert_false(vec2_close(&r, &a, EPS));
}

static void vec3_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct vec3 a, b, r;
	vec3_set(&a, 1.0f, 2.0f, 3.0f);
	vec3_set(&b, 4.0f, 5.0f, 6.0f);
	vec3_add(&r, &a, &b);
	assert_near(r.z, 9.0f);
	vec3_sub(&r, &b, &a);
	assert_near(r.x, 3.0f);
	vec3_mul(&r, &a, &b);
	assert_near(r.z, 18.0f);
	vec3_div(&r, &b, &a);
	assert_near(r.y, 2.5f);
	vec3_addf(&r, &a, 1.0f);
	assert_near(r.z, 4.0f);
	vec3_subf(&r, &a, 1.0f);
	assert_near(r.x, 0.0f);
	vec3_mulf(&r, &a, 2.0f);
	assert_near(r.z, 6.0f);
	vec3_divf(&r, &a, 2.0f);
	assert_near(r.y, 1.0f);
	assert_near(vec3_dot(&a, &b), 32.0f);
	vec3_cross(&r, &a, &b);
	assert_near(r.x, -3.0f);
	assert_near(r.y, 6.0f);
	assert_near(r.z, -3.0f);
	vec3_neg(&r, &a);
	assert_near(r.y, -2.0f);
	assert_near(vec3_len(&a), sqrtf(14.0f));
	assert_near(vec3_dist(&a, &b), sqrtf(27.0f));
	vec3_norm(&r, &a);
	assert_near(vec3_len(&r), 1.0f);
	vec3_zero(&r);
	vec3_norm(&r, &r);
	assert_near(r.x, 0.0f);
	vec3_copy(&r, &a);
	assert_true(vec3_close(&r, &a, EPS));
	r.z += 0.1f;
	assert_false(vec3_close(&r, &a, EPS));
	/* Regression: a smaller vector compared as close, the difference was not taken absolutely */
	assert_false(vec3_close(&a, &r, EPS));
	vec3_min(&r, &a, &b);
	assert_near(r.z, 3.0f);
	vec3_minf(&r, &a, 2.0f);
	assert_near(r.z, 2.0f);
	vec3_max(&r, &a, &b);
	assert_near(r.x, 4.0f);
	vec3_maxf(&r, &a, 2.0f);
	assert_near(r.x, 2.0f);
	vec3_set(&r, -1.5f, 1.5f, -0.5f);
	vec3_abs(&r, &r);
	assert_near(r.x, 1.5f);
	vec3_set(&r, -1.5f, 1.5f, -0.5f);
	vec3_floor(&r, &r);
	assert_near(r.x, -2.0f);
	vec3_set(&r, -1.5f, 1.5f, -0.5f);
	vec3_ceil(&r, &r);
	assert_near(r.y, 2.0f);

	struct vec4 v4;
	vec4_set(&v4, 7.0f, 8.0f, 9.0f, 1.0f);
	vec3_from_vec4(&r, &v4);
	assert_near(r.z, 9.0f);

	/* plane distance and mirroring across z = 1 */
	struct plane p;
	struct vec3 n;
	vec3_set(&n, 0.0f, 0.0f, 1.0f);
	plane_set(&p, &n, 1.0f);
	assert_near(vec3_plane_dist(&a, &p), 2.0f);
	vec3_mirror(&r, &a, &p);
	assert_near(r.z, -1.0f);
	vec3_mirrorv(&r, &a, &n);
	assert_near(r.z, -3.0f);

	/* transforms by an identity matrix and a translation */
	struct matrix4 m4;
	matrix4_identity(&m4);
	m4.t.x = 10.0f;
	vec3_transform(&r, &a, &m4);
	assert_near(r.x, 11.0f);
	struct matrix3 m3;
	matrix3_identity(&m3);
	m3.t.y = 10.0f;
	vec3_rotate(&r, &a, &m3);
	assert_near(r.y, 2.0f);
	/* a matrix3 is a 3x4 frame: transform3x4 moves into it, subtracting the origin */
	vec3_transform3x4(&r, &a, &m3);
	assert_near(r.y, -8.0f);

	for (int i = 0; i < 20; i++) {
		vec3_rand(&r, 1);
		assert_true(r.x >= 0.0f && r.y >= 0.0f && r.z >= 0.0f);
		vec3_rand(&r, 0);
		assert_true(r.x >= -1.0f && r.x <= 1.0f);
	}
}

static void vec4_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct vec4 a, b, r;
	vec4_set(&a, 1.0f, 2.0f, 3.0f, 4.0f);
	vec4_set(&b, 2.0f, 2.0f, 2.0f, 2.0f);
	vec4_add(&r, &a, &b);
	assert_near(r.w, 6.0f);
	vec4_sub(&r, &a, &b);
	assert_near(r.x, -1.0f);
	vec4_mul(&r, &a, &b);
	assert_near(r.z, 6.0f);
	vec4_div(&r, &a, &b);
	assert_near(r.w, 2.0f);
	vec4_addf(&r, &a, 1.0f);
	assert_near(r.x, 2.0f);
	vec4_subf(&r, &a, 1.0f);
	assert_near(r.x, 0.0f);
	vec4_mulf(&r, &a, 3.0f);
	assert_near(r.y, 6.0f);
	vec4_divf(&r, &a, 2.0f);
	assert_near(r.w, 2.0f);
	assert_near(vec4_dot(&a, &b), 20.0f);
	vec4_neg(&r, &a);
	assert_near(r.w, -4.0f);
	assert_near(vec4_len(&a), sqrtf(30.0f));
	assert_near(vec4_dist(&a, &b), sqrtf(1 + 0 + 1 + 4));
	vec4_norm(&r, &a);
	assert_near(vec4_len(&r), 1.0f);
	vec4_zero(&r);
	vec4_norm(&r, &r);
	assert_near(r.w, 0.0f);
	vec4_copy(&r, &a);
	assert_true(vec4_close(&r, &a, EPS));
	r.w += 0.5f;
	assert_false(vec4_close(&r, &a, EPS));
	assert_false(vec4_close(&a, &r, EPS)); /* regression, as for vec3_close() */
	vec4_min(&r, &a, &b);
	assert_near(r.w, 2.0f);
	vec4_minf(&r, &a, 2.5f);
	assert_near(r.z, 2.5f);
	vec4_max(&r, &a, &b);
	assert_near(r.x, 2.0f);
	vec4_maxf(&r, &a, 2.5f);
	assert_near(r.x, 2.5f);
	vec4_set(&r, -1.5f, 1.5f, -0.5f, 0.5f);
	vec4_abs(&r, &r);
	assert_near(r.x, 1.5f);
	vec4_set(&r, -1.5f, 1.5f, -0.5f, 0.5f);
	vec4_floor(&r, &r);
	assert_near(r.z, -1.0f);
	vec4_set(&r, -1.5f, 1.5f, -0.5f, 0.5f);
	vec4_ceil(&r, &r);
	assert_near(r.w, 1.0f);

	struct vec3 v3;
	vec3_set(&v3, 1.0f, 2.0f, 3.0f);
	vec4_from_vec3(&r, &v3);
	assert_near(r.w, 1.0f);

	/* colors */
	vec4_set(&r, 1.0f, 0.5f, 0.0f, 1.0f);
	uint32_t rgba = vec4_to_rgba(&r);
	assert_int_equal(rgba & 0xff, 0xff);
	assert_int_equal((rgba >> 24) & 0xff, 0xff);
	uint32_t bgra = vec4_to_bgra(&r);
	assert_int_equal((bgra >> 16) & 0xff, 0xff);
	struct vec4 back;
	vec4_from_rgba(&back, rgba);
	assert_true(vec4_close(&back, &r, 0.01f));
	vec4_from_bgra(&back, bgra);
	assert_true(vec4_close(&back, &r, 0.01f));
	vec4_from_rgba_srgb(&back, 0xff808080);
	assert_true(back.x > 0.2f && back.x < 0.25f); /* sRGB 0.5 is about 0.214 linear */
	assert_near(back.w, 1.0f);

	struct matrix4 m;
	matrix4_identity(&m);
	m.t.y = 5.0f;
	vec4_transform(&r, &a, &m);
	assert_near(r.y, 2.0f + 5.0f * 4.0f);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(vec2_test),
		obs_unit_test(vec3_test),
		obs_unit_test(vec4_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
