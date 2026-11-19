#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <obs-data.h>
#include <obs-properties.h>

static int param_destroyed;
static void destroy_param(void *param)
{
	UNUSED_PARAMETER(param);
	param_destroyed++;
}

static void basics_test(void **state)
{
	UNUSED_PARAMETER(state);

	int param = 0;
	param_destroyed = 0;
	obs_properties_t *props = obs_properties_create_param(&param, destroy_param);
	assert_ptr_equal(obs_properties_get_param(props), &param);
	/* replacing the param destroys the old one */
	obs_properties_set_param(props, &param, destroy_param);
	assert_int_equal(param_destroyed, 1);
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);
	assert_int_equal(obs_properties_get_flags(props), OBS_PROPERTIES_DEFER_UPDATE);
	assert_null(obs_properties_first(props));
	assert_null(obs_properties_get_parent(props));

	obs_property_t *b = obs_properties_add_bool(props, "b", "Bool");
	assert_non_null(b);
	/* names are unique */
	assert_null(obs_properties_add_bool(props, "b", "Again"));
	assert_string_equal(obs_property_name(b), "b");
	assert_string_equal(obs_property_description(b), "Bool");
	assert_int_equal(obs_property_get_type(b), OBS_PROPERTY_BOOL);
	assert_true(obs_property_enabled(b));
	assert_true(obs_property_visible(b));
	obs_property_set_enabled(b, false);
	obs_property_set_visible(b, false);
	assert_false(obs_property_enabled(b));
	assert_false(obs_property_visible(b));
	obs_property_set_description(b, "New");
	assert_string_equal(obs_property_description(b), "New");
	assert_null(obs_property_long_description(b));
	obs_property_set_long_description(b, "Long");
	assert_string_equal(obs_property_long_description(b), "Long");

	assert_non_null(obs_properties_add_color(props, "c", "Color"));
	assert_non_null(obs_properties_add_color_alpha(props, "ca", "Color alpha"));
	assert_non_null(obs_properties_add_font(props, "f", "Font"));

	int count = 0;
	obs_property_t *p = obs_properties_first(props);
	do {
		count++;
	} while (obs_property_next(&p));
	assert_int_equal(count, 4);
	assert_false(obs_property_next(NULL));

	assert_ptr_equal(obs_properties_get(props, "c"), obs_properties_get(props, "c"));
	assert_null(obs_properties_get(props, "missing"));
	obs_properties_remove_by_name(props, "c");
	assert_null(obs_properties_get(props, "c"));
	obs_properties_remove_by_name(props, "missing");

	obs_properties_destroy(props);
	assert_int_equal(param_destroyed, 2);

	/* NULL is ignored everywhere */
	obs_properties_destroy(NULL);
	assert_null(obs_properties_first(NULL));
	assert_null(obs_properties_get(NULL, "x"));
	assert_null(obs_properties_get_param(NULL));
	assert_int_equal(obs_properties_get_flags(NULL), 0);
	assert_null(obs_properties_add_bool(NULL, "x", "x"));
	assert_null(obs_property_name(NULL));
	assert_int_equal(obs_property_get_type(NULL), OBS_PROPERTY_INVALID);
	assert_false(obs_property_enabled(NULL));
	assert_false(obs_property_modified(NULL, NULL));
	assert_false(obs_property_button_clicked(NULL, NULL));
	/* Regression: obs_properties_get_parent(NULL) dereferenced NULL */
	assert_null(obs_properties_get_parent(NULL));
}

static void null_names_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* Regression: a NULL name reached strlen() in the hash lookups */
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_bool(props, "b", "Bool");
	assert_null(obs_properties_add_bool(props, NULL, "x"));
	assert_null(obs_properties_add_int(props, NULL, "x", 0, 1, 1));
	assert_null(obs_properties_get(props, NULL));
	obs_properties_remove_by_name(props, NULL);
	assert_non_null(obs_properties_get(props, "b"));
	obs_properties_destroy(props);
}

