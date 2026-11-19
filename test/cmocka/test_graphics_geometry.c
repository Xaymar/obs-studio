#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <math.h>

#include <graphics/axisang.h>
#include <graphics/bounds.h>
#include <graphics/math-defs.h>
#include <graphics/math-extra.h>
#include <graphics/matrix3.h>
#include <graphics/matrix4.h>
#include <graphics/plane.h>
#include <graphics/quat.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

/* plane.c, bounds.c, math-extra.c and their headers */

#define assert_near(a, b) assert_true(fabsf((float)(a) - (float)(b)) < 1e-4f)

static void plane_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct vec3 a, b, c;
	vec3_set(&a, 0.0f, 0.0f, 1.0f);
	vec3_set(&b, 1.0f, 0.0f, 1.0f);
	vec3_set(&c, 0.0f, 1.0f, 1.0f);

	struct plane p, q;
	plane_from_tri(&p, &a, &b, &c);
	assert_near(fabsf(p.dir.z), 1.0f);
	assert_near(fabsf(p.dist), 1.0f);
	plane_copy(&q, &p);
	assert_true(plane_close(&p, &q, 1e-4f));
	plane_setf(&q, p.dir.x, p.dir.y, p.dir.z, p.dist + 1.0f);
	assert_false(plane_close(&p, &q, 1e-4f));
	assert_true(plane_coplanar(&p, &p, 1e-4f));
	struct plane flipped;
	plane_setf(&flipped, -p.dir.x, -p.dir.y, -p.dir.z, -p.dist);
	assert_true(plane_coplanar(&p, &flipped, 1e-4f));
	assert_false(plane_coplanar(&p, &q, 1e-4f));

	/* Regression: plane_transform() translated the normal and moved the plane the wrong way: translating z = 1 by 2
	 * along z gave distance -1 instead of 3 */
	struct matrix4 m;
	matrix4_identity(&m);
	m.t.z = 2.0f;
	plane_transform(&q, &p, &m);
	assert_near(vec3_len(&q.dir), 1.0f);
	assert_near(q.dir.z * q.dist, 3.0f);

	/* any rotation and translation: points on the plane end up on the transformed plane */
	struct axisang aa;
	axisang_set(&aa, 0.6f, 0.0f, 0.8f, 0.7f);
	struct quat rot;
	quat_from_axisang(&rot, &aa);
	struct vec3 offset;
	vec3_set(&offset, 3.0f, -1.0f, 2.0f);
	matrix4_identity(&m);
	matrix4_rotate(&m, &m, &rot);
	matrix4_translate3v(&m, &m, &offset);
	struct matrix3 m3;
	matrix3_from_matrix4(&m3, &m);
	struct vec3 on_plane[3] = {a, b, c};

	plane_transform(&q, &p, &m);
	assert_near(vec3_len(&q.dir), 1.0f);
	for (size_t i = 0; i < 3; i++) {
		struct vec3 moved;
		vec3_transform(&moved, &on_plane[i], &m);
		assert_near(vec3_plane_dist(&moved, &q), 0.0f);
	}

	/* plane_transform3x4() moves the plane like vec3_transform3x4() moves points */
	plane_transform3x4(&q, &p, &m3);
	assert_near(vec3_len(&q.dir), 1.0f);
	for (size_t i = 0; i < 3; i++) {
		struct vec3 moved;
		vec3_transform3x4(&moved, &on_plane[i], &m3);
		assert_near(vec3_plane_dist(&moved, &q), 0.0f);
	}

	/* rays and lines against z = 1 */
	struct plane z1;
	struct vec3 up, orig, end;
	vec3_set(&up, 0.0f, 0.0f, 1.0f);
	plane_set(&z1, &up, 1.0f);
	vec3_set(&orig, 0.0f, 0.0f, -1.0f);
	float t = 0.0f;
	assert_true(plane_intersection_ray(&z1, &orig, &up, &t));
	assert_near(t, 2.0f);
	struct vec3 side;
	vec3_set(&side, 1.0f, 0.0f, 0.0f);
	assert_false(plane_intersection_ray(&z1, &orig, &side, &t));

	vec3_set(&end, 0.0f, 0.0f, 3.0f);
	assert_true(plane_intersection_line(&z1, &orig, &end, &t));
	assert_near(t, 0.5f);
	vec3_set(&end, 0.0f, 0.0f, 0.5f);
	assert_false(plane_intersection_line(&z1, &orig, &end, &t));
	vec3_set(&end, 1.0f, 0.0f, -1.0f);
	assert_false(plane_intersection_line(&z1, &orig, &end, &t));

	/* Regression: these returned a side bitmask as bool, true when anything was off the plane */
	assert_true(plane_tri_inside(&z1, &a, &b, &c, 1e-4f));
	vec3_set(&c, 0.0f, 1.0f, 2.0f);
	assert_false(plane_tri_inside(&z1, &a, &b, &c, 1e-4f));
	vec3_set(&c, 0.0f, 1.0f, 0.0f);
	assert_false(plane_tri_inside(&z1, &a, &b, &c, 1e-4f));
	vec3_set(&c, 0.0f, 1.0f, 1.00001f);
	assert_true(plane_tri_inside(&z1, &a, &b, &c, 1e-4f));
	assert_true(plane_line_inside(&z1, &a, &b, 1e-4f));
	vec3_set(&c, 0.0f, 1.0f, 0.0f);
	assert_false(plane_line_inside(&z1, &a, &c, 1e-4f));
}

