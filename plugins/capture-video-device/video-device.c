/*
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */
#include "video-device.h"

#include <util/dstr.h>

#include <string.h>

struct video_device {
	obs_source_t *source;
	const struct vd_backend *backend;
	void *backend_data;
};

const struct vd_backend *vd_find_backend(const char *name)
{
	if (!name) {
		return NULL;
	}
	for (size_t i = 0; i < vd_num_backends; i++) {
		if (strcmp(vd_backends[i]->name, name) == 0) {
			return vd_backends[i];
		}
	}
	return NULL;
}

static bool backend_available(const struct vd_backend *backend)
{
	return backend && (!backend->available || backend->available());
}

/* An explicit backend wins. On Auto, prefer the backend that owns the saved
 * device, then the first available backend. */
static const struct vd_backend *choose_backend(obs_data_t *settings)
{
	const char *wanted = obs_data_get_string(settings, VD_BACKEND);
	if (wanted && *wanted && strcmp(wanted, VD_BACKEND_AUTO) != 0) {
		const struct vd_backend *b = vd_find_backend(wanted);
		if (backend_available(b)) {
			return b;
		}
	}

	const struct vd_backend *owner = vd_find_backend(obs_data_get_string(settings, VD_DEVICE_BACKEND));
	if (backend_available(owner)) {
		return owner;
	}

	for (size_t i = 0; i < vd_num_backends; i++) {
		if (backend_available(vd_backends[i])) {
			return vd_backends[i];
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */

static const char *vd_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("VideoDeviceCapture");
}

static void attach_backend(struct video_device *vd, obs_data_t *settings)
{
	vd->backend = choose_backend(settings);
	vd->backend_data = NULL;

	if (!vd->backend) {
		blog(LOG_WARNING, "[video-device] '%s': no backend available", obs_source_get_name(vd->source));
		return;
	}

	blog(LOG_INFO, "[video-device] '%s': using backend '%s'", obs_source_get_name(vd->source), vd->backend->name);
	vd->backend_data = vd->backend->create(vd->source, settings);
}

static void detach_backend(struct video_device *vd)
{
	if (vd->backend && vd->backend_data) {
		vd->backend->destroy(vd->backend_data);
	}
	vd->backend = NULL;
	vd->backend_data = NULL;
}

static void *vd_create(obs_data_t *settings, obs_source_t *source)
{
	struct video_device *vd = bzalloc(sizeof(*vd));
	vd->source = source;
	attach_backend(vd, settings);
	return vd;
}

static void vd_destroy(void *data)
{
	struct video_device *vd = data;
	detach_backend(vd);
	bfree(vd);
}

static void vd_update(void *data, obs_data_t *settings)
{
	struct video_device *vd = data;

	/* Switching backend means a different API: start over. */
	if (choose_backend(settings) != vd->backend) {
		detach_backend(vd);
		attach_backend(vd, settings);
		return;
	}

	if (vd->backend && vd->backend_data && vd->backend->update) {
		vd->backend->update(vd->backend_data, settings);
	}
}

static void vd_activate(void *data)
{
	struct video_device *vd = data;
	if (vd->backend && vd->backend_data && vd->backend->activate) {
		vd->backend->activate(vd->backend_data);
	}
}

static void vd_deactivate(void *data)
{
	struct video_device *vd = data;
	if (vd->backend && vd->backend_data && vd->backend->deactivate) {
		vd->backend->deactivate(vd->backend_data);
	}
}

static void vd_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, VD_BACKEND, VD_BACKEND_AUTO);
	obs_data_set_default_string(settings, VD_RESOLUTION, "");
	obs_data_set_default_string(settings, VD_FRAME_RATE, "");
	obs_data_set_default_string(settings, VD_PIXEL_FORMAT, "");
	obs_data_set_default_string(settings, VD_COLOR_SPACE, VD_AUTO);
	obs_data_set_default_string(settings, VD_COLOR_RANGE, VD_AUTO);
	obs_data_set_default_string(settings, VD_BUFFERING, VD_AUTO);
	obs_data_set_default_bool(settings, VD_FLIP_VERTICAL, false);
	obs_data_set_default_bool(settings, VD_DEACTIVATE_WHEN_HIDDEN, false);
	obs_data_set_default_bool(settings, VD_CAPTURE_AUDIO, true);

	for (size_t i = 0; i < vd_num_backends; i++) {
		if (vd_backends[i]->get_defaults) {
			vd_backends[i]->get_defaults(settings);
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Properties                                                                */

struct device_list_ctx {
	obs_property_t *list;
	const char *selected_id;
	bool found;
};

static void add_device_cb(void *param, const char *id, const char *name)
{
	struct device_list_ctx *ctx = param;
	obs_property_list_add_string(ctx->list, name && *name ? name : id, id);
	if (ctx->selected_id && strcmp(ctx->selected_id, id) == 0) {
		ctx->found = true;
	}
}

static void fill_devices(obs_property_t *list, const struct vd_backend *backend, obs_data_t *settings)
{
	obs_property_list_clear(list);
	if (!backend || !backend->enum_devices) {
		return;
	}

	struct device_list_ctx ctx = {list, obs_data_get_string(settings, VD_DEVICE_ID), false};
	backend->enum_devices(add_device_cb, &ctx);

	/* Keep a saved device that isn't connected right now visible, by name,
	 * instead of silently selecting another one. */
	if (!ctx.found && ctx.selected_id && *ctx.selected_id) {
		const char *name = obs_data_get_string(settings, VD_DEVICE_NAME);
		struct dstr label = {0};
		dstr_printf(&label, "%s (%s)", name && *name ? name : ctx.selected_id,
			    obs_module_text("DeviceNotFound"));
		size_t idx = obs_property_list_add_string(list, label.array, ctx.selected_id);
		obs_property_list_item_disable(list, idx, true);
		dstr_free(&label);
	}
}

static void add_string_list(obs_properties_t *props, const char *key, const char *label, const char *const *values,
			    const char *const *labels, size_t count)
{
	obs_property_t *p = obs_properties_add_list(props, key, label, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (size_t i = 0; i < count; i++) {
		obs_property_list_add_string(p, obs_module_text(labels[i]), values[i]);
	}
}

static void add_common_properties(obs_properties_t *props, const struct vd_backend *backend)
{
	static const char *const cs_values[] = {VD_AUTO, "601", "709", "2100pq", "2100hlg"};
	static const char *const cs_labels[] = {"Auto", "ColorSpace.601", "ColorSpace.709", "ColorSpace.2100PQ",
						"ColorSpace.2100HLG"};
	add_string_list(props, VD_COLOR_SPACE, obs_module_text("ColorSpace"), cs_values, cs_labels, 5);

	static const char *const cr_values[] = {VD_AUTO, "partial", "full"};
	static const char *const cr_labels[] = {"Auto", "ColorRange.Partial", "ColorRange.Full"};
	add_string_list(props, VD_COLOR_RANGE, obs_module_text("ColorRange"), cr_values, cr_labels, 3);

	static const char *const buf_values[] = {VD_AUTO, "on", "off"};
	static const char *const buf_labels[] = {"Auto", "Buffering.On", "Buffering.Off"};
	add_string_list(props, VD_BUFFERING, obs_module_text("Buffering"), buf_values, buf_labels, 3);

	obs_properties_add_bool(props, VD_FLIP_VERTICAL, obs_module_text("FlipVertical"));
	obs_properties_add_bool(props, VD_DEACTIVATE_WHEN_HIDDEN, obs_module_text("DeactivateWhenHidden"));

	if (backend && backend->has_audio) {
		obs_properties_add_bool(props, VD_CAPTURE_AUDIO, obs_module_text("CaptureAudio"));
	}
}

static bool backend_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings);

static void build_backend_list(obs_properties_t *props)
{
	obs_property_t *p = obs_properties_add_list(props, VD_BACKEND, obs_module_text("Backend"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Auto"), VD_BACKEND_AUTO);
	for (size_t i = 0; i < vd_num_backends; i++) {
		size_t idx = obs_property_list_add_string(p, obs_module_text(vd_backends[i]->display_name),
							  vd_backends[i]->name);
		if (!backend_available(vd_backends[i])) {
			obs_property_list_item_disable(p, idx, true);
		}
	}
	obs_property_set_modified_callback(p, backend_changed);
}

/* Everything below the backend list. */
static void build_backend_properties(obs_properties_t *props, struct video_device *vd, obs_data_t *settings)
{
	const struct vd_backend *backend = choose_backend(settings);

	obs_property_t *p = obs_properties_add_list(props, VD_DEVICE_ID, obs_module_text("Device"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	fill_devices(p, backend, settings);

	/* Device-dependent lists and "<backend>." options. Only pass the
	 * instance when it belongs to the backend being shown. */
	if (backend && backend->add_properties) {
		void *data = (vd && vd->backend == backend) ? vd->backend_data : NULL;
		backend->add_properties(data, props, settings);
	}

	add_common_properties(props, backend);
}

/* The property list depends on the backend: rebuild it in place. */
static bool backend_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);

	struct video_device *vd = obs_properties_get_param(props);
	for (;;) {
		obs_property_t *cur = obs_properties_first(props);
		while (cur && strcmp(obs_property_name(cur), VD_BACKEND) == 0) {
			obs_property_next(&cur);
		}
		if (!cur) {
			break;
		}
		obs_properties_remove_by_name(props, obs_property_name(cur));
	}

	build_backend_properties(props, vd, settings);
	return true;
}

static obs_properties_t *vd_get_properties(void *data)
{
	struct video_device *vd = data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, vd, NULL);

	obs_data_t *settings = vd ? obs_source_get_settings(vd->source) : obs_data_create();
	build_backend_list(props);
	build_backend_properties(props, vd, settings);
	obs_data_release(settings);
	return props;
}

struct obs_source_info video_device_capture_info = {
	.id = VIDEO_DEVICE_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.icon_type = OBS_ICON_TYPE_CAMERA,
	.get_name = vd_get_name,
	.create = vd_create,
	.destroy = vd_destroy,
	.update = vd_update,
	.activate = vd_activate,
	.deactivate = vd_deactivate,
	.get_defaults = vd_get_defaults,
	.get_properties = vd_get_properties,
};
