#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <math.h>
#include <string.h>

#include <graphics/axisang.h>
#include <graphics/math-defs.h>
#include <graphics/matrix3.h>
#include <graphics/matrix4.h>
#include <graphics/plane.h>
#include <graphics/quat.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

/* matrix3.c, matrix4.c, quat.c, axisang.c and their headers */

#define M_PI_D2 (M_PI / 2.0f)
#define assert_near(a, b) assert_true(fabsf((float)(a) - (float)(b)) < 1e-4f)

static bool m4_close(const struct matrix4 *a, const struct matrix4 *b)
{
	return vec4_close(&a->x, &b->x, 1e-4f) && vec4_close(&a->y, &b->y, 1e-4f) && vec4_close(&a->z, &b->z, 1e-4f) &&
	       vec4_close(&a->t, &b->t, 1e-4f);
}

static bool m3_close(const struct matrix3 *a, const struct matrix3 *b)
{
	return vec3_close(&a->x, &b->x, 1e-4f) && vec3_close(&a->y, &b->y, 1e-4f) && vec3_close(&a->z, &b->z, 1e-4f) &&
	       vec3_close(&a->t, &b->t, 1e-4f);
}

static void quat_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct quat q, r, s;
	quat_identity(&q);
	assert_near(q.w, 1.0f);
	/* Regression: quat_set() stored its components in reverse order */
	quat_set(&r, 1.0f, 2.0f, 3.0f, 4.0f);
	assert_near(r.x, 1.0f);
	assert_near(r.w, 4.0f);
	quat_copy(&s, &r);
	assert_true(quat_close(&s, &r, 1e-5f));
	quat_add(&s, &r, &q);
	assert_near(s.w, 5.0f);
	quat_sub(&s, &r, &q);
	assert_near(s.w, 3.0f);
	quat_addf(&s, &r, 1.0f);
	assert_near(s.x, 2.0f);
	quat_subf(&s, &r, 1.0f);
	assert_near(s.x, 0.0f);
	quat_mulf(&s, &r, 2.0f);
	assert_near(s.z, 6.0f);
	quat_divf(&s, &r, 2.0f);
	assert_near(s.w, 2.0f);
	assert_near(quat_dot(&r, &r), 30.0f);
	assert_near(quat_len(&r), sqrtf(30.0f));
	assert_near(quat_dist(&r, &r), 0.0f);
	quat_neg(&s, &r);
	assert_near(s.x, -1.0f);
	quat_norm(&s, &r);
	assert_near(quat_len(&s), 1.0f);
	struct quat zero = {0};
	quat_norm(&s, &zero);
	assert_near(s.w, 0.0f);

	/* q * q^-1 is the identity. Regression: quat_inv() did not write w */
	quat_norm(&s, &r);
	memset(&q, 0, sizeof(q));
	quat_inv(&q, &s);
	assert_near(q.w, s.w);
	quat_mul(&r, &s, &q);
	struct quat ident;
	quat_identity(&ident);
	assert_true(quat_close(&r, &ident, 1e-4f));

	/* 90 degrees around z: axis-angle, matrices and back */
	struct axisang aa;
	axisang_set(&aa, 0.0f, 0.0f, 1.0f, M_PI_D2);
	quat_from_axisang(&q, &aa);
	assert_near(q.z, sinf(M_PI_D2 / 2.0f));
	assert_near(q.w, cosf(M_PI_D2 / 2.0f));

	struct axisang back;
	axisang_from_quat(&back, &q);
	assert_near(back.z, 1.0f);
	assert_near(back.w, M_PI_D2);
	axisang_from_quat(&back, &ident); /* no rotation: zero axis */
	assert_near(back.w, 0.0f);
	struct axisang copy;
	axisang_copy(&copy, &aa);
	assert_near(copy.w, aa.w);
	axisang_zero(&copy);
	assert_near(copy.z, 0.0f);

	struct matrix3 m3;
	matrix3_from_quat(&m3, &q);
	quat_from_matrix3(&r, &m3);
	assert_true(quat_close(&r, &q, 1e-4f) || (quat_neg(&s, &r), quat_close(&s, &q, 1e-4f)));
	struct matrix4 m4;
	matrix4_from_quat(&m4, &q);
	quat_from_matrix4(&r, &m4);
	assert_true(quat_close(&r, &q, 1e-4f) || (quat_neg(&s, &r), quat_close(&s, &q, 1e-4f)));

	/* each diagonal branch of quat_from_matrix4: rotations of 180 degrees around x, y and z */
	const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	for (int i = 0; i < 3; i++) {
		axisang_set(&aa, axes[i][0], axes[i][1], axes[i][2], M_PI);
		quat_from_axisang(&q, &aa);
		matrix4_from_quat(&m4, &q);
		quat_from_matrix4(&r, &m4);
		assert_near(fabsf(quat_dot(&r, &q)), 1.0f);
	}

	/* direction and look direction */
	struct vec3 dir;
	quat_identity(&q);
	quat_get_dir(&dir, &q);
	assert_near(vec3_len(&dir), 1.0f);
	vec3_set(&dir, 0.0f, 0.0f, 1.0f);
	quat_set_look_dir(&q, &dir);
	assert_near(quat_len(&q), 1.0f);
	vec3_set(&dir, 1.0f, 1.0f, 0.0f);
	quat_set_look_dir(&q, &dir);
	assert_near(quat_len(&q), 1.0f);

	/* log/exp round trip and interpolation end points */
	axisang_set(&aa, 0.0f, 1.0f, 0.0f, 1.0f);
	quat_from_axisang(&q, &aa);
	quat_log(&r, &q);
	quat_exp(&s, &r);
	assert_true(quat_close(&s, &q, 1e-4f));
	quat_log(&r, &ident);
	quat_exp(&s, &r);
	assert_true(quat_close(&s, &ident, 1e-4f));

	quat_interpolate(&r, &ident, &q, 0.0f);
	assert_true(quat_close(&r, &ident, 1e-4f));
	quat_interpolate(&r, &ident, &q, 1.0f);
	assert_true(quat_close(&r, &q, 1e-4f));
	quat_interpolate(&r, &ident, &ident, 0.5f); /* nearly identical: linear path */
	assert_true(quat_close(&r, &ident, 1e-4f));
	quat_neg(&s, &q);
	quat_interpolate(&r, &ident, &s, 0.5f); /* opposite hemisphere */
	assert_near(quat_len(&r), 1.0f);

	struct quat tangent;
	quat_get_tangent(&tangent, &ident, &q, &ident);
	quat_interpolate_cubic(&r, &ident, &q, &tangent, &tangent, 0.0f);
	assert_true(quat_close(&r, &ident, 1e-4f));
}

