/*
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */
#include "migrate.h"
#include "video-device.h"
#include "backends/dshow/dshow-formats.h"

#include <util/darray.h>
#include <util/dstr.h>

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */

static bool has(obs_data_t *s, const char *key)
{
	return obs_data_has_user_value(s, key);
}

/* Moves a user value to a new key, keeping its type. Values that only exist
 * as defaults are not moved: the new source has its own defaults. */
static void move_key(obs_data_t *s, const char *from, const char *to)
{
	if (!has(s, from)) {
		return;
	}

	obs_data_item_t *item = obs_data_item_byname(s, from);
	switch (obs_data_item_gettype(item)) {
	case OBS_DATA_STRING:
		obs_data_set_string(s, to, obs_data_item_get_string(item));
		break;
	case OBS_DATA_NUMBER:
		if (obs_data_item_numtype(item) == OBS_DATA_NUM_DOUBLE) {
			obs_data_set_double(s, to, obs_data_item_get_double(item));
		} else {
			obs_data_set_int(s, to, obs_data_item_get_int(item));
		}
		break;
	case OBS_DATA_BOOLEAN:
		obs_data_set_bool(s, to, obs_data_item_get_bool(item));
		break;
	case OBS_DATA_OBJECT: {
		obs_data_t *obj = obs_data_item_get_obj(item);
		obs_data_set_obj(s, to, obj);
		obs_data_release(obj);
		break;
	}
	case OBS_DATA_ARRAY: {
		obs_data_array_t *arr = obs_data_item_get_array(item);
		obs_data_set_array(s, to, arr);
		obs_data_array_release(arr);
		break;
	}
	case OBS_DATA_NULL:
		break;
	}
	obs_data_item_release(&item);
	obs_data_erase(s, from);
}

/* Moves a key into the backend's namespace: "<backend>.<key>", with '_'
 * spelled '-' as in every new key. */
static void move_backend_key(obs_data_t *s, const char *backend, const char *key)
{
	struct dstr to = {0};
	dstr_printf(&to, "%s.%s", backend, key);
	dstr_replace(&to, "_", "-");
	move_key(s, key, to.array);
	dstr_free(&to);
}

static bool is_number(obs_data_t *s, const char *key)
{
	obs_data_item_t *item = obs_data_item_byname(s, key);
	bool number = item && obs_data_item_gettype(item) == OBS_DATA_NUMBER;
	obs_data_item_release(&item);
	return number;
}

/* Replaces a value that may have had another type under the same key. */
static void replace_string(obs_data_t *s, const char *key, const char *value)
{
	obs_data_erase(s, key);
	obs_data_set_string(s, key, value);
}

/* Keys the new source itself uses. Everything else left over after a
 * migration that has no '.' is a device control saved under its own name. */
