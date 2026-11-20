/* Video Device Capture: settings migration from the old camera sources,
 * backend selection and the alias path, with fake backends instead of
 * hardware. */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <obs.h>
#include <util/bmem.h>
#include <util/dstr.h>

#include <stdio.h>
#include <string.h>

#include "video-device.h"
#include "migrate.h"

/* The plugin's locale lookup, stubbed: tests compile the plugin sources
 * without the module loader. */
const char *obs_module_text(const char *val)
{
	return val;
}

/* ------------------------------------------------------------------------- */
/* Fake backends                                                             */

struct fake_state {
	int creates;
	int destroys;
	int updates;
	int hides;
	int shows;
	char last_device[64];
	bool available;
};

static struct fake_state fake_a_state;
static struct fake_state fake_b_state;

struct fake_instance {
	struct fake_state *state;
};

static void remember(struct fake_state *st, obs_data_t *settings)
{
	strncpy(st->last_device, obs_data_get_string(settings, VD_DEVICE_ID), sizeof(st->last_device) - 1);
}

#define FAKE_BACKEND(tag)                                                                      \
	static bool tag##_available(void)                                                      \
	{                                                                                      \
		return tag##_state.available;                                                  \
	}                                                                                      \
	static void tag##_enum(vd_device_cb cb, void *param)                                   \
	{                                                                                      \
		cb(param, #tag "-cam0", "Camera 0 (" #tag ")");                                \
		cb(param, #tag "-cam1", "Camera 1 (" #tag ")");                                \
	}                                                                                      \
	static void *tag##_create(obs_source_t *source, obs_data_t *settings)                  \
	{                                                                                      \
		UNUSED_PARAMETER(source);                                                      \
		struct fake_instance *f = bzalloc(sizeof(*f));                                 \
		f->state = &tag##_state;                                                       \
		f->state->creates++;                                                           \
		remember(f->state, settings);                                                  \
		return f;                                                                      \
	}                                                                                      \
	static void tag##_destroy(void *data)                                                  \
	{                                                                                      \
		struct fake_instance *f = data;                                                \
		f->state->destroys++;                                                          \
		bfree(f);                                                                      \
	}                                                                                      \
	static void tag##_update(void *data, obs_data_t *settings)                             \
	{                                                                                      \
		struct fake_instance *f = data;                                                \
		f->state->updates++;                                                           \
		remember(f->state, settings);                                                  \
	}                                                                                      \
	static const struct vd_backend tag##_backend = {                                       \
		.name = #tag,                                                                  \
		.display_name = "backend." #tag,                                               \
		.available = tag##_available,                                                  \
		.enum_devices = tag##_enum,                                                    \
		.create = tag##_create,                                                        \
		.destroy = tag##_destroy,                                                      \
		.update = tag##_update,                                                        \
	};

FAKE_BACKEND(fake_a)
FAKE_BACKEND(fake_b)

/* The plugin's backend table for this test build. "v4l2" is fake_b's role
 * for the alias test: the migration names the backend "v4l2". */
static const struct vd_backend v4l2_backend = {
	.name = "v4l2",
	.display_name = "backend.v4l2",
	.available = fake_b_available,
	.enum_devices = fake_b_enum,
	.create = fake_b_create,
	.destroy = fake_b_destroy,
	.update = fake_b_update,
};

/* A backend that pauses on hide instead of being destroyed. It shares
 * fake_b's state. */
static void pausable_hide(void *data)
{
	struct fake_instance *f = data;
	f->state->hides++;
}

static void pausable_show(void *data)
{
	struct fake_instance *f = data;
	f->state->shows++;
}

static const struct vd_backend pausable_backend = {
	.name = "pausable",
	.display_name = "backend.pausable",
	.available = fake_b_available,
	.enum_devices = fake_b_enum,
	.create = fake_b_create,
	.destroy = fake_b_destroy,
	.update = fake_b_update,
	.show = pausable_show,
	.hide = pausable_hide,
};

const struct vd_backend *const vd_backends[] = {&fake_a_backend, &fake_b_backend, &v4l2_backend, &pausable_backend,
						NULL};
const size_t vd_num_backends = 4;

/* ------------------------------------------------------------------------- */
/* Fixture                                                                   */

static long allocs_before;

static int setup(void **state)
{
	UNUSED_PARAMETER(state);
	memset(&fake_a_state, 0, sizeof(fake_a_state));
	memset(&fake_b_state, 0, sizeof(fake_b_state));
	fake_a_state.available = true;
	fake_b_state.available = true;

	allocs_before = bnum_allocs();
	if (!obs_startup("en-US", NULL, NULL)) {
		return -1;
	}
	obs_register_source(&video_device_capture_info);
	vd_register_aliases();
	return 0;
}

static int teardown(void **state)
{
	UNUSED_PARAMETER(state);
	obs_shutdown();
	if (bnum_allocs() != allocs_before) {
		fprintf(stderr, "leaked %ld allocations\n", bnum_allocs() - allocs_before);
		return -1;
	}
	return 0;
}

static obs_data_t *json(const char *text)
{
	obs_data_t *d = obs_data_create_from_json(text);
	assert_non_null(d);
	return d;
}

/* ------------------------------------------------------------------------- */
/* Migration                                                                 */

/* New keys use only lowercase letters, digits, '-' and '.'. Keys a
 * migration doesn't know (allowed) are left alone. */
static void assert_new_style_keys(obs_data_t *s, const char *allowed)
{
	for (obs_data_item_t *item = obs_data_first(s); item; obs_data_item_next(&item)) {
		const char *name = obs_data_item_get_name(item);
		if (allowed && strcmp(name, allowed) == 0) {
			continue;
		}
		for (const char *c = name; *c; c++) {
			bool ok = (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '.';
			if (!ok) {
				fail_msg("key '%s' is not lowercase with '-' and '.' only", name);
			}
		}
	}
}

static void migrate_dshow_custom(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = json("{\"video_device_id\":\"Cam#3A Pro#22 1:\\\\\\\\?\\\\usb#vid\","
			     "\"res_type\":1,\"resolution\":\"1920x1080\",\"frame_interval\":166666,"
			     "\"video_format\":200,\"color_space\":\"709\",\"color_range\":\"full\","
			     "\"buffering\":2,\"flip_vertically\":true,\"deactivate_when_not_showing\":true,"
			     "\"audio_output_mode\":1,\"hw_decode\":true,\"unknown_key\":\"kept\"}");

	vd_migrate_dshow(NULL, "dshow_input", s);

	assert_string_equal(obs_data_get_string(s, VD_BACKEND), "dshow");
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_BACKEND), "dshow");
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_ID), "Cam#3A Pro#22 1:\\\\?\\usb#vid");
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_NAME), "Cam: Pro# 1");
	assert_string_equal(obs_data_get_string(s, VD_RESOLUTION), "1920x1080");
	assert_string_equal(obs_data_get_string(s, VD_FRAME_RATE), "60/1");
	assert_string_equal(obs_data_get_string(s, VD_PIXEL_FORMAT), "YU12"); /* I420 */
	assert_string_equal(obs_data_get_string(s, VD_COLOR_SPACE), "709");
	assert_string_equal(obs_data_get_string(s, VD_COLOR_RANGE), "full");
	assert_string_equal(obs_data_get_string(s, VD_BUFFERING), "off");
	assert_true(obs_data_get_bool(s, VD_FLIP_VERTICAL));
	assert_true(obs_data_get_bool(s, VD_DEACTIVATE_WHEN_HIDDEN));
	assert_int_equal(obs_data_get_int(s, "dshow.audio-output-mode"), 1);
	assert_true(obs_data_get_bool(s, "dshow.hw-decode"));
	assert_string_equal(obs_data_get_string(s, "unknown_key"), "kept");

	assert_false(obs_data_has_user_value(s, "video_device_id"));
	assert_false(obs_data_has_user_value(s, "res_type"));
	assert_false(obs_data_has_user_value(s, "frame_interval"));
	assert_false(obs_data_has_user_value(s, "flip_vertically"));
	assert_false(obs_data_has_user_value(s, "video_format"));
	assert_new_style_keys(s, "unknown_key");
	obs_data_release(s);
}

