#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

#include <util/threading.h>

#include <obs-config.h>

static void lifecycle_test(void **state)
{
	UNUSED_PARAMETER(state);

	long before = bnum_allocs();
	assert_false(obs_initialized());
	assert_int_equal(obs_get_version(), LIBOBS_API_VER);
	assert_non_null(obs_get_version_string());

	core_start();
	assert_true(obs_initialized());
	/* a second startup fails */
	assert_false(obs_startup("en-US", NULL, NULL));

	assert_string_equal(obs_get_locale(), "en-US");
	obs_set_locale("de-DE");
	assert_string_equal(obs_get_locale(), "de-DE");

	struct obs_video_info ovi;
	assert_true(obs_get_video_info(&ovi));
	assert_int_equal(ovi.base_width, 640);
	assert_int_equal(ovi.fps_num, 60);
	struct obs_audio_info oai;
	assert_true(obs_get_audio_info(&oai));
	assert_int_equal(oai.samples_per_sec, 48000);
	assert_int_equal(oai.speakers, SPEAKERS_STEREO);

	assert_true(obs_get_frame_interval_ns() > 16000000 && obs_get_frame_interval_ns() < 17000000);
	core_wait_frames(3);
	assert_true(obs_get_total_frames() >= 3);
	assert_false(obs_video_active());

	/* resetting video while nothing is active works */
	ovi.base_width = 320;
	ovi.base_height = 180;
	ovi.output_width = 320;
	ovi.output_height = 180;
	assert_int_equal(obs_reset_video(&ovi), OBS_VIDEO_SUCCESS);
	assert_true(obs_get_video_info(&ovi));
	assert_int_equal(ovi.base_width, 320);
	/* audio can be reset while no output uses it */
	struct obs_audio_info oai2 = {.samples_per_sec = 44100, .speakers = SPEAKERS_MONO};
	assert_true(obs_reset_audio(&oai2));
	assert_true(obs_get_audio_info(&oai));
	assert_int_equal(oai.samples_per_sec, 44100);
	assert_int_equal(oai.speakers, SPEAKERS_MONO);

	const char *argv[] = {"test", "--flag"};
	obs_set_cmdline_args(2, argv);
	struct obs_cmdline_args args = obs_get_cmdline_args();
	assert_int_equal(args.argc, 2);
	assert_string_equal(args.argv[1], "--flag");

	core_stop();
	assert_false(obs_initialized());
	/* Regression: a second shutdown dereferenced the freed core */
	obs_shutdown();
	/* Regression: shutdown freed the arguments but kept pointing at them */
	args = obs_get_cmdline_args();
	assert_int_equal(args.argc, 0);
	assert_null(args.argv);
	assert_int_equal(bnum_allocs(), before);
}

static int renames;
static void on_rename(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	assert_string_equal(calldata_string(cd, "new_name"), "renamed");
	assert_string_equal(calldata_string(cd, "prev_name"), "src");
	renames++;
}

static bool count_sources(void *param, obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	(*(int *)param)++;
	return true;
}