static bool is_common_key(const char *key)
{
	static const char *const keys[] = {
		VD_BACKEND,       VD_DEVICE_BACKEND,         VD_DEVICE_ID,     VD_DEVICE_NAME, VD_RESOLUTION,
		VD_FRAME_RATE,    VD_PIXEL_FORMAT,           VD_COLOR_SPACE,   VD_COLOR_RANGE, VD_BUFFERING,
		VD_FLIP_VERTICAL, VD_DEACTIVATE_WHEN_HIDDEN, VD_CAPTURE_AUDIO,
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		if (strcmp(key, keys[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* Moves the camera controls, which the old sources saved under the device's
 * own control names ("White Balance Temperature"), to their new keys. */
static void move_controls(obs_data_t *s, const char *backend)
{
	DARRAY(char *) names;
	da_init(names);
	for (obs_data_item_t *item = obs_data_first(s); item; obs_data_item_next(&item)) {
		const char *name = obs_data_item_get_name(item);
		if (!strchr(name, '.') && !is_common_key(name) && obs_data_item_has_user_value(item)) {
			char *copy = bstrdup(name);
			da_push_back(names, &copy);
		}
	}

	struct dstr key = {0};
	for (size_t i = 0; i < names.num; i++) {
		vd_control_key(&key, backend, names.array[i]);
		move_key(s, names.array[i], key.array);
		bfree(names.array[i]);
	}
	dstr_free(&key);
	da_free(names);
}

/* The JSON object saved as a string under key, or NULL when there is none
 * (missing, empty, or another type). */
static obs_data_t *json_value(obs_data_t *s, const char *key)
{
	obs_data_item_t *item = obs_data_item_byname(s, key);
	bool is_string = item && obs_data_item_gettype(item) == OBS_DATA_STRING;
	obs_data_item_release(&item);
	if (!is_string) {
		return NULL;
	}
	const char *text = obs_data_get_string(s, key);
	return *text == '{' ? obs_data_create_from_json(text) : NULL;
}

static void set_device(obs_data_t *s, const char *backend, const char *id, const char *name)
{
	obs_data_set_string(s, VD_BACKEND, backend);
	obs_data_set_string(s, VD_DEVICE_BACKEND, backend);
	obs_data_set_string(s, VD_DEVICE_ID, id ? id : "");
	obs_data_set_string(s, VD_DEVICE_NAME, name ? name : "");
}

static void set_resolution(obs_data_t *s, long long w, long long h)
{
	if (w <= 0 || h <= 0) {
		return;
	}
	char buf[48];
	snprintf(buf, sizeof(buf), "%lldx%lld", w, h);
	obs_data_set_string(s, VD_RESOLUTION, buf);
}

static void set_fps(obs_data_t *s, long long num, long long den)
{
	if (num <= 0 || den <= 0) {
		return;
	}
	char buf[48];
	snprintf(buf, sizeof(buf), "%lld/%lld", num, den);
	obs_data_set_string(s, VD_FRAME_RATE, buf);
}

/* "default"/"709"/"2100PQ"... and "partial"/"full" as used by DirectShow and
 * AVFoundation, case-insensitively. Anything unknown becomes "auto". */
static const char *map_color_space(const char *v)
{
	if (!v) {
		return VD_AUTO;
	}
	if (astrcmpi(v, "601") == 0) {
		return "601";
	}
	if (astrcmpi(v, "709") == 0) {
		return "709";
	}
	if (astrcmpi(v, "2100PQ") == 0) {
		return "2100pq";
	}
	if (astrcmpi(v, "2100HLG") == 0) {
		return "2100hlg";
	}
	return VD_AUTO;
}

static const char *map_color_range(const char *v)
{
	if (!v) {
		return VD_AUTO;
	}
	if (astrcmpi(v, "partial") == 0) {
		return "partial";
	}
	if (astrcmpi(v, "full") == 0) {
		return "full";
	}
	return VD_AUTO;
}

/* enum video_range_type, used by V4L2 and AVFoundation. */
static const char *map_video_range(long long v)
{
	switch (v) {
	case VIDEO_RANGE_PARTIAL:
		return "partial";
	case VIDEO_RANGE_FULL:
		return "full";
	default:
		return VD_AUTO;
	}
}

/* enum video_colorspace, used by AVFoundation. */
static const char *map_video_colorspace(long long v)
{
	switch (v) {
	case VIDEO_CS_601:
		return "601";
	case VIDEO_CS_709:
	case VIDEO_CS_SRGB:
		return "709";
	case VIDEO_CS_2100_PQ:
		return "2100pq";
	case VIDEO_CS_2100_HLG:
		return "2100hlg";
	default:
		return VD_AUTO;
	}
}

/* Old V4L2 settings pack two values in one integer: 16 bits each before
 * the switch to 64 bit, 32 bits each after. */
static void unpack_tuple(long long packed, long long *a, long long *b)
{
	unsigned long long u = (unsigned long long)packed;
	if ((u & 0xffffffff00000000ULL) == 0) {
		*a = (long long)((u >> 16) & 0xffff);
		*b = (long long)(u & 0xffff);
	} else {
		*a = (long long)(int32_t)(u >> 32);
		*b = (long long)(int32_t)(u & 0xffffffff);
	}
}

static void fourcc_to_string(uint32_t fourcc, char out[5])
{
	for (int i = 0; i < 4; i++) {
		char c = (char)((fourcc >> (8 * i)) & 0xff);
		out[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
	}
	out[4] = 0;
}

/* ------------------------------------------------------------------------- */
/* DirectShow (dshow_input)                                                  */

/* dshow device IDs are "name:path", with '#' and ':' escaped as "#22" and
 * "#3A" in both parts. The name is the readable part. */
static void dshow_device_name(const char *encoded, struct dstr *name)
{
	const char *colon = encoded ? strchr(encoded, ':') : NULL;
	if (!colon) {
		dstr_copy(name, encoded ? encoded : "");
		return;
	}
	dstr_ncopy(name, encoded, colon - encoded);
	dstr_replace(name, "#3A", ":");
	dstr_replace(name, "#22", "#");
}

#define DSHOW_FPS_HIGHEST 0LL
#define DSHOW_FPS_MATCHING -1LL
#define DSHOW_RES_PREFERRED 0LL

void vd_migrate_dshow(void *param, const char *old_id, obs_data_t *s)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(old_id);

	const char *id = obs_data_get_string(s, "video_device_id");
	struct dstr name = {0};
	dshow_device_name(id, &name);
	set_device(s, "dshow", id, name.array);
	dstr_free(&name);
	obs_data_erase(s, "video_device_id");
	obs_data_erase(s, "last_video_device_id");

	/* Resolution only applies with the custom resolution type. */
	bool custom_res = has(s, "res_type") && obs_data_get_int(s, "res_type") != DSHOW_RES_PREFERRED;
	if (custom_res) {
		int w = 0, h = 0;
		if (sscanf(obs_data_get_string(s, "resolution"), "%dx%d", &w, &h) == 2) {
			set_resolution(s, w, h);
		} else {
			obs_data_erase(s, VD_RESOLUTION);
		}

		/* frame_interval is in 100ns units. */
		long long interval = obs_data_get_int(s, "frame_interval");
		if (interval == DSHOW_FPS_MATCHING) {
			obs_data_set_string(s, VD_FRAME_RATE, VD_FRAME_RATE_MATCH);
		} else if (interval > DSHOW_FPS_HIGHEST) {
			/* As the listed rate, so the list shows it selected. */
			const struct vd_dshow_frame_rate *rate = vd_dshow_find_frame_rate(interval);
			if (rate) {
				set_fps(s, rate->num, rate->den);
			} else {
				set_fps(s, 10000000, interval);
			}
		}

		/* The DShow::VideoFormat number; Any and unknown ones mean
		 * automatic. */
		const char *fourcc = vd_dshow_format_fourcc((int)obs_data_get_int(s, "video_format"));
		if (fourcc) {
			obs_data_set_string(s, VD_PIXEL_FORMAT, fourcc);
		}
	} else {
		obs_data_erase(s, VD_RESOLUTION);
	}
	obs_data_erase(s, "video_format");
	obs_data_erase(s, "res_type");
	obs_data_erase(s, "frame_interval");
	obs_data_erase(s, "last_resolution");

	if (has(s, "color_space")) {
		replace_string(s, VD_COLOR_SPACE, map_color_space(obs_data_get_string(s, "color_space")));
	}
	if (has(s, "color_range")) {
		replace_string(s, VD_COLOR_RANGE, map_color_range(obs_data_get_string(s, "color_range")));
	}
	obs_data_erase(s, "color_space");
	obs_data_erase(s, "color_range");

	if (has(s, "buffering")) {
		static const char *buffering[] = {VD_AUTO, "on", "off"};
		long long b = obs_data_get_int(s, "buffering");
		replace_string(s, VD_BUFFERING, (b >= 0 && b <= 2) ? buffering[b] : VD_AUTO);
	}

	move_key(s, "flip_vertically", VD_FLIP_VERTICAL);
	move_key(s, "deactivate_when_not_showing", VD_DEACTIVATE_WHEN_HIDDEN);

	move_backend_key(s, "dshow", "audio_output_mode");
	move_backend_key(s, "dshow", "use_custom_audio_device");
	move_backend_key(s, "dshow", "audio_device_id");
	move_backend_key(s, "dshow", "autorotation");
	move_backend_key(s, "dshow", "hw_decode");
	move_backend_key(s, "dshow", "active");
}

/* ------------------------------------------------------------------------- */
/* V4L2 (v4l2_input)                                                         */

void vd_migrate_v4l2(void *param, const char *old_id, obs_data_t *s)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(old_id);

	/* The device path is the ID. V4L2 settings never stored a name. */
	set_device(s, "v4l2", obs_data_get_string(s, "device_id"), "");
	obs_data_erase(s, "device_id");

	if (has(s, "resolution")) {
		long long packed = obs_data_get_int(s, "resolution");
		obs_data_erase(s, VD_RESOLUTION);
		if (packed != -1) {
			long long w, h;
			unpack_tuple(packed, &w, &h);
			set_resolution(s, w, h);
		}
	}

	/* Stored as the V4L2 time per frame, so fps = denominator / numerator. */
	if (has(s, "framerate")) {
		long long packed = obs_data_get_int(s, "framerate");
		if (packed != -1) {
			long long num, den;
			unpack_tuple(packed, &num, &den);
			set_fps(s, den, num);
		}
		obs_data_erase(s, "framerate");
	}

	if (has(s, "pixelformat")) {
		long long fmt = obs_data_get_int(s, "pixelformat");
		if (fmt != -1 && fmt != 0) {
			char fourcc[5];
			fourcc_to_string((uint32_t)fmt, fourcc);
			obs_data_set_string(s, VD_PIXEL_FORMAT, fourcc);
		}
		obs_data_erase(s, "pixelformat");
	}

	if (has(s, "color_range")) {
		replace_string(s, VD_COLOR_RANGE, map_video_range(obs_data_get_int(s, "color_range")));
		obs_data_erase(s, "color_range");
	}

	if (has(s, "buffering")) {
		replace_string(s, VD_BUFFERING, obs_data_get_bool(s, "buffering") ? "on" : "off");
	}

	move_backend_key(s, "v4l2", "input");
	move_backend_key(s, "v4l2", "standard");
	move_backend_key(s, "v4l2", "dv_timing");
	move_backend_key(s, "v4l2", "auto_reset");
	move_backend_key(s, "v4l2", "timeout_frames");

	move_controls(s, "v4l2");
}

/* ------------------------------------------------------------------------- */
/* PipeWire camera portal (pipewire-camera-source)                           */

/* enum spa_video_format values the camera could save, as the DRM FourCC the
 * PipeWire backend shows. Numbers, so this builds without PipeWire. */
static const char *spa_format_fourcc(long long spa_format)
{
	switch (spa_format) {
	case 2: /* SPA_VIDEO_FORMAT_I420 */
		return "YU12";
	case 3: /* SPA_VIDEO_FORMAT_YV12 */
		return "YV12";
	case 4: /* SPA_VIDEO_FORMAT_YUY2 */
		return "YUYV";
	case 5: /* SPA_VIDEO_FORMAT_UYVY */
		return "UYVY";
	case 7: /* SPA_VIDEO_FORMAT_RGBx */
		return "XB24";
	case 8: /* SPA_VIDEO_FORMAT_BGRx */
		return "XR24";
	case 11: /* SPA_VIDEO_FORMAT_RGBA */
		return "AB24";
	case 12: /* SPA_VIDEO_FORMAT_BGRA */
		return "AR24";
	case 23: /* SPA_VIDEO_FORMAT_NV12 */
		return "NV12";
	default:
		return NULL;
	}
}

static void set_pixel_format_from_spa(obs_data_t *s, long long spa_format)
{
	const char *fourcc = spa_format_fourcc(spa_format);
	if (fourcc) {
		obs_data_set_string(s, VD_PIXEL_FORMAT, fourcc);
	}
}

void vd_migrate_pipewire_camera(void *param, const char *old_id, obs_data_t *s)
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(old_id);

	set_device(s, "pipewire", obs_data_get_string(s, "device_id"), "");
	obs_data_erase(s, "device_id");

	/* Newer versions saved format and size together as JSON in "format";
	 * older ones saved "pixelformat" and a "resolution" JSON. */
	long long w = 0, h = 0;
	if (has(s, "format")) {
		obs_data_t *fmt = json_value(s, "format");
		if (fmt) {
			if (!obs_data_get_bool(fmt, "encoded")) {
				set_pixel_format_from_spa(s, obs_data_get_int(fmt, "video_format"));
			}
			w = obs_data_get_int(fmt, "width");
			h = obs_data_get_int(fmt, "height");
		}
		obs_data_release(fmt);
	} else {
		if (has(s, "pixelformat")) {
			set_pixel_format_from_spa(s, obs_data_get_int(s, "pixelformat"));
		}
		if (has(s, "resolution")) {
			obs_data_t *res = json_value(s, "resolution");
			if (res) {
				w = obs_data_get_int(res, "width");
				h = obs_data_get_int(res, "height");
			}
			obs_data_release(res);
		}
	}
	obs_data_erase(s, "format");
	obs_data_erase(s, "pixelformat");
	obs_data_erase(s, VD_RESOLUTION);
	set_resolution(s, w, h);

	if (has(s, "framerate")) {
		obs_data_t *fr = json_value(s, "framerate");
		struct media_frames_per_second fps = {0};
		if (fr && obs_data_get_frames_per_second(fr, "framerate", &fps, NULL)) {
			set_fps(s, fps.numerator, fps.denominator);
		}
		obs_data_release(fr);
		obs_data_erase(s, "framerate");
	}

	/* The old source always paused its stream while hidden. */
	obs_data_set_bool(s, VD_DEACTIVATE_WHEN_HIDDEN, true);

	move_controls(s, "pipewire");
}

/* ------------------------------------------------------------------------- */
/* AVFoundation (macos-avcapture, macos-avcapture-fast, av_capture_input)    */

void vd_migrate_avfoundation(void *param, const char *old_id, obs_data_t *s)
{
	UNUSED_PARAMETER(param);

	/* "device" is the AVCaptureDevice uniqueID. */
	const char *name = obs_data_get_string(s, "device_name");
	struct dstr name_copy = {0};
	dstr_copy(&name_copy, name);
	set_device(s, "avfoundation", obs_data_get_string(s, "device"), name_copy.array);
	dstr_free(&name_copy);
	obs_data_erase(s, "device");
	obs_data_erase(s, "device_name");

	/* The fast variant always captured with presets. */
	if (old_id && strcmp(old_id, "macos-avcapture-fast") == 0 && !has(s, "use_preset")) {
		obs_data_set_bool(s, "avfoundation.use-preset", true);
	}

	move_key(s, "enable_audio", VD_CAPTURE_AUDIO);

	if (has(s, "color_space") && is_number(s, "color_space")) {
		replace_string(s, VD_COLOR_SPACE, map_video_colorspace(obs_data_get_int(s, "color_space")));
	}
	obs_data_erase(s, "color_space");
	if (has(s, "video_range")) {
		replace_string(s, VD_COLOR_RANGE, map_video_range(obs_data_get_int(s, "video_range")));
		obs_data_erase(s, "video_range");
	}
	if (has(s, "buffering")) {
		replace_string(s, VD_BUFFERING, obs_data_get_bool(s, "buffering") ? "on" : "off");
	}

	move_backend_key(s, "avfoundation", "use_preset");
	move_backend_key(s, "avfoundation", "preset");
	move_backend_key(s, "avfoundation", "input_format");
	move_backend_key(s, "avfoundation", "frame_rate");
	move_backend_key(s, "avfoundation", "resolution");
}

/* ------------------------------------------------------------------------- */

void vd_register_aliases(void)
{
	struct {
		const char *id;
		obs_source_alias_migrate_t migrate;
	} aliases[] = {
		{"dshow_input", vd_migrate_dshow},
		{"v4l2_input", vd_migrate_v4l2},
		{"pipewire-camera-source", vd_migrate_pipewire_camera},
		{"macos-avcapture", vd_migrate_avfoundation},
		{"macos-avcapture-fast", vd_migrate_avfoundation},
		{"av_capture_input", vd_migrate_avfoundation},
		{"av_capture_input_v2", vd_migrate_avfoundation},
	};

	for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
		obs_register_source_alias(aliases[i].id, VIDEO_DEVICE_SOURCE_ID, aliases[i].migrate, NULL);
	}
}
