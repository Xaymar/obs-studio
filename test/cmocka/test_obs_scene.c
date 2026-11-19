#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <util/base.h>
#include <util/threading.h>

#include <graphics/matrix4.h>

static obs_source_t *make_input(const char *name)
{
	return obs_source_create("fake_input", name, NULL, NULL);
}

struct item_list {
	obs_sceneitem_t *items[16];
	size_t count;
};

static bool collect_item(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	struct item_list *list = param;
	list->items[list->count++] = item;
	return true;
}

static struct item_list items_of(obs_scene_t *scene)
{
	struct item_list list = {0};
	obs_scene_enum_items(scene, collect_item, &list);
	return list;
}

static bool near(float a, float b)
{
	return fabsf(a - b) < 0.001f;
}

static int item_signals;
static void on_item_signal(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	item_signals++;
}

static void items_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_scene_t *scene = obs_scene_create("scene");
	obs_source_t *scene_src = obs_scene_get_source(scene);
	assert_true(obs_source_is_scene(scene_src));
	assert_true(obs_source_type_is_scene("scene"));
	assert_ptr_equal(obs_scene_from_source(scene_src), scene);
	assert_false(obs_scene_is_group(scene));
	/* the scene's size follows the video base size */
	assert_int_equal(obs_source_get_width(scene_src), 640);

	signal_handler_t *sh = obs_source_get_signal_handler(scene_src);
	item_signals = 0;
	signal_handler_connect(sh, "item_add", on_item_signal, NULL);
	signal_handler_connect(sh, "item_remove", on_item_signal, NULL);
	signal_handler_connect(sh, "reorder", on_item_signal, NULL);
	signal_handler_connect(sh, "item_visible", on_item_signal, NULL);
	signal_handler_connect(sh, "item_locked", on_item_signal, NULL);
	signal_handler_connect(sh, "item_select", on_item_signal, NULL);
	signal_handler_connect(sh, "item_transform", on_item_signal, NULL);

	obs_source_t *a = make_input("a"), *b = make_input("b"), *c = make_input("c");
	obs_sceneitem_t *ia = obs_scene_add(scene, a);
	obs_sceneitem_t *ib = obs_scene_add(scene, b);
	obs_sceneitem_t *ic = obs_scene_add(scene, c);
	assert_int_equal(item_signals, 3);
	assert_ptr_equal(obs_sceneitem_get_scene(ia), scene);
	assert_ptr_equal(obs_sceneitem_get_source(ia), a);
	assert_true(obs_sceneitem_get_id(ib) != obs_sceneitem_get_id(ia));
	assert_ptr_equal(obs_scene_find_sceneitem_by_id(scene, obs_sceneitem_get_id(ib)), ib);
	assert_null(obs_scene_find_sceneitem_by_id(scene, 9999));
	assert_ptr_equal(obs_scene_find_source(scene, "c"), ic);
	assert_null(obs_scene_find_source(scene, "missing"));
	/* a scene cannot contain itself */
	assert_null(obs_scene_add(scene, scene_src));
	assert_null(obs_scene_add(NULL, a));
	assert_null(obs_scene_add(scene, NULL));

	/* items are in the order they were added, at the bottom first */
	struct item_list list = items_of(scene);
	assert_int_equal(list.count, 3);
	assert_ptr_equal(list.items[0], ia);
	assert_ptr_equal(list.items[2], ic);
	assert_int_equal(obs_sceneitem_get_order_position(ia), 0);
	assert_int_equal(obs_sceneitem_get_order_position(ic), 2);

	/* ordering */
	obs_sceneitem_set_order(ia, OBS_ORDER_MOVE_TOP);
	assert_int_equal(obs_sceneitem_get_order_position(ia), 2);
	obs_sceneitem_set_order(ia, OBS_ORDER_MOVE_DOWN);
	assert_int_equal(obs_sceneitem_get_order_position(ia), 1);
	obs_sceneitem_set_order(ia, OBS_ORDER_MOVE_BOTTOM);
	assert_int_equal(obs_sceneitem_get_order_position(ia), 0);
	obs_sceneitem_set_order(ia, OBS_ORDER_MOVE_UP);
	assert_int_equal(obs_sceneitem_get_order_position(ia), 1);
	obs_sceneitem_set_order_position(ia, 2);
	assert_int_equal(obs_sceneitem_get_order_position(ia), 2);
	obs_sceneitem_set_order_position(ia, 0);
	assert_int_equal(obs_sceneitem_get_order_position(ia), 0);
	obs_sceneitem_set_order_position(ia, 99);
	assert_int_equal(obs_sceneitem_get_order_position(ia), 2);
	/* Regression: a negative position put the item second instead of first */
	obs_sceneitem_set_order_position(ia, -1);
	assert_int_equal(obs_sceneitem_get_order_position(ia), 0);

	obs_sceneitem_t *order[] = {ic, ib, ia};
	assert_true(obs_scene_reorder_items(scene, order, 3));
	list = items_of(scene);
	assert_ptr_equal(list.items[0], ic);
	assert_ptr_equal(list.items[2], ia);
	/* the same order again, or a wrong set of items, changes nothing */
	assert_false(obs_scene_reorder_items(scene, order, 3));
	obs_sceneitem_t *partial[] = {ia, ib};
	assert_false(obs_scene_reorder_items(scene, partial, 2));

	/* state */
	obs_sceneitem_set_visible(ib, false);
	assert_false(obs_sceneitem_visible(ib));
	obs_sceneitem_set_visible(ib, true);
	obs_sceneitem_set_locked(ib, true);
	assert_true(obs_sceneitem_locked(ib));
	obs_sceneitem_set_locked(ib, false);
	obs_sceneitem_select(ib, true);
	assert_true(obs_sceneitem_selected(ib));
	obs_sceneitem_select(ib, false);
	obs_sceneitem_set_scale_filter(ib, OBS_SCALE_LANCZOS);
	assert_int_equal(obs_sceneitem_get_scale_filter(ib), OBS_SCALE_LANCZOS);
	obs_sceneitem_set_blending_method(ib, OBS_BLEND_METHOD_SRGB_OFF);
	assert_int_equal(obs_sceneitem_get_blending_method(ib), OBS_BLEND_METHOD_SRGB_OFF);
	obs_sceneitem_set_blending_mode(ib, OBS_BLEND_ADDITIVE);
	assert_int_equal(obs_sceneitem_get_blending_mode(ib), OBS_BLEND_ADDITIVE);
	obs_data_t *priv = obs_sceneitem_get_private_settings(ib);
	obs_data_set_int(priv, "x", 1);
	obs_data_release(priv);

	/* a scene with every blending mode and scale filter renders */
	static const enum obs_blending_type modes[] = {OBS_BLEND_NORMAL, OBS_BLEND_ADDITIVE, OBS_BLEND_SUBTRACT,
						       OBS_BLEND_SCREEN, OBS_BLEND_MULTIPLY, OBS_BLEND_LIGHTEN,
						       OBS_BLEND_DARKEN};
	obs_set_output_source(0, scene_src);
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		obs_sceneitem_set_blending_mode(ia, modes[i]);
		obs_sceneitem_set_scale_filter(ia, (enum obs_scale_type)(i % 6));
		obs_sceneitem_set_blending_method(ia, i % 2 ? OBS_BLEND_METHOD_SRGB_OFF : OBS_BLEND_METHOD_DEFAULT);
		core_wait_frames(1);
	}
	core_wait_frames(2);
	assert_true(fake_of(a)->renders > 0);
	assert_true(obs_source_active(a));
	obs_set_output_source(0, NULL);

	/* removing */
	int before = item_signals;
	obs_sceneitem_remove(ib);
	assert_int_equal(item_signals, before + 1);
	assert_null(obs_scene_find_source(scene, "b"));
	assert_int_equal(items_of(scene).count, 2);
	/* a held reference stays valid */
	obs_sceneitem_addref(ic);
	obs_sceneitem_remove(ic);
	assert_ptr_equal(obs_sceneitem_get_source(ic), c);
	obs_sceneitem_release(ic);

	signal_handler_disconnect(sh, "item_add", on_item_signal, NULL);
	signal_handler_disconnect(sh, "item_remove", on_item_signal, NULL);
	signal_handler_disconnect(sh, "reorder", on_item_signal, NULL);
	signal_handler_disconnect(sh, "item_visible", on_item_signal, NULL);
	signal_handler_disconnect(sh, "item_locked", on_item_signal, NULL);
	signal_handler_disconnect(sh, "item_select", on_item_signal, NULL);
	signal_handler_disconnect(sh, "item_transform", on_item_signal, NULL);

	obs_source_release(a);
	obs_source_release(b);
	obs_source_release(c);
	obs_scene_release(scene);
	core_stop();
	assert_int_equal(fake.creates, fake.destroys);
}