static void source_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	assert_string_equal(obs_source_get_display_name("fake_input"), "Fake");
	assert_null(obs_source_get_display_name("missing"));
	assert_int_equal(obs_get_source_output_flags("fake_async") & OBS_SOURCE_ASYNC_VIDEO, OBS_SOURCE_ASYNC_VIDEO);
	obs_data_t *defaults = obs_get_source_defaults("fake_input");
	assert_int_equal(obs_data_get_int(defaults, "width"), 32);
	obs_data_release(defaults);
	obs_properties_t *props = obs_get_source_properties("fake_input");
	assert_non_null(obs_properties_get(props, "width"));
	obs_properties_destroy(props);

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "value", 5);
	obs_source_t *src = obs_source_create("fake_input", "src", settings, NULL);
	obs_data_release(settings);
	assert_non_null(src);
	assert_int_equal(fake.creates, 1);
	struct fake_source *fs = fake_of(src);
	assert_int_equal(fs->value, 5);
	/* defaults apply */
	assert_int_equal(obs_source_get_width(src), 32);
	assert_int_equal(obs_source_get_height(src), 16);
	assert_string_equal(obs_source_get_name(src), "src");
	assert_string_equal(obs_source_get_id(src), "fake_input");
	assert_string_equal(obs_source_get_unversioned_id(src), "fake_input");
	assert_int_equal(obs_source_get_type(src), OBS_SOURCE_TYPE_INPUT);
	assert_int_equal(obs_obj_get_type(src), OBS_OBJ_TYPE_SOURCE);
	assert_string_equal(obs_obj_get_id(src), "fake_input");
	assert_false(obs_obj_invalid(src));
	assert_false(obs_obj_is_private(src));
	assert_true(obs_source_configurable(src));
	assert_false(obs_source_is_scene(src));
	assert_false(obs_source_is_group(src));
	assert_non_null(obs_source_get_uuid(src));

	/* lookups */
	obs_source_t *found = obs_get_source_by_name("src");
	assert_ptr_equal(found, src);
	obs_source_release(found);
	found = obs_get_source_by_uuid(obs_source_get_uuid(src));
	assert_ptr_equal(found, src);
	obs_source_release(found);
	assert_null(obs_get_source_by_name("missing"));
	int count = 0;
	obs_enum_sources(count_sources, &count);
	assert_int_equal(count, 1);

	/* private sources are not listed */
	obs_source_t *priv = obs_source_create_private("fake_input", "priv", NULL);
	assert_true(obs_obj_is_private(priv));
	assert_null(obs_get_source_by_name("priv"));
	count = 0;
	obs_enum_sources(count_sources, &count);
	assert_int_equal(count, 1);
	obs_source_release(priv);

	/* renaming signals */
	renames = 0;
	signal_handler_connect(obs_source_get_signal_handler(src), "rename", on_rename, NULL);
	obs_source_set_name(src, "renamed");
	assert_int_equal(renames, 1);
	assert_string_equal(obs_source_get_name(src), "renamed");
	signal_handler_disconnect(obs_source_get_signal_handler(src), "rename", on_rename, NULL);

	/* settings */
	settings = obs_data_create();
	obs_data_set_int(settings, "width", 100);
	obs_source_update(src, settings);
	obs_data_release(settings);
	/* video sources apply updates on the next video tick */
	core_wait_frames(2);
	assert_int_equal(obs_source_get_width(src), 100);
	assert_int_equal(fs->updates, 1);
	settings = obs_source_get_settings(src);
	assert_int_equal(obs_data_get_int(settings, "width"), 100);
	assert_int_equal(obs_data_get_int(settings, "value"), 5);
	obs_data_release(settings);
	obs_source_reset_settings(src, NULL);
	core_wait_frames(2);
	assert_int_equal(obs_source_get_width(src), 32);
	props = obs_source_properties(src);
	assert_non_null(obs_properties_get(props, "height"));
	obs_properties_destroy(props);

	/* audio state */
	obs_source_set_volume(src, 0.5f);
	assert_true(obs_source_get_volume(src) == 0.5f);
	obs_source_set_balance_value(src, 0.25f);
	assert_true(obs_source_get_balance_value(src) == 0.25f);
	obs_source_set_sync_offset(src, 1000);
	assert_int_equal(obs_source_get_sync_offset(src), 1000);
	obs_source_set_audio_mixers(src, 0x5);
	assert_int_equal(obs_source_get_audio_mixers(src), 0x5);
	obs_source_set_muted(src, true);
	assert_true(obs_source_muted(src));
	obs_source_set_muted(src, false);
	obs_source_set_enabled(src, false);
	assert_false(obs_source_enabled(src));
	obs_source_set_enabled(src, true);
	obs_source_enable_push_to_mute(src, true);
	assert_true(obs_source_push_to_mute_enabled(src));
	obs_source_set_push_to_mute_delay(src, 300);
	assert_int_equal(obs_source_get_push_to_mute_delay(src), 300);
	obs_source_enable_push_to_talk(src, true);
	assert_true(obs_source_push_to_talk_enabled(src));
	obs_source_set_push_to_talk_delay(src, 400);
	assert_int_equal(obs_source_get_push_to_talk_delay(src), 400);
	obs_source_set_monitoring_type(src, OBS_MONITORING_TYPE_NONE);
	assert_int_equal(obs_source_get_monitoring_type(src), OBS_MONITORING_TYPE_NONE);
	obs_source_set_flags(src, OBS_SOURCE_FLAG_FORCE_MONO);
	assert_int_equal(obs_source_get_flags(src), OBS_SOURCE_FLAG_FORCE_MONO);
	obs_source_set_flags(src, 0);
	obs_source_set_deinterlace_mode(src, OBS_DEINTERLACE_MODE_BLEND);
	assert_int_equal(obs_source_get_deinterlace_mode(src), OBS_DEINTERLACE_MODE_BLEND);
	obs_source_set_deinterlace_field_order(src, OBS_DEINTERLACE_FIELD_ORDER_BOTTOM);
	assert_int_equal(obs_source_get_deinterlace_field_order(src), OBS_DEINTERLACE_FIELD_ORDER_BOTTOM);
	obs_source_set_deinterlace_mode(src, OBS_DEINTERLACE_MODE_DISABLE);

	/* showing and activating; the callbacks run on the next video tick */
	obs_source_inc_showing(src);
	assert_true(obs_source_showing(src));
	core_wait_frames(2);
	assert_int_equal(fs->shows, 1);
	obs_source_dec_showing(src);
	assert_false(obs_source_showing(src));
	core_wait_frames(2);
	assert_int_equal(fs->hides, 1);
	obs_source_inc_active(src);
	assert_true(obs_source_active(src));
	core_wait_frames(2);
	assert_int_equal(fs->activates, 1);
	obs_source_dec_active(src);
	assert_false(obs_source_active(src));
	core_wait_frames(2);
	assert_int_equal(fs->deactivates, 1);

	/* rendering through the output channel */
	obs_set_output_source(0, src);
	obs_source_t *out = obs_get_output_source(0);
	assert_ptr_equal(out, src);
	obs_source_release(out);
	core_wait_frames(3);
	assert_true(fs->renders > 0);
	assert_true(fs->ticks > 0);
	assert_true(obs_source_active(src));
	obs_set_output_source(0, NULL);

	/* weak references */
	obs_weak_source_t *weak = obs_source_get_weak_source(src);
	assert_true(obs_weak_source_references_source(weak, src));
	assert_false(obs_weak_source_expired(weak));
	obs_source_t *strong = obs_weak_source_get_source(weak);
	assert_ptr_equal(strong, src);
	obs_source_release(strong);

	/* save, load and duplicate */
	obs_data_t *saved = obs_save_source(src);
	assert_string_equal(obs_data_get_string(saved, "name"), "renamed");
	assert_true(fs->saves > 0);
	obs_source_t *dup = obs_source_duplicate(src, "dup", false);
	assert_non_null(dup);
	assert_ptr_not_equal(dup, src);
	assert_int_equal(obs_source_get_width(dup), obs_source_get_width(src));
	obs_source_release(dup);

	obs_source_remove(src);
	assert_true(obs_source_removed(src));
	obs_source_release(src);
	assert_true(obs_weak_source_expired(weak));
	assert_null(obs_weak_source_get_source(weak));
	obs_weak_source_release(weak);

	obs_source_t *loaded = obs_load_source(saved);
	assert_non_null(loaded);
	assert_string_equal(obs_source_get_name(loaded), "renamed");
	/* the load callback runs from obs_source_load(), once every source exists */
	assert_int_equal(fake_of(loaded)->loads, 0);
	obs_source_load(loaded);
	assert_int_equal(fake_of(loaded)->loads, 1);
	assert_true(obs_source_get_volume(loaded) == 0.5f);
	obs_source_release(loaded);
	obs_data_release(saved);

	/* unknown ids create a source that keeps its settings */
	obs_source_t *missing = obs_source_create("missing_id", "missing", NULL, NULL);
	assert_non_null(missing);
	assert_string_equal(obs_source_get_id(missing), "missing_id");
	assert_int_equal(obs_source_get_width(missing), 0);
	obs_source_release(missing);

	core_stop();
	assert_int_equal(fake.creates, fake.destroys);
}