static void numbers_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *i = obs_properties_add_int(props, "i", "Int", -5, 10, 2);
	assert_int_equal(obs_property_int_min(i), -5);
	assert_int_equal(obs_property_int_max(i), 10);
	assert_int_equal(obs_property_int_step(i), 2);
	assert_int_equal(obs_property_int_type(i), OBS_NUMBER_SCROLLER);
	assert_null(obs_property_int_suffix(i));
	obs_property_int_set_limits(i, 0, 100, 5);
	obs_property_int_set_suffix(i, " px");
	assert_int_equal(obs_property_int_max(i), 100);
	assert_string_equal(obs_property_int_suffix(i), " px");

	obs_property_t *is = obs_properties_add_int_slider(props, "is", "Slider", 0, 1, 1);
	assert_int_equal(obs_property_int_type(is), OBS_NUMBER_SLIDER);

	obs_property_t *f = obs_properties_add_float(props, "f", "Float", 0.5, 1.5, 0.25);
	assert_true(obs_property_float_min(f) == 0.5);
	assert_true(obs_property_float_max(f) == 1.5);
	assert_true(obs_property_float_step(f) == 0.25);
	assert_int_equal(obs_property_float_type(f), OBS_NUMBER_SCROLLER);
	obs_property_float_set_limits(f, -1.0, 1.0, 0.1);
	obs_property_float_set_suffix(f, " s");
	assert_true(obs_property_float_min(f) == -1.0);
	assert_string_equal(obs_property_float_suffix(f), " s");

	obs_property_t *fs = obs_properties_add_float_slider(props, "fs", "Slider", 0, 1, 0.1);
	assert_int_equal(obs_property_float_type(fs), OBS_NUMBER_SLIDER);

	/* getters of the wrong type return zero */
	assert_int_equal(obs_property_int_min(f), 0);
	assert_true(obs_property_float_min(i) == 0.0);
	assert_null(obs_property_int_suffix(f));
	obs_property_int_set_limits(f, 1, 2, 3);
	obs_property_float_set_limits(i, 1, 2, 3);
	assert_int_equal(obs_property_int_max(i), 100);

	obs_properties_destroy(props);
}

static void text_path_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *t = obs_properties_add_text(props, "t", "Text", OBS_TEXT_MULTILINE);
	assert_int_equal(obs_property_text_type(t), OBS_TEXT_MULTILINE);
	assert_false(obs_property_text_monospace(t));
	obs_property_text_set_monospace(t, true);
	assert_true(obs_property_text_monospace(t));

	obs_property_t *info = obs_properties_add_text(props, "info", "Info", OBS_TEXT_INFO);
	assert_int_equal(obs_property_text_info_type(info), OBS_TEXT_INFO_NORMAL);
	obs_property_text_set_info_type(info, OBS_TEXT_INFO_ERROR);
	assert_int_equal(obs_property_text_info_type(info), OBS_TEXT_INFO_ERROR);
	assert_true(obs_property_text_info_word_wrap(info));
	obs_property_text_set_info_word_wrap(info, false);
	assert_false(obs_property_text_info_word_wrap(info));

	obs_property_t *path = obs_properties_add_path(props, "p", "Path", OBS_PATH_FILE, "*.png", "C:/");
	assert_int_equal(obs_property_path_type(path), OBS_PATH_FILE);
	assert_string_equal(obs_property_path_filter(path), "*.png");
	assert_string_equal(obs_property_path_default_path(path), "C:/");

	obs_property_t *el =
		obs_properties_add_editable_list(props, "el", "List", OBS_EDITABLE_LIST_TYPE_FILES, "*.txt", "D:/");
	assert_int_equal(obs_property_editable_list_type(el), OBS_EDITABLE_LIST_TYPE_FILES);
	assert_string_equal(obs_property_editable_list_filter(el), "*.txt");
	assert_string_equal(obs_property_editable_list_default_path(el), "D:/");

	/* wrong types */
	assert_null(obs_property_path_filter(t));
	assert_null(obs_property_editable_list_filter(path));
	assert_false(obs_property_text_monospace(path));

	obs_properties_destroy(props);
}

