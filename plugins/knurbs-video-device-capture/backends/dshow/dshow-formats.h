/*
 * DirectShow video formats as "pixel-format" FourCCs.
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