static void migrate_dshow_preferred_and_matching(void **state)
{
	UNUSED_PARAMETER(state);

	/* Preferred resolution: the stored resolution and format are ignored. */
	obs_data_t *s = json("{\"video_device_id\":\"Cam:path\",\"res_type\":0,"
			     "\"resolution\":\"640x480\",\"video_format\":3,\"color_space\":\"default\"}");
	vd_migrate_dshow(NULL, "dshow_input", s);
	assert_new_style_keys(s, NULL);
	assert_false(obs_data_has_user_value(s, VD_RESOLUTION));
	assert_false(obs_data_has_user_value(s, VD_PIXEL_FORMAT));
	assert_string_equal(obs_data_get_string(s, VD_COLOR_SPACE), "auto");
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_NAME), "Cam");
	obs_data_release(s);

	/* Custom with "match output" frame rate. */
	s = json("{\"video_device_id\":\"Cam:path\",\"res_type\":1,\"resolution\":\"1280x720\","
		 "\"frame_interval\":-1}");
	vd_migrate_dshow(NULL, "dshow_input", s);
	assert_string_equal(obs_data_get_string(s, VD_FRAME_RATE), VD_FRAME_RATE_MATCH);
	assert_false(obs_data_has_user_value(s, VD_PIXEL_FORMAT));
	obs_data_release(s);

	/* Custom with Any (0) and an unknown format: automatic. */
	s = json("{\"video_device_id\":\"Cam:path\",\"res_type\":1,\"resolution\":\"1280x720\","
		 "\"video_format\":0}");
	vd_migrate_dshow(NULL, "dshow_input", s);
	assert_false(obs_data_has_user_value(s, VD_PIXEL_FORMAT));
	assert_false(obs_data_has_user_value(s, "video_format"));
	obs_data_release(s);
	s = json("{\"video_device_id\":\"Cam:path\",\"res_type\":1,\"resolution\":\"1280x720\","
		 "\"video_format\":999}");
	vd_migrate_dshow(NULL, "dshow_input", s);
	assert_false(obs_data_has_user_value(s, VD_PIXEL_FORMAT));
	obs_data_release(s);

	/* Intervals near a listed rate become that rate; others stay exact. */
	static const struct {
		long long interval;
		const char *rate;
	} rates[] = {{166833, "60000/1001"},
		     {166834, "60000/1001"},
		     {333333, "30/1"},
		     {400000, "25/1"},
		     {123456, "10000000/123456"}};
	for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		s = obs_data_create();
		obs_data_set_string(s, "video_device_id", "Cam:path");
		obs_data_set_int(s, "res_type", 1);
		obs_data_set_string(s, "resolution", "640x480");
		obs_data_set_int(s, "frame_interval", rates[i].interval);
		vd_migrate_dshow(NULL, "dshow_input", s);
		assert_string_equal(obs_data_get_string(s, VD_FRAME_RATE), rates[i].rate);
		obs_data_release(s);
	}

	/* Each saved format maps to its FourCC. */
	static const struct {
		int value;
		const char *fourcc;
	} formats[] = {{100, "AR24"}, {101, "XR24"}, {201, "NV12"}, {204, "P010"},
		       {301, "YUYV"}, {303, "HDYC"}, {400, "MJPG"}, {402, "HEVC"}};
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		s = obs_data_create();
		obs_data_set_string(s, "video_device_id", "Cam:path");
		obs_data_set_int(s, "res_type", 1);
		obs_data_set_string(s, "resolution", "640x480");
		obs_data_set_int(s, "video_format", formats[i].value);
		vd_migrate_dshow(NULL, "dshow_input", s);
		assert_string_equal(obs_data_get_string(s, VD_PIXEL_FORMAT), formats[i].fourcc);
		obs_data_release(s);
	}
}

