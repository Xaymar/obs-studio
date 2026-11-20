/*
 * PipeWire camera backend: cameras through the XDG camera portal.
 *
 * Ported from linux-pipewire's camera-portal.c.
 *
 * Copyright 2021 columbarius <co1umbarius@protonmail.com>
 * Copyright 2021 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "video-device.h"

#include "formats.h"
#include "pipewire.h"
#include "portal.h"

#include <util/darray.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <spa/node/keys.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/defs.h>
#include <spa/utils/keys.h>
#include <spa/utils/result.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BACKEND_NAME "pipewire"
#define S_CONTROLS "pipewire.controls"

/* ------------------------------------------------------------------------- */
/* Devices                                                                   */

struct param {
	uint32_t id;
	int32_t seq;
	struct spa_list link;
	struct spa_pod *param;
};

struct camera_device {
	uint32_t id;
	struct pw_properties *properties;
	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook node_listener;
	struct pw_node_info *info;

	struct spa_list pending_list;
	struct spa_list param_list;
	int pending_sync;
};

struct camera_source;

/* One portal connection for every camera source. Created on the UI thread;
 * the device table and the source list are only touched under the PipeWire
 * loop lock once obs_pw exists. */
struct portal_connection {
	obs_pipewire *obs_pw;
	GHashTable *devices; /* node name -> struct camera_device */
	GCancellable *cancellable;
	DARRAY(struct camera_source *) sources;
	bool requested;
};

static struct portal_connection *connection;
static GDBusProxy *camera_proxy;
static pthread_once_t pw_init_once = PTHREAD_ONCE_INIT;
static bool pw_initialized;

static void init_pipewire(void)
{
	pw_init(NULL, NULL);
	pw_initialized = true;
}

static uint32_t clear_params(struct spa_list *param_list, uint32_t id)
{
	struct param *p, *t;
	uint32_t count = 0;

	spa_list_for_each_safe(p, t, param_list, link)
	{
		if (id == SPA_ID_INVALID || p->id == id) {
			spa_list_remove(&p->link);
			free(p);
			count++;
		}
	}
	return count;
}

static struct param *add_param(struct spa_list *params, int seq, uint32_t id, const struct spa_pod *param)
{
	struct param *p;

	if (id == SPA_ID_INVALID) {
		if (param == NULL || !spa_pod_is_object(param)) {
			errno = EINVAL;
			return NULL;
		}
		id = SPA_POD_OBJECT_ID(param);
	}

	p = malloc(sizeof(*p) + (param != NULL ? SPA_POD_SIZE(param) : 0));
	if (p == NULL) {
		return NULL;
	}

	p->id = id;
	p->seq = seq;
	if (param != NULL) {
		p->param = SPA_PTROFF(p, sizeof(*p), struct spa_pod);
		memcpy(p->param, param, SPA_POD_SIZE(param));
	} else {
		clear_params(params, id);
		p->param = NULL;
	}
	spa_list_append(params, &p->link);

	return p;
}

static void object_update_params(struct spa_list *param_list, struct spa_list *pending_list, uint32_t n_params,
				 struct spa_param_info *params)
{
	struct param *p, *t;

	for (uint32_t i = 0; i < n_params; i++) {
		spa_list_for_each_safe(p, t, pending_list, link)
		{
			if (p->id == params[i].id && p->seq != params[i].seq && p->param != NULL) {
				spa_list_remove(&p->link);
				free(p);
			}
		}
	}
	spa_list_consume(p, pending_list, link)
	{
		spa_list_remove(&p->link);
		if (p->param == NULL) {
			clear_params(param_list, p->id);
			free(p);
		} else {
			spa_list_append(param_list, &p->link);
		}
	}
}

static struct camera_device *camera_device_new(uint32_t id, const struct spa_dict *properties)
{
	struct camera_device *device = bzalloc(sizeof(struct camera_device));
	device->id = id;
	device->properties = properties ? pw_properties_new_dict(properties) : NULL;
	spa_list_init(&device->pending_list);
	spa_list_init(&device->param_list);
	return device;
}

static void camera_device_free(struct camera_device *device)
{
	if (!device) {
		return;
	}

	clear_params(&device->pending_list, SPA_ID_INVALID);
	clear_params(&device->param_list, SPA_ID_INVALID);
	g_clear_pointer(&device->info, pw_node_info_free);
	g_clear_pointer(&device->proxy, pw_proxy_destroy);
	g_clear_pointer(&device->properties, pw_properties_free);
	bfree(device);
}

static struct camera_device *find_device(const char *id)
{
	if (!connection || !connection->devices || !id || !*id) {
		return NULL;
	}
	return g_hash_table_lookup(connection->devices, id);
}

/* ------------------------------------------------------------------------- */
/* Formats offered by a device                                               */

/* The raw formats async frames can carry, which is what the backend lists. */
static const uint32_t candidate_formats[] = {
	SPA_VIDEO_FORMAT_YUY2, SPA_VIDEO_FORMAT_UYVY, SPA_VIDEO_FORMAT_NV12, SPA_VIDEO_FORMAT_I420,
	SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_BGRx,
};