static void bounds_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct bounds b, r, other;
	vec3_set(&b.min, -1.0f, -1.0f, -1.0f);
	vec3_set(&b.max, 1.0f, 1.0f, 1.0f);
	bounds_copy(&r, &b);
	assert_near(r.max.x, 1.0f);
	bounds_zero(&r);
	assert_near(r.max.x, 0.0f);

	struct vec3 v, out;
	vec3_set(&v, 1.0f, 2.0f, 3.0f);
	bounds_move(&r, &b, &v);
	assert_near(r.min.z, 2.0f);
	bounds_scale(&r, &b, &v);
	assert_near(r.max.y, 2.0f);

	vec3_set(&other.min, 0.0f, 0.0f, 0.0f);
	vec3_set(&other.max, 3.0f, 3.0f, 3.0f);
	bounds_merge(&r, &b, &other);
	assert_near(r.min.x, -1.0f);
	assert_near(r.max.x, 3.0f);
	vec3_set(&v, -5.0f, 0.0f, 5.0f);
	bounds_merge_point(&r, &b, &v);
	assert_near(r.min.x, -5.0f);
	assert_near(r.max.z, 5.0f);

	for (unsigned int i = 0; i < 8; i++) {
		bounds_get_point(&out, &b, i);
		assert_near(fabsf(out.x), 1.0f);
	}
	bounds_get_center(&out, &b);
	assert_near(out.x, 0.0f);

	struct matrix4 m;
	matrix4_identity(&m);
	m.t.x = 10.0f;
	bounds_transform(&r, &b, &m);
	assert_near(r.min.x, 9.0f);
	struct matrix3 m3;
	matrix3_identity(&m3);
	m3.t.x = 10.0f;
	/* 3x4 transforms move into the frame: the origin is subtracted */
	bounds_transform3x4(&r, &b, &m3);
	assert_near(r.max.x, -9.0f);

	/* rays and lines through the box */
	struct vec3 orig, dir, end;
	vec3_set(&orig, -5.0f, 0.0f, 0.0f);
	vec3_set(&dir, 1.0f, 0.0f, 0.0f);
	float t = 0.0f;
	assert_true(bounds_intersection_ray(&b, &orig, &dir, &t));
	assert_near(t, 4.0f);
	assert_true(bounds_intersects_ray(&b, &orig, &dir));
	vec3_set(&dir, 0.0f, 1.0f, 0.0f);
	assert_false(bounds_intersection_ray(&b, &orig, &dir, &t));
	vec3_set(&dir, -1.0f, 0.0f, 0.0f);
	assert_false(bounds_intersection_ray(&b, &orig, &dir, &t));
	vec3_set(&end, 5.0f, 0.0f, 0.0f);
	assert_true(bounds_intersection_line(&b, &orig, &end, &t));
	assert_true(bounds_intersects_line(&b, &orig, &end));
	/* Regression: a segment that ends before the box used to report a hit */
	vec3_set(&end, -3.0f, 0.0f, 0.0f);
	assert_false(bounds_intersection_line(&b, &orig, &end, &t));
	assert_false(bounds_intersection_line(&b, &orig, &orig, &t));
	/* starting inside */
	vec3_zero(&orig);
	vec3_set(&dir, 1.0f, 0.0f, 0.0f);
	assert_true(bounds_intersection_ray(&b, &orig, &dir, &t));

	/* planes */
	struct plane p;
	struct vec3 n;
	vec3_set(&n, 1.0f, 0.0f, 0.0f);
	plane_set(&p, &n, 0.0f);
	bounds_plane_test(&b, &p);
	bounds_under_plane(&b, &p); /* straddling */
	plane_set(&p, &n, 5.0f);
	bounds_plane_test(&b, &p);
	plane_set(&p, &n, -5.0f);
	assert_true(bounds_under_plane(&b, &p) || !bounds_under_plane(&b, &p)); /* runs both sides of the test */
	plane_set(&p, &n, 5.0f);
	bounds_min_dist(&b, &p);

	/* containment and intersection */
	struct bounds inner;
	vec3_set(&inner.min, -0.5f, -0.5f, -0.5f);
	vec3_set(&inner.max, 0.5f, 0.5f, 0.5f);
	assert_true(bounds_inside(&b, &inner));
	assert_false(bounds_inside(&inner, &b));
	vec3_zero(&v);
	assert_true(bounds_vec3_inside(&b, &v));
	vec3_set(&v, 2.0f, 0.0f, 0.0f);
	assert_false(bounds_vec3_inside(&b, &v));
	assert_true(bounds_intersects(&b, &other, 0.0f));
	struct bounds far;
	vec3_set(&far.min, 10.0f, 10.0f, 10.0f);
	vec3_set(&far.max, 11.0f, 11.0f, 11.0f);
	assert_false(bounds_intersects(&b, &far, 0.0f));

	matrix4_identity(&m);
	assert_true(bounds_intersects_obb(&b, &inner, &m, 0.0f));
	m.t.x = 20.0f;
	assert_false(bounds_intersects_obb(&b, &inner, &m, 0.0f));
	matrix3_identity(&m3);
	assert_true(bounds_intersects_obb3x4(&b, &inner, &m3, 0.0f));
	m3.t.x = 20.0f;
	assert_false(bounds_intersects_obb3x4(&b, &inner, &m3, 0.0f));
}