static void migrate_v4l2_packed_formats(void **state)
{
	UNUSED_PARAMETER(state);

	/* 64-bit packing: (w << 32) | h, time per frame (1 << 32) | 30. */
	long long res64 = (1920LL << 32) | 1080;
	long long fps64 = (1LL << 32) | 30;
	long long yuyv = 'Y' | ('U' << 8) | ('Y' << 16) | ('V' << 24);

	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "device_id", "/dev/video0");
	obs_data_set_int(s, "resolution", res64);
	obs_data_set_int(s, "framerate", fps64);
	obs_data_set_int(s, "pixelformat", yuyv);
	obs_data_set_int(s, "color_range", VIDEO_RANGE_PARTIAL);
	obs_data_set_bool(s, "buffering", false);
	obs_data_set_int(s, "dv_timing", 3);
	obs_data_set_int(s, "input", 1);

	vd_migrate_v4l2(NULL, "v4l2_input", s);
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_ID), "/dev/video0");
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_BACKEND), "v4l2");
	assert_string_equal(obs_data_get_string(s, VD_RESOLUTION), "1920x1080");
	assert_string_equal(obs_data_get_string(s, VD_FRAME_RATE), "30/1");
	assert_string_equal(obs_data_get_string(s, VD_PIXEL_FORMAT), "YUYV");
	assert_string_equal(obs_data_get_string(s, VD_COLOR_RANGE), "partial");
	assert_string_equal(obs_data_get_string(s, VD_BUFFERING), "off");
	assert_int_equal(obs_data_get_int(s, "v4l2.dv-timing"), 3);
	assert_int_equal(obs_data_get_int(s, "v4l2.input"), 1);
	assert_false(obs_data_has_user_value(s, "framerate"));
	assert_false(obs_data_has_user_value(s, "pixelformat"));
	assert_new_style_keys(s, NULL);
	obs_data_release(s);

	/* Old 32-bit packing: 16 bits each. -1 means "leave unchanged". */
	s = obs_data_create();
	obs_data_set_int(s, "resolution", (640 << 16) | 480);
	obs_data_set_int(s, "framerate", -1);
	obs_data_set_int(s, "pixelformat", -1);
	vd_migrate_v4l2(NULL, "v4l2_input", s);
	assert_string_equal(obs_data_get_string(s, VD_RESOLUTION), "640x480");
	assert_false(obs_data_has_user_value(s, VD_FRAME_RATE));
	assert_false(obs_data_has_user_value(s, VD_PIXEL_FORMAT));
	obs_data_release(s);
}