static void fourcc_string(uint32_t fourcc, char out[5])
{
	for (int i = 0; i < 4; i++) {
		char c = (char)((fourcc >> (8 * i)) & 0xff);
		out[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
	}
	out[4] = 0;
}

/* VD_PIXEL_FORMAT is the DRM FourCC of the format. */
static bool format_from_fourcc(const char *fourcc, struct obs_pw_video_format *out)
{
	if (!fourcc || strlen(fourcc) != 4) {
		return false;
	}

	for (size_t i = 0; i < SPA_N_ELEMENTS(candidate_formats); i++) {
		struct obs_pw_video_format fmt;
		char name[5];
		if (!obs_pw_video_format_from_spa_format(candidate_formats[i], &fmt)) {
			continue;
		}
		fourcc_string(fmt.drm_format, name);
		if (strcmp(name, fourcc) == 0) {
			*out = fmt;
			return true;
		}
	}
	return false;
}

static bool is_candidate(uint32_t spa_format)
{
	for (size_t i = 0; i < SPA_N_ELEMENTS(candidate_formats); i++) {
		if (candidate_formats[i] == spa_format) {
			return true;
		}
	}
	return false;
}

/* One EnumFormat entry: a raw format, a size and its frame rates. */
struct format_entry {
	uint32_t spa_format;
	struct spa_rectangle size;
	DARRAY(struct spa_fraction) rates;
};

typedef void (*format_entry_cb)(void *param, const struct format_entry *entry);

static void for_each_format(struct camera_device *dev, format_entry_cb cb, void *param)
{
	struct param *p;
	struct format_entry entry;
	da_init(entry.rates);

	spa_list_for_each(p, &dev->param_list, link)
	{
		uint32_t media_type, media_subtype, format = 0;

		if (p->id != SPA_PARAM_EnumFormat || p->param == NULL) {
			continue;
		}
		if (spa_format_parse(p->param, &media_type, &media_subtype) < 0) {
			continue;
		}
		if (media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw) {
			continue;
		}
		if (spa_pod_parse_object(p->param, SPA_TYPE_OBJECT_Format, NULL, SPA_FORMAT_VIDEO_format,
					 SPA_POD_Id(&format)) < 0) {
			continue;
		}
		if (!is_candidate(format)) {
			continue;
		}
		if (spa_pod_parse_object(p->param, SPA_TYPE_OBJECT_Format, NULL, SPA_FORMAT_VIDEO_size,
					 SPA_POD_Rectangle(&entry.size)) < 0) {
			continue;
		}

		entry.spa_format = format;
		da_resize(entry.rates, 0);

		const struct spa_pod_prop *rate_prop = spa_pod_find_prop(p->param, NULL, SPA_FORMAT_VIDEO_framerate);
		if (rate_prop) {
			uint32_t n_rates;
			uint32_t choice;
			const struct spa_pod *rates = spa_pod_get_values(&rate_prop->value, &n_rates, &choice);
			if (rates->type == SPA_TYPE_Fraction) {
				const struct spa_fraction *values = SPA_POD_BODY(rates);
				if (choice == SPA_CHOICE_None) {
					da_push_back(entry.rates, &values[0]);
				} else if (choice == SPA_CHOICE_Enum) {
					/* values[0] is the default, repeated in the list. */
					for (uint32_t i = 1; i < n_rates; i++) {
						da_push_back(entry.rates, &values[i]);
					}
				}
				/* Ranges and steps: no list, the stream negotiates. */
			}
		}

		cb(param, &entry);
	}

	da_free(entry.rates);
}

static double fraction_value(const struct spa_fraction *f)
{
	return f->denom ? (double)f->num / (double)f->denom : 0.0;
}

/* ------------------------------------------------------------------------- */
/* Sources                                                                   */

struct camera_source {
	obs_source_t *source;
	obs_data_t *settings;
	obs_pipewire_stream *stream;
	char *device_id;

	/* Requested from the settings. */
	bool format_set;
	struct obs_pw_video_format format;
	bool resolution_set;
	struct spa_rectangle resolution;
	char *frame_rate;
	struct obs_pipewire_frame_overrides overrides;

	bool hidden;
};

struct rate_pick {
	const struct camera_source *cam;
	const struct spa_fraction *target; /* NULL: highest */
	bool found;
	struct spa_fraction best;
};

static void pick_rate_cb(void *param, const struct format_entry *entry)
{
	struct rate_pick *pick = param;
	const struct camera_source *cam = pick->cam;

	if (cam->format_set && entry->spa_format != cam->format.spa_format) {
		return;
	}
	if (cam->resolution_set &&
	    (entry->size.width != cam->resolution.width || entry->size.height != cam->resolution.height)) {
		return;
	}

	for (size_t i = 0; i < entry->rates.num; i++) {
		const struct spa_fraction *r = &entry->rates.array[i];
		bool better;
		if (!pick->found) {
			better = true;
		} else if (pick->target) {
			double t = fraction_value(pick->target);
			better = fabs(fraction_value(r) - t) < fabs(fraction_value(&pick->best) - t);
		} else {
			better = fraction_value(r) > fraction_value(&pick->best);
		}
		if (better) {
			pick->best = *r;
			pick->found = true;
		}
	}
}

/* VD_FRAME_RATE: "num/den", "" for the highest the device offers for the
 * chosen format and size, or "match" for the one closest to the output. */
static bool choose_frame_rate(const struct camera_source *cam, struct camera_device *dev, struct spa_fraction *out)
{
	const char *rate = cam->frame_rate ? cam->frame_rate : "";
	struct spa_fraction target;
	struct rate_pick pick = {cam, NULL, false, {0, 1}};

	if (strcmp(rate, VD_FRAME_RATE_MATCH) == 0) {
		struct obs_video_info ovi;
		if (!obs_get_video_info(&ovi)) {
			return false;
		}
		target = SPA_FRACTION(ovi.fps_num, ovi.fps_den);
		pick.target = &target;
	} else if (*rate) {
		unsigned int num, den;
		if (sscanf(rate, "%u/%u", &num, &den) == 2 && num > 0 && den > 0) {
			*out = SPA_FRACTION(num, den);
			return true;
		}
		return false;
	}

	if (dev) {
		for_each_format(dev, pick_rate_cb, &pick);
	}
	if (pick.found) {
		*out = pick.best;
	}
	return pick.found;
}

static void read_settings(struct camera_source *cam, obs_data_t *settings)
{
	g_clear_pointer(&cam->frame_rate, bfree);
	cam->frame_rate = bstrdup(obs_data_get_string(settings, VD_FRAME_RATE));

	cam->format_set = format_from_fourcc(obs_data_get_string(settings, VD_PIXEL_FORMAT), &cam->format);

	unsigned int w, h;
	cam->resolution_set = sscanf(obs_data_get_string(settings, VD_RESOLUTION), "%ux%u", &w, &h) == 2 && w > 0 &&
			      h > 0;
	cam->resolution = cam->resolution_set ? SPA_RECTANGLE(w, h) : SPA_RECTANGLE(0, 0);

	const char *space = obs_data_get_string(settings, VD_COLOR_SPACE);
	const char *range = obs_data_get_string(settings, VD_COLOR_RANGE);
	cam->overrides = (struct obs_pipewire_frame_overrides){
		.color_space_set = !vd_is_auto(space),
		.color_space = vd_parse_color_space(space),
		.color_range_set = !vd_is_auto(range),
		.color_range = vd_parse_color_range(range),
		.flip = obs_data_get_bool(settings, VD_FLIP_VERTICAL),
	};
}

/* Sets one saved control on the device. */
static void apply_control(struct camera_device *dev, obs_data_t *settings, const struct spa_pod *info)
{
	const struct spa_pod *type;
	const char *name = NULL;
	uint32_t id, n_vals, choice;

	if (spa_pod_parse_object(info, SPA_TYPE_OBJECT_PropInfo, NULL, SPA_PROP_INFO_id, SPA_POD_Id(&id),
				 SPA_PROP_INFO_description, SPA_POD_OPT_String(&name), SPA_PROP_INFO_type,
				 SPA_POD_PodChoice(&type)) < 0 ||
	    !name) {
		return;
	}

	struct dstr key = {0};
	vd_control_key(&key, BACKEND_NAME, name);
	if (!obs_data_has_user_value(settings, key.array)) {
		dstr_free(&key);
		return;
	}

	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f;
	const struct spa_pod *pod = spa_pod_get_values(type, &n_vals, &choice);
	bool known = true;

	spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	switch (SPA_POD_TYPE(pod)) {
	case SPA_TYPE_Bool:
		spa_pod_builder_add(&b, id, SPA_POD_Bool(obs_data_get_bool(settings, key.array)), 0);
		break;
	case SPA_TYPE_Float:
		spa_pod_builder_add(&b, id, SPA_POD_Float((float)obs_data_get_double(settings, key.array)), 0);
		break;
	case SPA_TYPE_Int:
		spa_pod_builder_add(&b, id, SPA_POD_Int((int32_t)obs_data_get_int(settings, key.array)), 0);
		break;
	default:
		known = false;
		break;
	}
	struct spa_pod *param = spa_pod_builder_pop(&b, &f);

	if (known && dev->proxy) {
		pw_node_set_param((struct pw_node *)dev->proxy, SPA_PARAM_Props, 0, param);
	}
	dstr_free(&key);
}

static void apply_controls(struct camera_device *dev, obs_data_t *settings)
{
	struct param *p;
	spa_list_for_each(p, &dev->param_list, link)
	{
		if (p->id == SPA_PARAM_PropInfo && p->param) {
			apply_control(dev, settings, p->param);
		}
	}
}

/* Brings the stream in line with the settings. Called with the loop lock
 * held, from the UI thread or from PipeWire callbacks. */
static void apply(struct camera_source *cam, bool restart)
{
	struct camera_device *dev = find_device(cam->device_id);

	if (!dev) {
		g_clear_pointer(&cam->stream, obs_pipewire_stream_destroy);
		return;
	}

	struct spa_fraction rate;
	bool rate_set = choose_frame_rate(cam, dev, &rate);

	if (restart || !cam->stream) {
		g_clear_pointer(&cam->stream, obs_pipewire_stream_destroy);

		blog(LOG_INFO, "[video-device/pipewire] '%s': streaming camera '%s'", obs_source_get_name(cam->source),
		     cam->device_id);

		enum spa_media_subtype subtype = SPA_MEDIA_SUBTYPE_raw;
		struct obs_pipewire_connect_stream_info info = {
			.stream_name = "Knurbs PipeWire Camera",
			.stream_properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
							       "Capture", PW_KEY_MEDIA_ROLE, "Camera", NULL),
			.video =
				{
					.subtype = &subtype,
					.format = cam->format_set ? &cam->format : NULL,
					.resolution = cam->resolution_set ? &cam->resolution : NULL,
					.framerate = rate_set ? &rate : NULL,
				},
		};
		cam->stream = obs_pipewire_connect_stream(connection->obs_pw, cam->source, dev->id, &info);
		if (cam->stream && cam->hidden) {
			obs_pipewire_stream_hide(cam->stream);
		}
	} else {
		obs_pipewire_stream_set_resolution(cam->stream, cam->resolution_set ? &cam->resolution : NULL);
		obs_pipewire_stream_set_framerate(cam->stream, rate_set ? &rate : NULL);
	}

	if (cam->stream) {
		obs_pipewire_stream_set_frame_overrides(cam->stream, &cam->overrides);
	}

	apply_controls(dev, cam->settings);
}