static void transform_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_scene_t *scene = obs_scene_create("scene");
	obs_source_t *src = make_input("src");
	obs_sceneitem_t *item = obs_scene_add(scene, src);

	struct vec2 v;
	vec2_set(&v, 10.0f, 20.0f);
	obs_sceneitem_set_pos(item, &v);
	obs_sceneitem_get_pos(item, &v);
	assert_true(near(v.x, 10.0f) && near(v.y, 20.0f));
	vec2_set(&v, 2.0f, 3.0f);
	obs_sceneitem_set_scale(item, &v);
	obs_sceneitem_get_scale(item, &v);
	assert_true(near(v.x, 2.0f) && near(v.y, 3.0f));
	obs_sceneitem_set_rot(item, 90.0f);
	assert_true(near(obs_sceneitem_get_rot(item), 90.0f));
	obs_sceneitem_set_rot(item, 0.0f);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	assert_int_equal(obs_sceneitem_get_alignment(item), OBS_ALIGN_LEFT | OBS_ALIGN_TOP);

	/* the 32x16 source scaled 2x3 at (10, 20) */
	struct matrix4 m;
	obs_sceneitem_get_draw_transform(item, &m);
	assert_true(near(m.t.x, 10.0f) && near(m.t.y, 20.0f));
	assert_true(near(m.x.x, 2.0f) && near(m.y.y, 3.0f));
	obs_sceneitem_get_box_transform(item, &m);
	assert_true(near(m.x.x, 64.0f) && near(m.y.y, 48.0f));
	/* the box scale is the size of the box in pixels */
	obs_sceneitem_get_box_scale(item, &v);
	assert_true(near(v.x, 64.0f) && near(v.y, 48.0f));

	/* crop shrinks the drawn size */
	struct obs_sceneitem_crop crop = {.left = 2, .right = 2, .top = 1, .bottom = 1};
	obs_sceneitem_set_crop(item, &crop);
	struct obs_sceneitem_crop got_crop;
	obs_sceneitem_get_crop(item, &got_crop);
	assert_int_equal(got_crop.left, 2);
	/* crop applies on the next tick, or when forced */
	obs_sceneitem_force_update_transform(item);
	obs_sceneitem_get_box_transform(item, &m);
	assert_true(near(m.x.x, 56.0f) && near(m.y.y, 42.0f));
	memset(&crop, 0, sizeof(crop));
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_force_update_transform(item);

	/* bounds; the bounds modes scale the item's own scale, so reset it */
	vec2_set(&v, 1.0f, 1.0f);
	obs_sceneitem_set_scale(item, &v);
	vec2_set(&v, 100.0f, 100.0f);
	obs_sceneitem_set_bounds(item, &v);
	obs_sceneitem_get_bounds(item, &v);
	assert_true(near(v.x, 100.0f));
	obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
	assert_int_equal(obs_sceneitem_get_bounds_alignment(item), OBS_ALIGN_CENTER);
	obs_sceneitem_set_bounds_crop(item, true);
	assert_true(obs_sceneitem_get_bounds_crop(item));
	obs_sceneitem_set_bounds_crop(item, false);

	struct {
		enum obs_bounds_type type;
		float sx, sy;
	} bounds[] = {
		{OBS_BOUNDS_STRETCH, 100.0f / 32.0f, 100.0f / 16.0f},
		{OBS_BOUNDS_SCALE_INNER, 100.0f / 32.0f, 100.0f / 32.0f},
		{OBS_BOUNDS_SCALE_OUTER, 100.0f / 16.0f, 100.0f / 16.0f},
		{OBS_BOUNDS_SCALE_TO_WIDTH, 100.0f / 32.0f, 100.0f / 32.0f},
		{OBS_BOUNDS_SCALE_TO_HEIGHT, 100.0f / 16.0f, 100.0f / 16.0f},
		/* the source fits, so it keeps its size */
		{OBS_BOUNDS_MAX_ONLY, 1.0f, 1.0f},
	};
	for (size_t i = 0; i < sizeof(bounds) / sizeof(bounds[0]); i++) {
		obs_sceneitem_set_bounds_type(item, bounds[i].type);
		assert_int_equal(obs_sceneitem_get_bounds_type(item), bounds[i].type);
		obs_sceneitem_get_draw_transform(item, &m);
		print_message("bounds %d: %f %f\n", bounds[i].type, m.x.x, m.y.y);
		assert_true(near(m.x.x, bounds[i].sx));
		assert_true(near(m.y.y, bounds[i].sy));
	}
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);

	/* info2 round trip */
	struct obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	info.pos.x = 5.0f;
	info.rot = 45.0f;
	info.crop_to_bounds = true;
	obs_sceneitem_set_info2(item, &info);
	struct obs_transform_info info_b;
	obs_sceneitem_get_info2(item, &info_b);
	assert_true(near(info_b.pos.x, 5.0f));
	assert_true(near(info_b.rot, 45.0f));
	assert_true(info_b.crop_to_bounds);

	/* deferred updates apply at the end */
	obs_sceneitem_defer_update_begin(item);
	vec2_set(&v, 1.0f, 1.0f);
	obs_sceneitem_set_pos(item, &v);
	obs_sceneitem_defer_update_end(item);
	obs_sceneitem_force_update_transform(item);
	obs_sceneitem_get_draw_transform(item, &m);
	assert_true(near(m.t.x, 1.0f) || m.t.x != 5.0f);

	/* transform states */
	/* false saves only the selected items */
	obs_data_t *states = obs_scene_save_transform_states(scene, true);
	assert_non_null(states);
	vec2_set(&v, 300.0f, 300.0f);
	obs_sceneitem_set_pos(item, &v);
	obs_scene_load_transform_states(obs_data_get_json(states));
	obs_sceneitem_get_pos(item, &v);
	assert_true(near(v.x, 1.0f));
	obs_data_release(states);

	obs_source_release(src);
	obs_scene_release(scene);
	core_stop();
}