static void collect_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	obs_source_t **list = param;
	while (*list)
		list++;
	*list = child;
}

static void filter_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_source_t *a = obs_source_create("fake_filter", "a", NULL, NULL);
	obs_source_t *b = obs_source_create("fake_filter", "b", NULL, NULL);
	obs_source_t *c = obs_source_create("fake_filter", "c", NULL, NULL);
	assert_int_equal(obs_source_get_type(a), OBS_SOURCE_TYPE_FILTER);

	obs_source_filter_add(src, a);
	obs_source_filter_add(src, b);
	obs_source_filter_add(src, c);
	assert_int_equal(fake.filter_adds, 3);
	/* adding twice does nothing. Regression: a source that is not a filter was added as one */
	obs_source_filter_add(src, a);
	obs_source_t *other = obs_source_create("fake_input", "other", NULL, NULL);
	obs_source_filter_add(src, other);
	assert_int_equal(obs_source_filter_count(src), 3);

	assert_ptr_equal(obs_filter_get_parent(a), src);
	/* new filters go first: c targets b, b targets a, a targets the source */
	assert_int_equal(obs_source_filter_get_index(src, c), 0);
	assert_int_equal(obs_source_filter_get_index(src, a), 2);
	assert_ptr_equal(obs_filter_get_target(a), src);
	assert_ptr_equal(obs_filter_get_target(c), b);
	assert_int_equal(obs_source_filter_get_index(src, other), -1);

	obs_source_t *list[4] = {0};
	obs_source_enum_filters(src, collect_filter, list);
	assert_ptr_equal(list[0], a);
	assert_ptr_equal(list[2], c);

	obs_source_t *byname = obs_source_get_filter_by_name(src, "b");
	assert_ptr_equal(byname, b);
	obs_source_release(byname);
	assert_null(obs_source_get_filter_by_name(src, "missing"));

	/* ordering: the top of the list (applied first) is the end of the array */
	obs_source_filter_set_order(src, a, OBS_ORDER_MOVE_BOTTOM);
	assert_int_equal(obs_source_filter_get_index(src, a), 0);
	obs_source_filter_set_order(src, a, OBS_ORDER_MOVE_UP);
	assert_int_equal(obs_source_filter_get_index(src, a), 1);
	obs_source_filter_set_order(src, a, OBS_ORDER_MOVE_TOP);
	assert_int_equal(obs_source_filter_get_index(src, a), 2);
	obs_source_filter_set_order(src, a, OBS_ORDER_MOVE_DOWN);
	assert_int_equal(obs_source_filter_get_index(src, a), 1);
	obs_source_filter_set_index(src, a, 0);
	assert_int_equal(obs_source_filter_get_index(src, a), 0);
	/* Regression: an index past the end moved the filter out of bounds */
	obs_source_filter_set_index(src, a, 99);
	assert_int_equal(obs_source_filter_get_index(src, a), 0);
	assert_int_equal(obs_source_filter_count(src), 3);

	/* the filters render in the chain */
	obs_set_output_source(0, src);
	core_wait_frames(3);
	assert_true(fake_of(a)->renders > 0);
	obs_set_output_source(0, NULL);

	/* copying filters */
	obs_source_t *dst = obs_source_create("fake_input", "dst", NULL, NULL);
	obs_source_copy_filters(dst, src);
	assert_int_equal(obs_source_filter_count(dst), 3);
	obs_source_t *copy = obs_source_get_filter_by_name(dst, "a");
	assert_non_null(copy);
	assert_ptr_not_equal(copy, a);
	obs_source_release(copy);
	/* a single filter with a taken name gets a new one */
	obs_source_copy_single_filter(dst, b);
	assert_int_equal(obs_source_filter_count(dst), 4);

	/* backup and restore */
	obs_data_array_t *backup = obs_source_backup_filters(src);
	assert_int_equal(obs_data_array_count(backup), 3);
	obs_source_filter_remove(src, a);
	assert_int_equal(fake.filter_removes, 1);
	assert_null(obs_filter_get_parent(a));
	assert_int_equal(obs_source_filter_count(src), 2);
	obs_source_filter_remove(src, a);
	assert_int_equal(fake.filter_removes, 1);
	obs_source_restore_filters(src, backup);
	assert_int_equal(obs_source_filter_count(src), 3);
	obs_data_array_release(backup);

	/* duplicating a source copies its filters */
	obs_source_t *dup = obs_source_duplicate(src, "dup", false);
	assert_int_equal(obs_source_filter_count(dup), 3);
	obs_source_release(dup);

	obs_source_release(dst);
	obs_source_release(other);
	obs_source_release(a);
	obs_source_release(b);
	obs_source_release(c);
	obs_source_release(src);

	core_stop();
	assert_int_equal(fake.creates, fake.destroys);
}