static void migrate_pipewire_camera(void **state)
{
	UNUSED_PARAMETER(state);

	/* Older settings: "pixelformat" and a "resolution" JSON. */
	obs_data_t *s = json("{\"device_id\":\"node-42\","
			     "\"resolution\":\"{\\\"width\\\":1280,\\\"height\\\":720}\","
			     "\"framerate\":\"{\\\"framerate\\\":{\\\"numerator\\\":60,\\\"denominator\\\":1}}\","
			     "\"pixelformat\":7,\"Brightness\":5}");
	vd_migrate_pipewire_camera(NULL, "pipewire-camera-source", s);
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_BACKEND), "pipewire");
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_ID), "node-42");
	assert_string_equal(obs_data_get_string(s, VD_RESOLUTION), "1280x720");
	assert_string_equal(obs_data_get_string(s, VD_FRAME_RATE), "60/1");
	assert_string_equal(obs_data_get_string(s, VD_PIXEL_FORMAT), "XB24");
	/* The old source always paused while hidden. */
	assert_true(obs_data_get_bool(s, VD_DEACTIVATE_WHEN_HIDDEN));
	assert_int_equal(obs_data_get_int(s, "pipewire.control.brightness"), 5);
	assert_new_style_keys(s, NULL);
	obs_data_release(s);

	/* Newer settings: format and size together in "format". */
	s = json("{\"device_id\":\"node-7\","
		 "\"format\":\"{\\\"encoded\\\":false,\\\"video_format\\\":23,\\\"width\\\":1920,"
		 "\\\"height\\\":1080}\"}");
	vd_migrate_pipewire_camera(NULL, "pipewire-camera-source", s);
	assert_string_equal(obs_data_get_string(s, VD_PIXEL_FORMAT), "NV12");
	assert_string_equal(obs_data_get_string(s, VD_RESOLUTION), "1920x1080");
	assert_false(obs_data_has_user_value(s, "format"));
	assert_new_style_keys(s, NULL);
	obs_data_release(s);
}

