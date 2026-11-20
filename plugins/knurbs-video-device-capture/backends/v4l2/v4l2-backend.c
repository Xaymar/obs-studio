/*
Copyright (C) 2014 by Leonhard Oelke <leonhard@in-verted.de>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/select.h>

#include <linux/videodev2.h>
#include <libv4l2.h>

#include <util/threading.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <obs-module.h>

#include "video-device.h"

#include "v4l2-controls.h"
#include "v4l2-helpers.h"
#include "v4l2-decoder.h"

#define FALLBACK_FRAMERATE 30

#if HAVE_UDEV
#include "v4l2-udev.h"
#endif

/* The new dv timing api was introduced in Linux 3.4
 * Currently we simply disable dv timings when this is not defined */
#if !defined(VIDIOC_ENUM_DV_TIMINGS) || !defined(V4L2_IN_CAP_DV_TIMINGS)
#define V4L2_IN_CAP_DV_TIMINGS 0
#endif

#define V4L2_DATA(voidptr) struct v4l2_data *data = voidptr;

#define timeval2ns(tv) (((uint64_t)tv.tv_sec * 1000000000) + ((uint64_t)tv.tv_usec * 1000))

#define V4L2_FOURCC_STR(code)                                                                \
	(char[5])                                                                            \
	{                                                                                    \
		code & 0xFF, (code >> 8) & 0xFF, (code >> 16) & 0xFF, (code >> 24) & 0xFF, 0 \
	}

#define blog(level, msg, ...) blog(level, "video-device: v4l2: " msg, ##__VA_ARGS__)

/**
 * Data structure for the v4l2 source
 */
struct v4l2_data {
	/* settings */
	char *device_id;
	int input;
	int pixfmt;
	int standard;
	int dv_timing;
	int64_t resolution;
	int64_t framerate;
	int color_range;
	int color_space;
	bool flip;

	/* internal data */
	obs_source_t *source;
	pthread_t thread;
	os_event_t *event;
	struct v4l2_decoder decoder;

	bool framerate_unchanged;
	bool resolution_unchanged;
	int_fast32_t dev;
	int width;
	int height;
	int linesize;
	struct v4l2_buffer_data buffers;

	bool auto_reset;
	int timeout_frames;
};

/* forward declarations */
static void v4l2_init(struct v4l2_data *data);
static void v4l2_terminate(struct v4l2_data *data);
static void v4l2_backend_update(void *vptr, obs_data_t *settings);

/**
 * Prepare the output frame structure for obs and compute plane offsets
 * For encoded formats (mjpeg) this clears the frame and plane offsets,
 * which will be filled in after decoding.
 *
 * Basically all data apart from memory pointers and the timestamp is known
 * before the capture starts. This function prepares the obs_source_frame
 * struct with all the data that is already known.
 *
 * v4l2 uses a continuous memory segment for all planes so we simply compute
 * offsets to add to the start address in order to give obs the correct data
 * pointers for the individual planes.
 *
 */
static void v4l2_prep_obs_frame(struct v4l2_data *data, struct obs_source_frame *frame, size_t *plane_offsets)
{
	memset(frame, 0, sizeof(struct obs_source_frame));
	memset(plane_offsets, 0, sizeof(size_t) * MAX_AV_PLANES);

	const enum video_format format = v4l2_to_obs_video_format(data->pixfmt);

	frame->flags = OBS_SOURCE_FRAME_LINEAR_ALPHA;
	frame->width = data->width;
	frame->height = data->height;
	frame->format = format;
	frame->flip = data->flip;
	vd_frame_set_color(frame, data->color_space, data->color_range);

	switch (data->pixfmt) {
	case V4L2_PIX_FMT_NV12:
		frame->linesize[0] = data->linesize;
		frame->linesize[1] = data->linesize;
		plane_offsets[1] = data->linesize * data->height;
		break;
	case V4L2_PIX_FMT_YVU420:
		frame->linesize[0] = data->linesize;
		frame->linesize[1] = data->linesize / 2;
		frame->linesize[2] = data->linesize / 2;
		plane_offsets[1] = data->linesize * data->height * 5 / 4;
		plane_offsets[2] = data->linesize * data->height;
		break;
	case V4L2_PIX_FMT_YUV420:
		frame->linesize[0] = data->linesize;
		frame->linesize[1] = data->linesize / 2;
		frame->linesize[2] = data->linesize / 2;
		plane_offsets[1] = data->linesize * data->height;
		plane_offsets[2] = data->linesize * data->height * 5 / 4;
		break;
	default:
		frame->linesize[0] = data->linesize;
		break;
	}
}

/*
 * Worker thread to get video data
 */
