/*
 * DirectShow video formats as "pixel-format" FourCCs, and the frame rates
 * the DirectShow backend lists.
 *
 * Shared by the DirectShow backend and the settings migration, which
 * builds on every platform. The values are libdshowcapture's
 * DShow::VideoFormat, which the old source saved as "video_format"; the
 * backend checks them against the enum at compile time.
 *
 * The FourCCs are the V4L2/DRM names where one exists, so a saved format
 * means the same thing on every platform: YUY2 is "YUYV", I420 is "YU12",
 * Y800 is "GREY", and 32-bit BGRA/BGRX in memory are "AR24"/"XR24".
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 2 of the License, or (at your option)
 * any later version.
 */
#pragma once

#include <stddef.h>
#include <string.h>

struct vd_dshow_format {
	int value;          /* DShow::VideoFormat */
	const char *fourcc; /* "pixel-format" value */
	const char *label;  /* shown in the format list */
};

static const struct vd_dshow_format vd_dshow_formats[] = {
	/* raw formats */
	{100, "AR24", "ARGB"},
	{101, "XR24", "XRGB"},

	/* planar YUV formats */
	{200, "YU12", "I420"},
	{201, "NV12", "NV12"},
	{202, "YV12", "YV12"},
	{203, "GREY", "Y800"},
	{204, "P010", "P010"},

	/* packed YUV formats */
	{300, "YVYU", "YVYU"},
	{301, "YUYV", "YUY2"},
	{302, "UYVY", "UYVY"},
	{303, "HDYC", "HDYC"},

	/* encoded formats */
	{400, "MJPG", "MJPEG"},
	{401, "H264", "H264"},
	{402, "HEVC", "HEVC"},
};

#define VD_DSHOW_NUM_FORMATS (sizeof(vd_dshow_formats) / sizeof(vd_dshow_formats[0]))

static inline const char *vd_dshow_format_fourcc(int value)
{
	for (size_t i = 0; i < VD_DSHOW_NUM_FORMATS; i++) {
		if (vd_dshow_formats[i].value == value) {
			return vd_dshow_formats[i].fourcc;
		}
	}
	return NULL;
}

/* 0 (DShow::VideoFormat::Any) for an empty or unknown FourCC. */
static inline int vd_dshow_format_value(const char *fourcc)
{
	if (fourcc && *fourcc) {
		for (size_t i = 0; i < VD_DSHOW_NUM_FORMATS; i++) {
			if (strcmp(vd_dshow_formats[i].fourcc, fourcc) == 0) {
				return vd_dshow_formats[i].value;
			}
		}
	}
	return 0;
}

/* Frame rates offered in the list, as "frame-rate" values. DirectShow
 * works in frame intervals of 100 ns, so a device's 59.94 shows up as
 * 166833 and 60 as 166666. */
struct vd_dshow_frame_rate {
	const char *label;
	long long num, den;
};

static const struct vd_dshow_frame_rate vd_dshow_frame_rates[] = {
	{"360", 360, 1},
	{"240", 240, 1},
	{"144", 144, 1},
	{"120", 120, 1},
	{"119.88 NTSC", 120000, 1001},
	{"100", 100, 1},
	{"60", 60, 1},
	{"59.94 NTSC", 60000, 1001},
	{"50", 50, 1},
	{"48 film", 48000, 1001},
	{"40", 40, 1},
	{"30", 30, 1},
	{"29.97 NTSC", 30000, 1001},
	{"25", 25, 1},
	{"24 film", 24000, 1001},
	{"20", 20, 1},
	{"15", 15, 1},
	{"10", 10, 1},
	{"5", 5, 1},
	{"4", 4, 1},
	{"3", 3, 1},
	{"2", 2, 1},
	{"1", 1, 1},
};

#define VD_DSHOW_NUM_FRAME_RATES (sizeof(vd_dshow_frame_rates) / sizeof(vd_dshow_frame_rates[0]))

/* How far apart, in 100 ns units, a device's interval and a listed one
 * may be and still count as the same rate. */
#define VD_DSHOW_INTERVAL_TOLERANCE 20

static inline long long vd_dshow_interval(long long num, long long den)
{
	return num > 0 ? den * 10000000LL / num : 0;
}

/* The listed rate an interval stands for, or NULL. */
static inline const struct vd_dshow_frame_rate *vd_dshow_find_frame_rate(long long interval)
{
	for (size_t i = 0; i < VD_DSHOW_NUM_FRAME_RATES; i++) {
		long long diff = vd_dshow_interval(vd_dshow_frame_rates[i].num, vd_dshow_frame_rates[i].den) - interval;
		if (diff > -VD_DSHOW_INTERVAL_TOLERANCE && diff < VD_DSHOW_INTERVAL_TOLERANCE) {
			return &vd_dshow_frame_rates[i];
		}
	}
	return NULL;
}