static void matrix3_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct matrix3 m, r, ident;
	matrix3_identity(&ident);
	matrix3_copy(&m, &ident);
	assert_true(m3_close(&m, &ident));

	/* translate, scale and rotate, then check how a point moves. Regression: translating into a different
	 * matrix left its axes as they were (here zero) instead of copying them */
	struct vec3 p, out;
	vec3_set(&p, 1.0f, 0.0f, 0.0f);
	memset(&m, 0, sizeof(m));
	matrix3_translate3f(&m, &ident, 1.0f, 2.0f, 3.0f);
	assert_near(m.x.x, 1.0f);
	vec3_transform3x4(&out, &p, &m);
	assert_near(out.x, 2.0f);
	assert_near(out.z, 3.0f);
	matrix3_scale3f(&m, &ident, 2.0f, 3.0f, 4.0f);
	vec3_transform3x4(&out, &p, &m);
	assert_near(out.x, 2.0f);
	matrix3_rotate_aa4f(&m, &ident, 0.0f, 0.0f, 1.0f, M_PI_D2);
	vec3_transform3x4(&out, &p, &m);
	assert_near(fabsf(out.y), 1.0f);
	assert_near(out.x, 0.0f);

	struct quat q;
	struct axisang aa;
	axisang_set(&aa, 0.0f, 0.0f, 1.0f, M_PI_D2);
	quat_from_axisang(&q, &aa);
	matrix3_rotate(&r, &ident, &q);
	assert_true(m3_close(&r, &m));
	matrix3_rotate_aa(&r, &ident, &aa);
	assert_true(m3_close(&r, &m));
	matrix3_from_axisang(&r, &aa);
	assert_near(vec3_len(&r.x), 1.0f);

	/* M * M^-1 == I, transpose of a rotation is its inverse */
	matrix3_translate3f(&m, &m, 4.0f, 5.0f, 6.0f);
	/* inverting twice gives the original frame */
	matrix3_inv(&r, &m);
	struct matrix3 prod;
	matrix3_inv(&prod, &r);
	assert_true(m3_close(&prod, &m));
	matrix3_rotate_aa4f(&m, &ident, 1.0f, 0.0f, 0.0f, 0.3f);
	matrix3_transpose(&r, &m);
	matrix3_mul(&prod, &m, &r);
	assert_true(vec3_close(&prod.x, &ident.x, 1e-4f));

	struct matrix4 m4;
	matrix4_identity(&m4);
	m4.t.x = 7.0f;
	matrix3_from_matrix4(&r, &m4);
	assert_near(r.t.x, 7.0f);

	/* mirror across the plane x = 0 */
	struct plane pl;
	struct vec3 n;
	vec3_set(&n, 1.0f, 0.0f, 0.0f);
	plane_set(&pl, &n, 0.0f);
	matrix3_mirror(&r, &ident, &pl);
	vec3_transform3x4(&out, &p, &r);
	assert_near(out.x, -1.0f);
	matrix3_mirrorv(&r, &ident, &n);
	vec3_transform3x4(&out, &p, &r);
	assert_near(out.x, -1.0f);

	struct vec3 t;
	vec3_set(&t, 1.0f, 1.0f, 1.0f);
	/* a 3x4 frame stores the inverse translation */
	matrix3_translate(&r, &ident, &t);
	assert_near(r.t.y, -1.0f);
	matrix3_scale(&r, &ident, &t);
	assert_near(r.x.x, 1.0f);
}