static void transition_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *tr = obs_source_create("fake_transition", "tr", NULL, NULL);
	obs_source_t *a = obs_source_create("fake_input", "a", NULL, NULL);
	obs_source_t *b = obs_source_create("fake_input", "b", NULL, NULL);
	assert_int_equal(obs_source_get_type(tr), OBS_SOURCE_TYPE_TRANSITION);

	assert_null(obs_transition_get_active_source(tr));
	obs_transition_set(tr, a);
	obs_source_t *active = obs_transition_get_active_source(tr);
	assert_ptr_equal(active, a);
	obs_source_release(active);
	obs_source_t *sa = obs_transition_get_source(tr, OBS_TRANSITION_SOURCE_A);
	assert_ptr_equal(sa, a);
	obs_source_release(sa);
	assert_null(obs_transition_get_source(tr, OBS_TRANSITION_SOURCE_B));

	obs_transition_set_size(tr, 200, 100);
	uint32_t cx, cy;
	obs_transition_get_size(tr, &cx, &cy);
	assert_int_equal(cx, 200);
	assert_int_equal(cy, 100);
	obs_transition_set_alignment(tr, OBS_ALIGN_LEFT);
	assert_int_equal(obs_transition_get_alignment(tr), OBS_ALIGN_LEFT);
	obs_transition_set_scale_type(tr, OBS_TRANSITION_SCALE_STRETCH);
	assert_int_equal(obs_transition_get_scale_type(tr), OBS_TRANSITION_SCALE_STRETCH);
	obs_transition_enable_fixed(tr, true, 100);
	assert_true(obs_transition_fixed(tr));
	obs_transition_enable_fixed(tr, false, 0);

	/* a timed transition runs while the output renders it */
	obs_set_output_source(0, tr);
	assert_true(obs_transition_start(tr, OBS_TRANSITION_MODE_AUTO, 50, b));
	assert_true(obs_transition_is_active(tr));
	assert_int_equal(fake.transition_starts, 1);
	for (int i = 0; i < 200 && obs_transition_is_active(tr); i++)
		os_sleep_ms(5);
	assert_false(obs_transition_is_active(tr));
	assert_int_equal(fake.transition_stops, 1);
	assert_true(fake_of(tr)->renders > 0);
	/* the transition reports the size of its sources (updated each tick); the set size is what it
	 * renders at */
	assert_int_equal(obs_source_get_width(tr), 32);
	active = obs_transition_get_active_source(tr);
	assert_ptr_equal(active, b);
	obs_source_release(active);

	/* a manual transition follows the set time */
	assert_true(obs_transition_start(tr, OBS_TRANSITION_MODE_MANUAL, 0, a));
	obs_transition_set_manual_torque(tr, 0.0f, 0.0f);
	obs_transition_set_manual_time(tr, 0.5f);
	core_wait_frames(2);
	assert_true(obs_transition_is_active(tr));
	obs_transition_set_manual_time(tr, 1.0f);
	core_wait_frames(3);
	assert_false(obs_transition_is_active(tr));
	active = obs_transition_get_active_source(tr);
	assert_ptr_equal(active, a);
	obs_source_release(active);

	/* force stop only reports the stop; callers then set or clear the transition */
	int stops = fake.transition_stops;
	assert_true(obs_transition_start(tr, OBS_TRANSITION_MODE_AUTO, 10000, b));
	obs_transition_force_stop(tr);
	assert_int_equal(fake.transition_stops, stops + 1);
	obs_transition_set(tr, b);
	assert_false(obs_transition_is_active(tr));
	obs_set_output_source(0, NULL);

	/* swapping */
	obs_source_t *tr2 = obs_source_create("fake_transition", "tr2", NULL, NULL);
	obs_transition_swap_begin(tr2, tr);
	obs_transition_swap_end(tr2, tr);
	active = obs_transition_get_active_source(tr2);
	assert_ptr_equal(active, b);
	obs_source_release(active);
	obs_source_release(tr2);

	obs_transition_clear(tr);
	assert_null(obs_transition_get_active_source(tr));

	/* non-transitions are ignored */
	assert_false(obs_transition_start(a, OBS_TRANSITION_MODE_AUTO, 10, b));
	assert_null(obs_transition_get_active_source(a));

	obs_source_release(a);
	obs_source_release(b);
	obs_source_release(tr);
	core_stop();
	assert_int_equal(fake.creates, fake.destroys);
}