static void lock(void)
{
	if (connection && connection->obs_pw) {
		obs_pipewire_lock(connection->obs_pw);
	}
}

static void unlock(void)
{
	if (connection && connection->obs_pw) {
		obs_pipewire_unlock(connection->obs_pw);
	}
}

static void refresh_sources(const char *device_id, bool restart)
{
	for (size_t i = 0; i < connection->sources.num; i++) {
		struct camera_source *cam = connection->sources.array[i];
		obs_source_update_properties(cam->source);
		if (device_id && cam->device_id && strcmp(cam->device_id, device_id) == 0) {
			apply(cam, restart);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* PipeWire callbacks (loop lock held)                                       */

static void node_info(void *data, const struct pw_node_info *info)
{
	struct camera_device *device = data;
	uint32_t changed = 0;

	info = device->info = pw_node_info_update(device->info, info);
	if (info == NULL) {
		return;
	}

	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		for (uint32_t i = 0; i < info->n_params; i++) {
			uint32_t id = info->params[i].id;

			if (info->params[i].user == 0) {
				continue;
			}
			info->params[i].user = 0;

			changed++;
			add_param(&device->pending_list, 0, id, NULL);
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ)) {
				continue;
			}

			int res = pw_node_enum_params((struct pw_node *)device->proxy, ++info->params[i].seq, id, 0, -1,
						      NULL);
			if (SPA_RESULT_IS_ASYNC(res)) {
				info->params[i].seq = res;
			}
		}
	}

	if (changed) {
		device->pending_sync = pw_proxy_sync(device->proxy, device->pending_sync);
	}
}