static void migrate_v4l2_controls(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "device_id", "/dev/video0");
	obs_data_set_bool(s, "White Balance, Auto", true);
	obs_data_set_int(s, "Power Line Frequency", 2);

	vd_migrate_v4l2(NULL, "v4l2_input", s);
	assert_true(obs_data_get_bool(s, "v4l2.control.white-balance-auto"));
	assert_int_equal(obs_data_get_int(s, "v4l2.control.power-line-frequency"), 2);
	assert_new_style_keys(s, NULL);
	obs_data_release(s);
}

static void control_keys_and_colors(void **state)
{
	UNUSED_PARAMETER(state);
	struct dstr key = {0};
	vd_control_key(&key, "v4l2", "White Balance, Auto");
	assert_string_equal(key.array, "v4l2.control.white-balance-auto");
	vd_control_key(&key, "pipewire", "  Exposure (Absolute) ");
	assert_string_equal(key.array, "pipewire.control.exposure-absolute");
	dstr_free(&key);

	assert_int_equal(vd_parse_color_space("2100pq"), VIDEO_CS_2100_PQ);
	assert_int_equal(vd_parse_color_space("auto"), VIDEO_CS_DEFAULT);
	assert_int_equal(vd_parse_color_range("full"), VIDEO_RANGE_FULL);
	assert_true(vd_is_auto("auto"));
	assert_true(vd_is_auto(""));
	assert_false(vd_is_auto("709"));

	struct obs_source_frame f = {0};
	f.format = VIDEO_FORMAT_BGRA;
	vd_frame_set_color(&f, VIDEO_CS_2100_HLG, VIDEO_RANGE_FULL);
	assert_int_equal(f.trc, VIDEO_TRC_HLG);
	assert_true(f.full_range);
}

static void migrate_avfoundation(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = json("{\"device\":\"0x1234\",\"device_name\":\"FaceTime HD\",\"enable_audio\":false,"
			     "\"preset\":\"AVCaptureSessionPreset1280x720\",\"video_range\":2,"
			     "\"color_space\":3,\"buffering\":true}");
	vd_migrate_avfoundation(NULL, "macos-avcapture-fast", s);
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_BACKEND), "avfoundation");
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_ID), "0x1234");
	assert_string_equal(obs_data_get_string(s, VD_DEVICE_NAME), "FaceTime HD");
	assert_false(obs_data_get_bool(s, VD_CAPTURE_AUDIO));
	assert_true(obs_data_get_bool(s, "avfoundation.use-preset"));
	assert_string_equal(obs_data_get_string(s, "avfoundation.preset"), "AVCaptureSessionPreset1280x720");
	assert_string_equal(obs_data_get_string(s, VD_COLOR_RANGE), "full");
	assert_string_equal(obs_data_get_string(s, VD_COLOR_SPACE), "709");
	assert_string_equal(obs_data_get_string(s, VD_BUFFERING), "on");
	assert_new_style_keys(s, NULL);
	obs_data_release(s);
}

/* ------------------------------------------------------------------------- */
/* Source behaviour                                                          */

static void old_id_creates_video_device(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, "device_id", "/dev/video2");

	obs_source_t *src = obs_source_create("v4l2_input", "cam", s, NULL);
	assert_non_null(src);
	assert_string_equal(obs_source_get_id(src), VIDEO_DEVICE_SOURCE_ID);
	assert_int_equal(fake_b_state.creates, 1);
	assert_string_equal(fake_b_state.last_device, "/dev/video2");
	assert_int_equal(fake_a_state.creates, 0);

	obs_source_release(src);
	obs_wait_for_destroy_queue();
	assert_int_equal(fake_b_state.destroys, 1);
	obs_data_release(s);
}