static void *v4l2_thread(void *vptr)
{
	V4L2_DATA(vptr);
	int r;
	fd_set fds;
	uint8_t *start;
	uint64_t frames;
	uint64_t first_ts;
	struct timeval tv;
	struct v4l2_buffer buf;
	struct obs_source_frame out;
	size_t plane_offsets[MAX_AV_PLANES];
	int fps_num, fps_denom;
	float ffps;
	uint64_t timeout_usec;

	blog(LOG_DEBUG, "%s: new capture thread", data->device_id);
	os_set_thread_name("v4l2: capture");

	/* Get framerate and calculate appropriate select timeout value. */
	v4l2_unpack_tuple(&fps_num, &fps_denom, data->framerate);
	ffps = (float)fps_denom / fps_num;
	blog(LOG_DEBUG, "%s: framerate: %.2f fps", data->device_id, ffps);
	/* Timeout set to 5 frame periods. */
	timeout_usec = (1000000 * data->timeout_frames) / ffps;
	blog(LOG_INFO, "%s: select timeout set to %" PRIu64 " (%dx frame periods)", data->device_id, timeout_usec,
	     data->timeout_frames);

	if (v4l2_start_capture(data->dev, &data->buffers) < 0) {
		goto exit;
	}

	blog(LOG_DEBUG, "%s: new capture started", data->device_id);

	frames = 0;
	first_ts = 0;
	v4l2_prep_obs_frame(data, &out, plane_offsets);

	blog(LOG_DEBUG, "%s: obs frame prepared", data->device_id);

	while (os_event_try(data->event) == EAGAIN) {
		FD_ZERO(&fds);
		FD_SET(data->dev, &fds);

		/* Set timeout timevalue. */
		tv.tv_sec = 0;
		tv.tv_usec = timeout_usec;

		r = select(data->dev + 1, &fds, NULL, NULL, &tv);
		if (r < 0) {
			if (errno == EINTR) {
				continue;
			}
			blog(LOG_ERROR, "%s: select failed", data->device_id);
			break;
		} else if (r == 0) {
			blog(LOG_ERROR, "%s: select timed out", data->device_id);

#ifdef _DEBUG
			v4l2_query_all_buffers(data->dev, &data->buffers);
#endif

			if (v4l2_ioctl(data->dev, VIDIOC_LOG_STATUS) < 0) {
				blog(LOG_ERROR, "%s: failed to log status", data->device_id);
			}

			if (data->auto_reset) {
				if (v4l2_reset_capture(data->dev, &data->buffers) == 0) {
					blog(LOG_INFO, "%s: stream reset successful", data->device_id);
				} else {
					blog(LOG_ERROR, "%s: failed to reset", data->device_id);
				}
			}

			continue;
		}

		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		if (v4l2_ioctl(data->dev, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN) {
				blog(LOG_DEBUG, "%s: ioctl dqbuf eagain", data->device_id);
				continue;
			}
			blog(LOG_ERROR, "%s: failed to dequeue buffer", data->device_id);
			break;
		}

		blog(LOG_DEBUG, "%s: ts: %06ld buf id #%d, flags 0x%08X, seq #%d, len %d, used %d", data->device_id,
		     buf.timestamp.tv_usec, buf.index, buf.flags, buf.sequence, buf.length, buf.bytesused);

		if (buf.flags & V4L2_BUF_FLAG_ERROR) {
			blog(LOG_DEBUG, "skipping decoding of buffer with recoverable error-flag set");
			goto continue_queue_buffer;
		}

		out.timestamp = timeval2ns(buf.timestamp);
		if (!frames) {
			first_ts = out.timestamp;
		}
		out.timestamp -= first_ts;

		start = (uint8_t *)data->buffers.info[buf.index].start;

		if (data->pixfmt == V4L2_PIX_FMT_MJPEG || data->pixfmt == V4L2_PIX_FMT_H264) {
			if (v4l2_decode_frame(&out, start, buf.bytesused, &data->decoder) < 0) {
				blog(LOG_ERROR, "failed to unpack jpeg or h264");
				break;
			}
		} else {
			for (uint_fast32_t i = 0; i < MAX_AV_PLANES; ++i) {
				out.data[i] = start + plane_offsets[i];
			}
		}
		obs_source_output_video(data->source, &out);

	continue_queue_buffer:
		if (v4l2_ioctl(data->dev, VIDIOC_QBUF, &buf) < 0) {
			blog(LOG_ERROR, "%s: failed to enqueue buffer", data->device_id);
			break;
		}

		frames++;
	}

	blog(LOG_INFO, "%s: Stopped capture after %" PRIu64 " frames", data->device_id, frames);

exit:
	v4l2_stop_capture(data->dev);
	return NULL;
}

/*
 * List inputs for device
 */
static void v4l2_input_list(int_fast32_t dev, obs_property_t *prop)
{
	struct v4l2_input in;
	memset(&in, 0, sizeof(in));

	obs_property_list_clear(prop);

	while (v4l2_ioctl(dev, VIDIOC_ENUMINPUT, &in) == 0) {
		obs_property_list_add_int(prop, (char *)in.name, in.index);
		blog(LOG_INFO, "Found input '%s' (Index %d)", in.name, in.index);
		in.index++;
	}
}