static void list_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *s = obs_properties_add_list(props, "s", "S", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	/* editable lists must hold strings */
	assert_null(obs_properties_add_list(props, "e", "E", OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_INT));
	obs_property_t *e = obs_properties_add_list(props, "e", "E", OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	assert_int_equal(obs_property_list_type(e), OBS_COMBO_TYPE_EDITABLE);
	obs_property_t *i = obs_properties_add_list(props, "i", "I", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_t *f = obs_properties_add_list(props, "f", "F", OBS_COMBO_TYPE_RADIO, OBS_COMBO_FORMAT_FLOAT);
	obs_property_t *b = obs_properties_add_list(props, "b", "B", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_BOOL);
	assert_int_equal(obs_property_list_format(f), OBS_COMBO_FORMAT_FLOAT);

	assert_int_equal(obs_property_list_add_string(s, "A", "a"), 0);
	assert_int_equal(obs_property_list_add_string(s, "C", "c"), 1);
	obs_property_list_insert_string(s, 1, "B", "b");
	assert_int_equal(obs_property_list_item_count(s), 3);
	assert_string_equal(obs_property_list_item_name(s, 1), "B");
	assert_string_equal(obs_property_list_item_string(s, 2), "c");
	obs_property_list_item_disable(s, 0, true);
	assert_true(obs_property_list_item_disabled(s, 0));
	assert_false(obs_property_list_item_disabled(s, 1));
	obs_property_list_item_remove(s, 0);
	assert_int_equal(obs_property_list_item_count(s), 2);
	assert_string_equal(obs_property_list_item_name(s, 0), "B");

	obs_property_list_add_int(i, "one", 1);
	obs_property_list_insert_int(i, 0, "zero", 0);
	assert_int_equal(obs_property_list_item_int(i, 1), 1);

	obs_property_list_add_float(f, "half", 0.5);
	obs_property_list_insert_float(f, 0, "quarter", 0.25);
	assert_true(obs_property_list_item_float(f, 0) == 0.25);

	/* Regression: the bool getter read the double member of the item */
	obs_property_list_add_bool(b, "yes", true);
	obs_property_list_insert_bool(b, 0, "no", false);
	assert_false(obs_property_list_item_bool(b, 0));
	assert_true(obs_property_list_item_bool(b, 1));

	/* wrong formats add nothing */
	obs_property_list_add_int(s, "x", 1);
	obs_property_list_add_string(i, "x", "x");
	obs_property_list_insert_bool(f, 0, "x", true);
	assert_int_equal(obs_property_list_item_count(s), 2);
	assert_int_equal(obs_property_list_item_count(i), 2);
	assert_int_equal(obs_property_list_item_count(f), 2);

	/* out of range */
	assert_null(obs_property_list_item_name(s, 5));
	assert_null(obs_property_list_item_string(s, 5));
	assert_int_equal(obs_property_list_item_int(i, 5), 0);
	assert_true(obs_property_list_item_float(f, 5) == 0.0);
	assert_false(obs_property_list_item_bool(b, 5));
	obs_property_list_item_remove(s, 5);
	obs_property_list_item_disable(s, 5, true);
	/* Regression: inserting past the end wrote out of bounds */
	obs_property_list_insert_string(s, 5, "far", "far");
	obs_property_list_insert_int(i, 5, "far", 5);
	assert_int_equal(obs_property_list_item_count(s), 2);
	assert_int_equal(obs_property_list_item_count(i), 2);

	obs_property_list_clear(s);
	assert_int_equal(obs_property_list_item_count(s), 0);
	assert_int_equal(obs_property_list_item_count(NULL), 0);

	obs_properties_destroy(props);
}

static void frame_rate_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *fr = obs_properties_add_frame_rate(props, "fr", "FPS");
	struct media_frames_per_second lo = {30, 1}, hi = {60, 1};

	assert_int_equal(obs_property_frame_rate_option_add(fr, "a", "A"), 0);
	obs_property_frame_rate_option_insert(fr, 0, "z", "Z");
	assert_int_equal(obs_property_frame_rate_options_count(fr), 2);
	assert_string_equal(obs_property_frame_rate_option_name(fr, 0), "z");
	assert_string_equal(obs_property_frame_rate_option_description(fr, 1), "A");
	assert_null(obs_property_frame_rate_option_name(fr, 2));

	assert_int_equal(obs_property_frame_rate_fps_range_add(fr, lo, hi), 0);
	obs_property_frame_rate_fps_range_insert(fr, 0, hi, hi);
	assert_int_equal(obs_property_frame_rate_fps_ranges_count(fr), 2);
	assert_int_equal(obs_property_frame_rate_fps_range_min(fr, 1).numerator, 30);
	assert_int_equal(obs_property_frame_rate_fps_range_max(fr, 0).numerator, 60);
	assert_int_equal(obs_property_frame_rate_fps_range_min(fr, 2).numerator, 0);

	/* Regression: inserting past the end wrote out of bounds */
	obs_property_frame_rate_option_insert(fr, 9, "far", "far");
	obs_property_frame_rate_fps_range_insert(fr, 9, lo, hi);
	assert_int_equal(obs_property_frame_rate_options_count(fr), 2);
	assert_int_equal(obs_property_frame_rate_fps_ranges_count(fr), 2);

	obs_property_frame_rate_options_clear(fr);
	assert_int_equal(obs_property_frame_rate_options_count(fr), 0);
	obs_property_frame_rate_fps_ranges_clear(fr);
	assert_int_equal(obs_property_frame_rate_fps_ranges_count(fr), 0);
	obs_property_frame_rate_option_add(fr, "a", "A");
	obs_property_frame_rate_fps_range_add(fr, lo, hi);
	obs_property_frame_rate_clear(fr);
	assert_int_equal(obs_property_frame_rate_options_count(fr), 0);
	assert_int_equal(obs_property_frame_rate_fps_ranges_count(fr), 0);

	obs_properties_destroy(props);
}

static int clicks;
static void *clicked_data;
static bool on_click(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	clicks++;
	clicked_data = data;
	return true;
}

static void button_test(void **state)
{
	UNUSED_PARAMETER(state);

	int priv;
	clicks = 0;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *btn = obs_properties_add_button2(props, "btn", "Click", on_click, &priv);
	assert_int_equal(obs_property_get_type(btn), OBS_PROPERTY_BUTTON);
	assert_int_equal(obs_property_button_type(btn), OBS_BUTTON_DEFAULT);
	obs_property_button_set_type(btn, OBS_BUTTON_URL);
	obs_property_button_set_url(btn, "https://example.com");
	assert_int_equal(obs_property_button_type(btn), OBS_BUTTON_URL);
	assert_string_equal(obs_property_button_url(btn), "https://example.com");

	assert_true(obs_property_button_clicked(btn, NULL));
	assert_int_equal(clicks, 1);
	assert_ptr_equal(clicked_data, &priv);

	/* only buttons click */
	obs_property_t *b = obs_properties_add_bool(props, "b", "B");
	assert_false(obs_property_button_clicked(b, NULL));
	assert_string_equal(obs_property_button_url(b), "");
	assert_int_equal(clicks, 1);

	obs_properties_destroy(props);
}

static int modified_calls;
static void *modified_priv;
static bool on_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	assert_null(obs_properties_get_parent(props));
	modified_calls++;
	return obs_data_get_bool(settings, "refresh");
}
static bool on_modified2(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(settings);
	modified_calls++;
	modified_priv = priv;
	return false;
}