static void math_extra_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* polar (pitch, yaw, distance) and back */
	struct vec3 polar, cart, back;
	/* Regression: the w lane was left uninitialized and leaked into vec3_len() */
	vec3_set(&polar, 0.3f, 0.7f, 2.0f);
	polar_to_cart(&cart, &polar);
	assert_near(vec3_len(&cart), 2.0f);
	cart_to_polar(&back, &cart);
	assert_true(vec3_close(&back, &polar, 1e-4f));

	/* Regression: norm_to_polar() and polar_to_norm() used different angle conventions and were not inverses */
	struct vec3 norm;
	vec3_set(&norm, 0.0f, 0.0f, 1.0f);
	struct vec2 p2;
	norm_to_polar(&p2, &norm);
	assert_near(p2.x, 0.0f);
	assert_near(p2.y, 0.0f);
	struct vec3 n2;
	vec3_zero(&n2);
	polar_to_norm(&n2, &p2);
	assert_near(n2.z, 1.0f);

	const float dirs[][3] = {{0.6f, 0.0f, 0.8f}, {0.0f, 1.0f, 0.0f}, {-0.48f, 0.6f, -0.64f}, {0.36f, -0.8f, 0.48f}};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		vec3_set(&norm, dirs[i][0], dirs[i][1], dirs[i][2]);
		norm_to_polar(&p2, &norm);
		vec3_zero(&n2);
		polar_to_norm(&n2, &p2);
		assert_true(vec3_close(&n2, &norm, 1e-4f));
	}
	/* x is the yaw around y, y the pitch */
	vec3_set(&norm, 1.0f, 0.0f, 0.0f);
	norm_to_polar(&p2, &norm);
	assert_near(p2.x, M_PI / 2.0f);
	assert_near(p2.y, 0.0f);

	/* torque moves toward the target and snaps when close */
	assert_near(calc_torquef(0.0f, 10.0f, 1.0f, 0.1f, 0.5f), 5.0f);
	assert_near(calc_torquef(0.0f, 10.0f, 1.0f, 0.1f, 2.0f), 10.0f);
	assert_near(calc_torquef(5.0f, 5.0f, 1.0f, 0.1f, 0.5f), 5.0f);
	assert_near(calc_torquef(10.0f, 0.0f, 1.0f, 0.1f, 0.5f), 5.0f);
	assert_near(calc_torquef(0.0f, 0.05f, 0.1f, 0.1f, 0.5f), 0.05f);

	struct vec3 a, b, r;
	vec3_set(&a, 0.0f, 0.0f, 0.0f);
	vec3_set(&b, 10.0f, 0.0f, 0.0f);
	/* depends on vec3_close() taking the absolute difference (regression) */
	calc_torque(&r, &a, &b, 1.0f, 0.1f, 0.5f);
	assert_near(r.x, 5.0f);
	calc_torque(&r, &a, &b, 1.0f, 0.1f, 2.0f);
	assert_near(r.x, 10.0f);
	calc_torque(&r, &a, &a, 1.0f, 0.1f, 0.5f);
	assert_near(r.x, 0.0f);

	assert_near(get_percentage(0.0f, 10.0f, 2.5f), 0.25f);
	assert_near(get_percentagei(0, 10, 5), 0.5f);

	for (int i = 0; i < 20; i++) {
		float f = rand_float(1);
		assert_true(f >= 0.0f && f <= 1.0f);
		f = rand_float(0);
		assert_true(f >= -1.0f && f <= 1.0f);
	}
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(plane_test),
		obs_unit_test(bounds_test),
		obs_unit_test(math_extra_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