/*
 * List formats for device
 */
static void v4l2_format_list(int dev, obs_property_t *prop)
{
	struct v4l2_fmtdesc fmt;
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.index = 0;
	struct dstr buffer;
	dstr_init(&buffer);

	obs_property_list_clear(prop);
	obs_property_list_add_string(prop, obs_module_text("auto"), "");

	while (v4l2_ioctl(dev, VIDIOC_ENUM_FMT, &fmt) == 0) {
		dstr_copy(&buffer, (char *)fmt.description);
		if (fmt.flags & V4L2_FMT_FLAG_EMULATED) {
			dstr_cat(&buffer, " (Emulated)");
		}

		if (v4l2_to_obs_video_format(fmt.pixelformat) != VIDEO_FORMAT_NONE ||
		    fmt.pixelformat == V4L2_PIX_FMT_MJPEG || fmt.pixelformat == V4L2_PIX_FMT_H264) {
			obs_property_list_add_string(prop, buffer.array, V4L2_FOURCC_STR(fmt.pixelformat));
			blog(LOG_INFO, "Pixelformat: %s (available)", buffer.array);
		} else {
			blog(LOG_INFO, "Pixelformat: %s (unavailable)", buffer.array);
		}
		fmt.index++;
	}

	dstr_free(&buffer);
}

/*
 * List video standards for the device
 */
static void v4l2_standard_list(int dev, obs_property_t *prop)
{
	struct v4l2_standard std;
	std.index = 0;

	obs_property_list_clear(prop);

	obs_property_list_add_int(prop, obs_module_text("v4l2.leave-unchanged"), -1);

	while (v4l2_ioctl(dev, VIDIOC_ENUMSTD, &std) == 0) {
		obs_property_list_add_int(prop, (char *)std.name, std.id);
		std.index++;
	}
}

/*
 * List dv timings for the device
 */
static void v4l2_dv_timing_list(int dev, obs_property_t *prop)
{
	struct v4l2_dv_timings dvt;
	struct dstr buf;
	int index = 0;
	dstr_init(&buf);

	obs_property_list_clear(prop);

	obs_property_list_add_int(prop, obs_module_text("v4l2.leave-unchanged"), -1);

	while (v4l2_enum_dv_timing(dev, &dvt, index) == 0) {
		/* i do not pretend to understand, this is from qv4l2 ... */
		double h = (double)dvt.bt.height + dvt.bt.vfrontporch + dvt.bt.vsync + dvt.bt.vbackporch +
			   dvt.bt.il_vfrontporch + dvt.bt.il_vsync + dvt.bt.il_vbackporch;
		double w = (double)dvt.bt.width + dvt.bt.hfrontporch + dvt.bt.hsync + dvt.bt.hbackporch;
		double i = (dvt.bt.interlaced) ? 2.0f : 1.0f;
		double rate = (double)dvt.bt.pixelclock / (w * (h / i));

		dstr_printf(&buf, "%ux%u%c %.2f", dvt.bt.width, dvt.bt.height, (dvt.bt.interlaced) ? 'i' : 'p', rate);

		obs_property_list_add_int(prop, buf.array, index);

		index++;
	}

	dstr_free(&buf);
}

/*
 * List resolutions for device and format
 */