static void groups_test(void **state)
{
	UNUSED_PARAMETER(state);

	obs_properties_t *props = obs_properties_create();
	obs_properties_t *inner = obs_properties_create();
	obs_properties_add_bool(props, "top", "Top");
	obs_property_t *in = obs_properties_add_int(inner, "in", "In", 0, 1, 1);
	obs_properties_add_bool(inner, "dup", "Dup");

	obs_property_t *g = obs_properties_add_group(props, "g", "Group", OBS_GROUP_CHECKABLE, inner);
	assert_non_null(g);
	assert_int_equal(obs_property_group_type(g), OBS_GROUP_CHECKABLE);
	assert_ptr_equal(obs_property_group_content(g), inner);
	assert_ptr_equal(obs_properties_get_parent(inner), props);

	/* lookups and name checks go through groups */
	assert_ptr_equal(obs_properties_get(props, "in"), in);
	assert_null(obs_properties_add_bool(props, "in", "Dup"));
	assert_null(obs_properties_add_bool(inner, "top", "Dup"));

	/* a group must not contain itself, its parent, or names the parent has */
	assert_null(obs_properties_add_group(props, "self", "Self", OBS_GROUP_NORMAL, props));
	assert_null(obs_properties_add_group(inner, "loop", "Loop", OBS_GROUP_NORMAL, props));
	obs_properties_t *clash = obs_properties_create();
	obs_properties_add_bool(clash, "dup", "Dup");
	assert_null(obs_properties_add_group(props, "clash", "Clash", OBS_GROUP_NORMAL, clash));
	assert_null(obs_properties_add_group(props, "null", "Null", OBS_GROUP_NORMAL, NULL));

	/* Regression: a group already added elsewhere was owned by both parents and freed twice */
	obs_properties_t *other = obs_properties_create();
	assert_null(obs_properties_add_group(other, "g2", "Again", OBS_GROUP_NORMAL, inner));
	obs_properties_destroy(other);

	/* modified callbacks get the topmost properties */
	modified_calls = 0;
	obs_property_set_modified_callback(in, on_modified);
	obs_property_t *top = obs_properties_get(props, "top");
	int priv;
	obs_property_set_modified_callback2(top, on_modified2, &priv);
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "refresh", true);
	assert_true(obs_property_modified(in, settings));
	assert_false(obs_property_modified(top, settings));
	assert_ptr_equal(modified_priv, &priv);
	assert_int_equal(modified_calls, 2);
	assert_false(obs_property_modified(g, settings));

	obs_properties_apply_settings(props, settings);
	assert_int_equal(modified_calls, 4);
	obs_properties_apply_settings(NULL, settings);
	obs_data_release(settings);

	/* removing reaches into groups, and removing a group frees its content */
	obs_properties_remove_by_name(props, "in");
	assert_null(obs_properties_get(props, "in"));
	obs_properties_remove_by_name(props, "g");
	assert_null(obs_properties_get(props, "dup"));
	assert_non_null(obs_properties_add_group(props, "clash", "Clash", OBS_GROUP_NORMAL, clash));

	obs_properties_destroy(props);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(basics_test),    obs_unit_test(null_names_test), obs_unit_test(numbers_test),
		obs_unit_test(text_path_test), obs_unit_test(list_test),       obs_unit_test(frame_rate_test),
		obs_unit_test(button_test),    obs_unit_test(groups_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
