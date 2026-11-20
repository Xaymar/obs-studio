#pragma once

/* Starts libobs on libobs-null and registers fake source types for the core object tests. Include after
 * cmocka.h. Each test calls core_start() and core_stop(); core_stop() checks that the whole libobs session
 * left no bmalloc allocation behind. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/platform.h>

#ifndef NULL_GRAPHICS_MODULE
#error NULL_GRAPHICS_MODULE must be defined
#endif

struct fake_source {
	obs_source_t *source;
	obs_data_t *settings;
	int updates;
	int activates, deactivates, shows, hides;
	int renders, ticks;
	int filter_frames, filter_audios;
	int saves, loads;
	uint32_t width, height;
	long long value;
};

/* counters shared by every fake source, for objects the test has no pointer to */
static struct fake_counters {
	int creates, destroys;
	int filter_adds, filter_removes;
	int transition_starts, transition_stops;
} fake;

static const char *fake_get_name(void *type_data)
{
	return type_data ? (const char *)type_data : "Fake";
}

static void *fake_create(obs_data_t *settings, obs_source_t *source)
{
	struct fake_source *fs = bzalloc(sizeof(*fs));
	fs->source = source;
	fs->width = (uint32_t)obs_data_get_int(settings, "width");
	fs->height = (uint32_t)obs_data_get_int(settings, "height");
	fs->value = obs_data_get_int(settings, "value");
	fake.creates++;
	return fs;
}

static void fake_destroy(void *data)
{
	fake.destroys++;
	bfree(data);
}

static void fake_update(void *data, obs_data_t *settings)
{
	struct fake_source *fs = data;
	fs->updates++;
	fs->width = (uint32_t)obs_data_get_int(settings, "width");
	fs->height = (uint32_t)obs_data_get_int(settings, "height");
	fs->value = obs_data_get_int(settings, "value");
}

static void fake_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 32);
	obs_data_set_default_int(settings, "height", 16);
}

static obs_properties_t *fake_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "width", "Width", 0, 4096, 1);
	obs_properties_add_int(props, "height", "Height", 0, 4096, 1);
	return props;
}

static uint32_t fake_width(void *data)
{
	return ((struct fake_source *)data)->width;
}

static uint32_t fake_height(void *data)
{
	return ((struct fake_source *)data)->height;
}

static void fake_activate(void *data)
{
	((struct fake_source *)data)->activates++;
}

static void fake_deactivate(void *data)
{
	((struct fake_source *)data)->deactivates++;
}

static void fake_show(void *data)
{
	((struct fake_source *)data)->shows++;
}

static void fake_hide(void *data)
{
	((struct fake_source *)data)->hides++;
}

static void fake_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	((struct fake_source *)data)->ticks++;
}

static void fake_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	((struct fake_source *)data)->renders++;
}

static void fake_save(void *data, obs_data_t *settings)
{
	struct fake_source *fs = data;
	fs->saves++;
	obs_data_set_int(settings, "saved", fs->saves);
}

static void fake_load(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	((struct fake_source *)data)->loads++;
}

static void fake_filter_add(void *data, obs_source_t *source)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(source);
	fake.filter_adds++;
}

static void fake_filter_remove(void *data, obs_source_t *source)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(source);
	fake.filter_removes++;
}

static void fake_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct fake_source *fs = data;
	fs->renders++;
	obs_source_skip_video_filter(fs->source);
}

static struct obs_source_frame *fake_filter_video(void *data, struct obs_source_frame *frame)
{
	((struct fake_source *)data)->filter_frames++;
	return frame;
}

static struct obs_audio_data *fake_filter_audio(void *data, struct obs_audio_data *audio)
{
	((struct fake_source *)data)->filter_audios++;
	return audio;
}

static void fake_transition_start(void *data)
{
	UNUSED_PARAMETER(data);
	fake.transition_starts++;
}

static void fake_transition_stop(void *data)
{
	UNUSED_PARAMETER(data);
	fake.transition_stops++;
}

static void fake_transition_draw(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	UNUSED_PARAMETER(a);
	UNUSED_PARAMETER(b);
	UNUSED_PARAMETER(t);
	UNUSED_PARAMETER(cx);
	UNUSED_PARAMETER(cy);
	((struct fake_source *)data)->renders++;
}