static void node_param(void *data, int seq, uint32_t id, uint32_t index, uint32_t next, const struct spa_pod *param)
{
	UNUSED_PARAMETER(index);
	UNUSED_PARAMETER(next);

	struct camera_device *device = data;
	add_param(&device->pending_list, seq, id, param);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_info,
	.param = node_param,
};

static void on_proxy_removed_cb(void *data)
{
	struct camera_device *device = data;
	pw_proxy_destroy(device->proxy);
}

static void on_destroy_proxy_cb(void *data)
{
	struct camera_device *device = data;
	spa_hook_remove(&device->proxy_listener);
	device->proxy = NULL;
}

static const char *device_name_of(struct camera_device *device)
{
	return device->properties ? pw_properties_get(device->properties, SPA_KEY_NODE_NAME) : NULL;
}

/* The device's formats and controls are known: lists and the "highest" frame
 * rate can be worked out now, and saved controls applied. */
static void on_done_proxy_cb(void *data, int seq)
{
	struct camera_device *device = data;
	if (device->info == NULL || device->pending_sync != seq) {
		return;
	}

	object_update_params(&device->param_list, &device->pending_list, device->info->n_params, device->info->params);
	refresh_sources(device_name_of(device), false);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = on_proxy_removed_cb,
	.destroy = on_destroy_proxy_cb,
	.done = on_done_proxy_cb,
};

