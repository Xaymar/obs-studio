/*
 * formats.c
 *
 * Copyright 2023 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "formats.h"

#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>

static const struct obs_pw_video_format supported_formats[] = {
	{
		SPA_VIDEO_FORMAT_BGRA,
		DRM_FORMAT_ARGB8888,
		GS_BGRA,
		VIDEO_FORMAT_BGRA,
		false,
		4,
		"ARGB8888",
	},
	{
		SPA_VIDEO_FORMAT_RGBA,
		DRM_FORMAT_ABGR8888,
		GS_RGBA,
		VIDEO_FORMAT_RGBA,
		false,
		4,
		"ABGR8888",
	},
	{
		SPA_VIDEO_FORMAT_BGRx,
		DRM_FORMAT_XRGB8888,
		GS_BGRX,
		VIDEO_FORMAT_BGRX,
		false,
		4,
		"XRGB8888",
	},
	{
		SPA_VIDEO_FORMAT_RGBx,
		DRM_FORMAT_XBGR8888,
		GS_BGRX,
		VIDEO_FORMAT_NONE,
		true,
		4,
		"XBGR8888",
	},
	{
		SPA_VIDEO_FORMAT_YUY2,
		DRM_FORMAT_YUYV,
		GS_UNKNOWN,
		VIDEO_FORMAT_YUY2,
		false,
		2,
		"YUYV422",
	},
	{
		SPA_VIDEO_FORMAT_NV12,
		DRM_FORMAT_NV12,
		GS_UNKNOWN,
		VIDEO_FORMAT_NV12,
		false,
		2,
		"NV12",
	},
	{
		SPA_VIDEO_FORMAT_UYVY,
		DRM_FORMAT_UYVY,
		GS_UNKNOWN,
		VIDEO_FORMAT_UYVY,
		false,
		2,
		"UYVY422",
	},
	{
		SPA_VIDEO_FORMAT_I420,
		DRM_FORMAT_YUV420,
		GS_UNKNOWN,
		VIDEO_FORMAT_I420,
		false,
		1,
		"I420",
	},
#if PW_CHECK_VERSION(0, 3, 41)
	{
		SPA_VIDEO_FORMAT_ABGR_210LE,
		DRM_FORMAT_ABGR2101010,
		GS_R10G10B10A2,
		VIDEO_FORMAT_NONE,
		false,
		10,
		"ABGR2101010",

	},
	{
		SPA_VIDEO_FORMAT_xBGR_210LE,
		DRM_FORMAT_XBGR2101010,
		GS_R10G10B10A2,
		VIDEO_FORMAT_NONE,
		false,
		10,
		"XBGR2101010",

	},
#endif
};

#define N_SUPPORTED_FORMATS (sizeof(supported_formats) / sizeof(supported_formats[0]))

bool obs_pw_video_format_from_spa_format(uint32_t spa_format, struct obs_pw_video_format *out_video_format)
{
	for (size_t i = 0; i < N_SUPPORTED_FORMATS; i++) {
		if (supported_formats[i].spa_format != spa_format)
			continue;

		if (out_video_format)
			*out_video_format = supported_formats[i];

		return true;
	}
	return false;
}

/* Bytes per row and rows of each plane of a tightly packed frame. Returns the
 * number of planes, or 0 for a format async frames can't carry. */
static uint32_t plane_layout(enum video_format format, uint32_t width, uint32_t height, uint32_t *linesize,
			     uint32_t *rows)
{
	switch (format) {
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_UYVY:
		linesize[0] = SPA_ROUND_UP_N(width * 2, 4);
		rows[0] = height;
		return 1;
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_RGBA:
		linesize[0] = width * 4;
		rows[0] = height;
		return 1;
	case VIDEO_FORMAT_NV12:
		linesize[0] = width;
		linesize[1] = SPA_ROUND_UP_N(width, 2);
		rows[0] = height;
		rows[1] = (height + 1) / 2;
		return 2;
	case VIDEO_FORMAT_I420:
		linesize[0] = width;
		linesize[1] = linesize[2] = (width + 1) / 2;
		rows[0] = height;
		rows[1] = rows[2] = (height + 1) / 2;
		return 3;
	default:
		return 0;
	}
}

bool obs_pw_frame_set_planes(struct obs_source_frame *frame, const struct spa_buffer *buffer)
{
	uint32_t linesize[MAX_AV_PLANES] = {0};
	uint32_t rows[MAX_AV_PLANES] = {0};
	uint32_t planes = plane_layout(frame->format, frame->width, frame->height, linesize, rows);

	if (planes == 0 || buffer->n_datas == 0)
		return false;

	for (uint32_t i = 0; i < planes; i++) {
		if (i < buffer->n_datas) {
			/* One data per plane, each with its own offset and stride. */
			const struct spa_data *d = &buffer->datas[i];
			if (!d->data)
				return false;
			frame->data[i] = (uint8_t *)d->data + d->chunk->offset;
			frame->linesize[i] = d->chunk->stride > 0 ? (uint32_t)d->chunk->stride : linesize[i];
		} else {
			/* The remaining planes follow the previous one in the same
			 * memory. Chroma strides scale with the luma stride. */
			uint32_t stride = frame->linesize[0] * linesize[i] / linesize[0];
			frame->data[i] = frame->data[i - 1] + (size_t)frame->linesize[i - 1] * rows[i - 1];
			frame->linesize[i] = stride;
		}
	}

	const struct spa_data *last = &buffer->datas[SPA_MIN(planes, buffer->n_datas) - 1];
	const uint8_t *end = (const uint8_t *)last->data + last->maxsize;
	const uint8_t *need = frame->data[planes - 1] + (size_t)frame->linesize[planes - 1] * rows[planes - 1];
	return need <= end;
}
