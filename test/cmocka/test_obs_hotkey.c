#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

#include <util/dstr.h>

static int presses, releases;
static obs_hotkey_id last_id;
static void on_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(hotkey);
	last_id = id;
	if (pressed)
		presses++;
	else
		releases++;
}

static int pair_calls[2];
static bool on_pair0(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		pair_calls[0]++;
	return pressed;
}

static bool on_pair1(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		pair_calls[1]++;
	return pressed;
}

/* Injecting only presses: the hotkey thread releases the binding on its next poll (every 25 ms), when it
 * sees the physical key is up. Wait for that, so the next press is a new one. */
static void press(uint32_t modifiers, obs_key_t key)
{
	obs_key_combination_t combo = {modifiers, key};
	obs_hotkey_inject_event(combo, true);
	obs_hotkey_inject_event(combo, false);
	os_sleep_ms(80);
}

static bool find_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	UNUSED_PARAMETER(key);
	obs_hotkey_id *wanted = data;
	if (id == *wanted) {
		*wanted = OBS_INVALID_HOTKEY_ID;
		return false;
	}
	return true;
}

static int binding_count;
static bool count_binding(void *data, size_t idx, obs_hotkey_binding_t *binding)
{
	UNUSED_PARAMETER(idx);
	obs_hotkey_id *id = data;
	if (obs_hotkey_binding_get_hotkey_id(binding) == *id) {
		binding_count++;
		assert_non_null(obs_hotkey_binding_get_hotkey(binding));
	}
	return true;
}

static void keys_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	assert_int_equal(obs_key_from_name("OBS_KEY_F13"), OBS_KEY_F13);
	assert_string_equal(obs_key_to_name(OBS_KEY_F13), "OBS_KEY_F13");
	assert_int_equal(obs_key_from_name("OBS_KEY_A"), OBS_KEY_A);
	assert_int_equal(obs_key_from_name("not a key"), OBS_KEY_NONE);
	/* every key survives a round trip through its name */
	for (int key = 0; key < OBS_KEY_LAST_VALUE; key++)
		assert_int_equal(obs_key_from_name(obs_key_to_name((obs_key_t)key)), key);

	/* virtual keys */
	int vk = obs_key_to_virtual_key(OBS_KEY_A);
	assert_true(vk != 0);
	assert_int_equal(obs_key_from_virtual_key(vk), OBS_KEY_A);

	struct dstr str = {0};
	obs_key_to_str(OBS_KEY_A, &str);
	assert_true(str.len > 0);
	obs_key_combination_t combo = {INTERACT_CONTROL_KEY | INTERACT_SHIFT_KEY, OBS_KEY_A};
	obs_key_combination_to_str(combo, &str);
	assert_true(strstr(str.array, "A") != NULL);
	dstr_free(&str);

	core_stop();
}

static void frontend_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();
	presses = releases = 0;

	obs_hotkey_id id = obs_hotkey_register_frontend("test.key", "Test key", on_hotkey, NULL);
	assert_true(id != OBS_INVALID_HOTKEY_ID);
	obs_hotkey_id wanted = id;
	obs_enum_hotkeys(find_hotkey, &wanted);
	assert_int_equal(wanted, OBS_INVALID_HOTKEY_ID);

	obs_key_combination_t f13 = {0, OBS_KEY_F13};
	obs_key_combination_t ctrl_f14 = {INTERACT_CONTROL_KEY, OBS_KEY_F14};
	obs_key_combination_t bindings[] = {f13, ctrl_f14};
	obs_hotkey_load_bindings(id, bindings, 2);
	binding_count = 0;
	obs_enum_hotkey_bindings(count_binding, &id);
	assert_int_equal(binding_count, 2);

	press(0, OBS_KEY_F13);
	assert_int_equal(presses, 1);
	assert_int_equal(releases, 1);
	assert_int_equal(last_id, id);
	/* the modifier is required */
	press(0, OBS_KEY_F14);
	assert_int_equal(presses, 1);
	press(INTERACT_CONTROL_KEY, OBS_KEY_F14);
	assert_int_equal(presses, 2);

	/* saving and loading the bindings */
	obs_data_array_t *saved = obs_hotkey_save(id);
	assert_int_equal(obs_data_array_count(saved), 2);
	obs_hotkey_load_bindings(id, NULL, 0);
	press(0, OBS_KEY_F13);
	assert_int_equal(presses, 2);
	obs_hotkey_load(id, saved);
	press(0, OBS_KEY_F13);
	assert_int_equal(presses, 3);
	obs_data_array_release(saved);

	/* routing sends presses to the router, which triggers them */
	obs_hotkey_set_callback_routing_func(NULL, NULL);
	obs_hotkey_enable_callback_rerouting(true);
	press(0, OBS_KEY_F13);
	assert_int_equal(presses, 3);
	obs_hotkey_trigger_routed_callback(id, true);
	assert_int_equal(presses, 4);
	obs_hotkey_enable_callback_rerouting(false);

	/* atomic updates and background presses */
	obs_hotkey_enable_background_press(false);
	obs_hotkey_enable_background_press(true);

	/* unregistered hotkeys no longer fire */
	obs_hotkey_unregister(id);
	press(0, OBS_KEY_F13);
	assert_int_equal(presses, 4);

	core_stop();
}