static void on_registry_global_cb(void *user_data, uint32_t id, uint32_t permissions, const char *type,
				  uint32_t version, const struct spa_dict *props)
{
	UNUSED_PARAMETER(user_data);
	UNUSED_PARAMETER(permissions);

	if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
		return;
	}

	const char *device_id = spa_dict_lookup(props, SPA_KEY_NODE_NAME);
	if (!device_id) {
		return;
	}

	blog(LOG_INFO, "[video-device/pipewire] Found device %s", device_id);

	struct camera_device *device = camera_device_new(id, props);
	device->proxy = pw_registry_bind(obs_pipewire_get_registry(connection->obs_pw), id, type, version, 0);
	if (!device->proxy) {
		blog(LOG_WARNING, "[video-device/pipewire] Failed to bind device %s", device_id);
		camera_device_free(device);
		return;
	}
	pw_proxy_add_listener(device->proxy, &device->proxy_listener, &proxy_events, device);
	pw_node_add_listener((struct pw_node *)device->proxy, &device->node_listener, &node_events, device);

	g_hash_table_insert(connection->devices, bstrdup(device_id), device);
	refresh_sources(device_id, true);
}

static void on_registry_global_remove_cb(void *user_data, uint32_t id)
{
	UNUSED_PARAMETER(user_data);

	struct camera_device *device;
	const char *device_id;
	GHashTableIter iter;

	g_hash_table_iter_init(&iter, connection->devices);
	while (g_hash_table_iter_next(&iter, (gpointer *)&device_id, (gpointer *)&device)) {
		if (device->id != id) {
			continue;
		}
		blog(LOG_INFO, "[video-device/pipewire] Removed device %s", device_id);
		g_hash_table_iter_remove(&iter);
	}
	refresh_sources(NULL, false);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_registry_global_cb,
	.global_remove = on_registry_global_remove_cb,
};

/* ------------------------------------------------------------------------- */
/* Portal                                                                    */

/* Set once the session bus or the portal turned out to be unreachable, so
 * every properties refresh doesn't retry and log again. */
static bool camera_proxy_failed;

static GDBusProxy *get_camera_proxy(void)
{
	if (!camera_proxy && !camera_proxy_failed) {
		GDBusConnection *bus = portal_get_dbus_connection();
		g_autoptr(GError) error = NULL;
		if (!bus) {
			camera_proxy_failed = true;
			return NULL;
		}
		camera_proxy = g_dbus_proxy_new_sync(bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
						     "org.freedesktop.portal.Desktop",
						     "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Camera",
						     NULL, &error);
		if (error) {
			blog(LOG_WARNING, "[video-device/pipewire] Error retrieving D-Bus proxy: %s", error->message);
			camera_proxy_failed = true;
			return NULL;
		}
	}
	return camera_proxy;
}

static void on_pipewire_remote_opened_cb(GObject *source, GAsyncResult *res, void *user_data)
{
	UNUSED_PARAMETER(user_data);

	g_autoptr(GUnixFDList) fd_list = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;
	int fd_index;

	result = g_dbus_proxy_call_with_unix_fd_list_finish(G_DBUS_PROXY(source), &fd_list, res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			blog(LOG_ERROR, "[video-device/pipewire] Error retrieving PipeWire fd: %s", error->message);
		}
		return;
	}

	g_variant_get(result, "(h)", &fd_index);

	int pipewire_fd = g_unix_fd_list_get(fd_list, fd_index, &error);
	if (error) {
		blog(LOG_ERROR, "[video-device/pipewire] Error retrieving PipeWire fd: %s", error->message);
		return;
	}

	connection->obs_pw = obs_pipewire_connect_fd(pipewire_fd, &registry_events, connection);
	if (connection->obs_pw) {
		obs_pipewire_roundtrip(connection->obs_pw);
	}
}

static void on_access_camera_response_cb(GVariant *parameters, void *user_data)
{
	UNUSED_PARAMETER(user_data);

	g_autoptr(GVariant) result = NULL;
	uint32_t response;

	g_variant_get(parameters, "(u@a{sv})", &response, &result);
	if (response != 0) {
		blog(LOG_WARNING, "[video-device/pipewire] Camera access denied or cancelled");
		return;
	}

	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_dbus_proxy_call_with_unix_fd_list(get_camera_proxy(), "OpenPipeWireRemote",
					    g_variant_new("(a{sv})", &builder), G_DBUS_CALL_FLAGS_NONE, -1, NULL,
					    connection->cancellable, on_pipewire_remote_opened_cb, NULL);
}

static void on_access_camera_finished_cb(GObject *source, GAsyncResult *res, void *user_data)
{
	UNUSED_PARAMETER(user_data);

	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
	if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		blog(LOG_ERROR, "[video-device/pipewire] Error accessing camera: %s", error->message);
	}
}

/* Asks the portal for camera access once. Devices arrive through the
 * registry afterwards. */