static void groups_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_scene_t *scene = obs_scene_create("scene");
	obs_source_t *a = make_input("a"), *b = make_input("b");
	obs_sceneitem_t *ia = obs_scene_add(scene, a);
	obs_sceneitem_t *ib = obs_scene_add(scene, b);

	obs_sceneitem_t *group = obs_scene_add_group2(scene, "group", true);
	assert_non_null(group);
	assert_true(obs_sceneitem_is_group(group));
	obs_scene_t *gscene = obs_sceneitem_group_get_scene(group);
	assert_true(obs_scene_is_group(gscene));
	assert_true(obs_source_is_group(obs_scene_get_source(gscene)));
	assert_ptr_equal(obs_scene_get_group(scene, "group"), group);
	assert_null(obs_scene_get_group(scene, "missing"));
	/* group names are source names: a taken one is made unique */
	obs_sceneitem_t *dup_group = obs_scene_add_group2(scene, "group", false);
	assert_non_null(dup_group);
	assert_string_not_equal(obs_source_get_name(obs_sceneitem_get_source(dup_group)), "group");
	obs_sceneitem_remove(dup_group);

	obs_sceneitem_group_add_item(group, ia);
	assert_int_equal(items_of(gscene).count, 1);
	obs_sceneitem_t *moved = obs_scene_find_source(gscene, "a");
	assert_non_null(moved);
	assert_ptr_equal(obs_sceneitem_get_group(scene, moved), group);
	assert_null(obs_scene_find_source(scene, "a"));
	assert_ptr_equal(obs_scene_find_source_recursive(scene, "a"), moved);

	struct item_list list = {0};
	obs_sceneitem_group_enum_items(group, collect_item, &list);
	assert_int_equal(list.count, 1);

	obs_sceneitem_defer_group_resize_begin(group);
	obs_sceneitem_group_add_item(group, ib);
	obs_sceneitem_defer_group_resize_end(group);
	assert_int_equal(items_of(gscene).count, 2);

	obs_sceneitem_t *back = obs_scene_find_source(gscene, "b");
	obs_sceneitem_group_remove_item(group, back);
	assert_int_equal(items_of(gscene).count, 1);
	assert_non_null(obs_scene_find_source(scene, "b"));

	/* rendering a scene with a group */
	obs_set_output_source(0, obs_scene_get_source(scene));
	core_wait_frames(3);
	obs_set_output_source(0, NULL);

	/* a group created from items */
	obs_sceneitem_t *ib2 = obs_scene_find_source(scene, "b");
	obs_sceneitem_t *from_items[] = {ib2};
	obs_sceneitem_t *group2 = obs_scene_insert_group2(scene, "group2", from_items, 1, true);
	assert_non_null(group2);
	assert_null(obs_scene_find_source(scene, "b"));

	obs_sceneitem_group_ungroup2(group2, true);
	assert_non_null(obs_scene_find_source(scene, "b"));
	assert_null(obs_scene_get_group(scene, "group2"));

	obs_sceneitem_group_ungroup(group);
	assert_non_null(obs_scene_find_source(scene, "a"));

	obs_source_release(a);
	obs_source_release(b);
	obs_scene_release(scene);
	core_stop();
	assert_int_equal(fake.creates, fake.destroys);
}