static void v4l2_resolution_list(int dev, uint_fast32_t pixelformat, obs_property_t *prop)
{
	struct v4l2_frmsizeenum frmsize;
	frmsize.pixel_format = pixelformat;
	frmsize.index = 0;
	struct dstr buffer;
	dstr_init(&buffer);

	obs_property_list_clear(prop);

	obs_property_list_add_string(prop, obs_module_text("v4l2.leave-unchanged"), "");

	v4l2_ioctl(dev, VIDIOC_ENUM_FRAMESIZES, &frmsize);

	switch (frmsize.type) {
	case V4L2_FRMSIZE_TYPE_DISCRETE:
		while (v4l2_ioctl(dev, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
			dstr_printf(&buffer, "%dx%d", frmsize.discrete.width, frmsize.discrete.height);
			obs_property_list_add_string(prop, buffer.array, buffer.array);
			frmsize.index++;
		}
		break;
	default:
		blog(LOG_INFO, "Stepwise and Continuous framesizes "
			       "are currently hardcoded");

		for (const int64_t *packed = v4l2_framesizes; *packed; ++packed) {
			int width;
			int height;
			v4l2_unpack_tuple(&width, &height, *packed);
			dstr_printf(&buffer, "%dx%d", width, height);
			obs_property_list_add_string(prop, buffer.array, buffer.array);
		}
		break;
	}

	dstr_free(&buffer);
}

/*
 * List framerates for device and resolution
 */
static void v4l2_framerate_list(int dev, uint_fast32_t pixelformat, uint_fast32_t width, uint_fast32_t height,
				obs_property_t *prop)
{
	struct v4l2_frmivalenum frmival;
	frmival.pixel_format = pixelformat;
	frmival.width = width;
	frmival.height = height;
	frmival.index = 0;
	struct dstr buffer;
	dstr_init(&buffer);

	struct dstr value;
	dstr_init(&value);

	obs_property_list_clear(prop);

	obs_property_list_add_string(prop, obs_module_text("v4l2.leave-unchanged"), "");

	v4l2_ioctl(dev, VIDIOC_ENUM_FRAMEINTERVALS, &frmival);

	switch (frmival.type) {
	case V4L2_FRMIVAL_TYPE_DISCRETE:
		while (v4l2_ioctl(dev, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
			float fps = (float)frmival.discrete.denominator / frmival.discrete.numerator;
			dstr_printf(&buffer, "%.2f", fps);
			dstr_printf(&value, "%u/%u", frmival.discrete.denominator, frmival.discrete.numerator);
			obs_property_list_add_string(prop, buffer.array, value.array);
			frmival.index++;
		}
		break;
	default:
		blog(LOG_INFO, "Stepwise and Continuous framerates "
			       "are currently hardcoded");

		for (const int64_t *packed = v4l2_framerates; *packed; ++packed) {
			int num;
			int denom;
			v4l2_unpack_tuple(&num, &denom, *packed);
			float fps = (float)denom / num;
			dstr_printf(&buffer, "%.2f", fps);
			dstr_printf(&value, "%d/%d", denom, num);
			obs_property_list_add_string(prop, buffer.array, value.array);
		}
		break;
	}

	dstr_free(&value);
	dstr_free(&buffer);
}

/*
 * List available devices for the core's device list
 */
static void v4l2_enum_devices(vd_device_cb cb, void *param)
{
	DIR *dirp;
	struct dirent *dp;
	struct dstr device;

#ifdef __FreeBSD__
	dirp = opendir("/dev");
#else
	dirp = opendir("/sys/class/video4linux");
#endif
	if (!dirp) {
		return;
	}

	dstr_init_copy(&device, "/dev/");

	while ((dp = readdir(dirp)) != NULL) {
		int fd;
		uint32_t caps;
		struct v4l2_capability video_cap;

#ifdef __FreeBSD__
		if (strstr(dp->d_name, "video") == NULL) {
			continue;
		}
#endif

		if (dp->d_type == DT_DIR) {
			continue;
		}

		dstr_resize(&device, 5);
		dstr_cat(&device, dp->d_name);

		if ((fd = v4l2_open(device.array, O_RDWR | O_NONBLOCK)) == -1) {
			blog(LOG_INFO, "Unable to open %s", device.array);
			continue;
		}

		if (v4l2_ioctl(fd, VIDIOC_QUERYCAP, &video_cap) == -1) {
			blog(LOG_INFO, "Failed to query capabilities for %s", device.array);
			v4l2_close(fd);
			continue;
		}

#ifndef V4L2_CAP_DEVICE_CAPS
		caps = video_cap.capabilities;
#else
		/* ... since Linux 3.3 */
		caps = (video_cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? video_cap.device_caps : video_cap.capabilities;
#endif

		if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
			blog(LOG_INFO, "%s seems to not support video capture", device.array);
			v4l2_close(fd);
			continue;
		}

		/* make sure device names are unique */
		char unique_device_name[68];
		snprintf(unique_device_name, sizeof(unique_device_name), "%s (%s)", video_cap.card, video_cap.bus_info);

		cb(param, device.array, unique_device_name);
		v4l2_close(fd);
	}

	closedir(dirp);
	dstr_free(&device);
}

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */

#define S_INPUT "v4l2.input"
#define S_STANDARD "v4l2.standard"
#define S_DV_TIMING "v4l2.dv-timing"
#define S_AUTO_RESET "v4l2.auto-reset"
#define S_TIMEOUT_FRAMES "v4l2.timeout-frames"

/* The common settings, as V4L2 values. */
struct v4l2_settings {
	const char *device_id;
	int input;
	int pixfmt;
	int standard;
	int dv_timing;
	int64_t resolution;
	int64_t framerate;
	int color_range;
	int color_space;
	bool flip;
	bool buffering;
	bool auto_reset;
	int timeout_frames;
};

static int parse_pixfmt(const char *fourcc)
{
	if (!fourcc || strlen(fourcc) != 4) {
		return -1;
	}
	return (int)v4l2_fourcc(fourcc[0], fourcc[1], fourcc[2], fourcc[3]);
}

static int64_t parse_resolution(const char *res)
{
	int w, h;
	if (!res || sscanf(res, "%dx%d", &w, &h) != 2 || w <= 0 || h <= 0) {
		return -1;
	}
	return v4l2_pack_tuple(w, h);
}

/* "fps_num/fps_den" to the V4L2 time per frame (fps_den/fps_num). */
static int64_t parse_framerate(const char *rate)
{
	int num, den;
	if (!rate || sscanf(rate, "%d/%d", &num, &den) != 2 || num <= 0 || den <= 0) {
		return -1;
	}
	return v4l2_pack_tuple(den, num);
}

static void read_settings(obs_data_t *settings, struct v4l2_settings *s)
{
	s->device_id = obs_data_get_string(settings, VD_DEVICE_ID);
	s->input = (int)obs_data_get_int(settings, S_INPUT);
	s->pixfmt = parse_pixfmt(obs_data_get_string(settings, VD_PIXEL_FORMAT));
	s->standard = (int)obs_data_get_int(settings, S_STANDARD);
	s->dv_timing = (int)obs_data_get_int(settings, S_DV_TIMING);
	s->resolution = parse_resolution(obs_data_get_string(settings, VD_RESOLUTION));
	const char *rate = obs_data_get_string(settings, VD_FRAME_RATE);
	if (strcmp(rate, VD_FRAME_RATE_MATCH) == 0) {
		/* The driver picks its interval closest to the output's. */
		struct obs_video_info ovi;
		s->framerate = obs_get_video_info(&ovi) ? v4l2_pack_tuple(ovi.fps_den, ovi.fps_num) : -1;
	} else {
		s->framerate = parse_framerate(rate);
	}
	s->color_range = vd_parse_color_range(obs_data_get_string(settings, VD_COLOR_RANGE));
	s->color_space = vd_parse_color_space(obs_data_get_string(settings, VD_COLOR_SPACE));
	s->flip = obs_data_get_bool(settings, VD_FLIP_VERTICAL);
	/* V4L2 buffered by default: Auto means on. */
	s->buffering = strcmp(obs_data_get_string(settings, VD_BUFFERING), "off") != 0;
	s->auto_reset = obs_data_get_bool(settings, S_AUTO_RESET);
	s->timeout_frames = (int)obs_data_get_int(settings, S_TIMEOUT_FRAMES);
}

static void v4l2_backend_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, S_INPUT, -1);
	obs_data_set_default_int(settings, S_STANDARD, -1);
	obs_data_set_default_int(settings, S_DV_TIMING, -1);
	obs_data_set_default_bool(settings, S_AUTO_RESET, false);
	obs_data_set_default_int(settings, S_TIMEOUT_FRAMES, 5);
}