static void matrix4_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct matrix4 m, r, ident, prod;
	matrix4_identity(&ident);
	matrix4_copy(&m, &ident);
	assert_true(m4_close(&m, &ident));
	assert_near(matrix4_determinant(&ident), 1.0f);

	struct vec3 p, out, v;
	vec3_set(&p, 1.0f, 0.0f, 0.0f);
	vec3_set(&v, 1.0f, 2.0f, 3.0f);

	/* post- and pre-multiplied variants */
	matrix4_translate3f(&m, &ident, 1.0f, 2.0f, 3.0f);
	vec3_transform(&out, &p, &m);
	assert_near(out.x, 2.0f);
	matrix4_translate3v(&r, &ident, &v);
	assert_true(m4_close(&r, &m));
	matrix4_translate3v_i(&r, &v, &ident);
	assert_true(m4_close(&r, &m));
	struct vec4 v4;
	vec4_set(&v4, 1.0f, 2.0f, 3.0f, 1.0f);
	matrix4_translate4v(&r, &ident, &v4);
	vec3_transform(&out, &p, &r);
	assert_near(out.x, 2.0f);
	matrix4_translate4v_i(&r, &v4, &ident);
	vec3_transform(&out, &p, &r);
	assert_near(out.x, 2.0f);

	matrix4_scale3f(&m, &ident, 2.0f, 3.0f, 4.0f);
	assert_near(matrix4_determinant(&m), 24.0f);
	matrix4_scale(&r, &ident, &v);
	assert_near(matrix4_determinant(&r), 6.0f);
	matrix4_scale_i(&r, &v, &ident);
	assert_near(matrix4_determinant(&r), 6.0f);

	struct axisang aa;
	axisang_set(&aa, 0.0f, 0.0f, 1.0f, M_PI_D2);
	struct quat q;
	quat_from_axisang(&q, &aa);
	matrix4_rotate_aa4f(&m, &ident, 0.0f, 0.0f, 1.0f, M_PI_D2);
	vec3_transform(&out, &p, &m);
	assert_near(fabsf(out.y), 1.0f);
	matrix4_rotate(&r, &ident, &q);
	assert_true(m4_close(&r, &m));
	matrix4_rotate_i(&r, &q, &ident);
	assert_true(m4_close(&r, &m));
	matrix4_rotate_aa(&r, &ident, &aa);
	assert_true(m4_close(&r, &m));
	matrix4_rotate_aa_i(&r, &aa, &ident);
	assert_true(m4_close(&r, &m));
	matrix4_from_axisang(&r, &aa);
	assert_true(m4_close(&r, &m));

	struct matrix3 m3;
	matrix3_from_quat(&m3, &q);
	matrix4_from_matrix3(&r, &m3);
	assert_true(m4_close(&r, &m));

	/* inverse of a general transform */
	matrix4_translate3f(&m, &m, 4.0f, -2.0f, 1.0f);
	matrix4_scale3f(&m, &m, 2.0f, 2.0f, 2.0f);
	assert_true(matrix4_inv(&r, &m));
	matrix4_mul(&prod, &m, &r);
	assert_true(m4_close(&prod, &ident));

	/* a singular matrix has no inverse */
	struct matrix4 zero = {0};
	assert_false(matrix4_inv(&r, &zero));
	assert_near(matrix4_determinant(&zero), 0.0f);

	matrix4_transpose(&r, &m);
	assert_near(r.x.y, m.y.x);
	assert_near(r.t.z, m.z.w);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(quat_test),
		obs_unit_test(matrix3_test),
		obs_unit_test(matrix4_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