/* a controllable media source, and an audio source that mixes its own audio (submix) */
static volatile long media_calls[6];
static int64_t media_time;
static volatile long submixes;

static void media_play_pause(void *data, bool pause)
{
	UNUSED_PARAMETER(data);
	os_atomic_inc_long(&media_calls[pause ? 0 : 1]);
}

static void media_restart(void *data)
{
	UNUSED_PARAMETER(data);
	os_atomic_inc_long(&media_calls[2]);
}

static void media_stop(void *data)
{
	UNUSED_PARAMETER(data);
	os_atomic_inc_long(&media_calls[3]);
}

static void media_next(void *data)
{
	UNUSED_PARAMETER(data);
	os_atomic_inc_long(&media_calls[4]);
}

static void media_previous(void *data)
{
	UNUSED_PARAMETER(data);
	os_atomic_inc_long(&media_calls[5]);
}

static int64_t media_duration(void *data)
{
	UNUSED_PARAMETER(data);
	return 60000;
}

static int64_t media_get_time(void *data)
{
	UNUSED_PARAMETER(data);
	return media_time;
}

static void media_set_time(void *data, int64_t ms)
{
	UNUSED_PARAMETER(data);
	media_time = ms;
}

static enum obs_media_state media_state(void *data)
{
	UNUSED_PARAMETER(data);
	return OBS_MEDIA_STATE_PLAYING;
}