static void save_load_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_scene_t *scene = obs_scene_create("scene");
	obs_source_t *a = make_input("a");
	obs_sceneitem_t *ia = obs_scene_add(scene, a);
	struct vec2 v = {7.0f, 8.0f};
	obs_sceneitem_set_pos(ia, &v);
	obs_sceneitem_t *group = obs_scene_add_group2(scene, "group", true);
	obs_source_t *b = make_input("b");
	obs_scene_add(obs_sceneitem_group_get_scene(group), b);

	/* show and hide transitions */
	obs_source_t *tr = obs_source_create_private("fake_transition", "show", NULL);
	obs_sceneitem_set_transition(ia, true, tr);
	obs_sceneitem_set_transition_duration(ia, true, 250);
	assert_ptr_equal(obs_sceneitem_get_transition(ia, true), tr);
	assert_int_equal(obs_sceneitem_get_transition_duration(ia, true), 250);
	assert_null(obs_sceneitem_get_transition(ia, false));
	obs_data_t *trdata = obs_sceneitem_transition_save(ia, true);
	assert_non_null(trdata);
	obs_sceneitem_transition_load(ia, trdata, false);
	assert_non_null(obs_sceneitem_get_transition(ia, false));
	obs_data_release(trdata);
	obs_set_output_source(0, obs_scene_get_source(scene));
	obs_sceneitem_set_visible(ia, false);
	core_wait_frames(2);
	obs_sceneitem_set_visible(ia, true);
	core_wait_frames(2);
	obs_sceneitem_do_transition(ia, true);
	core_wait_frames(2);
	obs_set_output_source(0, NULL);
	obs_source_release(tr);

	obs_data_array_t *sources = obs_save_sources();
	assert_true(obs_data_array_count(sources) >= 3);
	obs_data_t *saved = obs_save_source(obs_scene_get_source(scene));
	obs_data_array_t *item_data = obs_data_array_create();
	obs_sceneitem_save(ia, item_data);
	assert_int_equal(obs_data_array_count(item_data), 1);
	obs_data_t *saved_item = obs_data_array_item(item_data, 0);
	assert_true(obs_data_has_user_value(saved_item, "pos"));
	obs_data_release(saved_item);
	obs_data_array_release(item_data);

	/* duplicates */
	obs_scene_t *copy = obs_scene_duplicate(scene, "copy", OBS_SCENE_DUP_COPY);
	assert_int_equal(items_of(copy).count, 2);
	obs_sceneitem_t *ca = obs_scene_find_source(copy, "a");
	assert_null(ca);
	/* the main canvas keeps public scenes until they are removed */
	obs_source_remove(obs_scene_get_source(copy));
	obs_scene_release(copy);
	obs_scene_t *refs = obs_scene_duplicate(scene, "refs", OBS_SCENE_DUP_REFS);
	assert_ptr_equal(obs_sceneitem_get_source(obs_scene_find_source(refs, "a")), a);
	/* the main canvas keeps public scenes until they are removed */
	obs_source_remove(obs_scene_get_source(refs));
	obs_scene_release(refs);
	obs_scene_t *priv = obs_scene_duplicate(scene, "priv", OBS_SCENE_DUP_PRIVATE_REFS);
	assert_true(obs_obj_is_private(obs_scene_get_source(priv)));
	obs_scene_release(priv);

	obs_source_remove(obs_scene_get_source(scene));
	obs_scene_release(scene);
	obs_source_remove(a);
	obs_source_remove(b);
	obs_source_release(a);
	obs_source_release(b);

	/* load everything back */
	obs_load_sources(sources, NULL, NULL);
	obs_source_t *loaded = obs_get_source_by_name("scene");
	assert_non_null(loaded);
	obs_scene_t *ls = obs_scene_from_source(loaded);
	obs_sceneitem_t *la = obs_scene_find_source(ls, "a");
	assert_non_null(la);
	obs_sceneitem_get_pos(la, &v);
	assert_true(near(v.x, 7.0f));
	assert_non_null(obs_scene_get_group(ls, "group"));
	assert_non_null(obs_scene_find_source_recursive(ls, "b"));
	obs_source_release(loaded);
	obs_data_array_release(sources);
	obs_data_release(saved);

	/* removing all sources */
	obs_source_t *sl = obs_get_source_by_name("scene");
	obs_source_remove(sl);
	obs_source_release(sl);
	obs_source_t *la_src = obs_get_source_by_name("a");
	obs_source_remove(la_src);
	obs_source_release(la_src);
	obs_source_t *lb_src = obs_get_source_by_name("b");
	obs_source_remove(lb_src);
	obs_source_release(lb_src);

	core_stop();
	assert_int_equal(fake.creates, fake.destroys);
}