/* ------------------------------------------------------------------------- */
/* Properties                                                                */

/*
 * Device selected callback
 */
static bool device_selected(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	int dev = v4l2_open(obs_data_get_string(settings, VD_DEVICE_ID), O_RDWR | O_NONBLOCK);
	if (dev == -1) {
		return false;
	}

	obs_property_t *prop = obs_properties_get(props, S_INPUT);
	obs_properties_t *ctrl_props_new = obs_properties_create();

	obs_properties_remove_by_name(props, "controls");

	v4l2_input_list(dev, prop);
	v4l2_update_controls(dev, ctrl_props_new, settings);
	v4l2_close(dev);

	obs_properties_add_group(props, "controls", obs_module_text("v4l2.camera-controls"), OBS_GROUP_NORMAL,
				 ctrl_props_new);

	obs_property_modified(prop, settings);

	return true;
}

/*
 * Input selected callback
 */
static bool input_selected(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	int dev = v4l2_open(obs_data_get_string(settings, VD_DEVICE_ID), O_RDWR | O_NONBLOCK);
	if (dev == -1) {
		return false;
	}

	obs_property_t *prop = obs_properties_get(props, VD_PIXEL_FORMAT);
	v4l2_format_list(dev, prop);
	v4l2_close(dev);

	obs_property_modified(prop, settings);

	return true;
}

/*
 * Format selected callback
 */
static bool format_selected(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	int dev = v4l2_open(obs_data_get_string(settings, VD_DEVICE_ID), O_RDWR | O_NONBLOCK);
	if (dev == -1) {
		return false;
	}

	int input = (int)obs_data_get_int(settings, S_INPUT);
	uint32_t caps = 0;
	if (v4l2_get_input_caps(dev, input, &caps) < 0) {
		v4l2_close(dev);
		return false;
	}
	caps &= V4L2_IN_CAP_STD | V4L2_IN_CAP_DV_TIMINGS;

	obs_property_t *resolution = obs_properties_get(props, VD_RESOLUTION);
	obs_property_t *framerate = obs_properties_get(props, VD_FRAME_RATE);
	obs_property_t *standard = obs_properties_get(props, S_STANDARD);
	obs_property_t *dv_timing = obs_properties_get(props, S_DV_TIMING);

	obs_property_set_visible(resolution, (!caps) ? true : false);
	obs_property_set_visible(framerate, (!caps) ? true : false);
	obs_property_set_visible(standard, (caps & V4L2_IN_CAP_STD) ? true : false);
	obs_property_set_visible(dv_timing, (caps & V4L2_IN_CAP_DV_TIMINGS) ? true : false);

	uint32_t pixfmt = (uint32_t)parse_pixfmt(obs_data_get_string(settings, VD_PIXEL_FORMAT));
	if (!caps) {
		v4l2_resolution_list(dev, pixfmt, resolution);
	}
	if (caps & V4L2_IN_CAP_STD) {
		v4l2_standard_list(dev, standard);
	}
	if (caps & V4L2_IN_CAP_DV_TIMINGS) {
		v4l2_dv_timing_list(dev, dv_timing);
	}

	v4l2_close(dev);

	if (!caps) {
		obs_property_modified(resolution, settings);
	}
	if (caps & V4L2_IN_CAP_STD) {
		obs_property_modified(standard, settings);
	}
	if (caps & V4L2_IN_CAP_DV_TIMINGS) {
		obs_property_modified(dv_timing, settings);
	}

	return true;
}