static void pair_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();
	pair_calls[0] = pair_calls[1] = 0;

	obs_hotkey_pair_id pair =
		obs_hotkey_pair_register_frontend("test.on", "On", "test.off", "Off", on_pair0, on_pair1, NULL, NULL);
	assert_true(pair != OBS_INVALID_HOTKEY_PAIR_ID);
	obs_hotkey_pair_set_names(pair, "test.on2", "test.off2");
	obs_hotkey_pair_set_descriptions(pair, "On 2", "Off 2");

	obs_key_combination_t f15 = {0, OBS_KEY_F15}, f16 = {0, OBS_KEY_F16};
	obs_data_array_t *b0 = obs_data_array_create(), *b1 = obs_data_array_create();
	obs_data_t *k0 = obs_data_create(), *k1 = obs_data_create();
	obs_data_set_string(k0, "key", obs_key_to_name(f15.key));
	obs_data_set_string(k1, "key", obs_key_to_name(f16.key));
	obs_data_array_push_back(b0, k0);
	obs_data_array_push_back(b1, k1);
	obs_hotkey_pair_load(pair, b0, b1);
	obs_data_release(k0);
	obs_data_release(k1);
	obs_data_array_release(b0);
	obs_data_array_release(b1);

	press(0, OBS_KEY_F15);
	assert_int_equal(pair_calls[0], 1);
	press(0, OBS_KEY_F16);
	assert_int_equal(pair_calls[1], 1);

	obs_data_array_t *s0 = NULL, *s1 = NULL;
	obs_hotkey_pair_save(pair, &s0, &s1);
	assert_int_equal(obs_data_array_count(s0), 1);
	assert_int_equal(obs_data_array_count(s1), 1);
	obs_data_array_release(s0);
	obs_data_array_release(s1);

	obs_hotkey_pair_unregister(pair);
	press(0, OBS_KEY_F15);
	assert_int_equal(pair_calls[0], 1);

	core_stop();
}

static void source_hotkeys_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();
	presses = releases = 0;

	obs_source_t *src = obs_source_create("fake_async", "src", NULL, NULL);
	obs_hotkey_id id = obs_hotkey_register_source(src, "src.key", "Source key", on_hotkey, NULL);
	assert_true(id != OBS_INVALID_HOTKEY_ID);
	obs_hotkey_id wanted = id;
	obs_enum_hotkeys(find_hotkey, &wanted);
	assert_int_equal(wanted, OBS_INVALID_HOTKEY_ID);

	obs_key_combination_t f17 = {0, OBS_KEY_F17};
	obs_hotkey_load_bindings(id, &f17, 1);
	press(0, OBS_KEY_F17);
	assert_int_equal(presses, 1);

	/* the source's hotkeys save and load with it */
	obs_data_t *hotkeys = obs_hotkeys_save_source(src);
	assert_non_null(hotkeys);
	assert_true(obs_data_has_user_value(hotkeys, "src.key"));
	obs_hotkey_load_bindings(id, NULL, 0);
	obs_hotkeys_load_source(src, hotkeys);
	press(0, OBS_KEY_F17);
	assert_int_equal(presses, 2);
	obs_data_release(hotkeys);

	/* push to mute through a hotkey binding: the built-in source hotkeys exist */
	obs_source_enable_push_to_mute(src, true);
	obs_source_set_push_to_mute_delay(src, 0);
	obs_source_set_muted(src, false);

	obs_source_release(src);
	/* a released source's hotkeys go with it */
	obs_wait_for_destroy_queue();
	press(0, OBS_KEY_F17);
	assert_int_equal(presses, 2);

	core_stop();
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(keys_test),
		cmocka_unit_test(frontend_test),
		cmocka_unit_test(pair_test),
		cmocka_unit_test(source_hotkeys_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