/* like real transitions, render through obs_transition_video_render(), which also advances the transition */
static void fake_transition_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct fake_source *fs = data;
	obs_transition_video_render(fs->source, fake_transition_draw);
}

static float fake_mix_a(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return 1.0f - t;
}

static float fake_mix_b(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return t;
}

static bool fake_transition_audio(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers,
				  size_t channels, size_t sample_rate)
{
	struct fake_source *fs = data;
	return obs_transition_audio_render(fs->source, ts_out, audio, mixers, channels, sample_rate, fake_mix_a,
					   fake_mix_b);
}

#define FAKE_COMMON                                                                                  \
	.get_name = fake_get_name, .create = fake_create, .destroy = fake_destroy, .update = fake_update, \
	.get_defaults = fake_defaults, .get_properties = fake_properties, .get_width = fake_width,        \
	.get_height = fake_height, .activate = fake_activate, .deactivate = fake_deactivate,              \
	.show = fake_show, .hide = fake_hide, .save = fake_save, .load = fake_load

static void register_fake_types(void)
{
	struct obs_source_info input = {
		.id = "fake_input",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_CUSTOM_DRAW,
		.video_tick = fake_tick,
		.video_render = fake_render,
		FAKE_COMMON,
	};
	obs_register_source(&input);

	struct obs_source_info async = {
		.id = "fake_async",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
		FAKE_COMMON,
	};
	async.get_width = NULL;
	async.get_height = NULL;
	obs_register_source(&async);

	struct obs_source_info filter = {
		.id = "fake_filter",
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_VIDEO,
		.video_render = fake_filter_render,
		.filter_add = fake_filter_add,
		.filter_remove = fake_filter_remove,
		FAKE_COMMON,
	};
	filter.get_width = NULL;
	filter.get_height = NULL;
	obs_register_source(&filter);

	struct obs_source_info async_filter = {
		.id = "fake_async_filter",
		.type = OBS_SOURCE_TYPE_FILTER,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO,
		.filter_video = fake_filter_video,
		.filter_audio = fake_filter_audio,
		FAKE_COMMON,
	};
	async_filter.get_width = NULL;
	async_filter.get_height = NULL;
	obs_register_source(&async_filter);

	struct obs_source_info transition = {
		.id = "fake_transition",
		.type = OBS_SOURCE_TYPE_TRANSITION,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_COMPOSITE,
		.video_render = fake_transition_render,
		.audio_render = fake_transition_audio,
		.transition_start = fake_transition_start,
		.transition_stop = fake_transition_stop,
		FAKE_COMMON,
	};
	transition.get_width = NULL;
	transition.get_height = NULL;
	obs_register_source(&transition);
}

/* sets or (value NULL) clears an environment variable the null graphics module reads */
static void core_set_env(const char *name, const char *value)
{
#ifdef _WIN32
	char buf[256];
	snprintf(buf, sizeof(buf), "%s=%s", name, value ? value : "");
	_putenv(buf);
#else
	if (value)
		setenv(name, value, 1);
	else
		unsetenv(name);
#endif
}

static long core_allocs_before;

static void core_start(void)
{
	/* a failed test leaves its session running */
	if (obs_initialized()) {
		obs_shutdown();
	}

	memset(&fake, 0, sizeof(fake));
	core_allocs_before = bnum_allocs();
	assert_true(obs_startup("en-US", NULL, NULL));

	struct obs_video_info ovi = {
		.graphics_module = NULL_GRAPHICS_MODULE,
		.fps_num = 60,
		.fps_den = 1,
		.base_width = 640,
		.base_height = 360,
		.output_width = 640,
		.output_height = 360,
		.output_format = VIDEO_FORMAT_NV12,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	assert_int_equal(obs_reset_video(&ovi), OBS_VIDEO_SUCCESS);

	struct obs_audio_info oai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	assert_true(obs_reset_audio(&oai));

	register_fake_types();
}

static void core_stop(void)
{
	obs_shutdown();
	assert_int_equal(bnum_allocs(), core_allocs_before);
}

/* waits until the graphics thread rendered at least count more frames */
static void core_wait_frames(uint32_t count)
{
	uint32_t target = obs_get_total_frames() + count;
	for (int i = 0; i < 500 && obs_get_total_frames() < target; i++) {
		os_sleep_ms(5);
	}
}

static struct fake_source *fake_of(obs_source_t *source)
{
	return obs_obj_get_data(source);
}