/*
 * Resolution selected callback
 */
static bool resolution_selected(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	int width = 0, height = 0;
	int dev = v4l2_open(obs_data_get_string(settings, VD_DEVICE_ID), O_RDWR | O_NONBLOCK);
	if (dev == -1) {
		return false;
	}

	obs_property_t *prop = obs_properties_get(props, VD_FRAME_RATE);
	int64_t packed = parse_resolution(obs_data_get_string(settings, VD_RESOLUTION));
	if (packed != -1) {
		v4l2_unpack_tuple(&width, &height, packed);
	}
	v4l2_framerate_list(dev, (uint32_t)parse_pixfmt(obs_data_get_string(settings, VD_PIXEL_FORMAT)), width, height,
			    prop);
	v4l2_close(dev);

	obs_property_modified(prop, settings);

	return true;
}

static void v4l2_add_properties(void *vptr, obs_properties_t *props, obs_data_t *settings)
{
	UNUSED_PARAMETER(vptr);
	UNUSED_PARAMETER(settings);

	obs_property_t *device_list = obs_properties_get(props, VD_DEVICE_ID);

	obs_property_t *input_list = obs_properties_add_list(props, S_INPUT, obs_module_text("v4l2.input"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_t *format_list = obs_properties_add_list(props, VD_PIXEL_FORMAT, obs_module_text("pixel-format"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_t *standard_list = obs_properties_add_list(props, S_STANDARD, obs_module_text("v4l2.standard"),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_visible(standard_list, false);

	obs_property_t *dv_timing_list = obs_properties_add_list(props, S_DV_TIMING, obs_module_text("v4l2.dv-timing"),
								 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_visible(dv_timing_list, false);

	obs_property_t *resolution_list = obs_properties_add_list(props, VD_RESOLUTION, obs_module_text("resolution"),
								  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_properties_add_list(props, VD_FRAME_RATE, obs_module_text("frame-rate"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);

	obs_properties_add_bool(props, S_AUTO_RESET, obs_module_text("v4l2.auto-reset"));
	obs_properties_add_int(props, S_TIMEOUT_FRAMES, obs_module_text("v4l2.timeout-frames"), 2, 120, 1);

	// a group to contain the camera control
	obs_properties_t *ctrl_props = obs_properties_create();
	obs_properties_add_group(props, "controls", obs_module_text("v4l2.camera-controls"), OBS_GROUP_NORMAL,
				 ctrl_props);

	if (device_list) {
		obs_property_set_modified_callback(device_list, device_selected);
	}
	obs_property_set_modified_callback(input_list, input_selected);
	obs_property_set_modified_callback(format_list, format_selected);
	obs_property_set_modified_callback(resolution_list, resolution_selected);
}

#if HAVE_UDEV
/**
 * Device added callback
 *
 * If everything went fine we can start capturing again when the device is
 * reconnected
 */
static void device_added(void *vptr, calldata_t *calldata)
{
	V4L2_DATA(vptr);

	obs_source_update_properties(data->source);

	const char *dev;
	calldata_get_string(calldata, "device", &dev);

	if (!data->device_id || strcmp(data->device_id, dev)) {
		return;
	}

	blog(LOG_INFO, "Device %s reconnected", dev);

	v4l2_init(data);
}
/**
 * Device removed callback
 *
 * We stop recording here so we don't block the device node
 */
static void device_removed(void *vptr, calldata_t *calldata)
{
	V4L2_DATA(vptr);

	obs_source_update_properties(data->source);

	const char *dev;
	calldata_get_string(calldata, "device", &dev);

	if (!data->device_id || strcmp(data->device_id, dev)) {
		return;
	}

	blog(LOG_INFO, "Device %s disconnected", dev);

	v4l2_terminate(data);
}

#endif

static void v4l2_terminate(struct v4l2_data *data)
{
	if (data->thread) {
		os_event_signal(data->event);
		pthread_join(data->thread, NULL);
		os_event_destroy(data->event);
		data->thread = 0;
	}

	if (data->pixfmt == V4L2_PIX_FMT_MJPEG || data->pixfmt == V4L2_PIX_FMT_H264) {
		v4l2_destroy_decoder(&data->decoder);
	}
	v4l2_destroy_mmap(&data->buffers);

	if (data->dev != -1) {
		v4l2_close(data->dev);
		data->dev = -1;
	}
}

static void v4l2_destroy(void *vptr)
{
	V4L2_DATA(vptr);

	if (!data) {
		return;
	}

	v4l2_terminate(data);

	if (data->device_id) {
		bfree(data->device_id);
	}

#if HAVE_UDEV
	signal_handler_t *sh = v4l2_get_udev_signalhandler();

	signal_handler_disconnect(sh, "device_added", device_added, data);
	signal_handler_disconnect(sh, "device_removed", device_removed, data);

	v4l2_unref_udev();
#endif

	bfree(data);
}

/**
 * Initialize the v4l2 device
 *
 * This function:
 * - tries to open the device
 * - sets pixelformat and requested resolution
 * - sets the requested framerate
 * - maps the buffers
 * - starts the capture thread
 */
static void v4l2_init(struct v4l2_data *data)
{
	uint32_t input_caps;
	int fps_num, fps_denom;

	blog(LOG_INFO, "Start capture from %s", data->device_id);
	data->dev = v4l2_open(data->device_id, O_RDWR | O_NONBLOCK);
	if (data->dev == -1) {
		blog(LOG_ERROR, "Unable to open device");
		goto fail;
	}

	/* set input */
	if (v4l2_set_input(data->dev, &data->input) < 0) {
		blog(LOG_ERROR, "Unable to set input %d", data->input);
		goto fail;
	}
	blog(LOG_INFO, "Input: %d", data->input);

	/* Saved control values, so they apply without opening the properties. */
	obs_data_t *settings = obs_source_get_settings(data->source);
	v4l2_apply_controls(data->dev, settings);
	obs_data_release(settings);
	if (v4l2_get_input_caps(data->dev, -1, &input_caps) < 0) {
		blog(LOG_ERROR, "Unable to get input capabilities");
		goto fail;
	}

	/* set video standard if supported */
	if (input_caps & V4L2_IN_CAP_STD) {
		if (v4l2_set_standard(data->dev, &data->standard) < 0) {
			blog(LOG_ERROR, "Unable to set video standard");
			goto fail;
		}
		data->resolution = -1;
		data->framerate = -1;
	}
	/* set dv timing if supported */
	if (input_caps & V4L2_IN_CAP_DV_TIMINGS) {
		if (v4l2_set_dv_timing(data->dev, &data->dv_timing) < 0) {
			blog(LOG_ERROR, "Unable to set dv timing");
			goto fail;
		}
		data->resolution = -1;
		data->framerate = -1;
	}

	/* set pixel format and resolution */
	if (v4l2_set_format(data->dev, &data->resolution, &data->pixfmt, &data->linesize) < 0) {
		blog(LOG_ERROR, "Unable to set format");
		goto fail;
	}
	if (v4l2_to_obs_video_format(data->pixfmt) == VIDEO_FORMAT_NONE && data->pixfmt != V4L2_PIX_FMT_MJPEG &&
	    data->pixfmt != V4L2_PIX_FMT_H264) {
		blog(LOG_ERROR, "Selected video format not supported");
		goto fail;
	}
	v4l2_unpack_tuple(&data->width, &data->height, data->resolution);
	blog(LOG_INFO, "Resolution: %dx%d", data->width, data->height);
	blog(LOG_INFO, "Pixelformat: %s", V4L2_FOURCC_STR(data->pixfmt));
	blog(LOG_INFO, "Linesize: %d Bytes", data->linesize);

	/* set framerate */
	if (v4l2_set_framerate(data->dev, &data->framerate) < 0) {
		blog(LOG_ERROR, "Unable to set framerate");
		goto fail;
	}
	if (data->framerate == 0) {
		blog(LOG_ERROR, "Framerate is not set, falling back to %i", FALLBACK_FRAMERATE);
		data->framerate = v4l2_pack_tuple(1, FALLBACK_FRAMERATE);
	}
	v4l2_unpack_tuple(&fps_num, &fps_denom, data->framerate);
	blog(LOG_INFO, "Framerate: %.2f fps", (float)fps_denom / fps_num);

	/* map buffers */
	if (v4l2_create_mmap(data->dev, &data->buffers) < 0) {
		blog(LOG_ERROR, "Failed to map buffers");
		goto fail;
	}

	if (data->pixfmt == V4L2_PIX_FMT_MJPEG || data->pixfmt == V4L2_PIX_FMT_H264) {
		if (v4l2_init_decoder(&data->decoder, data->pixfmt) < 0) {
			blog(LOG_ERROR, "Failed to initialize decoder");
			goto fail;
		}
	}

	/* start the capture thread */
	if (os_event_init(&data->event, OS_EVENT_TYPE_MANUAL) != 0) {
		goto fail;
	}
	if (pthread_create(&data->thread, NULL, v4l2_thread, data) != 0) {
		goto fail;
	}
	return;
fail:
	blog(LOG_ERROR, "Initialization failed, errno: %s", strerror(errno));
	v4l2_terminate(data);
}

/**
 * Checking if any of the settings have changed so that we can restart the
 * stream
 */
static bool v4l2_settings_changed(struct v4l2_data *data, const struct v4l2_settings *s)
{
	bool res = false;

	if (s->device_id != NULL && data->device_id != NULL) {
		res |= strcmp(data->device_id, s->device_id) != 0;
		res |= data->input != s->input;
		res |= data->pixfmt != s->pixfmt;
		res |= data->standard != s->standard;
		res |= data->dv_timing != s->dv_timing;

		if (s->resolution == -1 && !data->resolution_unchanged) {
			data->resolution_unchanged = true;
			res |= true;
		} else if (s->resolution == -1 && data->resolution_unchanged) {
			res |= false;
		} else {
			data->resolution_unchanged = false;
			res |= (data->resolution != s->resolution) && (s->resolution != -1);
		}

		if (s->framerate == -1 && !data->framerate_unchanged) {
			data->framerate_unchanged = true;
			res |= true;
		} else if (s->framerate == -1 && data->framerate_unchanged) {
			res |= false;
		} else {
			data->framerate_unchanged = false;
			res |= (data->framerate != s->framerate) && (s->framerate != -1);
		}

		res |= data->color_range != s->color_range;
		res |= data->color_space != s->color_space;
		res |= data->flip != s->flip;
	} else {
		res = true;
	}

	return res;
}

/**
 * Update the settings for the v4l2 source
 *
 * There are a few settings that can be changed without restarting the stream
 * Whenever this is called the currently active stream (if exists) is stopped,
 * the settings are updated and finally the new stream is started.
 */
static void v4l2_backend_update(void *vptr, obs_data_t *settings)
{
	V4L2_DATA(vptr);

	struct v4l2_settings s;
	read_settings(settings, &s);

	bool needs_restart = v4l2_settings_changed(data, &s);

	if (needs_restart) {
		v4l2_terminate(data);
	}

	if (data->device_id) {
		bfree(data->device_id);
	}

	data->device_id = bstrdup(s.device_id);
	data->input = s.input;
	data->pixfmt = s.pixfmt;
	data->standard = s.standard;
	data->dv_timing = s.dv_timing;
	data->resolution = s.resolution;
	data->framerate = s.framerate;
	data->color_range = s.color_range;
	data->color_space = s.color_space;
	data->flip = s.flip;
	data->auto_reset = s.auto_reset;
	data->timeout_frames = s.timeout_frames;

	obs_source_set_async_unbuffered(data->source, !s.buffering);

	if (needs_restart && data->device_id && *data->device_id) {
		v4l2_init(data);
	}
}

static void *v4l2_backend_create(obs_source_t *source, obs_data_t *settings)
{
	struct v4l2_data *data = bzalloc(sizeof(struct v4l2_data));
	data->dev = -1;
	data->source = source;
	data->resolution_unchanged = false;
	data->framerate_unchanged = false;

#ifndef V4L2_CAP_DEVICE_CAPS
	blog(LOG_WARNING, "Plugin built without device caps support!");
#endif
#if !defined(VIDIOC_ENUM_DV_TIMINGS) || !defined(V4L2_IN_CAP_DV_TIMINGS)
	blog(LOG_WARNING, "Plugin built without dv-timing support!");
#endif

	v4l2_backend_update(data, settings);

#if HAVE_UDEV
	v4l2_init_udev();
	signal_handler_t *sh = v4l2_get_udev_signalhandler();

	signal_handler_connect(sh, "device_added", &device_added, data);
	signal_handler_connect(sh, "device_removed", &device_removed, data);
#else
	blog(LOG_INFO, "Compiled without libudev, you can't reconnect devices");
#endif

	return data;
}

static bool v4l2_backend_available(void)
{
#ifdef __FreeBSD__
	return true;
#else
	return os_file_exists("/sys/class/video4linux");
#endif
}

const struct vd_backend vd_v4l2_backend = {
	.name = "v4l2",
	.display_name = "backend.v4l2",
	.available = v4l2_backend_available,
	.enum_devices = v4l2_enum_devices,
	.create = v4l2_backend_create,
	.destroy = v4l2_destroy,
	.update = v4l2_backend_update,
	.get_defaults = v4l2_backend_defaults,
	.add_properties = v4l2_add_properties,
	.has_audio = false,
};