static void ensure_connection(void)
{
	pthread_once(&pw_init_once, init_pipewire);

	if (!connection) {
		connection = bzalloc(sizeof(*connection));
		connection->devices =
			g_hash_table_new_full(g_str_hash, g_str_equal, bfree, (GDestroyNotify)camera_device_free);
		connection->cancellable = g_cancellable_new();
		da_init(connection->sources);
	}

	if (connection->requested || !get_camera_proxy()) {
		return;
	}
	connection->requested = true;

	char *request_token;
	char *request_path;
	portal_create_request_path(&request_path, &request_token);
	portal_signal_subscribe(request_path, NULL, on_access_camera_response_cb, NULL);

	GVariantBuilder builder;
	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
	g_dbus_proxy_call(get_camera_proxy(), "AccessCamera", g_variant_new("(a{sv})", &builder),
			  G_DBUS_CALL_FLAGS_NONE, -1, connection->cancellable, on_access_camera_finished_cb, NULL);

	bfree(request_token);
	bfree(request_path);
}

/* ------------------------------------------------------------------------- */
/* Backend interface                                                         */

static bool pipewire_available(void)
{
	GDBusProxy *proxy = get_camera_proxy();
	if (!proxy) {
		return false;
	}
	g_autoptr(GVariant) present = g_dbus_proxy_get_cached_property(proxy, "IsCameraPresent");
	return present && g_variant_get_boolean(present);
}

static void pipewire_enum_devices(vd_device_cb cb, void *param)
{
	ensure_connection();
	if (!connection->obs_pw) {
		return;
	}

	lock();
	GHashTableIter iter;
	const char *id;
	struct camera_device *device;
	g_hash_table_iter_init(&iter, connection->devices);
	while (g_hash_table_iter_next(&iter, (gpointer *)&id, (gpointer *)&device)) {
		const char *name = pw_properties_get(device->properties, PW_KEY_NODE_DESCRIPTION);
		cb(param, id, name ? name : id);
	}
	unlock();
}

static void *pipewire_create(obs_source_t *source, obs_data_t *settings)
{
	struct camera_source *cam = bzalloc(sizeof(*cam));
	cam->source = source;
	obs_data_addref(settings);
	cam->settings = settings;
	cam->device_id = bstrdup(obs_data_get_string(settings, VD_DEVICE_ID));
	read_settings(cam, settings);

	ensure_connection();

	lock();
	da_push_back(connection->sources, &cam);
	if (connection->obs_pw) {
		apply(cam, true);
	}
	unlock();
	return cam;
}

static void pipewire_destroy(void *data)
{
	struct camera_source *cam = data;

	lock();
	if (connection) {
		da_erase_item(connection->sources, &cam);
	}
	unlock();

	g_clear_pointer(&cam->stream, obs_pipewire_stream_destroy);
	obs_data_release(cam->settings);
	bfree(cam->device_id);
	bfree(cam->frame_rate);
	bfree(cam);
}

static void pipewire_update(void *data, obs_data_t *settings)
{
	struct camera_source *cam = data;
	const char *device_id = obs_data_get_string(settings, VD_DEVICE_ID);
	struct obs_pw_video_format old_format = cam->format;
	bool old_format_set = cam->format_set;

	lock();
	bool restart = strcmp(cam->device_id, device_id) != 0;
	if (restart) {
		bfree(cam->device_id);
		cam->device_id = bstrdup(device_id);
	}

	obs_data_release(cam->settings);
	obs_data_addref(settings);
	cam->settings = settings;
	read_settings(cam, settings);

	/* The format is fixed when the stream connects; size and rate can be
	 * renegotiated. */
	restart |= old_format_set != cam->format_set ||
		   (cam->format_set && old_format.spa_format != cam->format.spa_format);

	if (connection->obs_pw) {
		apply(cam, restart);
	}
	unlock();
}

static void pipewire_show(void *data)
{
	struct camera_source *cam = data;
	cam->hidden = false;
	if (cam->stream) {
		obs_pipewire_stream_show(cam->stream);
	}
}

static void pipewire_hide(void *data)
{
	struct camera_source *cam = data;
	cam->hidden = true;
	if (cam->stream) {
		obs_pipewire_stream_hide(cam->stream);
	}
}

static void pipewire_unload(void)
{
	if (connection) {
		g_cancellable_cancel(connection->cancellable);
		if (connection->obs_pw) {
			obs_pipewire_destroy(connection->obs_pw);
		}
		g_clear_pointer(&connection->devices, g_hash_table_destroy);
		g_clear_object(&connection->cancellable);
		da_free(connection->sources);
		g_clear_pointer(&connection, bfree);
	}
	g_clear_object(&camera_proxy);
	camera_proxy_failed = false;
	if (pw_initialized) {
#if PW_CHECK_VERSION(0, 3, 49)
		pw_deinit();
#endif
		pw_initialized = false;
	}
}

/* ------------------------------------------------------------------------- */
/* Properties                                                                */

struct list_ctx {
	obs_property_t *list;
	obs_data_t *settings;
	DARRAY(uint64_t) seen;
	struct obs_pw_video_format format;
	bool format_set;
	struct spa_rectangle size;
	bool size_set;
};

static bool seen_before(struct list_ctx *ctx, uint64_t key)
{
	for (size_t i = 0; i < ctx->seen.num; i++) {
		if (ctx->seen.array[i] == key) {
			return true;
		}
	}
	da_push_back(ctx->seen, &key);
	return false;
}