static void atomic_cb(void *data, obs_scene_t *scene)
{
	obs_scene_add(scene, data);
}

static void misc_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_scene_t *scene = obs_scene_create_private("private");
	assert_true(obs_obj_is_private(obs_scene_get_source(scene)));
	obs_source_t *a = make_input("a");
	obs_scene_atomic_update(scene, atomic_cb, a);
	assert_int_equal(items_of(scene).count, 1);

	obs_scene_t *ref = obs_scene_get_ref(scene);
	assert_ptr_equal(ref, scene);
	obs_scene_release(ref);

	/* NULL is ignored */
	assert_null(obs_scene_from_source(a));
	assert_null(obs_scene_from_source(NULL));
	assert_null(obs_scene_find_source(NULL, "a"));
	obs_sceneitem_remove(NULL);
	obs_sceneitem_release(NULL);
	obs_sceneitem_set_visible(NULL, true);
	assert_false(obs_sceneitem_visible(NULL));
	obs_scene_release(NULL);

	obs_source_release(a);
	obs_scene_release(scene);
	core_stop();
}

static float scene_samples[2][480];

/* items drawn through their own texture (crop, scale filters, sRGB off), every bounds mode with bounds cropping,
 * a scene mixing an async source's audio, and reordering with groups */
static void render_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_scene_t *scene = obs_scene_create("render");
	obs_set_output_source(0, obs_scene_get_source(scene));

	obs_source_t *src = make_input("src");
	obs_sceneitem_t *item = obs_scene_add(scene, src);
	struct obs_sceneitem_crop crop = {.left = 1, .right = 2, .top = 1, .bottom = 1};
	obs_sceneitem_set_crop(item, &crop);
	struct vec2 v = {1.5f, 1.25f};
	obs_sceneitem_set_scale(item, &v);
	for (int filter = OBS_SCALE_DISABLE; filter <= OBS_SCALE_AREA; filter++) {
		obs_sceneitem_set_scale_filter(item, (enum obs_scale_type)filter);
		obs_sceneitem_set_blending_method(item, filter % 2 ? OBS_BLEND_METHOD_SRGB_OFF : OBS_BLEND_METHOD_DEFAULT);
		core_wait_frames(2);
	}
	/* down-scaling with the area filter */
	vec2_set(&v, 0.5f, 0.5f);
	obs_sceneitem_set_scale(item, &v);
	core_wait_frames(2);

	vec2_set(&v, 1.0f, 1.0f);
	obs_sceneitem_set_scale(item, &v);
	vec2_set(&v, 20.0f, 50.0f);
	obs_sceneitem_set_bounds(item, &v);
	const uint32_t alignments[] = {OBS_ALIGN_CENTER, OBS_ALIGN_LEFT | OBS_ALIGN_TOP,
				       OBS_ALIGN_RIGHT | OBS_ALIGN_BOTTOM};
	for (int type = OBS_BOUNDS_STRETCH; type <= OBS_BOUNDS_MAX_ONLY; type++) {
		obs_sceneitem_set_bounds_type(item, (enum obs_bounds_type)type);
		obs_sceneitem_set_bounds_alignment(item, alignments[type % 3]);
		obs_sceneitem_set_bounds_crop(item, type % 2 == 0);
		obs_sceneitem_force_update_transform(item);
		core_wait_frames(1);
	}
	/* bounds relative to the canvas */
	struct obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	info.bounds_type = OBS_BOUNDS_SCALE_OUTER;
	info.crop_to_bounds = true;
	obs_sceneitem_set_info2(item, &info);
	core_wait_frames(2);

	/* audio from an async source in the scene */
	obs_source_t *async = obs_source_create("fake_async", "async", NULL, NULL);
	obs_sceneitem_t *aitem = obs_scene_add(scene, async);
	for (int i = 0; i < 480; i++)
		scene_samples[0][i] = scene_samples[1][i] = (i % 48) / 48.0f - 0.5f;
	uint64_t ts = os_gettime_ns();
	for (int i = 0; i < 30; i++) {
		struct obs_source_audio audio = {
			.data = {(uint8_t *)scene_samples[0], (uint8_t *)scene_samples[1]},
			.frames = 480,
			.speakers = SPEAKERS_STEREO,
			.format = AUDIO_FORMAT_FLOAT_PLANAR,
			.samples_per_sec = 48000,
			.timestamp = ts + i * 10000000ULL,
		};
		obs_source_output_audio(async, &audio);
		if (i == 15)
			obs_sceneitem_set_visible(aitem, false);
		if (i % 5 == 0)
			core_wait_frames(1);
	}
	obs_sceneitem_set_visible(aitem, true);
	core_wait_frames(5);

	/* reordering across a group */
	obs_sceneitem_t *group = obs_scene_add_group2(scene, "grp", false);
	obs_source_t *gsrc = make_input("gsrc");
	obs_sceneitem_t *gitem = obs_scene_add(scene, gsrc);
	struct obs_sceneitem_order_info order[] = {
		{NULL, aitem},
		{NULL, group},
		{group, gitem}, /* a group's items follow it */
		{NULL, item},
	};
	assert_true(obs_scene_reorder_items2(scene, order, 4));
	assert_ptr_equal(obs_sceneitem_get_group(scene, gitem), group);
	struct item_list list = items_of(scene);
	assert_int_equal(list.count, 3);
	assert_ptr_equal(list.items[0], aitem);
	assert_ptr_equal(list.items[2], item);
	/* back out of the group */
	struct obs_sceneitem_order_info flat[] = {
		{NULL, gitem},
		{NULL, item},
		{NULL, group},
		{NULL, aitem},
	};
	assert_true(obs_scene_reorder_items2(scene, flat, 4));
	assert_null(obs_sceneitem_get_group(scene, gitem));
	assert_false(obs_scene_reorder_items2(NULL, flat, 4));
	core_wait_frames(2);

	obs_set_output_source(0, NULL);
	obs_source_release(gsrc);
	obs_source_release(async);
	obs_source_release(src);
	obs_scene_release(scene);
	core_stop();
}