static void auto_prefers_device_owner_then_first(void **state)
{
	UNUSED_PARAMETER(state);

	/* No device saved: first available backend. */
	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "a", NULL, NULL);
	assert_int_equal(fake_a_state.creates, 1);
	obs_source_release(src);

	/* Saved device belongs to fake_b: Auto uses fake_b. */
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_DEVICE_BACKEND, "fake_b");
	obs_data_set_string(s, VD_DEVICE_ID, "fake_b-cam1");
	src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "b", s, NULL);
	assert_int_equal(fake_b_state.creates, 1);
	obs_source_release(src);

	/* fake_a unavailable: the first available backend is fake_b. */
	fake_a_state.available = false;
	src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "c", NULL, NULL);
	assert_int_equal(fake_a_state.creates, 1);
	assert_int_equal(fake_b_state.creates, 2);
	obs_source_release(src);
	obs_data_release(s);
}

/* Video sources apply updates on the next video tick, and these tests run
 * without a video thread, so call the source's update directly. */
static void apply_update(obs_source_t *src, obs_data_t *s)
{
	obs_data_t *cur = obs_source_get_settings(src);
	obs_data_apply(cur, s);
	video_device_capture_info.update(obs_obj_get_data(src), cur);
	obs_data_release(cur);
}

static void explicit_backend_and_switching(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "fake_b");

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "x", s, NULL);
	assert_int_equal(fake_b_state.creates, 1);

	/* Same backend: update in place. */
	obs_data_set_string(s, VD_DEVICE_ID, "fake_b-cam0");
	apply_update(src, s);
	assert_int_equal(fake_b_state.updates, 1);
	assert_string_equal(fake_b_state.last_device, "fake_b-cam0");

	/* Other backend: the old instance goes, a new one starts. */
	obs_data_set_string(s, VD_BACKEND, "fake_a");
	apply_update(src, s);
	assert_int_equal(fake_b_state.destroys, 1);
	assert_int_equal(fake_a_state.creates, 1);

	obs_source_release(src);
	obs_wait_for_destroy_queue();
	assert_int_equal(fake_a_state.destroys, 1);
	obs_data_release(s);
}

static void missing_device_stays_listed(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "fake_a");
	obs_data_set_string(s, VD_DEVICE_ID, "gone-device");
	obs_data_set_string(s, VD_DEVICE_NAME, "Old Camera");

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "p", s, NULL);
	obs_properties_t *props = obs_source_properties(src);
	obs_property_t *list = obs_properties_get(props, VD_DEVICE_ID);
	assert_non_null(list);
	assert_int_equal(obs_property_list_item_count(list), 3);
	assert_string_equal(obs_property_list_item_string(list, 2), "gone-device");
	assert_true(obs_property_list_item_disabled(list, 2));
	assert_non_null(strstr(obs_property_list_item_name(list, 2), "Old Camera"));

	assert_non_null(obs_properties_get(props, VD_COLOR_SPACE));
	assert_null(obs_properties_get(props, VD_CAPTURE_AUDIO)); /* fake_a has no audio */

	obs_properties_destroy(props);
	obs_source_release(src);
	obs_data_release(s);
}

static void deactivate_when_hidden_releases_backend(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "fake_a");

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "h", s, NULL);
	void *data = obs_obj_get_data(src);
	assert_int_equal(fake_a_state.creates, 1);

	/* Off: hiding keeps the backend. */
	video_device_capture_info.hide(data);
	video_device_capture_info.show(data);
	assert_int_equal(fake_a_state.destroys, 0);

	/* On: fake_a has no hide of its own, so hiding destroys it, an update
	 * while hidden waits, and showing creates it again. */
	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, true);
	apply_update(src, s);
	video_device_capture_info.hide(data);
	assert_int_equal(fake_a_state.destroys, 1);
	apply_update(src, s);
	assert_int_equal(fake_a_state.creates, 1);
	video_device_capture_info.show(data);
	assert_int_equal(fake_a_state.creates, 2);

	obs_source_release(src);
	obs_wait_for_destroy_queue();
	assert_int_equal(fake_a_state.destroys, 2);
	obs_data_release(s);
}

