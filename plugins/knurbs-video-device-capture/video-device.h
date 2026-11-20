/*
 * Video Device Capture: one user-facing source for cameras and capture cards,
 * with the platform API (DirectShow, AVFoundation, PipeWire, V4L2) chosen as a
 * backend behind it.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */
#pragma once

#include <obs-module.h>
#include <util/dstr.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reverse-DNS, as all Knurbs source and filter IDs; capture sources share
 * com.xaymar.knurbs.capture. */
#define VIDEO_DEVICE_SOURCE_ID "com.xaymar.knurbs.capture.video-device"

/* ------------------------------------------------------------------------- */
/* Common settings, the same on every platform.                              */

/* Backend name, or "auto". */
#define VD_BACKEND "backend"
#define VD_BACKEND_AUTO "auto"

/* Device identity: backend that owns the ID, the backend's stable ID, and the
 * display name used as a fallback when the ID no longer matches (another
 * machine, another port, another platform). */
#define VD_DEVICE_BACKEND "device-backend"
#define VD_DEVICE_ID "device-id"
#define VD_DEVICE_NAME "device-name"

/* "WxH", or empty for the device's preferred resolution. */
#define VD_RESOLUTION "resolution"
/* Frame rate: "num/den" (for example "30000/1001"), empty for the highest the
 * device offers, or "match" for the device rate closest to the output. */
#define VD_FRAME_RATE "frame-rate"
#define VD_FRAME_RATE_MATCH "match"
/* FourCC such as "NV12" or "MJPG", or empty for automatic. */
#define VD_PIXEL_FORMAT "pixel-format"

#define VD_COLOR_SPACE "color-space" /* auto, 601, 709, 2100pq, 2100hlg */
#define VD_COLOR_RANGE "color-range" /* auto, partial, full */
#define VD_BUFFERING "buffering"     /* auto, on, off */
#define VD_FLIP_VERTICAL "flip-vertical"
#define VD_DEACTIVATE_WHEN_HIDDEN "deactivate-when-hidden"
#define VD_CAPTURE_AUDIO "capture-audio"

#define VD_AUTO "auto"

/* Names use only lowercase letters, digits, '-' and '.', never '_' or
 * anything else that needs Shift.
 *
 * Backend-specific settings are stored as "<backend>.<key>", for example
 * "v4l2.dv-timing" or "dshow.audio-output-mode", so they survive a trip
 * through another platform untouched. */

/* ------------------------------------------------------------------------- */
/* Backend interface                                                         */

typedef void (*vd_device_cb)(void *param, const char *id, const char *name);

struct vd_backend {
	/* Stable name, used in settings ("dshow", "avfoundation", "pipewire",
	 * "v4l2"). */
	const char *name;
	/* Locale key of the name shown to users. */
	const char *display_name;

	/* Whether the backend can run on this system right now. */
	bool (*available)(void);

	/* Lists the devices the backend can open. */
	void (*enum_devices)(vd_device_cb cb, void *param);

	/* Instance lifetime. The backend delivers frames with
	 * obs_source_output_video (and audio with obs_source_output_audio).
	 * A backend that keeps frames on the GPU converts them to RGBA8,
	 * RGB10A2 or RGBA16F on its own device before handing them over. */
	void *(*create)(obs_source_t *source, obs_data_t *settings);
	void (*destroy)(void *data);
	void (*update)(void *data, obs_data_t *settings);

	/* Optional. */
	void (*get_defaults)(obs_data_t *settings);
	/* Adds the device-dependent properties (resolution, frame rate, pixel
	 * format lists) and any "<backend>." properties. data may be NULL. */
	void (*add_properties)(void *data, obs_properties_t *props, obs_data_t *settings);
	void (*activate)(void *data);
	void (*deactivate)(void *data);
	/* Called on show and hide when "deactivate when not showing" is on. A
	 * backend without them is destroyed on hide and created again on
	 * show, which releases the device either way. */
	void (*show)(void *data);
	void (*hide)(void *data);
	/* Optional. Frees what the backend shares between instances, on
	 * module unload. */
	void (*unload)(void);

	/* Whether the backend can capture the device's own audio. */
	bool has_audio;
};

/* Registered backends for this platform, in Auto preference order. */
extern const struct vd_backend *const vd_backends[];
extern const size_t vd_num_backends;

const struct vd_backend *vd_find_backend(const char *name);

/* Helpers for backends. */

/* VD_COLOR_SPACE and VD_COLOR_RANGE values. "auto" and unknown values give
 * VIDEO_CS_DEFAULT and VIDEO_RANGE_DEFAULT; use vd_is_auto to tell them
 * apart from an explicit choice. */
enum video_colorspace vd_parse_color_space(const char *value);
enum video_range_type vd_parse_color_range(const char *value);
bool vd_is_auto(const char *value);

/* Fills the frame's matrix, range, full_range and transfer for a colour
 * space and range. */
void vd_frame_set_color(struct obs_source_frame *frame, enum video_colorspace space, enum video_range_type range);

/* The settings key of a device control: "<backend>.control.<name>", with
 * the name lowercased and every run of other characters turned into '-'
 * ("White Balance, Auto" becomes "v4l2.control.white-balance-auto"). */
void vd_control_key(struct dstr *out, const char *backend, const char *name);

extern struct obs_source_info video_device_capture_info;

#ifdef __cplusplus
}
#endif