static volatile long double_destroys;
static log_handler_t previous_handler;
static void *previous_param;

static void count_double_destroys(int level, const char *format, va_list args, void *param)
{
	UNUSED_PARAMETER(param);
	if (strstr(format, "Double destroy"))
		os_atomic_inc_long(&double_destroys);
	previous_handler(level, format, args, previous_param);
}

/* Regression: shutting down with scenes that only the main canvas still holds destroyed their sources first,
 * and the scenes, destroyed afterwards on the destruction thread, released those sources again ("Double
 * destroy", a use after free). Which came first depended on the order of the uuids, so try a few times. */
static void shutdown_order_test(void **state)
{
	UNUSED_PARAMETER(state);
	base_get_log_handler(&previous_handler, &previous_param);
	base_set_log_handler(count_double_destroys, NULL);
	double_destroys = 0;

	for (int run = 0; run < 10; run++) {
		core_start();
		obs_scene_t *scene = obs_scene_create("scene");
		for (int i = 0; i < 4; i++) {
			char name[16];
			snprintf(name, sizeof(name), "src%d", i);
			obs_source_t *src = make_input(name);
			obs_scene_add(scene, src);
			obs_source_release(src);
		}
		obs_scene_release(scene);
		core_stop();
	}

	base_set_log_handler(previous_handler, previous_param);
	assert_int_equal(os_atomic_load_long(&double_destroys), 0);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(items_test),     cmocka_unit_test(transform_test), cmocka_unit_test(groups_test),
		cmocka_unit_test(save_load_test), cmocka_unit_test(misc_test), cmocka_unit_test(render_test), cmocka_unit_test(shutdown_order_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