/* Sources start out hidden. With the option on, the device isn't opened
 * until the source shows. */
static void created_hidden_waits_for_show(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "fake_a");
	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, true);

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "h", s, NULL);
	void *data = obs_obj_get_data(src);
	assert_int_equal(fake_a_state.creates, 0);

	video_device_capture_info.show(data);
	assert_int_equal(fake_a_state.creates, 1);
	video_device_capture_info.hide(data);
	assert_int_equal(fake_a_state.destroys, 1);

	obs_source_release(src);
	obs_wait_for_destroy_queue();
	assert_int_equal(fake_a_state.destroys, 1);
	obs_data_release(s);
}

static void created_hidden_pauses(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "pausable");
	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, true);

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "h", s, NULL);
	void *data = obs_obj_get_data(src);
	assert_int_equal(fake_b_state.creates, 1);
	assert_int_equal(fake_b_state.hides, 1);

	/* Hiding again doesn't hide twice; showing resumes once. */
	video_device_capture_info.hide(data);
	assert_int_equal(fake_b_state.hides, 1);
	video_device_capture_info.show(data);
	video_device_capture_info.show(data);
	assert_int_equal(fake_b_state.shows, 1);
	assert_int_equal(fake_b_state.destroys, 0);

	obs_source_release(src);
	obs_data_release(s);
}

/* Turning the option on while hidden lets go of the device right away, and
 * turning it off again takes it back. */
static void toggling_while_hidden(void **state)
{
	UNUSED_PARAMETER(state);
	obs_data_t *s = obs_data_create();
	obs_data_set_string(s, VD_BACKEND, "fake_a");

	obs_source_t *src = obs_source_create(VIDEO_DEVICE_SOURCE_ID, "h", s, NULL);
	assert_int_equal(fake_a_state.creates, 1);

	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, true);
	apply_update(src, s);
	assert_int_equal(fake_a_state.destroys, 1);

	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, false);
	apply_update(src, s);
	assert_int_equal(fake_a_state.creates, 2);

	obs_source_release(src);
	obs_wait_for_destroy_queue();
	assert_int_equal(fake_a_state.destroys, 2);
	obs_data_release(s);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test_setup_teardown(migrate_dshow_custom, setup, teardown),
		cmocka_unit_test_setup_teardown(migrate_dshow_preferred_and_matching, setup, teardown),
		cmocka_unit_test_setup_teardown(migrate_v4l2_packed_formats, setup, teardown),
		cmocka_unit_test_setup_teardown(migrate_pipewire_camera, setup, teardown),
		cmocka_unit_test_setup_teardown(migrate_v4l2_controls, setup, teardown),
		cmocka_unit_test_setup_teardown(control_keys_and_colors, setup, teardown),
		cmocka_unit_test_setup_teardown(migrate_avfoundation, setup, teardown),
		cmocka_unit_test_setup_teardown(old_id_creates_video_device, setup, teardown),
		cmocka_unit_test_setup_teardown(auto_prefers_device_owner_then_first, setup, teardown),
		cmocka_unit_test_setup_teardown(explicit_backend_and_switching, setup, teardown),
		cmocka_unit_test_setup_teardown(missing_device_stays_listed, setup, teardown),
		cmocka_unit_test_setup_teardown(deactivate_when_hidden_releases_backend, setup, teardown),
		cmocka_unit_test_setup_teardown(created_hidden_waits_for_show, setup, teardown),
		cmocka_unit_test_setup_teardown(created_hidden_pauses, setup, teardown),
		cmocka_unit_test_setup_teardown(toggling_while_hidden, setup, teardown),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