static bool submix_audio(void *data, uint64_t *ts_out, struct audio_output_data *audio_output, size_t channels,
			 size_t sample_rate)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(sample_rate);
	for (size_t ch = 0; ch < channels; ch++)
		for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
			audio_output->data[ch][i] = 0.25f;
	*ts_out = os_gettime_ns();
	return os_atomic_inc_long(&submixes) % 4 != 0;
}

static volatile long media_signals;
static void on_media_signal(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	os_atomic_inc_long(&media_signals);
}

static bool wait_long_at_least(volatile long *v, long count)
{
	for (int i = 0; i < 400 && os_atomic_load_long(v) < count; i++)
		os_sleep_ms(5);
	return os_atomic_load_long(v) >= count;
}

static void media_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	struct obs_source_info media = {
		.id = "fake_media",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CONTROLLABLE_MEDIA,
		.media_play_pause = media_play_pause,
		.media_restart = media_restart,
		.media_stop = media_stop,
		.media_next = media_next,
		.media_previous = media_previous,
		.media_get_duration = media_duration,
		.media_get_time = media_get_time,
		.media_set_time = media_set_time,
		.media_get_state = media_state,
		FAKE_COMMON,
	};
	media.get_width = NULL;
	media.get_height = NULL;
	obs_register_source(&media);

	struct obs_source_info submix = {
		.id = "fake_submix",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_SUBMIX,
		.audio_mix = submix_audio,
		FAKE_COMMON,
	};
	submix.get_width = NULL;
	submix.get_height = NULL;
	obs_register_source(&submix);

	obs_source_t *src = obs_source_create("fake_media", "media", NULL, NULL);
	signal_handler_t *sh = obs_source_get_signal_handler(src);
	const char *signals[] = {"media_play", "media_pause", "media_restart", "media_stopped",
				 "media_next", "media_previous", "media_started", "media_ended"};
	for (size_t i = 0; i < 8; i++)
		signal_handler_connect(sh, signals[i], on_media_signal, NULL);
	obs_set_output_source(0, src);

	/* actions queue up and run on the next tick, each with its signal */
	media_signals = 0;
	obs_source_media_play_pause(src, true);
	obs_source_media_play_pause(src, false);
	obs_source_media_restart(src);
	obs_source_media_stop(src);
	obs_source_media_next(src);
	obs_source_media_previous(src);
	obs_source_media_set_time(src, 1234);
	assert_true(wait_long_at_least(&media_signals, 6));
	for (int i = 0; i < 6; i++)
		assert_int_equal(os_atomic_load_long(&media_calls[i]), 1);
	assert_int_equal(obs_source_media_get_time(src), 1234);
	assert_int_equal(obs_source_media_get_duration(src), 60000);
	assert_int_equal(obs_source_media_get_state(src), OBS_MEDIA_STATE_PLAYING);
	obs_source_media_started(src);
	obs_source_media_ended(src);
	assert_true(wait_long_at_least(&media_signals, 8));

	/* a source that is not controllable ignores them */
	obs_source_t *plain = obs_source_create("fake_input", "plain", NULL, NULL);
	obs_source_media_play_pause(plain, true);
	obs_source_media_restart(plain);
	obs_source_media_stop(plain);
	obs_source_media_next(plain);
	obs_source_media_previous(plain);
	obs_source_media_set_time(plain, 1);
	assert_int_equal(obs_source_media_get_duration(plain), 0);
	assert_int_equal(obs_source_media_get_time(plain), 0);
	assert_int_equal(obs_source_media_get_state(plain), OBS_MEDIA_STATE_NONE);
	obs_source_release(plain);

	/* the submix source's audio is mixed in the audio thread; every fourth mix has nothing */
	obs_source_t *mix = obs_source_create("fake_submix", "submix", NULL, NULL);
	obs_set_output_source(1, mix);
	submixes = 0;
	assert_true(wait_long_at_least(&submixes, 10));
	obs_set_output_source(1, NULL);
	obs_source_release(mix);

	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