static void add_format_cb(void *param, const struct format_entry *entry)
{
	struct list_ctx *ctx = param;
	struct obs_pw_video_format fmt;
	char fourcc[5];

	if (seen_before(ctx, entry->spa_format) || !obs_pw_video_format_from_spa_format(entry->spa_format, &fmt)) {
		return;
	}
	fourcc_string(fmt.drm_format, fourcc);
	obs_property_list_add_string(ctx->list, fmt.pretty_name, fourcc);
}

static void add_size_cb(void *param, const struct format_entry *entry)
{
	struct list_ctx *ctx = param;
	if (ctx->format_set && entry->spa_format != ctx->format.spa_format) {
		return;
	}
	if (seen_before(ctx, ((uint64_t)entry->size.width << 32) | entry->size.height)) {
		return;
	}

	char value[32];
	snprintf(value, sizeof(value), "%ux%u", entry->size.width, entry->size.height);
	obs_property_list_add_string(ctx->list, value, value);
}

static void add_rate_cb(void *param, const struct format_entry *entry)
{
	struct list_ctx *ctx = param;
	if (ctx->format_set && entry->spa_format != ctx->format.spa_format) {
		return;
	}
	if (ctx->size_set && (entry->size.width != ctx->size.width || entry->size.height != ctx->size.height)) {
		return;
	}

	for (size_t i = 0; i < entry->rates.num; i++) {
		const struct spa_fraction *r = &entry->rates.array[i];
		if (seen_before(ctx, ((uint64_t)r->num << 32) | r->denom)) {
			continue;
		}

		char value[32], label[32];
		snprintf(value, sizeof(value), "%u/%u", r->num, r->denom);
		if (r->denom == 1) {
			snprintf(label, sizeof(label), "%u", r->num);
		} else {
			snprintf(label, sizeof(label), "%.2f", fraction_value(r));
		}
		obs_property_list_add_string(ctx->list, label, value);
	}
}

static void fill_list(obs_properties_t *props, const char *key, struct camera_device *dev, obs_data_t *settings,
		      format_entry_cb cb)
{
	obs_property_t *list = obs_properties_get(props, key);
	if (!list) {
		return;
	}

	obs_property_list_clear(list);
	if (strcmp(key, VD_FRAME_RATE) == 0) {
		obs_property_list_add_string(list, obs_module_text("frame-rate.highest"), "");
		obs_property_list_add_string(list, obs_module_text("frame-rate.match-output"), VD_FRAME_RATE_MATCH);
	} else {
		obs_property_list_add_string(list, obs_module_text("auto"), "");
	}

	if (!dev) {
		return;
	}

	unsigned int w, h;
	struct list_ctx ctx = {.list = list, .settings = settings};
	da_init(ctx.seen);
	ctx.format_set = format_from_fourcc(obs_data_get_string(settings, VD_PIXEL_FORMAT), &ctx.format);
	ctx.size_set = sscanf(obs_data_get_string(settings, VD_RESOLUTION), "%ux%u", &w, &h) == 2;
	ctx.size = SPA_RECTANGLE(ctx.size_set ? w : 0, ctx.size_set ? h : 0);
	for_each_format(dev, cb, &ctx);
	da_free(ctx.seen);
}

static bool control_changed(void *data, obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(prop);

	if (!connection || !connection->obs_pw) {
		return false;
	}

	lock();
	struct camera_device *dev = find_device(obs_data_get_string(settings, VD_DEVICE_ID));
	if (dev) {
		apply_controls(dev, settings);
	}
	unlock();
	return false;
}

static void add_control_property(obs_properties_t *props, obs_data_t *settings, const struct spa_pod *info)
{
	const struct spa_pod *type, *pod, *labels = NULL;
	uint32_t id, n_vals, choice;
	obs_property_t *prop = NULL;
	const char *name = NULL;

	if (spa_pod_parse_object(info, SPA_TYPE_OBJECT_PropInfo, NULL, SPA_PROP_INFO_id, SPA_POD_Id(&id),
				 SPA_PROP_INFO_description, SPA_POD_OPT_String(&name), SPA_PROP_INFO_type,
				 SPA_POD_PodChoice(&type), SPA_PROP_INFO_labels, SPA_POD_OPT_PodStruct(&labels)) < 0 ||
	    !name) {
		return;
	}

	struct dstr key = {0};
	vd_control_key(&key, BACKEND_NAME, name);
	pod = spa_pod_get_values(type, &n_vals, &choice);
	if (n_vals < 1) {
		goto out;
	}

	switch (SPA_POD_TYPE(pod)) {
	case SPA_TYPE_Int: {
		int32_t *vals = SPA_POD_BODY(pod);
		int32_t def = vals[0];
		if (choice == SPA_CHOICE_Enum) {
			struct spa_pod_parser prs;
			struct spa_pod_frame f;

			if (labels == NULL) {
				goto out;
			}

			prop = obs_properties_add_list(props, key.array, name, OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
			spa_pod_parser_pod(&prs, (struct spa_pod *)labels);
			if (spa_pod_parser_push_struct(&prs, &f) < 0) {
				goto out;
			}
			for (;;) {
				int32_t value;
				const char *desc;
				if (spa_pod_parser_get_int(&prs, &value) < 0 ||
				    spa_pod_parser_get_string(&prs, &desc) < 0) {
					break;
				}
				obs_property_list_add_int(prop, desc, value);
			}
		} else {
			int32_t min = n_vals > 1 ? vals[1] : def;
			int32_t max = n_vals > 2 ? vals[2] : def;
			int32_t step = n_vals > 3 ? vals[3] : SPA_MAX((max - min) / 256, 1);
			prop = obs_properties_add_int_slider(props, key.array, name, min, max, step);
		}
		obs_data_set_default_int(settings, key.array, def);
		break;
	}
	case SPA_TYPE_Bool: {
		int32_t *vals = SPA_POD_BODY(pod);
		prop = obs_properties_add_bool(props, key.array, name);
		obs_data_set_default_bool(settings, key.array, vals[0]);
		break;
	}
	case SPA_TYPE_Float: {
		float *vals = SPA_POD_BODY(pod);
		float def = vals[0];
		float min = n_vals > 1 ? vals[1] : def;
		float max = n_vals > 2 ? vals[2] : def;
		float step = n_vals > 3 ? vals[3] : (max - min) / 256.0f;
		prop = obs_properties_add_float_slider(props, key.array, name, min, max, step);
		obs_data_set_default_double(settings, key.array, def);
		break;
	}
	default:
		break;
	}

	if (prop) {
		obs_property_set_modified_callback2(prop, control_changed, NULL);
	}
out:
	dstr_free(&key);
}

static void fill_controls(obs_properties_t *props, struct camera_device *dev, obs_data_t *settings)
{
	obs_properties_remove_by_name(props, S_CONTROLS);
	if (!dev) {
		return;
	}

	obs_properties_t *controls = obs_properties_create();
	struct param *p;
	spa_list_for_each(p, &dev->param_list, link)
	{
		if (p->id == SPA_PARAM_PropInfo && p->param) {
			add_control_property(controls, settings, p->param);
		}
	}
	obs_properties_add_group(props, S_CONTROLS, obs_module_text("pipewire.camera-controls"), OBS_GROUP_NORMAL,
				 controls);
}

/* Refills the lists below the one that changed. */
static void refill(obs_properties_t *props, obs_data_t *settings, const char *changed)
{
	if (!connection || !connection->obs_pw) {
		fill_list(props, VD_PIXEL_FORMAT, NULL, settings, add_format_cb);
		fill_list(props, VD_RESOLUTION, NULL, settings, add_size_cb);
		fill_list(props, VD_FRAME_RATE, NULL, settings, add_rate_cb);
		return;
	}

	lock();
	struct camera_device *dev = find_device(obs_data_get_string(settings, VD_DEVICE_ID));
	bool device = strcmp(changed, VD_DEVICE_ID) == 0;
	if (device) {
		fill_list(props, VD_PIXEL_FORMAT, dev, settings, add_format_cb);
		fill_controls(props, dev, settings);
	}
	if (device || strcmp(changed, VD_PIXEL_FORMAT) == 0) {
		fill_list(props, VD_RESOLUTION, dev, settings, add_size_cb);
	}
	fill_list(props, VD_FRAME_RATE, dev, settings, add_rate_cb);
	unlock();
}

static bool device_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	refill(props, settings, VD_DEVICE_ID);
	return true;
}

static bool format_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	refill(props, settings, VD_PIXEL_FORMAT);
	return true;
}

static bool resolution_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	refill(props, settings, VD_RESOLUTION);
	return true;
}

static void pipewire_add_properties(void *data, obs_properties_t *props, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);

	obs_property_t *device = obs_properties_get(props, VD_DEVICE_ID);
	if (device) {
		obs_property_set_modified_callback(device, device_modified);
	}

	obs_property_t *p = obs_properties_add_list(props, VD_PIXEL_FORMAT, obs_module_text("pixel-format"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(p, format_modified);
	p = obs_properties_add_list(props, VD_RESOLUTION, obs_module_text("resolution"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_set_modified_callback(p, resolution_modified);
	obs_properties_add_list(props, VD_FRAME_RATE, obs_module_text("frame-rate"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);

	refill(props, settings, VD_DEVICE_ID);
}

const struct vd_backend vd_pipewire_backend = {
	.name = BACKEND_NAME,
	.display_name = "backend.pipewire",
	.available = pipewire_available,
	.enum_devices = pipewire_enum_devices,
	.create = pipewire_create,
	.destroy = pipewire_destroy,
	.update = pipewire_update,
	.add_properties = pipewire_add_properties,
	.show = pipewire_show,
	.hide = pipewire_hide,
	.unload = pipewire_unload,
	.has_audio = false,
};