static int enum_count;
static void count_child(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	UNUSED_PARAMETER(child);
	UNUSED_PARAMETER(param);
	enum_count++;
}

static void keep_loaded(void *param, obs_source_t *source)
{
	*(obs_source_t **)param = obs_source_get_ref(source);
}

/* obs_load_sources() releases what it loaded, so the callback keeps a reference */
static obs_source_t *load_transition(obs_data_t *saved, const char *name, const char *source_a)
{
	obs_source_t *loaded = NULL;
	obs_data_t *copy = obs_data_create();
	obs_data_apply(copy, saved);
	obs_data_set_string(copy, "name", name);
	obs_data_set_string(copy, "transition_source_a", source_a);
	obs_data_array_t *array = obs_data_array_create();
	obs_data_array_push_back(array, copy);
	obs_load_sources(array, keep_loaded, &loaded);
	obs_data_array_release(array);
	obs_data_release(copy);
	return loaded;
}

static void transition_save_load_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *a = obs_source_create("fake_input", "a", NULL, NULL);
	obs_source_t *b = obs_source_create("fake_input", "b", NULL, NULL);
	obs_source_t *tr = obs_source_create("fake_transition", "tr", NULL, NULL);
	obs_transition_set(tr, a);
	obs_transition_set_alignment(tr, OBS_ALIGN_RIGHT);
	obs_transition_set_scale_type(tr, OBS_TRANSITION_SCALE_ASPECT);
	obs_transition_set_size(tr, 320, 180);

	obs_data_t *saved = obs_save_source(tr);
	assert_string_equal(obs_data_get_string(saved, "transition_source_a"), "a");
	assert_int_equal(obs_data_get_int(saved, "transition_cx"), 320);

	/* a loaded transition takes over the saved state */
	obs_source_t *tr2 = load_transition(saved, "tr2", "a");
	assert_non_null(tr2);
	obs_source_t *active = obs_transition_get_active_source(tr2);
	assert_ptr_equal(active, a);
	obs_source_release(active);
	assert_int_equal(obs_transition_get_alignment(tr2), OBS_ALIGN_RIGHT);
	assert_int_equal(obs_transition_get_scale_type(tr2), OBS_TRANSITION_SCALE_ASPECT);
	uint32_t cx, cy;
	obs_transition_get_size(tr2, &cx, &cy);
	assert_int_equal(cx, 320);
	assert_int_equal(cy, 180);
	obs_source_release(tr2);

	/* a missing source, and a transition that would contain itself, load without a source */
	obs_source_t *tr3 = load_transition(saved, "tr3", "missing");
	assert_null(obs_transition_get_active_source(tr3));
	obs_source_release(tr3);
	obs_source_t *tr4 = load_transition(saved, "tr4", "tr4");
	assert_null(obs_transition_get_active_source(tr4));
	obs_source_release(tr4);
	obs_data_release(saved);

	/* both sides are active children while transitioning */
	enum_count = 0;
	obs_source_enum_active_sources(tr, count_child, NULL);
	assert_int_equal(enum_count, 1);
	assert_true(obs_transition_start(tr, OBS_TRANSITION_MODE_MANUAL, 0, b));
	obs_transition_set_manual_torque(tr, 0.0f, 0.0f);
	obs_transition_set_manual_time(tr, 0.25f);
	enum_count = 0;
	obs_source_enum_active_sources(tr, count_child, NULL);
	assert_int_equal(enum_count, 2);

	/* while it renders and mixes audio on the output */
	obs_set_output_source(0, tr);
	core_wait_frames(5);
	obs_transition_set_manual_time(tr, 0.75f);
	core_wait_frames(5);
	assert_true(obs_transition_is_active(tr));
	assert_true(obs_transition_get_time(tr) == 0.75f);
	obs_set_output_source(0, NULL);

	/* saved while transitioning: the destination is saved */
	saved = obs_save_source(tr);
	assert_string_equal(obs_data_get_string(saved, "transition_source_a"), "b");
	obs_data_release(saved);
	obs_transition_force_stop(tr);
	obs_transition_clear(tr);

	obs_source_release(tr);
	obs_source_release(a);
	obs_source_release(b);
	core_stop();
	assert_int_equal(fake.creates, fake.destroys);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(lifecycle_test),
		cmocka_unit_test(source_test),
		cmocka_unit_test(filter_test),
		cmocka_unit_test(transition_test),
		cmocka_unit_test(transition_save_load_test),
		cmocka_unit_test(media_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
